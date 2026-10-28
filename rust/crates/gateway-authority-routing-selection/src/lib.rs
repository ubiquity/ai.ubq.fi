//! Checked global serial selection. The opaque cohort proof and its actual final CAS share this owner.
mod backend;
pub mod types;
use backend::Transaction;
use gateway_authority_capacity::{CapacityFactory, CapacityOperations, CapacityService};
use gateway_authority_capacity_policy::capacity_for;
use gateway_authority_contracts::{
    AuthorityClock, TokenSource, invalid, next_generation, unavailable, validate_time,
};
use gateway_authority_keys as keys;
use gateway_authority_routing_evidence::{
    capacity::CapacityStore,
    models::{ModelAvailability, model_quota_class},
    reader::{RoutingEvidenceReader, RoutingReadMode},
    validation::validate_binding,
};
use gateway_authority_routing_policy::{Evaluation, RoutingPolicyData, evaluate};
use gateway_authority_routing_records::{
    codec::parse_routing_state,
    types::{ProbeCircuit, RoutingState, SubscriptionPool},
};
use gateway_contracts::{
    identity::ActiveSelection, identity::ActiveTransitionReason, model::ModelId,
    provider::CodexQuotaClass,
};
use gateway_core::state::AuthorityStore;
use gateway_schema_values::source_view::js_trim_source;
use gateway_source::error::GatewayError;
use std::collections::BTreeMap;
use types::{
    AdmittedAccount, CohortProof, LiveAccountSelection, LiveRouteSelection, RouteSelection,
};

