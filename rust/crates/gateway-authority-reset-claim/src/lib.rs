//! Complete checked reset effects; each source read set stays in its original final CAS.
use backend::Transaction;
use gateway_authority_contracts::{AuthorityClock, PROBE_LEASE_MS, TokenSource, add, invalid};
use gateway_authority_contracts::{next_generation, text, unavailable, validate_time};
use gateway_authority_reset_authorization::{ResetAuthorization, shadow_episode};
use gateway_authority_reset_evidence::types::ResetCandidate;
use gateway_authority_reset_records::{ResetClaim, ResetConfig, ResetCredit, reset_invocation};
use gateway_authority_reset_records::{ResetInvocation, ResetMode, ResetRecord, ResetShadow};
use gateway_authority_reset_records::{ResetState, hash};
use gateway_authority_routing_selection::types::CohortProof;
use gateway_core::{error::GatewayError, state::AuthorityStore};
mod backend;
use gateway_authority_keys as keys;
struct RoutingView {
    pool: gateway_authority_routing_records::types::SubscriptionPool,
    state: gateway_authority_routing_records::types::RoutingState,
}

async fn read_view<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
) -> Result<RoutingView, GatewayError> {
    let now = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        gateway_authority_routing_evidence::reader::RoutingEvidenceReader::new(&reader, now)?
            .read(gateway_authority_routing_evidence::reader::RoutingReadMode::GlobalSelection)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(gateway_authority_contracts::invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(RoutingView {
        pool: evidence.pool().clone(),
        state: evidence.state().clone(),
    })
}

async fn authorization_fences<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    authorization: ResetAuthorization<'_>,
) -> Result<ResetInvocation, GatewayError> {
    let now = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        authorization.fence_data(&reader, now).await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(evidence.invocation().clone())
}
async fn validate_authorization<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    authorization: ResetAuthorization<'_>,
) -> Result<(), GatewayError> {
    let now = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        authorization.current_data(&reader, now).await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(())
}
async fn reset_fences<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    candidate: &ResetCandidate,
) -> Result<ResetInvocation, GatewayError> {
    authorization_fences(tx, ResetAuthorization::Legacy(candidate)).await
}
enum PreparedResetClaim {
    Ready {
        row: ResetRecord,
        invocation: ResetInvocation,
    },
    Declined(ResetClaim),
}
pub struct ResetClaimService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
}
impl<'a, S: AuthorityStore, I: TokenSource> ResetClaimService<'a, S, I> {
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
                crate::unavailable(
                    "authority_clock_unavailable",
                    "Time-sensitive authority actions require a trusted current clock.",
                )
            })?
            .now_ms()?;
        validate_time(now)?;
        if now < started_at {
            return Err(crate::unavailable(
                "invalid_clock",
                "Authority clock moved backwards.",
            ));
        }
        Ok(now)
    }
    fn check_reset_authorization_deadline(
        &self,
        authorization: ResetAuthorization<'_>,
        anchor: i64,
    ) -> Result<(), GatewayError> {
        if let ResetAuthorization::FullPool(permit) = authorization {
            permit.check_deadline(self.fresh_time(anchor)?)?;
        }
        Ok(())
    }

    async fn claim_reset_guarded(
        &self,
        authorization: ResetAuthorization<'_>,
        now: i64,
    ) -> Result<ResetClaim, GatewayError> {
        let candidate = authorization.candidate();
        for _ in 0..4 {
            let mut tx = self.transaction(now)?;
            validate_authorization(&mut tx, authorization).await?;
            let view = read_view(&mut tx).await?;
            let slot = usize::try_from(candidate.binding.slot)
                .map_err(|_| invalid("Reset slot is invalid."))?;
            let account = view
                .pool
                .accounts
                .get(slot)
                .filter(|account| account.account_id_hash == candidate.binding.account_id_hash)
                .ok_or_else(|| unavailable("routing_fence_stale", "Reset account was removed."))?;
            let invocation = reset_invocation(
                &account.admin_account_hash.0,
                &candidate.binding.credential_version.0,
                candidate.quota_reset_at_ms,
            )?;
            let key = keys::reset_redemption(
                &invocation.identity.account_id_hash,
                &invocation.identity.quota_generation,
            );
            let existing: Option<ResetRecord> = tx.read(&key).await?;
            let config: ResetConfig = tx.read(&keys::reset_config()).await?.unwrap_or_default();
            if let Some(record) = &existing {
                if record.v != 1 || record.identity != invocation.identity {
                    return Err(unavailable(
                        "redemption_record_context_mismatch",
                        "Reset record context does not match this credential/window.",
                    ));
                }
                if record.state == ResetState::Verified {
                    return Ok(ResetClaim::Verified(record.clone()));
                }
                if record.state == ResetState::Rejected {
                    return Ok(ResetClaim::Rejected(record.clone()));
                }
                if record.lease_expires_at_ms > now {
                    return Ok(ResetClaim::InProgress(record.clone()));
                }
                if matches!(record.state, ResetState::Submitted | ResetState::Unknown) {
                    let mut takeover = record.clone();
                    takeover.owner_token = self.tokens.next_token()?;
                    text(&takeover.owner_token)?;
                    takeover.fence = next_generation(record.fence)?;
                    takeover.lease_expires_at_ms = add(now, PROBE_LEASE_MS)?;
                    takeover.updated_at_ms = now;
                    tx.put(&key, &takeover, None)?;
                    self.check_reset_authorization_deadline(authorization, now)?;
                    if tx.commit().await? {
                        return Ok(ResetClaim::Reconcile {
                            record: takeover,
                            invocation,
                        });
                    } else {
                        continue;
                    }
                }
            }
            let (row, invocation) = match self
                .prepare_reset_claim(&mut tx, authorization, &config, existing, now)
                .await?
            {
                PreparedResetClaim::Ready { row, invocation } => (row, invocation),
                PreparedResetClaim::Declined(result) => return Ok(result),
            };
            tx.put(&key, &row, None)?;
            self.check_reset_authorization_deadline(authorization, now)?;
            if tx.commit().await? {
                return Ok(ResetClaim::Submit {
                    record: row,
                    invocation,
                });
            }
        }
        Err(unavailable(
            "stale_owner",
            "Reset claim changed concurrently.",
        ))
    }
    async fn prepare_reset_claim(
        &self,
        tx: &mut Transaction<'_, S, I>,
        authorization: ResetAuthorization<'_>,
        config: &ResetConfig,
        existing: Option<ResetRecord>,
        now: i64,
    ) -> Result<PreparedResetClaim, GatewayError> {
        let candidate = authorization.candidate();
        if let Some(reason) = config.live_reason() {
            return Ok(PreparedResetClaim::Declined(ResetClaim::Disabled(reason)));
        }
        let invocation = authorization_fences(tx, authorization).await?;
        if candidate.quota_reset_at_ms <= now {
            return Err(unavailable(
                "quota_window_expired",
                "Reset quota window expired.",
            ));
        }
        if let ResetAuthorization::Legacy(_) = authorization {
            let shadow: ResetShadow = tx
                .read(&keys::reset_shadow(&shadow_episode(candidate)))
                .await?
                .ok_or_else(|| {
                    unavailable(
                        "shadow_decision_missing",
                        "Reset requires a prior live-armed decision.",
                    )
                })?;
            if shadow.account_id_hash != invocation.identity.account_id_hash
                || shadow.quota_generation != invocation.identity.quota_generation
            {
                return Err(unavailable(
                    "shadow_decision_stale",
                    "Shadow decision selected a different reset account/window.",
                ));
            }
            if shadow.expires_at_ms <= now
                || shadow
                    .armed_at_ms
                    .is_none_or(|armed| armed >= candidate.request_started_at_ms)
            {
                return Ok(PreparedResetClaim::Declined(ResetClaim::Disabled(
                    "live_armed",
                )));
            }
        }
        let row = if let Some(mut row) = existing {
            if row.routing_generation != candidate.binding.routing_generation {
                return Err(unavailable(
                    "routing_fence_stale",
                    "Claimed reset generation changed.",
                ));
            }
            row.owner_token = self.tokens.next_token()?;
            row.fence = next_generation(row.fence)?;
            row.lease_expires_at_ms = add(now, PROBE_LEASE_MS)?;
            row.updated_at_ms = now;
            row
        } else {
            ResetRecord {
                v: 1,
                identity: invocation.identity.clone(),
                routing_generation: candidate.binding.routing_generation,
                state: ResetState::Claimed,
                owner_token: self.tokens.next_token()?,
                fence: 1,
                lease_expires_at_ms: add(now, PROBE_LEASE_MS)?,
                provider_receipt_id: None,
                created_at_ms: now,
                updated_at_ms: now,
                submitted_at_ms: None,
                verified_at_ms: None,
                last_error_code: None,
            }
        };
        text(&row.owner_token)?;
        let before_claim = self.fresh_time(now)?;
        if before_claim >= candidate.quota_reset_at_ms {
            return Err(unavailable(
                "quota_window_expired",
                "Reset quota window expired during fence reads.",
            ));
        }
        if row.lease_expires_at_ms <= before_claim {
            return Err(unavailable(
                "stale_owner",
                "Reset claim lease elapsed during fence reads.",
            ));
        }
        Ok(PreparedResetClaim::Ready { row, invocation })
    }
}
pub trait ResetClaimFactory {
    type Store: AuthorityStore;
    type Tokens: TokenSource;
    fn reset_claim(&self) -> ResetClaimService<'_, Self::Store, Self::Tokens>;
}
pub trait ResetClaimOperations: ResetClaimFactory {
    /// Preserve the original audit (including expiry) so inspection cannot revive or arm it.
    fn read_reset_shadow(
        &self,
        candidate: &ResetCandidate,
    ) -> impl std::future::Future<Output = Result<Option<ResetShadow>, GatewayError>> {
        async move {
            let context = self.reset_claim();
            for _ in 0..3 {
                let now = context.fresh_time(0)?;
                let mut tx = context.transaction(now)?;
                reset_fences(&mut tx, candidate).await?;
                let config: ResetConfig = tx.read(&keys::reset_config()).await?.unwrap_or_default();
                if !config.enabled || config.mode == ResetMode::Disabled {
                    return Err(unavailable(
                        "feature_disabled",
                        "Reset policy forbids observations.",
                    ));
                }
                let row: Option<ResetShadow> = tx
                    .read(&keys::reset_shadow(&shadow_episode(candidate)))
                    .await?;
                if row.as_ref().is_some_and(|r| r.v != 1) {
                    return Err(invalid("Malformed reset shadow audit."));
                }
                context.fresh_time(now)?;
                if tx.commit().await? {
                    return Ok(row);
                }
            }
            Err(unavailable(
                "routing_fence_stale",
                "Reset audit changed concurrently.",
            ))
        }
    }
    /// Finite pre-inventory projection; the fixed host rechecks again before every effect.
    fn reset_candidates(
        &self,
        proof: &gateway_authority_routing_selection::types::CohortProof,
    ) -> impl std::future::Future<Output = Result<Vec<ResetCandidate>, GatewayError>> {
        async move {
            let context = self.reset_claim();
            for _ in 0..3 {
                let now = context.fresh_time(0)?;
                let mut tx = context.transaction(now)?;
                if !proof.full_cohort_exhausted() || !validate_cohort(&mut tx, proof, true).await? {
                    return Err(unavailable(
                        "routing_fence_stale",
                        "Current full quota cohort is required.",
                    ));
                }
                let config: ResetConfig = tx.read(&keys::reset_config()).await?.unwrap_or_default();
                if !config.enabled
                    || config.mode == ResetMode::Disabled
                    || config.max_per_account_per_day <= 0
                    || config.max_per_account_per_window != 1
                {
                    return Ok(Vec::new());
                }
                let view = read_view(&mut tx).await?;
                let mut candidates = Vec::new();
                for binding in proof.account_bindings() {
                    let Some(slot) = view.state.slots.get(binding.slot as usize) else {
                        continue;
                    };
                    let Some(block) = slot.quota_block_for(proof.quota_class()) else {
                        continue;
                    };
                    if block.blocked_until_ms <= now
                        || block.reset_at_ms != Some(block.blocked_until_ms)
                        || !block.reset_is_stable
                        || block.reset_ambiguous
                        || slot.reset_identity_ambiguous
                    {
                        continue;
                    }
                    let candidate = ResetCandidate {
                        binding: binding.clone(),
                        quota_reset_at_ms: block.blocked_until_ms,
                        request_started_at_ms: now,
                        proof: proof.clone(),
                    };
                    let index = binding.slot as usize;
                    let setting: Option<serde_json::Value> = tx
                        .read(&keys::reset_usage(
                            &view.pool.accounts[index].admin_account_hash.0,
                        ))
                        .await?;
                    if setting.as_ref().is_some_and(|v| {
                        v.get("enabled").and_then(serde_json::Value::as_bool) != Some(true)
                    }) {
                        continue;
                    }
                    reset_fences(&mut tx, &candidate).await?;
                    candidates.push(candidate);
                }
                if context.fresh_time(now)?
                    >= candidates
                        .iter()
                        .map(|c| c.quota_reset_at_ms)
                        .min()
                        .unwrap_or(i64::MAX)
                {
                    continue;
                }
                if tx.commit().await? {
                    return Ok(candidates);
                }
            }
            Err(unavailable(
                "routing_fence_stale",
                "Reset inventory cohort changed concurrently.",
            ))
        }
    }
    /// Persist the complete observed shadow selection before a later live request may consume.
    fn record_reset_shadow(
        &self,
        candidate: &ResetCandidate,
        credit: &ResetCredit,
        now: i64,
    ) -> impl std::future::Future<Output = Result<ResetShadow, GatewayError>> {
        async move {
            let context = self.reset_claim();
            text(&credit.credit_id)?;
            if !credit.eligible
                || credit.observed_at_ms > now
                || now - credit.observed_at_ms > 30_000
                || credit.expires_at_ms.is_some_and(|expiry| expiry <= now)
            {
                return Err(unavailable(
                    "inventory_response_invalid_or_expired",
                    "Reset credit inventory is not current.",
                ));
            }
            for _ in 0..4 {
                let mut tx = context.transaction(now)?;
                let invocation = reset_fences(&mut tx, candidate).await?;
                let config: ResetConfig = tx.read(&keys::reset_config()).await?.unwrap_or_default();
                if !config.enabled || config.mode == ResetMode::Disabled {
                    return Err(unavailable(
                        "feature_disabled",
                        "Reset feature is disabled.",
                    ));
                }
                let episode_hash = shadow_episode(candidate);
                let key = keys::reset_shadow(&episode_hash);
                let prior: Option<ResetShadow> = tx.read(&key).await?;
                let credit_hash =
                    hash(&format!("uos_ai\0codex_reset_credit\0{}", credit.credit_id));
                let same = prior.as_ref().filter(|row| {
                    row.expires_at_ms > now
                        && row.selected_credit_id_hash == credit_hash
                        && row.account_id_hash == invocation.identity.account_id_hash
                });
                let row = ResetShadow {
                    v: 1,
                    episode_hash,
                    account_id_hash: invocation.identity.account_id_hash,
                    quota_generation: invocation.identity.quota_generation,
                    selected_credit_id_hash: credit_hash,
                    credit_expires_at_ms: credit.expires_at_ms,
                    observed_at_ms: credit.observed_at_ms,
                    created_at_ms: same.map_or(now, |row| row.created_at_ms),
                    expires_at_ms: add(now, 30_000)?.min(credit.expires_at_ms.unwrap_or(i64::MAX)),
                    armed_at_ms: same.and_then(|row| row.armed_at_ms),
                };
                tx.put(&key, &row, None)?;
                if tx.commit().await? {
                    return Ok(row);
                }
            }
            Err(unavailable(
                "routing_fence_stale",
                "Shadow selection changed concurrently.",
            ))
        }
    }
    fn arm_reset_live(
        &self,
        candidate: &ResetCandidate,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.reset_claim();
            for _ in 0..4 {
                let mut tx = context.transaction(now)?;
                let invocation = reset_fences(&mut tx, candidate).await?;
                let config: ResetConfig = tx.read(&keys::reset_config()).await?.unwrap_or_default();
                if let Some(reason) = config.live_reason() {
                    return Err(unavailable(reason, "Reset live policy is not authorized."));
                }
                let key = keys::reset_shadow(&shadow_episode(candidate));
                let mut row: ResetShadow = tx.read(&key).await?.ok_or_else(|| {
                    unavailable(
                        "shadow_decision_missing",
                        "A prior shadow decision is required.",
                    )
                })?;
                if row.account_id_hash != invocation.identity.account_id_hash
                    || row.quota_generation != invocation.identity.quota_generation
                {
                    return Err(unavailable(
                        "shadow_decision_stale",
                        "Shadow decision selected a different reset account/window.",
                    ));
                }
                if row.expires_at_ms <= now
                    || row.credit_expires_at_ms.is_some_and(|expiry| expiry <= now)
                {
                    return Err(unavailable(
                        "shadow_decision_stale",
                        "Reset shadow decision expired.",
                    ));
                }
                let newly_armed = row.armed_at_ms.is_none();
                row.armed_at_ms.get_or_insert(now);
                tx.put(&key, &row, None)?;
                if tx.commit().await? {
                    return Ok(newly_armed);
                }
            }
            Err(unavailable(
                "routing_fence_stale",
                "Live arming changed concurrently.",
            ))
        }
    }
    fn claim_reset(
        &self,
        candidate: &ResetCandidate,
        now: i64,
    ) -> impl std::future::Future<Output = Result<ResetClaim, GatewayError>> {
        async move {
            let context = self.reset_claim();
            context
                .claim_reset_guarded(ResetAuthorization::Legacy(candidate), now)
                .await
        }
    }
    /// Complete source audit permission, validated inside the existing shared spend transaction.
    fn claim_full_pool_reset(
        &self,
        permit: &gateway_authority_reset_permit::types::AutomaticResetPermit,
    ) -> impl std::future::Future<Output = Result<ResetClaim, GatewayError>> {
        async move {
            let context = self.reset_claim();
            let now = context.fresh_time(0)?;
            context
                .claim_reset_guarded(ResetAuthorization::FullPool(permit), now)
                .await
        }
    }
}
impl<T: ResetClaimFactory> ResetClaimOperations for T {}

pub(crate) async fn validate_cohort<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    proof: &CohortProof,
    quota_only: bool,
) -> Result<bool, GatewayError> {
    for check in proof.revision_fences() {
        if check.key == keys::models() {
            let _: Option<gateway_core::source_json::SourceJson> =
                tx.read_source_control(&check.key).await?;
        } else {
            let _: Option<serde_json::Value> = tx.read(&check.key).await?;
        }
        if !tx.expect(&check.key, &check.expected_revision) {
            return Ok(false);
        }
    }
    let now = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        gateway_authority_routing_evidence::reader::RoutingEvidenceReader::new(&reader, now)?
            .read(gateway_authority_routing_evidence::reader::RoutingReadMode::GlobalSelection)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(proof.matches_current_evidence(&evidence, quota_only))
}
