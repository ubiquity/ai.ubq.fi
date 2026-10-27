//! Complete checked campaign, cycle, evidence and publication ownership; records alone never mint permits.
mod backend;
pub mod promotion;
use backend::Transaction;
use gateway_authority_contracts::{AuthorityClock, TokenSource, add, validate_time};
use gateway_cache_experiment_evidence::CacheEvidenceReader;
use gateway_cache_experiment_records::{
    model::{
        Cycle, Evidence, LEASE_MS, Outcome, PROVIDER, Reason, SESSION_MS, State,
        TELEMETRY_PROVIDER, TargetBinding, busy, failed, unavailable,
    },
    source::{CampaignLease, CycleLease, active, campaign_owned, owns, record_key},
};
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
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
async fn resolve<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    expected: &TargetBinding,
) -> Result<Result<TargetBinding, Reason>, GatewayError> {
    let now = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        CacheEvidenceReader::new(&reader, now)?
            .resolve(expected)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(gateway_authority_contracts::invalid(
                "Cache evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(evidence.target().clone())
}
pub struct CyclePermit {
    state: State,
    cycle_owner: String,
}
impl CyclePermit {
    pub fn state(&self) -> &State {
        &self.state
    }
    pub fn cycle_owner(&self) -> &str {
        &self.cycle_owner
    }
}
pub struct PersistedCycle {
    result: Value,
    permit: CyclePermit,
}
impl PersistedCycle {
    pub fn into_response(self) -> Value {
        self.result
    }
    pub fn permit(&self) -> &CyclePermit {
        &self.permit
    }
}
pub struct CacheCampaignService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
}
impl<'a, S: AuthorityStore, I: TokenSource> CacheCampaignService<'a, S, I> {
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
    async fn cache_transition(
        &self,
        permit: &CyclePermit,
        next: Option<&State>,
        outcome: Outcome,
        cycle: Option<&Cycle>,
        reason: Option<Reason>,
        now: i64,
    ) -> Result<Value, GatewayError> {
        for _ in 0..3 {
            let mut tx = self.transaction(now)?;
            let state = &permit.state;
            let (persisted, prior) = records(&mut tx, &state.target)
                .await
                .map_err(|e| failed(&e.envelope.error.message))?;
            let cycle_key = record_key("cycle_lease", &state.target);
            let campaign_key = record_key("campaign", &state.target);
            let lease = tx.read::<Value>(&cycle_key).await?;
            let campaign = tx.read::<Value>(&campaign_key).await?;
            let current = self.fresh_time(now)?;
            if !owns(&lease, &permit.cycle_owner, current)
                || !campaign_owned(&campaign, state, current)
            {
                return Err(failed("Prompt-cache scope experiment lost its lease."));
            }
            let persisted = persisted.ok_or_else(|| {
                failed("Prompt-cache scope experiment state changed concurrently.")
            })?;
            if persisted.started_at_ms != state.started_at_ms
                || persisted.campaign_owner != state.campaign_owner
                || !persisted.target.same_definition(&state.target)
                || (next.is_some() && persisted.next_cycle != state.next_cycle)
            {
                return Err(failed(
                    "Prompt-cache scope experiment state changed concurrently.",
                ));
            }
            let mut cycles = prior.map(|e| e.cycles).unwrap_or_default();
            if let Some(cycle) = cycle {
                cycles.push(cycle.clone());
            }
            let evidence = Evidence {
                v: 3,
                target: next.unwrap_or(state).target.clone(),
                outcome,
                started_at_ms: state.started_at_ms,
                verified_at_ms: current,
                cycles,
                inconclusive_reason: reason,
            };
            let expiry = next.map(|s| s.expires_at_ms);
            tx.put(&record_key("evidence", &state.target), &evidence, expiry)?;
            if let Some(next) = next {
                tx.put(&record_key("state", &state.target), next, expiry)?;
                if next.next_cycle <= 3 {
                    tx.delete(&cycle_key)?;
                }
            } else {
                tx.delete(&record_key("state", &state.target))?;
                tx.delete(&cycle_key)?;
                tx.delete(&campaign_key)?;
            }
            if tx.commit().await? {
                let count = state.classifications.len()
                    + usize::from(cycle.is_some_and(|c| c.classification.is_some()));
                let mut result = json!({"provider":PROVIDER,"telemetry_provider":TELEMETRY_PROVIDER,"target_id":state.target.id,"model":state.target.model,"status":if next.is_some(){"in_progress"}else if outcome==Outcome::Completed{"completed"}else{"inconclusive"},"completed_cycles":count,"verified_at_ms":current});
                if let Some(reason) = reason {
                    result["inconclusive_reason"] = json!(reason);
                }
                if outcome == Outcome::Completed {
                    result["scope"] = json!(state.pending_scope);
                }
                return Ok(result);
            }
        }
        Err(failed(if next.is_some() {
            "Prompt-cache scope experiment evidence could not be persisted."
        } else {
            "Prompt-cache scope experiment final result could not be persisted."
        }))
    }
}
pub trait CacheCampaignFactory {
    type Store: AuthorityStore;
    type Tokens: TokenSource;
    fn cache_campaign(&self) -> CacheCampaignService<'_, Self::Store, Self::Tokens>;
}
impl<S: AuthorityStore, I: TokenSource> CacheCampaignFactory for CacheCampaignService<'_, S, I> {
    type Store = S;
    type Tokens = I;
    fn cache_campaign(&self) -> CacheCampaignService<'_, S, I> {
        CacheCampaignService::new(self.store, self.tokens, self.clock)
    }
}
pub trait CacheCampaignOperations: CacheCampaignFactory {
    fn acquire_cache_cycle(
        &self,
        target: &TargetBinding,
        pool_revision: &str,
        now: i64,
    ) -> impl std::future::Future<Output = Result<CyclePermit, GatewayError>> {
        async move {
            let service = self.cache_campaign();

            for _ in 0..3 {
                let mut tx = service.transaction(now)?;
                let (existing, evidence) = records(&mut tx, target).await?;
                let cycle_key = record_key("cycle_lease", target);
                let campaign_key = record_key("campaign", target);
                let cycle = tx.read::<Value>(&cycle_key).await?;
                let campaign = tx.read::<Value>(&campaign_key).await?;
                let current = service.fresh_time(now)?;
                tx.now = current;
                if active(&cycle, current) {
                    return Err(busy());
                }
                let resetting = existing.as_ref().is_none_or(|s| s.expires_at_ms <= current);
                let state = if let Some(mut state) = existing.filter(|s| s.expires_at_ms > current)
                {
                    if !state.target.same_core(target)
                        || state.target.inventory_fingerprint != target.inventory_fingerprint
                    {
                        return Err(unavailable(
                            "Prompt-cache scope experiment target changed while its campaign was active.",
                        ));
                    }
                    if !campaign_owned(&campaign, &state, current) {
                        return Err(if active(&campaign, current) {
                            busy()
                        } else {
                            unavailable(
                                "Prompt-cache scope experiment lost its provider campaign lease.",
                            )
                        });
                    }
                    state.target = target.clone();
                    state
                } else {
                    if active(&campaign, current) {
                        return Err(busy());
                    }
                    State {
                        v: 3,
                        target: target.clone(),
                        campaign_owner: service.tokens.next_token()?,
                        started_at_ms: current,
                        expires_at_ms: add(current, SESSION_MS)?,
                        auth_pool_versionstamp: pool_revision.into(),
                        next_cycle: 1,
                        classifications: vec![],
                        pending_scope: None,
                    }
                };
                let owner = service.tokens.next_token()?;
                tx.put(
                    &cycle_key,
                    &CycleLease {
                        owner: owner.clone(),
                        lease_until_ms: add(current, LEASE_MS)?,
                    },
                    Some(add(current, LEASE_MS * 2)?),
                )?;
                tx.put(
                    &campaign_key,
                    &CampaignLease {
                        owner: state.campaign_owner.clone(),
                        target_id: state.target.id.clone(),
                        inventory_fingerprint: state.target.inventory_fingerprint.clone(),
                        lease_until_ms: state.expires_at_ms,
                    },
                    Some(state.expires_at_ms),
                )?;
                tx.put(
                    &record_key("state", target),
                    &state,
                    Some(state.expires_at_ms),
                )?;
                if resetting && evidence.is_some() {
                    tx.delete(&record_key("evidence", target))?;
                }
                if tx.commit().await? {
                    return Ok(CyclePermit {
                        state,
                        cycle_owner: owner,
                    });
                }
            }
            Err(busy())
        }
    }
    fn renew_cache_cycle(
        &self,
        permit: &CyclePermit,
        now: i64,
    ) -> impl std::future::Future<Output = Result<(), GatewayError>> {
        async move {
            let service = self.cache_campaign();

            for _ in 0..3 {
                let mut tx = service.transaction(now)?;
                let state = &permit.state;
                let cycle_key = record_key("cycle_lease", &state.target);
                let campaign_key = record_key("campaign", &state.target);
                let cycle = tx.read::<Value>(&cycle_key).await?;
                let campaign = tx.read::<Value>(&campaign_key).await?;
                let current = service.fresh_time(now)?;
                if !owns(&cycle, &permit.cycle_owner, current)
                    || !campaign_owned(&campaign, state, current)
                {
                    return Err(failed("Prompt-cache scope experiment lost its lease."));
                }
                tx.put(
                    &cycle_key,
                    &CycleLease {
                        owner: permit.cycle_owner.clone(),
                        lease_until_ms: add(current, LEASE_MS)?,
                    },
                    Some(add(current, LEASE_MS * 2)?),
                )?;
                tx.put(
                    &campaign_key,
                    &CampaignLease {
                        owner: state.campaign_owner.clone(),
                        target_id: state.target.id.clone(),
                        inventory_fingerprint: state.target.inventory_fingerprint.clone(),
                        lease_until_ms: state.expires_at_ms,
                    },
                    Some(state.expires_at_ms),
                )?;
                if tx.commit().await? {
                    return Ok(());
                }
            }
            Err(failed(
                "Prompt-cache scope experiment could not renew its lease.",
            ))
        }
    }
    fn persist_cache_cycle(
        &self,
        permit: &CyclePermit,
        next: &State,
        cycle: &Cycle,
        now: i64,
    ) -> impl std::future::Future<Output = Result<PersistedCycle, GatewayError>> {
        async move {
            let service = self.cache_campaign();

            let result = service
                .cache_transition(
                    permit,
                    Some(next),
                    if next.next_cycle == 4 {
                        Outcome::ReadyToPromote
                    } else {
                        Outcome::InProgress
                    },
                    Some(cycle),
                    None,
                    now,
                )
                .await?;
            Ok(PersistedCycle {
                result,
                permit: CyclePermit {
                    state: next.clone(),
                    cycle_owner: permit.cycle_owner.clone(),
                },
            })
        }
    }
    fn finalize_cache_cycle(
        &self,
        permit: &CyclePermit,
        outcome: Outcome,
        cycle: Option<&Cycle>,
        reason: Option<Reason>,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Value, GatewayError>> {
        async move {
            let service = self.cache_campaign();

            service
                .cache_transition(permit, None, outcome, cycle, reason, now)
                .await
        }
    }
    fn current_cache_target(
        &self,
        expected: &TargetBinding,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Result<TargetBinding, Reason>, GatewayError>>
    {
        async move {
            let service = self.cache_campaign();

            resolve(&mut service.transaction(now)?, expected).await
        }
    }
}
impl<T: CacheCampaignFactory> CacheCampaignOperations for T {}