/// Closed borrowed context, constructed by the root from the SAME store, token source and trusted clock.
/// It exposes only finite checked operations through SelectionFactory, never these capabilities.
pub struct SelectionService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
}
impl<S, I> Copy for SelectionService<'_, S, I> {}
impl<S, I> Clone for SelectionService<'_, S, I> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<'a, S: AuthorityStore, I: TokenSource> SelectionService<'a, S, I> {
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
            .ok_or_else(|| {
                unavailable(
                    "authority_clock_unavailable",
                    "Time-sensitive authority actions require a trusted current clock.",
                )
            })?
            .now_ms()?;
        validate_time(now)?;
        if now < started_at {
            return Err(unavailable(
                "invalid_clock",
                "Authority clock moved backwards.",
            ));
        }
        Ok(now)
    }
    async fn select_account_source(
        &self,
        model: &ModelId,
        class: CodexQuotaClass,
        now: i64,
    ) -> Result<RouteSelection, GatewayError> {
        self.reconcile_capacity_for_model(model, now).await?;
        for _ in 0..3 {
            let mut tx = self.transaction(now)?;
            let mut view = read_view(&mut tx, now).await?;
            let prior = tx
                .read::<serde_json::Value>(&keys::routing())
                .await?
                .as_ref()
                .map(parse_routing_state)
                .transpose()?;
            let evaluated: Vec<_> = view
                .pool
                .accounts
                .iter()
                .enumerate()
                .filter(|(_, account)| account.enabled)
                .map(|(slot, _)| evaluate(&view.policy(), slot, model, class, now))
                .collect();
            let active_slot = view.active.as_ref().and_then(|active| {
                evaluated.iter().find(|evaluation| {
                    view.pool.accounts[evaluation.slot].account_id_hash
                        == active.binding.account_id_hash
                })
            });
            if prior.as_ref().is_none_or(|p| {
                p.slots != view.state.slots
                    || p.legacy_identity_unresolved != view.state.legacy_identity_unresolved
            }) {
                view.state.updated_at_ms = now;
                tx.put(&keys::routing(), &view.state, None)?;
            }
            let chosen = match (&view.active, active_slot) {
                (None, _) => evaluated.iter().find(|evaluation| evaluation.eligible),
                (Some(_), Some(active)) if active.eligible => Some(active),
                (Some(_), Some(active)) if active.reason.is_none() => {
                    if tx.commit().await? {
                        return Ok(RouteSelection::RoutingUnavailable);
                    } else {
                        continue;
                    }
                }
                _ => evaluated.iter().find(|evaluation| evaluation.eligible),
            };
            let result = selection_result(
                &mut tx,
                &view,
                &evaluated,
                Election {
                    active_slot,
                    chosen,
                },
                SelectionRequest { model, class, now },
            )?;
            if tx.commit().await? {
                return Ok(result);
            }
        }
        Err(unavailable(
            "codex_routing_unavailable",
            "Codex routing changed concurrently.",
        ))
    }
}
struct SelectionRequest<'a> {
    model: &'a ModelId,
    class: CodexQuotaClass,
    now: i64,
}
struct Election<'a> {
    active_slot: Option<&'a Evaluation>,
    chosen: Option<&'a Evaluation>,
}
struct SelectionView {
    pool: SubscriptionPool,
    state: RoutingState,
    active: Option<ActiveSelection>,
    models: ModelAvailability,
    capacity: CapacityStore,
    overage: BTreeMap<String, bool>,
}
impl SelectionView {
    fn policy(&self) -> RoutingPolicyData<'_> {
        RoutingPolicyData {
            pool: &self.pool,
            state: &self.state,
            active: self.active.as_ref(),
            models: &self.models,
            capacity: &self.capacity,
            overage: &self.overage,
        }
    }
    fn binding(&self, slot: usize) -> gateway_contracts::identity::AccountBinding {
        self.policy().binding(slot)
    }
}
async fn read_view<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    now: i64,
) -> Result<SelectionView, GatewayError> {
    let evidence = {
        let reader = tx.routing_read_store();
        RoutingEvidenceReader::new(&reader, now)?
            .read(RoutingReadMode::GlobalSelection)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(SelectionView {
        pool: evidence.pool().clone(),
        state: evidence.state().clone(),
        active: evidence.active().cloned(),
        models: evidence.models().clone(),
        capacity: evidence.capacity().clone(),
        overage: evidence.overage().clone(),
    })
}
impl<S: AuthorityStore, I: TokenSource> CapacityFactory for SelectionService<'_, S, I> {
    type Store = S;
    type Tokens = I;
    fn capacity(&self) -> CapacityService<'_, S, I> {
        CapacityService::new(self.store, self.tokens, self.clock)
    }
}
/// The root implements this one composition boundary; checked bodies cannot be overridden.
pub trait SelectionFactory {
    type Store: AuthorityStore;
    type Tokens: TokenSource;
    fn selection(&self) -> SelectionService<'_, Self::Store, Self::Tokens>;
}
pub trait SelectionOperations: SelectionFactory {
    fn materialize_capacity_quota_blocks(
        &self,
        model: &ModelId,
        class: CodexQuotaClass,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.selection();
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let mut view = read_view(&mut tx, now).await?;
                let mut changed = false;
                for slot in 0..view.pool.accounts.len() {
                    if !view.pool.accounts[slot].enabled {
                        continue;
                    }
                    let Some((observation, available)) = capacity_for(
                        &view.capacity,
                        &view.pool.accounts[slot].account_id_hash,
                        model,
                        now,
                    ) else {
                        continue;
                    };
                    if available != 0.0 {
                        continue;
                    }
                    let Some(reset) = observation
                        .windows
                        .primary
                        .as_ref()
                        .and_then(|window| window.reset_at_ms)
                        .filter(|reset| *reset > now)
                    else {
                        continue;
                    };
                    let prior = view.state.slots[slot].quota_block_for(class);
                    // A live quota signal observed after this sample remains newer
                    // and authoritative. An older but still-future block must not
                    // suppress reconciliation of a changed provider window.
                    if prior
                        .as_ref()
                        .and_then(|block| block.observed_at_ms)
                        .is_some_and(|at| at >= observation.snapshot_at_ms)
                    {
                        continue;
                    }
                    let current = &mut view.state.slots[slot];
                    let foreign_probe = current.probe_lease.as_ref().is_some_and(|lease| {
                        lease.circuit == ProbeCircuit::Quota
                            && lease.quota_class.is_some_and(|c| c != class)
                    });
                    if current.probe_lease.is_some() && !foreign_probe {
                        continue;
                    }
                    let block =
                        current.capacity_quota_block(class, reset, observation.snapshot_at_ms);
                    current.insert_quota_block(class, block);
                    if !foreign_probe {
                        current.generation = next_generation(current.generation)?;
                    }
                    changed = true;
                }
                if changed {
                    view.state.updated_at_ms = now;
                    tx.put(&keys::routing(), &view.state, None)?;
                }
                if tx.commit().await? {
                    return Ok(changed);
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Capacity materialization changed concurrently.",
            ))
        }
    }
    fn select_account(
        &self,
        model: &ModelId,
        class: CodexQuotaClass,
        now: i64,
    ) -> impl std::future::Future<Output = Result<RouteSelection, GatewayError>> {
        async move {
            let context = self.selection();
            if js_trim_source(model.as_js_string()).as_units().is_empty() {
                return Err(invalid("Missing or oversized authority identity."));
            }
            context.select_account_source(model, class, now).await
        }
    }
    fn select_live_account(
        &self,
        model: Option<&gateway_source::js_string::JsString16>,
        now: i64,
    ) -> impl std::future::Future<Output = Result<LiveRouteSelection, GatewayError>> {
        async move {
            let context = self.selection();
            let lexical = model
                .cloned()
                .map(ModelId::from_source)
                .unwrap_or_else(|| ModelId::from_scalar(""));
            let class = model_quota_class(&lexical);
            Ok(
                match context.select_account_source(&lexical, class, now).await? {
                    RouteSelection::Eligible(a) => {
                        LiveRouteSelection::Eligible(LiveAccountSelection {
                            binding: a.binding,
                            active_generation: a.active_generation,
                            quota_class: class,
                            probe_required: a.probe_required,
                            model: model.cloned(),
                        })
                    }
                    RouteSelection::QuotaBlocked { retry_at_ms, .. } => {
                        LiveRouteSelection::QuotaBlocked { retry_at_ms }
                    }
                    RouteSelection::CredentialsInvalid { .. } => {
                        LiveRouteSelection::CredentialsInvalid
                    }
                    RouteSelection::ModelUnavailable { detail, .. } => {
                        LiveRouteSelection::ModelUnavailable { detail }
                    }
                    RouteSelection::RoutingUnavailable => LiveRouteSelection::RoutingUnavailable,
                },
            )
        }
    }
    fn ensure_live_account_current(
        &self,
        selection: &LiveAccountSelection,
        now: i64,
    ) -> impl std::future::Future<Output = Result<(), GatewayError>> {
        async move {
            let context = self.selection();
            validate_binding(&selection.binding)?;
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let view = read_view(&mut tx, now).await?;
                let slot = usize::try_from(selection.binding.slot)
                    .map_err(|_| invalid("Invalid live slot."))?;
                if !view.policy().current_binding(&selection.binding)
                    || view.active.as_ref().is_none_or(|active| {
                        active.binding != selection.binding
                            || active.generation != selection.active_generation
                    })
                    || view.state.slots[slot].invalid_credential_version.as_ref()
                        == Some(&selection.binding.credential_version)
                {
                    return Err(unavailable(
                        "codex_routing_unavailable",
                        "Live selection changed.",
                    ));
                }
                if tx.commit().await? {
                    return Ok(());
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Live selection changed concurrently.",
            ))
        }
    }
    fn ensure_admission_current(
        &self,
        admission: &AdmittedAccount,
        now: i64,
    ) -> impl std::future::Future<Output = Result<(), GatewayError>> {
        async move {
            let context = self.selection();
            validate_binding(&admission.binding)?;
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let view = read_view(&mut tx, now).await?;
                if !view
                    .policy()
                    .matches_admission(&admission.binding, admission.active_generation)
                    || view.state.slots[admission.binding.slot as usize]
                        .invalid_credential_version
                        .as_ref()
                        == Some(&admission.binding.credential_version)
                {
                    return Err(unavailable(
                        "codex_routing_unavailable",
                        "Codex admission is stale.",
                    ));
                }
                if let Some(probe) = &admission.probe {
                    let slot = &view.state.slots[admission.binding.slot as usize];
                    if slot.probe_lease.as_ref() != Some(probe)
                        || probe.expires_at_ms <= context.fresh_time(now)?
                    {
                        return Err(unavailable(
                            "codex_routing_unavailable",
                            "Codex probe lease is stale.",
                        ));
                    }
                }
                if tx.commit().await? {
                    return Ok(());
                }
            }
            Err(unavailable(
                "codex_routing_unavailable",
                "Codex admission changed concurrently.",
            ))
        }
    }
}
impl<T: SelectionFactory + ?Sized> SelectionOperations for T {}

