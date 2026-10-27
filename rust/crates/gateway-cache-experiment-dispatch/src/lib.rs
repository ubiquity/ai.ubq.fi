//! Complete checked source/session, dispatch, probe settlement and refresh-permit ownership.
mod backend;
mod view;
use backend::Transaction;
use gateway_auth::{
    crypto::Secret,
    provider_credentials::{CodexSourcePool, codex_source_pool_key, parse_codex_source_pool},
};
use gateway_authority_contracts::{
    AuthorityClock, PROBE_LEASE_MS, TokenSource, add, validate_time,
};
use gateway_authority_routing_evidence::models::model_quota_class;
use gateway_authority_routing_records::types::{ProbeCircuit, ProbeLease};
use gateway_authority_routing_signal::SignalOperations;
use gateway_cache_experiment_campaign::CyclePermit;
use gateway_cache_experiment_evidence::CacheEvidenceReader;
use gateway_cache_experiment_records::{
    RefreshContext,
    model::{Evidence, Reason, Sample, State, TargetBinding, failed},
    source::record_key,
};
use gateway_core::{
    error::GatewayError,
    identity::{AccountBinding, PoolVersionstamp},
    state::AuthorityStore,
};
use serde_json::Value;
use view::read_pinned_view;
pub struct Session {
    revision: String,
    ids: [String; 2],
    versions: [String; 2],
}
impl Session {
    pub fn revision(&self) -> &str {
        &self.revision
    }
}
pub struct CacheDispatch {
    binding: AccountBinding,
    probe: Option<ProbeLease>,
    model: String,
    account_id: String,
    access_token: Secret,
}
impl CacheDispatch {
    pub fn binding(&self) -> &AccountBinding {
        &self.binding
    }
    pub fn slot(&self) -> u8 {
        u8::try_from(self.binding.slot + 1).expect("validated two-slot index")
    }
    pub fn account_id(&self) -> &str {
        &self.account_id
    }
    pub fn access_token(&self) -> &Secret {
        &self.access_token
    }
}
/// Deserializing a stored context cannot manufacture an operation permit.
/// ```compile_fail
/// use gateway_cache_experiment_dispatch::RefreshPermit;
/// let forged: RefreshPermit = serde_json::from_str("{}").unwrap();
/// ```
pub struct RefreshPermit {
    context: RefreshContext,
    binding: gateway_core::identity::AccountBinding,
}
impl RefreshPermit {
    pub fn context(&self) -> &RefreshContext {
        &self.context
    }
    pub fn binding(&self) -> &gateway_core::identity::AccountBinding {
        &self.binding
    }
}

