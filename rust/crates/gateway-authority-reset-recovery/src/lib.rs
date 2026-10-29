//! Complete checked reset effects; each source read set stays in its original final CAS.
use backend::Transaction;
use gateway_authority_contracts::{AuthorityClock, PROBE_LEASE_MS, TokenSource, add, invalid};
use gateway_authority_contracts::{next_generation, text, unavailable, validate_time};
use gateway_authority_reset_authorization::ResetAuthorization;
use gateway_authority_reset_evidence::{types::FullPoolResetReconciliation, types::ResetCandidate};
use gateway_authority_reset_permit::ResetPermitOperations;
use gateway_authority_reset_records::reset_invocation;
use gateway_authority_reset_records::{ResetInvocation, ResetRecord, ResetState, check_record};
use gateway_authority_routing_records::types::ProbeLease;
use gateway_authority_routing_selection::{types::AdmittedAccount, types::CohortProof};
use gateway_contracts::identity::RequestContext;
use gateway_contracts::{identity::ActiveSelection, identity::ActiveTransitionReason};
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
mod backend;
use gateway_authority_keys as keys;
struct RoutingView {
    pool: gateway_authority_routing_records::types::SubscriptionPool,
    state: gateway_authority_routing_records::types::RoutingState,
    active: Option<gateway_contracts::identity::ActiveSelection>,
    models: gateway_authority_routing_evidence::models::ModelAvailability,
    capacity: gateway_authority_routing_evidence::capacity::CapacityStore,
    overage: std::collections::BTreeMap<String, bool>,
}
impl RoutingView {
    fn policy(&self) -> gateway_authority_routing_policy::RoutingPolicyData<'_> {
        gateway_authority_routing_policy::RoutingPolicyData {
            pool: &self.pool,
            state: &self.state,
            active: self.active.as_ref(),
            models: &self.models,
            capacity: &self.capacity,
            overage: &self.overage,
        }
    }
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
        active: evidence.active().cloned(),
        models: evidence.models().clone(),
        capacity: evidence.capacity().clone(),
        overage: evidence.overage().clone(),
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
pub(crate) async fn capture_reconciliation_candidates<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    proof: &CohortProof,
    original: &RequestContext,
) -> Result<Vec<ResetCandidate>, GatewayError> {
    let now = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        gateway_authority_reset_evidence::capture_existing_facts(&reader, proof, original, now)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(evidence
        .facts()
        .members()
        .iter()
        .filter(|member| member.enabled)
        .map(|member| member.candidate.clone())
        .collect())
}
fn existing_reset_block_reason(row: &ResetRecord, now: i64) -> Result<Option<&str>, GatewayError> {
    let error = row.last_error_code.as_deref();
    Ok(Some(match row.state {
        ResetState::Verified if row.verified_at_ms.is_some() => return Ok(None),
        ResetState::Claimed if row.lease_expires_at_ms <= now => "unsubmitted_transaction",
        ResetState::Claimed => "in_progress",
        ResetState::Submitted | ResetState::Unknown => "terminal_outcome_ambiguous",
        ResetState::Rejected => error.unwrap_or("previously_rejected"),
        ResetState::Verified => return Err(invalid("Verified reset timestamp is missing.")),
    }))
}
pub struct ResetRecoveryService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
}
impl<'a, S: AuthorityStore, I: TokenSource> ResetRecoveryService<'a, S, I> {
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

