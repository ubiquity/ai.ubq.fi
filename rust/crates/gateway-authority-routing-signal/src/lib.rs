//! Complete non-actor R-state observations and probe lifecycles over the original final CAS.
mod backend;
use backend::Transaction;
use gateway_authority_contracts::{PROBE_LEASE_MS, TokenSource, add, invalid, next_generation};
use gateway_authority_contracts::{nonnegative, text, unavailable};
use gateway_authority_keys as keys;
use gateway_authority_routing_evidence::{capacity::CapacityStore, models::ModelAvailability};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_authority_routing_records::{slot::class_name, types::ProbeCircuit, types::ProbeLease};
use gateway_authority_routing_records::{types::QuotaBlock, types::RoutingState};
use gateway_authority_routing_selection::{types::AdmittedAccount, types::CohortProof};
use gateway_authority_routing_selection::{types::LiveAccountSelection, types::ScopedOverageProbe};
use gateway_contracts::identity::RoutingAccountHash;
use gateway_contracts::provider::CodexQuotaClass;
use gateway_contracts::{identity::AccountBinding, identity::ActiveSelection};
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
use gateway_source::js_string::JsString16;
use std::collections::BTreeMap;
pub(crate) struct RoutingView {
    pub pool: SubscriptionPool,
    pub state: RoutingState,
    pub active: Option<ActiveSelection>,
    models: ModelAvailability,
    models_source: gateway_source::source_json::SourceJson,
    source_ids: BTreeMap<String, String>,
    pub(crate) capacity: CapacityStore,
    pub(crate) overage: BTreeMap<String, bool>,
}

pub(crate) async fn read_view<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
) -> Result<RoutingView, GatewayError> {
    read_view_for(tx, true).await
}

/// Source slot-pinned callers hydrate the same state without consulting the global election.
pub(crate) async fn read_pinned_view<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
) -> Result<RoutingView, GatewayError> {
    read_view_for(tx, false).await
}

