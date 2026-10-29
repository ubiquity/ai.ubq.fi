//! Pure source eligibility and binding policy. Ordinary data never elects or authorizes an effect.
use gateway_authority_capacity_policy::capacity_for;
use gateway_authority_contracts::DAY_MS;
use gateway_authority_routing_evidence::{
    capacity::CapacityStore, models::ModelAvailability, reader::RoutingEvidence,
};
use gateway_authority_routing_records::types::{ProbeCircuit, RoutingState, SubscriptionPool};
use gateway_contracts::{
    identity::AccountBinding, identity::ActiveSelection, identity::ActiveTransitionReason,
    identity::RoutingAccountHash, model::ModelId, provider::CodexQuotaClass,
};
use gateway_source::js_string::JsString16;
use std::collections::BTreeMap;

/// Borrowed observation inputs, freely constructible and never a currentness grant.
#[derive(Clone, Copy)]
pub struct RoutingPolicyData<'a> {
    pub pool: &'a SubscriptionPool,
    pub state: &'a RoutingState,
    pub active: Option<&'a ActiveSelection>,
    pub models: &'a ModelAvailability,
    pub capacity: &'a CapacityStore,
    pub overage: &'a BTreeMap<String, bool>,
}
impl<'a> RoutingPolicyData<'a> {
    pub fn from_evidence(evidence: &'a RoutingEvidence) -> Self {
        Self {
            pool: evidence.pool(),
            state: evidence.state(),
            active: evidence.active(),
            models: evidence.models(),
            capacity: evidence.capacity(),
            overage: evidence.overage(),
        }
    }
    pub fn binding(&self, slot: usize) -> AccountBinding {
        AccountBinding {
            account_id_hash: self.pool.accounts[slot].account_id_hash.clone(),
            credential_version: self.pool.accounts[slot].credential_version.clone(),
            pool_versionstamp: self.pool.pool_versionstamp.clone(),
            slot: slot as u64,
            routing_generation: self.state.slots[slot].generation,
        }
    }
    pub fn matches_admission(&self, binding: &AccountBinding, active_generation: u64) -> bool {
        let Ok(slot) = usize::try_from(binding.slot) else {
            return false;
        };
        self.pool
            .accounts
            .get(slot)
            .is_some_and(|account| account.enabled)
            && slot < self.state.slots.len()
            && self.binding(slot) == *binding
            && self.active.is_some_and(|active| {
                active.generation == active_generation && active.binding == *binding
            })
    }
    pub fn current_binding(&self, binding: &AccountBinding) -> bool {
        usize::try_from(binding.slot)
            .ok()
            .is_some_and(|slot| slot < self.state.slots.len() && self.binding(slot) == *binding)
    }
}
pub fn unavailable_model(
    models: &ModelAvailability,
    pool: &SubscriptionPool,
    account: &RoutingAccountHash,
    model: &ModelId,
    now: i64,
) -> Option<Option<JsString16>> {
    if let Some(learned) = models
        .unsupported
        .get(&account.0)
        .and_then(|row| row.get(model))
        && learned.observed_at_ms > 0
        && learned.observed_at_ms <= now
        && now - learned.observed_at_ms <= 7 * DAY_MS
    {
        return Some(Some(learned.detail.clone()));
    }
    let catalog = models.catalogs.get(&account.0)?;
    if catalog.models.iter().any(|slug| slug == model) {
        return None;
    }
    pool.accounts
        .iter()
        .filter(|other| other.enabled && &other.account_id_hash != account)
        .find_map(|other| {
            let sibling = models.catalogs.get(&other.account_id_hash.0)?;
            (sibling.client_version == catalog.client_version
                && sibling.models.iter().any(|slug| slug == model))
            .then_some(None)
        })
}

pub struct Evaluation {
    pub slot: usize,
    pub reason: Option<ActiveTransitionReason>,
    pub eligible: bool,
    pub probe: bool,
    pub retry: Option<i64>,
    pub detail: Option<JsString16>,
}

pub fn evaluate(
    view: &RoutingPolicyData<'_>,
    slot: usize,
    model: &ModelId,
    class: CodexQuotaClass,
    now: i64,
) -> Evaluation {
    let row = &view.state.slots[slot];
    let mut result = Evaluation {
        slot,
        reason: None,
        eligible: false,
        probe: false,
        retry: None,
        detail: None,
    };
    if row.invalid_credential_version.as_ref() == Some(&row.credential_version) {
        result.reason = Some(ActiveTransitionReason::CredentialInvalid);
        return result;
    }
    if let Some(detail) = unavailable_model(
        view.models,
        view.pool,
        &view.pool.accounts[slot].account_id_hash,
        model,
        now,
    ) {
        result.reason = Some(ActiveTransitionReason::ModelUnavailable);
        result.detail = detail;
        return result;
    }
    let block = row.quota_block_for(class);
    let observed = capacity_for(
        view.capacity,
        &view.pool.accounts[slot].account_id_hash,
        model,
        now,
    );
    let newer_signal = observed.is_some_and(|(observation, _)| {
        row.quota_signal_for(class)
            .is_some_and(|at| at >= observation.snapshot_at_ms)
    });
    let override_positive = !newer_signal && observed.is_some_and(|(_, available)| available > 0.0);
    if let Some(block) = &block
        && block.blocked_until_ms > now
        && !override_positive
    {
        result.reason = Some(ActiveTransitionReason::QuotaExhausted);
        result.retry = Some(block.blocked_until_ms);
        return result;
    }
    if let Some((observation, available)) =
        observed.filter(|(_, available)| *available == 0.0 && !newer_signal)
    {
        debug_assert_eq!(available, 0.0);
        if !view
            .overage
            .get(&view.pool.accounts[slot].account_id_hash.0)
            .copied()
            .unwrap_or(false)
            && let Some(reset) = observation
                .windows
                .primary
                .as_ref()
                .and_then(|window| window.reset_at_ms)
                .filter(|reset| *reset > now)
        {
            result.reason = Some(ActiveTransitionReason::QuotaExhausted);
            result.retry = Some(reset);
            return result;
        }
        result.probe = true;
        result.eligible = true;
        return result;
    }
    if row.probe_lease.as_ref().is_some_and(|lease| {
        lease.expires_at_ms > now
            && lease.circuit == ProbeCircuit::Quota
            && lease.quota_class.is_none_or(|c| c == class)
    }) {
        return result;
    }
    result.eligible = true;
    result.probe |= block.is_some() && !override_positive;
    result
}

/// Pure predicate only. The effect owner must compare every proof revision against its same cached reads and retain those fences in its original final CAS.
pub fn cohort_matches(
    view: &RoutingPolicyData<'_>,
    bindings: &[AccountBinding],
    model: &ModelId,
    class: CodexQuotaClass,
    quota_only: bool,
    now: i64,
) -> bool {
    let selected: Vec<_> = view
        .pool
        .accounts
        .iter()
        .enumerate()
        .filter(|(_, account)| account.enabled)
        .map(|(slot, _)| slot)
        .collect();
    !selected.is_empty()
        && selected
            .iter()
            .map(|slot| view.binding(*slot))
            .collect::<Vec<_>>()
            == bindings
        && selected.iter().all(|slot| {
            let reason = evaluate(view, *slot, model, class, now).reason;
            reason == Some(ActiveTransitionReason::QuotaExhausted)
                || (!quota_only && reason == Some(ActiveTransitionReason::CredentialInvalid))
        })
}