    async fn elect_reset_recovery(
        &self,
        authorization: ResetAuthorization<'_>,
        expected: &ResetRecord,
        now: i64,
        stale: bool,
    ) -> Result<Option<AdmittedAccount>, GatewayError> {
        let candidate = authorization.candidate();
        for _ in 0..4 {
            let mut tx = self.transaction(now)?;
            let invocation = authorization_fences(&mut tx, authorization).await?;
            let key = keys::reset_redemption(
                &invocation.identity.account_id_hash,
                &invocation.identity.quota_generation,
            );
            let row: ResetRecord = tx
                .read(&key)
                .await?
                .ok_or_else(|| invalid("Verified reset record is missing."))?;
            if stale {
                if row.identity != expected.identity {
                    return Ok(None);
                }
            } else {
                check_record(&row, expected)?;
            }
            if row.state != ResetState::Verified
                || row.verified_at_ms.is_none()
                || (!stale && row.routing_generation != candidate.binding.routing_generation)
            {
                return Ok(None);
            }
            let mut view = read_view(&mut tx).await?;
            let slot_index = candidate.binding.slot as usize;
            let slot = &mut view.state.slots[slot_index];
            if slot
                .probe_lease
                .as_ref()
                .is_some_and(|lease| lease.expires_at_ms > now)
            {
                return Ok(None);
            }
            let released = slot
                .quota_block_for(candidate.proof.quota_class())
                .ok_or_else(|| unavailable("routing_fence_stale", "Reset quota class changed."))?;
            slot.remove_quota_class(candidate.proof.quota_class());
            slot.retained_reset_at_ms = released.reset_at_ms;
            slot.reset_identity_stable = released.reset_is_stable;
            slot.reset_identity_ambiguous = true;
            slot.recovery_pending = true;
            slot.generation = next_generation(slot.generation)?;
            let lease = ProbeLease {
                token: self.tokens.next_token()?,
                generation: slot.generation as i64,
                circuit: gateway_authority_routing_records::types::ProbeCircuit::Quota,
                quota_class: Some(candidate.proof.quota_class()),
                expires_at_ms: add(now, PROBE_LEASE_MS)?,
            };
            text(&lease.token)?;
            slot.probe_lease = Some(lease.clone());
            let binding = view.policy().binding(slot_index);
            let (active_generation, reason) = view.active.as_ref().map_or((1, None), |active| {
                (active.generation, active.transition_reason)
            });
            let changed = view
                .active
                .as_ref()
                .is_some_and(|active| active.binding.account_id_hash != binding.account_id_hash);
            let active_generation = if changed {
                next_generation(active_generation)?
            } else {
                active_generation
            };
            let active = ActiveSelection {
                v: 1,
                binding: binding.clone(),
                generation: active_generation,
                transition_reason: if changed {
                    Some(ActiveTransitionReason::QuotaExhausted)
                } else {
                    reason
                },
                updated_at_ms: now,
            };
            view.state.updated_at_ms = now;
            tx.put(&keys::routing(), &view.state, None)?;
            tx.put(&keys::active(), &active, None)?;
            let admitted = AdmittedAccount {
                binding,
                active_generation,
                requested_model: candidate.proof.model().clone(),
                quota_class: candidate.proof.quota_class(),
                probe_required: false,
                probe: Some(lease),
            };
            let before_commit = self.fresh_time(now)?;
            self.check_reset_authorization_deadline(authorization, before_commit)?;
            if admitted
                .probe
                .as_ref()
                .is_some_and(|lease| lease.expires_at_ms <= before_commit)
            {
                return Err(unavailable(
                    "routing_fence_stale",
                    "Reset recovery lease elapsed.",
                ));
            }
            if tx.commit().await? {
                return Ok(Some(admitted));
            }
        }
        Err(unavailable(
            "routing_fence_stale",
            "Verified reset election changed concurrently.",
        ))
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_reset_permit::ResetPermitFactory
    for ResetRecoveryService<'_, S, I>
{
    type Store = S;
    type Tokens = I;
    fn reset_permit(&self) -> gateway_authority_reset_permit::ResetPermitService<'_, S, I> {
        gateway_authority_reset_permit::ResetPermitService::new(self.store, self.tokens, self.clock)
    }
}
pub trait ResetRecoveryFactory {
    type Store: AuthorityStore;
    type Tokens: TokenSource;
    fn reset_recovery(&self) -> ResetRecoveryService<'_, Self::Store, Self::Tokens>;
}
pub trait ResetRecoveryOperations: ResetRecoveryFactory {
    /// Source preexistingBankedReset: never request inventory or create a redemption/budget.
    fn reconcile_full_pool_reset(
        &self,
        proof: &gateway_authority_routing_selection::types::CohortProof,
        original: &gateway_contracts::identity::RequestContext,
    ) -> impl std::future::Future<Output = Result<FullPoolResetReconciliation, GatewayError>> {
        async move {
            let context = self.reset_recovery();
            if !context.migrate_full_pool_reset_settings().await? {
                return Ok(FullPoolResetReconciliation::Blocked {
                    reason: "configuration_unavailable".into(),
                });
            }
            for _ in 0..4 {
                let now = context.fresh_time(original.started_at_ms)?;
                let mut tx = context.transaction(now)?;
                let candidates =
                    match capture_reconciliation_candidates(&mut tx, proof, original).await {
                        Ok(candidates) => candidates,
                        Err(error) => {
                            return Ok(FullPoolResetReconciliation::Blocked {
                                reason: error
                                    .envelope
                                    .error
                                    .code
                                    .unwrap_or_else(|| "configuration_unavailable".into()),
                            });
                        }
                    };
                let view = read_view(&mut tx).await?;
                let mut existing = None;
                for candidate in candidates {
                    let invocation = reset_invocation(
                        &view.pool.accounts[candidate.binding.slot as usize]
                            .admin_account_hash
                            .0,
                        &candidate.binding.credential_version.0,
                        candidate.quota_reset_at_ms,
                    )?;
                    let key = keys::reset_redemption(
                        &invocation.identity.account_id_hash,
                        &invocation.identity.quota_generation,
                    );
                    let Some(row) = tx.read::<ResetRecord>(&key).await? else {
                        continue;
                    };
                    if row.v != 1 || row.identity != invocation.identity {
                        return Err(invalid("Existing reset record context does not match."));
                    }
                    if let Some(reason) = existing_reset_block_reason(&row, now)? {
                        return Ok(FullPoolResetReconciliation::Blocked {
                            reason: reason.into(),
                        });
                    }
                    existing = Some((candidate, row));
                    break;
                }
                let before_commit = context.fresh_time(now)?;
                if !tx.commit().await? {
                    continue;
                }
                let Some((candidate, record)) = existing else {
                    return Ok(FullPoolResetReconciliation::NoExisting);
                };
                let stale = record.routing_generation != candidate.binding.routing_generation;
                return Ok(
                    match context
                        .elect_reset_recovery(
                            ResetAuthorization::ExistingLedger(&candidate, original),
                            &record,
                            before_commit,
                            stale,
                        )
                        .await?
                    {
                        Some(account) => FullPoolResetReconciliation::Recovered(account),
                        None => FullPoolResetReconciliation::Blocked {
                            reason: "routing_fence_stale".into(),
                        },
                    },
                );
            }
            Err(unavailable(
                "routing_fence_stale",
                "Existing reset reconciliation changed concurrently.",
            ))
        }
    }
    fn elect_verified_reset(
        &self,
        candidate: &ResetCandidate,
        expected: &ResetRecord,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Option<AdmittedAccount>, GatewayError>> {
        async move {
            let context = self.reset_recovery();
            context
                .elect_reset_recovery(ResetAuthorization::Legacy(candidate), expected, now, false)
                .await
        }
    }
    /// A documented reset terminal grants a fenced probe, never recovered capacity.
    fn elect_full_pool_reset(
        &self,
        permit: &gateway_authority_reset_permit::types::AutomaticResetPermit,
        expected: &ResetRecord,
    ) -> impl std::future::Future<Output = Result<Option<AdmittedAccount>, GatewayError>> {
        async move {
            let context = self.reset_recovery();
            let now = context.fresh_time(0)?;
            context
                .elect_reset_recovery(
                    ResetAuthorization::ExistingLedger(permit.candidate(), permit.original()),
                    expected,
                    now,
                    expected.routing_generation != permit.candidate().binding.routing_generation,
                )
                .await
        }
    }
    /// A stale verified ledger permits one current-generation recovery probe, never another spend.
    /// It retains the reset identity as ambiguous and releases only this class's deadline.
    fn claim_verified_reset_recovery(
        &self,
        candidate: &ResetCandidate,
        expected: &ResetRecord,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Option<AdmittedAccount>, GatewayError>> {
        async move {
            let context = self.reset_recovery();
            context
                .elect_reset_recovery(ResetAuthorization::Legacy(candidate), expected, now, true)
                .await
        }
    }
}
impl<T: ResetRecoveryFactory> ResetRecoveryOperations for T {}