fn selection_result<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    view: &SelectionView,
    evaluated: &[Evaluation],
    election: Election<'_>,
    request: SelectionRequest<'_>,
) -> Result<RouteSelection, GatewayError> {
    let Election {
        active_slot,
        chosen,
    } = election;
    let SelectionRequest { model, class, now } = request;
    let result = if let Some(chosen) = chosen {
        let binding = view.binding(chosen.slot);
        let changed_identity = view
            .active
            .as_ref()
            .is_some_and(|active| active.binding.account_id_hash != binding.account_id_hash);
        let (active_generation, reason) = match &view.active {
            None => (1, None),
            Some(active) if changed_identity => (
                next_generation(active.generation)?,
                active_slot
                    .and_then(|row| row.reason)
                    .or(Some(ActiveTransitionReason::AccountRemovedOrReplaced)),
            ),
            Some(active) => (active.generation, active.transition_reason),
        };
        let next = ActiveSelection {
            v: 1,
            binding: binding.clone(),
            generation: active_generation,
            transition_reason: reason,
            updated_at_ms: now,
        };
        if view.active.as_ref().is_none_or(|active| {
            active.binding != next.binding || active.generation != next.generation
        }) {
            tx.put(&keys::active(), &next, None)?;
        }
        RouteSelection::Eligible(AdmittedAccount {
            binding,
            active_generation,
            requested_model: model.to_owned(),
            quota_class: class,
            probe_required: chosen.probe,
            probe: None,
        })
    } else {
        if let (Some(active), Some(active_slot)) = (&view.active, active_slot) {
            let binding = view.binding(active_slot.slot);
            if active.binding != binding {
                let refreshed = ActiveSelection {
                    binding,
                    updated_at_ms: now,
                    ..active.clone()
                };
                tx.put(&keys::active(), &refreshed, None)?;
            }
        }
        let proof = CohortProof {
            checks: tx.post_checks(),
            model: model.to_owned(),
            quota_class: class,
            exhausted: !evaluated.is_empty()
                && evaluated
                    .iter()
                    .all(|row| row.reason == Some(ActiveTransitionReason::QuotaExhausted)),
            unavailable: !evaluated.is_empty()
                && evaluated.iter().all(|row| {
                    matches!(
                        row.reason,
                        Some(
                            ActiveTransitionReason::QuotaExhausted
                                | ActiveTransitionReason::CredentialInvalid
                        )
                    )
                }),
            bindings: evaluated.iter().map(|row| view.binding(row.slot)).collect(),
        };
        if evaluated
            .iter()
            .any(|row| row.reason == Some(ActiveTransitionReason::QuotaExhausted))
        {
            RouteSelection::QuotaBlocked {
                retry_at_ms: evaluated.iter().filter_map(|row| row.retry).min(),
                proof,
            }
        } else if let Some(row) = evaluated
            .iter()
            .find(|row| row.reason == Some(ActiveTransitionReason::ModelUnavailable))
        {
            RouteSelection::ModelUnavailable {
                model: model.to_owned(),
                detail: row.detail.clone(),
            }
        } else {
            RouteSelection::CredentialsInvalid { proof }
        }
    };
    Ok(result)
}
