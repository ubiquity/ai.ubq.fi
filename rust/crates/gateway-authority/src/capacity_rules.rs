//! Source capacity row codecs and named-window applicability; no producer trust is inferred here.
use gateway_authority_routing_evidence::capacity::{
    CapacityObservation, CapacityState, CapacityStore, CapacityWindows,
};
use gateway_core::{identity::RoutingAccountHash, model::ModelId};
pub const CAPACITY_MAX_AGE_MS: i64 = 30 * crate::MINUTE_MS;
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
        .map(crate::routing::model_quota_label)
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
pub(crate) fn capacity_for<'a>(
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
pub(crate) fn merge_capacity(
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
