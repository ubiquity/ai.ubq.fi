//! Source capacity applicability and finite routing data reconciliation; never producer/write authority.
use gateway_authority_contracts::next_generation;
use gateway_authority_routing_evidence::capacity::{
    CapacityObservation, CapacityState, CapacityStore, CapacityWindows,
};
use gateway_authority_routing_records::types::RoutingState;
use gateway_contracts::{identity::RoutingAccountHash, model::ModelId, provider::CodexQuotaClass};
use gateway_source::error::GatewayError;

pub const CAPACITY_MAX_AGE_MS: i64 = 30 * gateway_authority_contracts::MINUTE_MS;
pub fn capacity_observation_is_fresh(row: &CapacityObservation, now: i64) -> bool {
    row.state == CapacityState::Available
        && row.snapshot_at_ms >= 0
        && row.snapshot_at_ms <= now
        && now - row.snapshot_at_ms <= CAPACITY_MAX_AGE_MS
}
fn headroom(windows: &CapacityWindows) -> Option<f64> {
    [windows.primary.as_ref(), windows.secondary.as_ref()]
        .into_iter()
        .flatten()
        .filter_map(|w| w.used_percent)
        .filter(|n| n.is_finite() && (0.0..=100.0).contains(n))
        .map(|n| 100.0 - n)
        .reduce(f64::min)
}
fn label(text: &str) -> String {
    text.chars()
        .filter(char::is_ascii_alphanumeric)
        .map(|c| c.to_ascii_lowercase())
        .collect()
}
pub fn capacity_headroom_for_observation(
    row: &CapacityObservation,
    model: Option<&ModelId>,
) -> Option<f64> {
    let model = model
        .map(gateway_authority_routing_evidence::models::model_quota_label)
        .unwrap_or_default();
    if model.is_empty() {
        return headroom(&row.windows);
    }
    let limits: Vec<_> = row
        .additional_rate_limits
        .iter()
        .filter(|limit| label(&limit.limit_name) == model)
        .collect();
    if limits.is_empty() {
        headroom(&row.windows)
    } else {
        limits
            .into_iter()
            .filter_map(|limit| headroom(&limit.windows))
            .reduce(f64::max)
    }
}
pub fn capacity_for<'a>(
    store: &'a CapacityStore,
    account: &RoutingAccountHash,
    model: &ModelId,
    now: i64,
) -> Option<(&'a CapacityObservation, f64)> {
    let row = store
        .observations
        .iter()
        .filter(|row| &row.account_id_hash == account)
        .max_by_key(|row| row.snapshot_at_ms)?;
    capacity_observation_is_fresh(row, now).then_some(())?;
    Some((row, capacity_headroom_for_observation(row, Some(model))?))
}
pub fn merge_capacity(
    store: &CapacityStore,
    inputs: &[CapacityObservation],
    now: i64,
) -> CapacityStore {
    let mut merged: Vec<&CapacityObservation> = Vec::new();
    for row in store.observations.iter().chain(inputs) {
        if let Some(prior) = merged
            .iter_mut()
            .find(|prior| prior.account_id_hash == row.account_id_hash)
        {
            if row.snapshot_at_ms >= prior.snapshot_at_ms {
                *prior = row;
            }
        } else {
            merged.push(row);
        }
    }
    // The source merges first, then drops stale/future winners. Filtering inputs
    // first could resurrect an older observation shadowed by a newer row.
    let mut observations: Vec<_> = merged
        .into_iter()
        .filter(|row| {
            row.snapshot_at_ms >= now.saturating_sub(CAPACITY_MAX_AGE_MS).max(0)
                && row.snapshot_at_ms <= now
        })
        .cloned()
        .collect();
    observations.sort_by_key(|row| row.slot);
    CapacityStore {
        v: 1,
        updated_at_ms: now,
        observations,
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct CapacityObservationStorePlan {
    pub capacity_revision: Option<String>,
    pub capacity: CapacityStore,
}
/// Pure source write plan, not an authority capability or caller-selected write set.
pub fn plan_capacity_observation_store(
    revision: Option<String>,
    store: &CapacityStore,
    inputs: &[CapacityObservation],
    now: i64,
) -> CapacityObservationStorePlan {
    CapacityObservationStorePlan {
        capacity_revision: revision,
        capacity: merge_capacity(store, inputs, now),
    }
}

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
        .map(gateway_authority_routing_evidence::models::model_quota_class)
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

pub mod manual;
