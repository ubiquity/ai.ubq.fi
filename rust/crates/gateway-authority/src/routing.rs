//! Global serial subscription selection; source routing-serial.ts and routing-evaluation.ts.
use crate::store::{Authority, Transaction};
use crate::{invalid, next_generation, nonnegative, unavailable};
use gateway_authority_contracts::TokenSource;
use gateway_authority_keys as keys;
use gateway_authority_routing_evidence::capacity::CapacityStore;
use gateway_authority_routing_evidence::models::ModelAvailability;
use gateway_authority_routing_records::slot::class_name;
use gateway_authority_routing_records::types::{
    ProbeCircuit, ProbeLease, QuotaBlock, RoutingState, SubscriptionPool,
};
use gateway_authority_routing_selection::types::{
    AdmittedAccount, CohortProof, LiveAccountSelection,
};
use gateway_core::{
    error::GatewayError,
    identity::{AccountBinding, ActiveSelection},
    js_string::JsString16,
    model::ModelId,
    provider::CodexQuotaClass,
    state::AuthorityStore,
};
use std::collections::BTreeMap;

// Only finite ASCII class comparisons consume this projection; original model data is untouched.

pub(crate) struct RoutingView {
    pub pool: SubscriptionPool,
    pub state: RoutingState,
    pub active: Option<ActiveSelection>,
    models: ModelAvailability,
    models_source: gateway_core::source_json::SourceJson,
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

impl<S: AuthorityStore, I: TokenSource> Authority<S, I> {
    pub async fn mark_live_credential_invalid(
        &self,
        selection: &LiveAccountSelection,
        now: i64,
    ) -> Result<bool, GatewayError> {
        for _ in 0..3 {
            let mut tx = self.transaction(now)?;
            let mut view = read_view(&mut tx).await?;
            if !view.policy().current_binding(&selection.binding) {
                return Ok(false);
            }
            let slot = &mut view.state.slots[selection.binding.slot as usize];
            slot.invalid_credential_version = Some(selection.binding.credential_version.clone());
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

    pub async fn record_quota_block(
        &self,
        admission: &AdmittedAccount,
        block: QuotaBlock,
        now: i64,
    ) -> Result<bool, GatewayError> {
        self.record_bound_quota_block(
            &admission.binding,
            admission.quota_class,
            admission.probe.as_ref(),
            block,
            now,
        )
        .await
    }

    /// Shared source writer; non-inference callers retain their own sealed physical dispatch.
    pub(crate) async fn record_bound_quota_block(
        &self,
        binding: &AccountBinding,
        class: CodexQuotaClass,
        probe: Option<&ProbeLease>,
        block: QuotaBlock,
        now: i64,
    ) -> Result<bool, GatewayError> {
        nonnegative(block.blocked_until_ms)?;
        if let Some(at) = block.observed_at_ms {
            nonnegative(at)?;
        }
        for _ in 0..3 {
            let mut tx = self.transaction(now)?;
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

    pub async fn mark_credential_invalid(
        &self,
        admission: &AdmittedAccount,
        now: i64,
    ) -> Result<bool, GatewayError> {
        for _ in 0..3 {
            let mut tx = self.transaction(now)?;
            let mut view = read_view(&mut tx).await?;
            if !view.policy().current_binding(&admission.binding) {
                return Ok(false);
            }
            let slot = &mut view.state.slots[admission.binding.slot as usize];
            slot.invalid_credential_version = Some(admission.binding.credential_version.clone());
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

    pub async fn learn_model_unavailable(
        &self,
        admission: &AdmittedAccount,
        detail: &JsString16,
        now: i64,
    ) -> Result<bool, GatewayError> {
        let expected = crate::source_models::unsupported_detail(&admission.requested_model);
        if detail != &expected {
            return Ok(false);
        }
        for _ in 0..3 {
            let mut tx = self.transaction(now)?;
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
            let models = crate::source_models::learn(
                &view.models_source,
                id,
                &admission.requested_model,
                now,
                self.source_string_collator(),
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
