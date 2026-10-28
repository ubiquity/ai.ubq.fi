//! Fixed manual-reset capacity DATA predicates; no actor, lease or redemption authority.
use gateway_authority_routing_evidence::capacity::{CapacityObservation, CapacityState};
use gateway_contracts::identity::AccountBinding;
pub fn manual_observation_is_current(
    row: &CapacityObservation,
    binding: &AccountBinding,
    now: i64,
) -> bool {
    !(row.account_id_hash != binding.account_id_hash
        || u64::from(row.slot) != binding.slot
        || row.state != CapacityState::Available
        || row.source_observed_at_ms != Some(row.snapshot_at_ms)
        || row.snapshot_at_ms > now
        || row.snapshot_at_ms < 0
        || row.snapshot_at_ms < now.saturating_sub(30_000)
        || (row.windows.primary.is_none() && row.windows.secondary.is_none()))
}
pub fn manual_reset_deadline(row: &CapacityObservation, now: i64) -> Option<i64> {
    let windows = [row.windows.primary.as_ref(), row.windows.secondary.as_ref()]
        .into_iter()
        .flatten()
        .filter(|w| w.reset_at_ms.is_some_and(|at| at > now))
        .collect::<Vec<_>>();
    let exhausted = windows.iter().any(|w| w.used_percent == Some(100.0));
    windows
        .into_iter()
        .filter(|w| !exhausted || w.used_percent == Some(100.0))
        .filter_map(|w| w.reset_at_ms)
        .max()
}
pub fn manual_recovered_models(
    usage: &CapacityObservation,
) -> Vec<gateway_contracts::model::ModelId> {
    let mut models = Vec::new();
    if super::capacity_headroom_for_observation(usage, None).unwrap_or(0.0) > 0.0 {
        models.push(gateway_contracts::model::ModelId::from_scalar("standard"));
    }
    for limit in &usage.additional_rate_limits {
        let model = gateway_contracts::model::ModelId::from_scalar(&limit.limit_name);
        if !matches!(
            gateway_authority_routing_evidence::models::model_quota_class(&model),
            gateway_contracts::provider::CodexQuotaClass::Standard
                | gateway_contracts::provider::CodexQuotaClass::Unknown
        ) && super::capacity_headroom_for_observation(usage, Some(&model)).unwrap_or(0.0) > 0.0
        {
            models.push(model);
        }
    }
    models
}
