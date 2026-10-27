//! Source capacity reconciliation over raw routing rows; producer custody stays in authority.
use crate::{
    capacity_rules::{capacity_headroom_for_observation, capacity_observation_is_fresh},
    next_generation,
};
use gateway_authority_routing_evidence::capacity::CapacityObservation;
use gateway_authority_routing_records::types::RoutingState;
use gateway_core::{error::GatewayError, model::ModelId, provider::CodexQuotaClass};

pub fn apply_capacity_observation(
    state: &RoutingState,
    row: &CapacityObservation,
    now: i64,
    model: Option<&ModelId>,
    updated_at_ms: i64,
) -> Result<Option<RoutingState>, GatewayError> {
    if row.snapshot_at_ms > now {
        return Ok(None);
    }
    let Some(current) = state.slots.get(usize::from(row.slot)) else {
        return Ok(None);
    };
    if current.account_id_hash.as_ref() != Some(&row.account_id_hash)
        || current
            .capacity_observed_at_ms
            .is_some_and(|at| row.snapshot_at_ms < at)
    {
        return Ok(None);
    }
    let class = model
        .map(crate::routing::model_quota_class)
        .unwrap_or(CodexQuotaClass::Unknown);
    let positive = capacity_observation_is_fresh(row, now)
        && capacity_headroom_for_observation(row, model).is_some_and(|h| h > 0.0);
    let newer = current
        .quota_signal_for(class)
        .is_some_and(|at| at >= row.snapshot_at_ms);
    let clear = positive && !newer;
    let mut slot = current.clone();
    if clear {
        slot.remove_quota_class(class);
    }
    slot.primary_used_percent = row.windows.primary.as_ref().and_then(|w| w.used_percent);
    slot.secondary_used_percent = row.windows.secondary.as_ref().and_then(|w| w.used_percent);
    slot.capacity_observed_at_ms = Some(row.snapshot_at_ms);
    if clear {
        if current.reset_identity_ambiguous
            || current.recovery_pending
            || current.probe_lease.is_some()
        {
            slot.retained_reset_at_ms = current.retained_reset_at_ms;
            slot.reset_identity_stable = current.reset_identity_stable;
            slot.reset_identity_ambiguous = current.reset_identity_ambiguous;
            slot.recovery_pending = current.recovery_pending;
            slot.probe_lease = current.probe_lease.clone();
        } else if slot.quota_blocks.is_empty() {
            slot.retained_reset_at_ms = None;
            slot.reset_identity_stable = false;
            slot.reset_identity_ambiguous = false;
            slot.recovery_pending = false;
            slot.generation = next_generation(current.generation)?;
            slot.probe_lease = None;
        }
    }
    if &slot == current {
        return Ok(None);
    }
    let mut next = state.clone();
    next.slots[usize::from(row.slot)] = slot;
    next.updated_at_ms = updated_at_ms;
    Ok(Some(next))
}
