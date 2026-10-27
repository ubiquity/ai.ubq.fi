//! Closed reset source observations. DATA is never permission and every fence joins the actual effect CAS.
use gateway_authority_contracts::unavailable;
mod backend;
pub mod types;
use backend::ReadTransaction;
use gateway_authority_keys as keys;
use gateway_authority_reset_records::ResetConfig;
use gateway_authority_reset_records::ResetInvocation;
use gateway_authority_reset_records::full_pool::FullPoolFence;
use gateway_authority_reset_records::full_pool::policy;
use gateway_authority_reset_records::reset_invocation;
use gateway_authority_routing_records::slot::class_name;
use gateway_authority_routing_selection::types::CohortProof;
use gateway_core::{error::GatewayError, identity::MAX_SAFE_INTEGER, identity::RequestContext};
use gateway_core::{state::AuthorityStore, state::RevisionCheck};
use serde_json::{Value, json};
use types::{Facts, Member, ResetCandidate};
fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Full-pool reset shadow schema or custody is invalid.",
        "codex_reset_shadow_schema_invalid",
    )
}
fn declined(reason: &str) -> GatewayError {
    GatewayError::unavailable("Automatic banked reset is not authorized.", reason)
}
fn marker_key() -> gateway_core::state::RecordKey {
    gateway_auth::store::key(json!([
        "uos_ai",
        "migrations",
        "codex_reset_usage_legacy_global_v1"
    ]))
}
async fn validate_cohort<S: AuthorityStore>(
    tx: &mut ReadTransaction<'_, S>,
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
            return Err(gateway_authority_contracts::invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(proof.matches_current_evidence(&evidence, quota_only))
}

struct RoutingView {
    pool: gateway_authority_routing_records::types::SubscriptionPool,
    state: gateway_authority_routing_records::types::RoutingState,
    active: Option<gateway_core::identity::ActiveSelection>,
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
async fn read_view<S: AuthorityStore>(
    tx: &mut ReadTransaction<'_, S>,
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
async fn reset_fences<S: AuthorityStore>(
    tx: &mut ReadTransaction<'_, S>,
    candidate: &ResetCandidate,
) -> Result<ResetInvocation, GatewayError> {
    if !candidate.proof.full_cohort_exhausted()
        || candidate.proof.account_bindings().is_empty()
        || !candidate
            .proof
            .account_bindings()
            .contains(&candidate.binding)
    {
        return Err(unavailable(
            "routing_fence_stale",
            "Every selected account must prove quota exhaustion before a reset.",
        ));
    }
    if !validate_cohort(tx, &candidate.proof, true).await? {
        return Err(unavailable(
            "routing_fence_stale",
            "Reset cohort changed or its quota window recovered.",
        ));
    }
    let view = read_view(tx).await?;
    let slot = usize::try_from(candidate.binding.slot)
        .map_err(|_| gateway_authority_contracts::invalid("Reset slot is invalid."))?;
    if slot >= view.state.slots.len()
        || view.policy().binding(slot) != candidate.binding
        || !view.pool.accounts[slot].enabled
        || view.state.legacy_identity_unresolved
    {
        return Err(unavailable(
            "routing_fence_stale",
            "Reset account binding changed.",
        ));
    }
    let block = view.state.slots[slot]
        .quota_blocks
        .get(class_name(candidate.proof.quota_class()))
        .ok_or_else(|| {
            unavailable(
                "routing_fence_missing",
                "Reset requires a persisted class quota fence.",
            )
        })?;
    if block.blocked_until_ms != candidate.quota_reset_at_ms
        || block.reset_at_ms != Some(candidate.quota_reset_at_ms)
        || !block.reset_is_stable
        || block.reset_ambiguous
        || view.state.slots[slot].reset_identity_ambiguous
    {
        return Err(unavailable(
            "routing_fence_stale",
            "Reset quota generation is not stable.",
        ));
    }
    let setting: Option<serde_json::Value> = tx
        .read(&keys::reset_usage(
            &view.pool.accounts[slot].admin_account_hash.0,
        ))
        .await?;
    if setting.as_ref().is_some_and(|setting| {
        setting.get("enabled").and_then(serde_json::Value::as_bool) != Some(true)
    }) {
        return Err(unavailable(
            "usage_disabled",
            "Reset usage is disabled for this subscription.",
        ));
    }
    reset_invocation(
        &view.pool.accounts[slot].admin_account_hash.0,
        &candidate.binding.credential_version.0,
        candidate.quota_reset_at_ms,
    )
}
async fn capture<S: AuthorityStore>(
    tx: &mut ReadTransaction<'_, S>,
    proof: &CohortProof,
    original: &RequestContext,
) -> Result<Facts, GatewayError> {
    capture_for(tx, proof, original, CapturePurpose::NewSpend).await
}
async fn capture_reconciliation<S: AuthorityStore>(
    tx: &mut ReadTransaction<'_, S>,
    proof: &CohortProof,
    original: &RequestContext,
) -> Result<Facts, GatewayError> {
    capture_for(tx, proof, original, CapturePurpose::ExistingLedger).await
}
#[derive(Clone, Copy)]
enum CapturePurpose {
    NewSpend,
    ExistingLedger,
}
async fn capture_for<S: AuthorityStore>(
    tx: &mut ReadTransaction<'_, S>,
    proof: &CohortProof,
    original: &RequestContext,
    purpose: CapturePurpose,
) -> Result<Facts, GatewayError> {
    if !proof.full_cohort_exhausted()
        || proof.account_bindings().is_empty()
        || !validate_cohort(tx, proof, true).await?
    {
        return Err(declined("routing_fence_stale"));
    }
    if original.request_id.0.trim().is_empty()
        || original.started_at_ms < 0
        || original.started_at_ms > tx.now
        || original.started_at_ms > MAX_SAFE_INTEGER
    {
        return Err(invalid());
    }
    let config: ResetConfig = tx.read(&keys::reset_config()).await?.unwrap_or_default();
    policy(&config)?;
    if tx
        .read::<Value>(&marker_key())
        .await?
        .is_none_or(|value| value.is_null())
    {
        return Err(declined("configuration_unavailable"));
    }
    let view = read_view(tx).await?;
    if view.state.legacy_identity_unresolved {
        return Err(declined("invalid_quota_generation"));
    }
    let mut members = Vec::new();
    let mut fences = Vec::new();
    let mut versions = Vec::new();
    for binding in proof.account_bindings() {
        let index = usize::try_from(binding.slot).map_err(|_| invalid())?;
        let account = view.pool.accounts.get(index).ok_or_else(invalid)?;
        let state = view.state.slots.get(index).ok_or_else(invalid)?;
        if view.policy().binding(index) != *binding || !account.enabled {
            return Err(declined("routing_fence_stale"));
        }
        let block = state
            .quota_block_for(proof.quota_class())
            .ok_or_else(|| declined("routing_fence_missing"))?;
        let reset = block
            .reset_at_ms
            .ok_or_else(|| declined("invalid_quota_generation"))?;
        let current = match purpose {
            CapturePurpose::NewSpend => {
                !block.reset_ambiguous
                    && !state.reset_identity_ambiguous
                    && reset == block.blocked_until_ms
                    && reset > tx.now
            }
            // Source staleVerifiedResetProbeEligible retains a stable old
            // observation beneath a newer block. This read-only path grants
            // no inventory, audit insertion or new-spend permission.
            CapturePurpose::ExistingLedger => block.blocked_until_ms >= reset,
        };
        if !block.reset_is_stable || !current {
            return Err(declined("invalid_quota_generation"));
        }
        let invocation = reset_invocation(
            &account.admin_account_hash.0,
            &binding.credential_version.0,
            reset,
        )?;
        let key = keys::reset_usage(&account.admin_account_hash.0);
        let setting: Option<Value> = tx.read(&key).await?;
        let enabled = setting.as_ref().is_none_or(|value| {
            value.is_null() || value.get("enabled").and_then(Value::as_bool) == Some(true)
        });
        versions.push(if setting.is_none() {
            "unset".into()
        } else {
            tx.revision(&key).unwrap_or_else(|| "unset".into())
        });
        fences.push(FullPoolFence {
            slot: binding.slot,
            account_id_hash: invocation.identity.account_id_hash,
            quota_generation: invocation.identity.quota_generation,
            routing_generation: binding.routing_generation,
            quota_reset_at_ms: reset,
        });
        members.push(Member {
            candidate: ResetCandidate {
                binding: binding.clone(),
                quota_reset_at_ms: reset,
                request_started_at_ms: tx.now,
                proof: proof.clone(),
            },
            administrative_hash: account.admin_account_hash.0.clone(),
            enabled,
        });
    }
    if !members.iter().any(|member| member.enabled) {
        return Err(declined("usage_disabled"));
    }
    let settings_version = versions.join(",");
    let episode_hash =
        gateway_authority_reset_records::full_pool::audit::episode_hash(&settings_version, &fences);
    Ok(Facts {
        proof: proof.clone(),
        original: original.clone(),
        config,
        members,
        fences,
        settings_version,
        episode_hash,
    })
}

pub struct ResetFactsEvidence {
    facts: Facts,
    checks: Vec<RevisionCheck>,
}
impl ResetFactsEvidence {
    pub fn facts(&self) -> &Facts {
        &self.facts
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
}
pub struct ResetFenceEvidence {
    invocation: ResetInvocation,
    checks: Vec<RevisionCheck>,
}
impl ResetFenceEvidence {
    pub fn invocation(&self) -> &ResetInvocation {
        &self.invocation
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
}
pub async fn capture_spend_facts<S: AuthorityStore>(
    store: &S,
    proof: &CohortProof,
    original: &RequestContext,
    now: i64,
) -> Result<ResetFactsEvidence, GatewayError> {
    let mut tx = ReadTransaction::new(store, now)?;
    let facts = capture(&mut tx, proof, original).await?;
    Ok(ResetFactsEvidence {
        facts,
        checks: tx.checks(),
    })
}
pub async fn capture_existing_facts<S: AuthorityStore>(
    store: &S,
    proof: &CohortProof,
    original: &RequestContext,
    now: i64,
) -> Result<ResetFactsEvidence, GatewayError> {
    let mut tx = ReadTransaction::new(store, now)?;
    let facts = capture_reconciliation(&mut tx, proof, original).await?;
    Ok(ResetFactsEvidence {
        facts,
        checks: tx.checks(),
    })
}
pub async fn capture_candidate_fences<S: AuthorityStore>(
    store: &S,
    candidate: &ResetCandidate,
    now: i64,
) -> Result<ResetFenceEvidence, GatewayError> {
    let mut tx = ReadTransaction::new(store, now)?;
    let invocation = reset_fences(&mut tx, candidate).await?;
    Ok(ResetFenceEvidence {
        invocation,
        checks: tx.checks(),
    })
}