async fn source<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
) -> Result<(String, CodexSourcePool), GatewayError> {
    let key = codex_source_pool_key();
    let value: Value = tx.read(&key).await?.ok_or_else(|| {
        failed("Codex auth pool is unavailable for the prompt-cache scope experiment.")
    })?;
    let pool = parse_codex_source_pool(&value)?
        .filter(|p| p.accounts.len() == 2)
        .ok_or_else(|| {
            failed("Prompt-cache scope experiments require exactly two configured Codex slots.")
        })?;
    let revision = tx.revision(&key).ok_or_else(|| {
        failed("Codex auth pool is unavailable for the prompt-cache scope experiment.")
    })?;
    Ok((revision, pool))
}
async fn records<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    target: &TargetBinding,
) -> Result<(Option<State>, Option<Evidence>), GatewayError> {
    let observed_at_ms = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        CacheEvidenceReader::new(&reader, observed_at_ms)?
            .records(target)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(gateway_authority_contracts::invalid(
                "Cache evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok((evidence.state().cloned(), evidence.evidence().cloned()))
}
pub(crate) async fn validate_refresh_context<
    S: gateway_core::state::AuthorityStore,
    I: gateway_authority_contracts::TokenSource,
>(
    tx: &mut crate::backend::Transaction<'_, S, I>,
    context: &RefreshContext,
    now: i64,
) -> Result<(), gateway_core::error::GatewayError> {
    let observed_at_ms = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        gateway_cache_experiment_evidence::CacheEvidenceReader::new(&reader, observed_at_ms)?
            .validate_refresh_context(context, now)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(gateway_authority_contracts::invalid(
                "Cache evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(())
}
pub struct CacheDispatchService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
}
impl<'a, S: AuthorityStore, I: TokenSource> CacheDispatchService<'a, S, I> {
    pub fn new(store: &'a S, tokens: &'a I, clock: Option<&'a dyn AuthorityClock>) -> Self {
        Self {
            store,
            tokens,
            clock,
        }
    }
    fn transaction(&self, now: i64) -> Result<Transaction<'_, S, I>, GatewayError> {
        Transaction::new(self.store, self.tokens, now)
    }
    fn fresh_time(&self, started_at: i64) -> Result<i64, GatewayError> {
        let now = self
            .clock
            .as_ref()
            .ok_or_else(|| {
                gateway_authority_contracts::unavailable(
                    "authority_clock_unavailable",
                    "Time-sensitive authority actions require a trusted current clock.",
                )
            })?
            .now_ms()?;
        validate_time(now)?;
        if now < started_at {
            return Err(gateway_authority_contracts::unavailable(
                "invalid_clock",
                "Authority clock moved backwards.",
            ));
        }
        Ok(now)
    }
}
pub trait CacheDispatchFactory: gateway_authority_routing_signal::SignalFactory {
    fn cache_transport(&self) -> CacheDispatchService<'_, Self::Store, Self::Tokens>;
}
pub trait CacheDispatchOperations: CacheDispatchFactory + SignalOperations {
    fn ensure_cache_dispatch_current(
        &self,
        dispatch: &CacheDispatch,
        now: i64,
    ) -> impl std::future::Future<Output = Result<(), GatewayError>> {
        async move {
            let service = self.cache_transport();

            let mut tx = service.transaction(now)?;
            let (revision, pool) = source(&mut tx).await?;
            let current = pool
                .accounts
                .get(dispatch.binding.slot as usize)
                .ok_or_else(|| failed("Prompt-cache scope experiment slot is invalid."))?;
            if revision != dispatch.binding.pool_versionstamp.0
                || current.account_id != dispatch.account_id
                || current.identity().credential_version != dispatch.binding.credential_version
                || current.access_token != dispatch.access_token
            {
                return Err(failed(
                    "Codex credentials changed before dispatch during the prompt-cache scope experiment.",
                ));
            }
            Ok(())
        }
    }
    fn record_cache_quota_block(
        &self,
        dispatch: &CacheDispatch,
        block: gateway_authority_routing_records::types::QuotaBlock,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            self.record_bound_quota_block(
                &dispatch.binding,
                model_quota_class(&gateway_core::model::ModelId::from_scalar(&dispatch.model)),
                dispatch.probe.as_ref(),
                block,
                now,
            )
            .await
        }
    }
    fn cache_session(
        &self,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Session, GatewayError>> {
        async move {
            let service = self.cache_transport();

            let (revision, pool) = source(&mut service.transaction(now)?).await?;
            Ok(Session {
                revision,
                ids: [
                    pool.accounts[0].account_id.clone(),
                    pool.accounts[1].account_id.clone(),
                ],
                versions: [
                    pool.accounts[0].identity().credential_version.0,
                    pool.accounts[1].identity().credential_version.0,
                ],
            })
        }
    }
    fn cache_session_current(
        &self,
        session: &Session,
        now: i64,
    ) -> impl std::future::Future<Output = Result<(), GatewayError>> {
        async move {
            let current = self.cache_session(now).await?;
            if current.revision != session.revision
                || current.ids != session.ids
                || current.versions != session.versions
            {
                return Err(failed(
                    "Codex auth pool changed during the prompt-cache scope experiment.",
                ));
            }
            Ok(())
        }
    }
    fn cache_session_after_refresh(
        &self,
        session: &Session,
        publication: &PoolVersionstamp,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Result<Session, Reason>, GatewayError>> {
        async move {
            let service = self.cache_transport();

            let mut tx = service.transaction(now)?;
            let key = codex_source_pool_key();
            let value: Value = tx.read(&key).await?.ok_or_else(|| {
                failed("Codex auth pool is unavailable for the prompt-cache scope experiment.")
            })?;
            let pool = parse_codex_source_pool(&value)?.ok_or_else(|| {
                failed("Codex auth pool is unavailable for the prompt-cache scope experiment.")
            })?;
            let [first, second] = pool.accounts.as_slice() else {
                return Ok(Err(Reason::AuthPoolDrift));
            };
            let current = Session {
                revision: tx.revision(&key).ok_or_else(|| {
                    failed("Codex auth pool is unavailable for the prompt-cache scope experiment.")
                })?,
                ids: [first.account_id.clone(), second.account_id.clone()],
                versions: [
                    first.identity().credential_version.0,
                    second.identity().credential_version.0,
                ],
            };
            if current.revision != publication.0
                || current.ids != session.ids
                || current.versions[1] != session.versions[1]
            {
                return Ok(Err(Reason::AuthPoolDrift));
            }
            if current.versions[0] == session.versions[0] {
                return Ok(Err(Reason::RefreshUnchanged));
            }
            Ok(Ok(current))
        }
    }
    fn cache_pool_changed_after_refresh_error(
        &self,
        session: &Session,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let service = self.cache_transport();

            let mut tx = service.transaction(now)?;
            let key = codex_source_pool_key();
            let Some(value) = tx.read::<Value>(&key).await? else {
                return Ok(false);
            };
            if parse_codex_source_pool(&value)?.is_none() {
                return Ok(false);
            }
            Ok(tx
                .revision(&key)
                .is_some_and(|revision| revision != session.revision))
        }
    }
    fn cache_dispatch(
        &self,
        session: &Session,
        target: &TargetBinding,
        slot: u8,
        now: i64,
    ) -> impl std::future::Future<Output = Result<CacheDispatch, GatewayError>> {
        async move {
            let service = self.cache_transport();

            if !(1..=2).contains(&slot) {
                return Err(failed("Prompt-cache scope experiment slot is invalid."));
            }
            let index = usize::from(slot - 1);
            for _ in 0..3 {
                let mut tx = service.transaction(now)?;
                let (revision, pool) = source(&mut tx).await?;
                if revision != session.revision
                    || pool
                        .accounts
                        .iter()
                        .zip(&session.ids)
                        .any(|(a, b)| a.account_id != *b)
                    || pool
                        .accounts
                        .iter()
                        .zip(&session.versions)
                        .any(|(a, b)| a.identity().credential_version.0 != *b)
                {
                    return Err(failed(
                        "Codex auth pool changed during the prompt-cache scope experiment.",
                    ));
                }
                let mut view = read_pinned_view(&mut tx).await?;
                let current = service.fresh_time(now)?;
                let evaluation = gateway_authority_routing_policy::evaluate(
                    &view.policy(),
                    index,
                    &gateway_core::model::ModelId::from_scalar(&target.model),
                    model_quota_class(&gateway_core::model::ModelId::from_scalar(&target.model)),
                    current,
                );
                if !evaluation.eligible {
                    return Err(failed(
                        "The requested Codex slot is not eligible for the prompt-cache scope experiment.",
                    ));
                }
                let binding = view.policy().binding(index);
                let probe = if evaluation.probe {
                    let row = &mut view.state.slots[index];
                    if row
                        .probe_lease
                        .as_ref()
                        .is_some_and(|p| p.expires_at_ms > current)
                    {
                        return Err(failed(
                            "The requested Codex slot could not be claimed for the prompt-cache scope experiment.",
                        ));
                    }
                    let lease = ProbeLease {
                        token: service.tokens.next_token()?,
                        generation: row.generation as i64,
                        circuit: ProbeCircuit::Quota,
                        quota_class: Some(model_quota_class(
                            &gateway_core::model::ModelId::from_scalar(&target.model),
                        )),
                        expires_at_ms: add(current, PROBE_LEASE_MS)?,
                    };
                    row.probe_lease = Some(lease.clone());
                    view.state.updated_at_ms = current;
                    tx.put(&gateway_authority_keys::routing(), &view.state, None)?;
                    Some(lease)
                } else {
                    None
                };
                if tx.commit().await? {
                    return Ok(CacheDispatch {
                        binding,
                        probe,
                        model: target.model.clone(),
                        account_id: pool.accounts[index].account_id.clone(),
                        access_token: pool.accounts[index].access_token.clone(),
                    });
                }
            }
            Err(failed(
                "The requested Codex slot could not be claimed for the prompt-cache scope experiment.",
            ))
        }
    }
    fn finish_cache_dispatch(
        &self,
        dispatch: &CacheDispatch,
        completed: bool,
        now: i64,
    ) -> impl std::future::Future<Output = Result<(), GatewayError>> {
        async move {
            let service = self.cache_transport();

            let Some(expected) = &dispatch.probe else {
                return Ok(());
            };
            for _ in 0..3 {
                let mut tx = service.transaction(now)?;
                let mut view = read_pinned_view(&mut tx).await?;
                if !view.policy().current_binding(&dispatch.binding) {
                    return Ok(());
                }
                let row = &mut view.state.slots[dispatch.binding.slot as usize];
                if row.probe_lease.as_ref() != Some(expected) {
                    return Ok(());
                }
                row.probe_lease = None;
                match expected.circuit {
                    ProbeCircuit::Quota => {
                        let class = model_quota_class(&gateway_core::model::ModelId::from_scalar(
                            &dispatch.model,
                        ));
                        let retained = (row.retained_reset_at_ms, row.reset_identity_stable);
                        let recovering = row.recovery_pending;
                        let block = row.quota_block_for(class);
                        row.remove_quota_class(class);
                        if !completed && recovering {
                            row.retained_reset_at_ms = block
                                .as_ref()
                                .and_then(|block| block.reset_at_ms)
                                .or(retained.0);
                            row.reset_identity_stable = block
                                .as_ref()
                                .map(|block| block.reset_is_stable)
                                .unwrap_or(retained.1);
                            row.reset_identity_ambiguous = true;
                        }
                    }
                    ProbeCircuit::UpstreamTimeout => {
                        row.upstream_timeout_blocked_until_ms = None;
                    }
                }
                if completed && row.quota_blocks.is_empty() {
                    row.retained_reset_at_ms = None;
                    row.reset_identity_ambiguous = false;
                    row.reset_identity_stable = false;
                    row.recovery_pending = false;
                }
                view.state.updated_at_ms = now;
                tx.put(&gateway_authority_keys::routing(), &view.state, None)?;
                if tx.commit().await? {
                    return Ok(());
                }
            }
            Err(failed(
                "Prompt-cache scope experiment probe settlement changed concurrently.",
            ))
        }
    }
    fn cache_refresh_permit(
        &self,
        permit: &CyclePermit,
        target: &TargetBinding,
        prefix: [Sample; 5],
        release: &str,
        now: i64,
    ) -> impl std::future::Future<Output = Result<RefreshPermit, GatewayError>> {
        async move {
            let service = self.cache_transport();

            let mut tx = service.transaction(now)?;
            let _ = records(&mut tx, target).await?;
            let cycle_key = record_key("cycle_lease", target);
            let campaign_key = record_key("campaign", target);
            let _: Option<Value> = tx.read(&cycle_key).await?;
            let _: Option<Value> = tx.read(&campaign_key).await?;
            let view = read_pinned_view(&mut tx).await?;
            let current = service.fresh_time(now)?;
            if view.pool.accounts.len() != 2
                || view.pool.pool_versionstamp.0 != target.auth_pool_versionstamp
                || !gateway_authority_routing_policy::evaluate(
                    &view.policy(),
                    0,
                    &gateway_core::model::ModelId::from_scalar(&target.model),
                    model_quota_class(&gateway_core::model::ModelId::from_scalar(&target.model)),
                    current,
                )
                .eligible
            {
                return Err(failed(
                    "The requested Codex slot is not eligible for OAuth refresh during the prompt-cache scope experiment.",
                ));
            }
            let context = RefreshContext {
                v: 1,
                release_git_sha: release.into(),
                target: target.clone(),
                campaign_owner: permit.state().campaign_owner.clone(),
                cycle_owner: permit.cycle_owner().to_owned(),
                cycle: permit.state().next_cycle,
                source_pool_revision: target.auth_pool_versionstamp.clone(),
                slot: 1,
                state_revision: tx
                    .revision(&record_key("state", target))
                    .ok_or_else(|| failed("Prompt-cache scope experiment lost its lease."))?,
                cycle_lease_revision: tx
                    .revision(&cycle_key)
                    .ok_or_else(|| failed("Prompt-cache scope experiment lost its lease."))?,
                campaign_lease_revision: tx
                    .revision(&campaign_key)
                    .ok_or_else(|| failed("Prompt-cache scope experiment lost its lease."))?,
                prefix,
            };
            validate_refresh_context(&mut tx, &context, current).await?;
            if !tx.commit().await? {
                return Err(failed(
                    "Prompt-cache scope experiment refresh context changed concurrently.",
                ));
            }
            Ok(RefreshPermit {
                context,
                binding: view.policy().binding(0),
            })
        }
    }
}
impl<T: CacheDispatchFactory + SignalOperations> CacheDispatchOperations for T {}