async fn read_view_for<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    read_active: bool,
) -> Result<RoutingView, GatewayError> {
    let now = tx.now;
    let mode = if read_active {
        gateway_authority_routing_evidence::reader::RoutingReadMode::GlobalSelection
    } else {
        gateway_authority_routing_evidence::reader::RoutingReadMode::PinnedObservation
    };
    let evidence = {
        let reader = tx.routing_read_store();
        gateway_authority_routing_evidence::reader::RoutingEvidenceReader::new(&reader, now)?
            .read(mode)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(RoutingView {
        pool: evidence.pool().clone(),
        state: evidence.state().clone(),
        active: evidence.active().cloned(),
        models: evidence.models().clone(),
        models_source: evidence.models_source().clone(),
        source_ids: evidence.source_ids().clone(),
        capacity: evidence.capacity().clone(),
        overage: evidence.overage().clone(),
    })
}
impl RoutingView {
    pub(crate) fn policy(&self) -> gateway_authority_routing_policy::RoutingPolicyData<'_> {
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
pub(crate) async fn validate_cohort<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    proof: &CohortProof,
    quota_only: bool,
) -> Result<bool, GatewayError> {
    for check in proof.revision_fences() {
        if check.key == keys::models() {
            let _: Option<gateway_source::source_json::SourceJson> =
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
pub struct SignalService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
    collator: &'a dyn gateway_schema::aliases::SourceStringCollator,
}
impl<'a, S: AuthorityStore, I: TokenSource> SignalService<'a, S, I> {
    pub fn new(
        store: &'a S,
        tokens: &'a I,
        collator: &'a dyn gateway_schema::aliases::SourceStringCollator,
    ) -> Self {
        Self {
            store,
            tokens,
            collator,
        }
    }
    fn transaction(&self, now: i64) -> Result<Transaction<'_, S, I>, GatewayError> {
        Transaction::new(self.store, self.tokens, now)
    }
    fn source_string_collator(&self) -> &dyn gateway_schema::aliases::SourceStringCollator {
        self.collator
    }
}
pub trait SignalFactory {
    type Store: AuthorityStore;
    type Tokens: TokenSource;
    fn routing_signal(&self) -> SignalService<'_, Self::Store, Self::Tokens>;
}
pub trait SignalOperations: SignalFactory {
    fn mark_live_credential_invalid(
        &self,
        selection: &LiveAccountSelection,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.routing_signal();
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let mut view = read_view(&mut tx).await?;
                if !view.policy().current_binding(&selection.binding) {
                    return Ok(false);
                }
                let slot = &mut view.state.slots[selection.binding.slot as usize];
                slot.invalid_credential_version =
                    Some(selection.binding.credential_version.clone());
                slot.upstream_timeout_blocked_until_ms = None;
                slot.quota_signal_observed_at_ms = Some(now);
                slot.retained_reset_at_ms = slot
                    .quota_blocks
                    .values()
                    .filter_map(|block| block.reset_at_ms)
                    .max()
                    .or(slot.retained_reset_at_ms);
                slot.clear_quota_blocks();
                slot.probe_lease = None;
                tx.put(&keys::routing(), &view.state, None)?;
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Live credential changed concurrently.",
            ))
        }
    }
    fn record_quota_block(
        &self,
        admission: &AdmittedAccount,
        block: QuotaBlock,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.routing_signal();
            self.record_bound_quota_block(
                &admission.binding,
                admission.quota_class,
                admission.probe.as_ref(),
                block,
                now,
            )
            .await
        }
    }
    /// Shared source writer; non-inference callers retain their own sealed physical dispatch.
    fn record_bound_quota_block(
        &self,
        binding: &AccountBinding,
        class: CodexQuotaClass,
        probe: Option<&ProbeLease>,
        block: QuotaBlock,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.routing_signal();
            nonnegative(block.blocked_until_ms)?;
            if let Some(at) = block.observed_at_ms {
                nonnegative(at)?;
            }
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let mut view = read_pinned_view(&mut tx).await?;
                let slot_index = binding.slot as usize;
                let foreign_probe = view.state.slots.get(slot_index).is_some_and(|slot| {
                    slot.account_id_hash.as_ref() == Some(&binding.account_id_hash)
                        && slot.credential_version == binding.credential_version
                        && view.pool.pool_versionstamp == binding.pool_versionstamp
                        && slot
                            .probe_lease
                            .as_ref()
                            .is_some_and(|lease| lease.quota_class.is_some_and(|c| c != class))
                        && probe.is_none()
                });
                if !view.policy().current_binding(binding) && !foreign_probe {
                    return Ok(false);
                }
                let slot = &mut view.state.slots[binding.slot as usize];
                let prior = slot.quota_blocks.get(class_name(class));
                if prior.is_some_and(|prior| prior.observed_at_ms > block.observed_at_ms) {
                    return Ok(false);
                }
                let mut next = block.clone();
                if let Some(prior) = prior {
                    next.blocked_until_ms = next.blocked_until_ms.max(prior.blocked_until_ms);
                    next.reset_ambiguous |= prior.reset_ambiguous
                        || (prior.reset_is_stable && prior.reset_at_ms != next.reset_at_ms);
                }
                slot.insert_quota_block(class, next);
                if !foreign_probe {
                    slot.probe_lease = None;
                    slot.generation = next_generation(slot.generation)?;
                }
                view.state.updated_at_ms = now;
                tx.put(&keys::routing(), &view.state, None)?;
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Quota observation changed concurrently.",
            ))
        }
    }
    fn mark_credential_invalid(
        &self,
        admission: &AdmittedAccount,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.routing_signal();
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let mut view = read_view(&mut tx).await?;
                if !view.policy().current_binding(&admission.binding) {
                    return Ok(false);
                }
                let slot = &mut view.state.slots[admission.binding.slot as usize];
                slot.invalid_credential_version =
                    Some(admission.binding.credential_version.clone());
                slot.upstream_timeout_blocked_until_ms = None;
                slot.quota_signal_observed_at_ms = Some(now);
                slot.retained_reset_at_ms = slot
                    .quota_blocks
                    .values()
                    .filter_map(|block| block.reset_at_ms)
                    .max()
                    .or(slot.retained_reset_at_ms);
                slot.clear_quota_blocks();
                slot.probe_lease = None;
                tx.put(&keys::routing(), &view.state, None)?;
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Credential observation changed concurrently.",
            ))
        }
    }
    fn learn_model_unavailable(
        &self,
        admission: &AdmittedAccount,
        detail: &JsString16,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.routing_signal();
            let expected = gateway_authority_routing_evidence::models::unsupported_detail(
                &admission.requested_model,
            );
            if detail != &expected {
                return Ok(false);
            }
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let view = read_view(&mut tx).await?;
                if !view
                    .policy()
                    .matches_admission(&admission.binding, admission.active_generation)
                {
                    return Ok(false);
                }
                let id = view
                    .source_ids
                    .get(&admission.binding.account_id_hash.0)
                    .ok_or_else(|| invalid("Current source account mapping is missing."))?;
                let models = gateway_authority_routing_evidence::learning::learn(
                    &view.models_source,
                    id,
                    &admission.requested_model,
                    now,
                    context.source_string_collator(),
                )?;
                tx.put_source_control(&keys::models(), &models, None)?;
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Model evidence changed concurrently.",
            ))
        }
    }
    /// Called only by the bounded probe/overage route; permission is strongly checked here.
    fn claim_probe(
        &self,
        admission: &AdmittedAccount,
        overage_reason: Option<&str>,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Option<AdmittedAccount>, GatewayError>> {
        async move {
            let context = self.routing_signal();
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let mut view = read_view(&mut tx).await?;
                if !view
                    .policy()
                    .matches_admission(&admission.binding, admission.active_generation)
                {
                    return Ok(None);
                }
                let allowed = view
                    .overage
                    .get(&admission.binding.account_id_hash.0)
                    .copied()
                    .unwrap_or(false);
                if let Some(reason) = overage_reason
                    && (!allowed
                        || !matches!(
                            reason,
                            "no_eligible_credit"
                                | "inventory_empty"
                                | "inventory_no_eligible_codex_credit"
                                | "inventory_response_invalid_or_expired"
                                | "inventory_unavailable"
                                | "account_day_limit_reached"
                        ))
                {
                    return Ok(None);
                }
                let slot = &mut view.state.slots[admission.binding.slot as usize];
                if slot
                    .probe_lease
                    .as_ref()
                    .is_some_and(|lease| lease.expires_at_ms > now)
                {
                    return Ok(None);
                }
                if overage_reason.is_none()
                    && slot
                        .quota_blocks
                        .get(class_name(admission.quota_class))
                        .is_some_and(|block| block.blocked_until_ms > now)
                    && !allowed
                {
                    return Ok(None);
                }
                let lease = ProbeLease {
                    token: context.tokens.next_token()?,
                    generation: slot.generation as i64,
                    circuit: gateway_authority_routing_records::types::ProbeCircuit::Quota,
                    quota_class: Some(admission.quota_class),
                    expires_at_ms: add(now, PROBE_LEASE_MS)?,
                };
                text(&lease.token)?;
                slot.probe_lease = Some(lease.clone());
                let mut admitted = admission.clone();
                admitted.probe = Some(lease);
                admitted.probe_required = false;
                let mut active = view
                    .active
                    .ok_or_else(|| invalid("Probe is missing its active selection."))?;
                active.binding = admitted.binding.clone();
                active.updated_at_ms = now;
                tx.put(&keys::routing(), &view.state, None)?;
                tx.put(&keys::active(), &active, None)?;
                if tx.commit().await? {
                    return Ok(Some(admitted));
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Probe claim changed concurrently.",
            ))
        }
    }
    fn finish_probe(
        &self,
        admission: &AdmittedAccount,
        recovered: bool,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.routing_signal();
            let Some(expected) = &admission.probe else {
                return Ok(false);
            };
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let mut view = read_view(&mut tx).await?;
                if !view.policy().current_binding(&admission.binding) {
                    return Ok(false);
                }
                let slot = &mut view.state.slots[admission.binding.slot as usize];
                if slot.probe_lease.as_ref() != Some(expected) {
                    return Ok(false);
                }
                slot.probe_lease = None;
                if recovered {
                    slot.remove_quota_class(admission.quota_class);
                    if slot.quota_blocks.is_empty() {
                        slot.retained_reset_at_ms = None;
                        slot.reset_identity_ambiguous = false;
                        slot.reset_identity_stable = false;
                        slot.recovery_pending = false;
                    }
                }
                tx.put(&keys::routing(), &view.state, None)?;
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Probe completion changed concurrently.",
            ))
        }
    }
    /// An explicitly permitted overage probe has a narrow lease, not a new global election.
    fn claim_overage_probe(
        &self,
        proof: &CohortProof,
        preferred: Option<&RoutingAccountHash>,
        reason: &str,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Option<ScopedOverageProbe>, GatewayError>> {
        async move {
            let context = self.routing_signal();
            if !matches!(
                reason,
                "no_eligible_credit"
                    | "inventory_empty"
                    | "inventory_no_eligible_codex_credit"
                    | "inventory_response_invalid_or_expired"
                    | "inventory_unavailable"
                    | "account_day_limit_reached"
            ) {
                return Ok(None);
            }
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                if !validate_cohort(&mut tx, proof, true).await? {
                    return Ok(None);
                }
                let mut view = read_view(&mut tx).await?;
                let selected = preferred
                    .and_then(|hash| {
                        view.pool
                            .accounts
                            .iter()
                            .position(|account| account.enabled && &account.account_id_hash == hash)
                    })
                    .or_else(|| {
                        let active = view.active.as_ref()?;
                        view.pool.accounts.iter().position(|account| {
                            account.enabled
                                && account.account_id_hash == active.binding.account_id_hash
                        })
                    })
                    .or_else(|| {
                        view.pool
                            .accounts
                            .iter()
                            .position(|account| account.enabled)
                    });
                let Some(slot_index) = selected else {
                    return Ok(None);
                };
                let account = &view.pool.accounts[slot_index];
                if !view
                    .overage
                    .get(&account.account_id_hash.0)
                    .copied()
                    .unwrap_or(false)
                {
                    return Ok(None);
                }
                let slot = &mut view.state.slots[slot_index];
                if slot
                    .probe_lease
                    .as_ref()
                    .is_some_and(|lease| lease.expires_at_ms > now)
                {
                    return Ok(None);
                }
                slot.generation = next_generation(slot.generation)?;
                let lease = ProbeLease {
                    token: context.tokens.next_token()?,
                    generation: slot.generation as i64,
                    circuit: gateway_authority_routing_records::types::ProbeCircuit::Quota,
                    quota_class: Some(proof.quota_class()),
                    expires_at_ms: add(now, PROBE_LEASE_MS)?,
                };
                text(&lease.token)?;
                slot.probe_lease = Some(lease.clone());
                let probe = ScopedOverageProbe {
                    binding: view.policy().binding(slot_index),
                    requested_model: proof.model().clone(),
                    quota_class: proof.quota_class(),
                    probe: lease,
                };
                tx.put(&keys::routing(), &view.state, None)?;
                if tx.commit().await? {
                    return Ok(Some(probe));
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Overage probe changed concurrently.",
            ))
        }
    }
    fn ensure_overage_probe_current(
        &self,
        probe: &ScopedOverageProbe,
        now: i64,
    ) -> impl std::future::Future<Output = Result<(), GatewayError>> {
        async move {
            let context = self.routing_signal();
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let view = read_view(&mut tx).await?;
                let slot = probe.binding.slot as usize;
                if slot >= view.state.slots.len()
                    || view.policy().binding(slot) != probe.binding
                    || view.state.slots[slot].probe_lease.as_ref() != Some(&probe.probe)
                    || probe.probe.expires_at_ms <= now
                {
                    return Err(unavailable(
                        "codex_routing_unavailable",
                        "Overage lease is no longer current.",
                    ));
                }
                if tx.commit().await? {
                    return Ok(());
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Overage dispatch changed concurrently.",
            ))
        }
    }
    fn finish_overage_probe(
        &self,
        probe: &ScopedOverageProbe,
        recovered: bool,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.routing_signal();
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let mut view = read_view(&mut tx).await?;
                let slot = probe.binding.slot as usize;
                if slot >= view.state.slots.len()
                    || view.policy().binding(slot) != probe.binding
                    || view.state.slots[slot].probe_lease.as_ref() != Some(&probe.probe)
                {
                    return Ok(false);
                }
                view.state.slots[slot].probe_lease = None;
                if recovered {
                    view.state.slots[slot]
                        .quota_blocks
                        .remove(class_name(probe.quota_class));
                }
                tx.put(&keys::routing(), &view.state, None)?;
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Overage completion changed concurrently.",
            ))
        }
    }
    fn record_overage_quota_block(
        &self,
        probe: &ScopedOverageProbe,
        block: QuotaBlock,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.routing_signal();
            nonnegative(block.blocked_until_ms)?;
            if let Some(at) = block.observed_at_ms {
                nonnegative(at)?;
            }
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let mut view = read_view(&mut tx).await?;
                let slot = probe.binding.slot as usize;
                if !view.policy().current_binding(&probe.binding)
                    || view.state.slots[slot].probe_lease.as_ref() != Some(&probe.probe)
                {
                    return Ok(false);
                }
                let row = &mut view.state.slots[slot];
                row.insert_quota_block(probe.quota_class, block.clone());
                row.probe_lease = None;
                row.generation = next_generation(row.generation)?;
                view.state.updated_at_ms = now;
                tx.put(&keys::routing(), &view.state, None)?;
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Overage quota evidence changed concurrently.",
            ))
        }
    }
}
impl<T: SignalFactory> SignalOperations for T {}
