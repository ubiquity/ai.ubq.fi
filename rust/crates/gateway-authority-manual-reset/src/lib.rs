//! Complete same-store source2 manual reset effects; provider I/O and terminal settlement retain their owners.
mod backend;
mod view;
use backend::Transaction;
use gateway_auth_contracts::types::{AuthConfig, AuthHost};
use gateway_auth_credentials::provider_credentials::{
    codex_access_expiry_ms, codex_source_pool_key, parse_codex_source_pool,
};
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_authority_actor_currentness::{
    app::ActorService,
    source_actor::{SourceActorCurrentData, SourceNativeRenewalActor},
};
use gateway_authority_actor_records::{scope::NativeActorScope, source::StoredNativeActorV2};
use gateway_authority_capacity_policy as capacity_rules;
use gateway_authority_contracts::{
    AuthorityClock, PROBE_LEASE_MS, TokenSource, add, validate_time,
};
use gateway_authority_keys as keys;
use gateway_authority_reset_records::{
    AccountDayBudget, ResetClaim, ResetConfig, ResetInvocation, ResetMode, ResetRecord, ResetState,
    manual::{ManualIntent, ManualResetPromotion, error, intent_key, parse_manual_intent},
};
use gateway_authority_routing_evidence::capacity::CapacityObservation;
use gateway_contracts::identity::{
    AccountBinding, ActiveSelection, ActiveTransitionReason, RequestContext,
};
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
use serde_json::Value;
use view::RoutingView;
/// Minted only from a real admin actor. No serde/public constructor.
pub struct SourceManualResetTarget<'a, S> {
    actor: StoredNativeActorV2,
    context: RequestContext,
    store: &'a S,
    account_id: String,
    hash: String,
    binding: AccountBinding,
}
impl<S> SourceManualResetTarget<'_, S> {
    pub fn account_id(&self) -> &str {
        &self.account_id
    }
    pub fn credential_version(&self) -> &str {
        &self.binding.credential_version.0
    }
    pub fn binding(&self) -> &AccountBinding {
        &self.binding
    }
}
pub struct SourceManualResetAction<'a, S> {
    target: SourceManualResetTarget<'a, S>,
    intent: ManualIntent,
}
impl<S> SourceManualResetAction<'_, S> {
    pub fn account_id(&self) -> &str {
        self.target.account_id()
    }
    pub fn credential_version(&self) -> &str {
        self.target.credential_version()
    }
    pub fn intent(&self) -> &ManualIntent {
        &self.intent
    }
    pub fn target(&self) -> &SourceManualResetTarget<'_, S> {
        &self.target
    }
    pub fn invocation(&self) -> Result<ResetInvocation, GatewayError> {
        gateway_authority_reset_records::reset_invocation(
            &self.intent.account_id_hash,
            &self.intent.credential_version,
            self.intent.quota_reset_at_ms,
        )
    }
}
pub enum SourceManualResetPreparation<'a, S> {
    NeedsUsage(SourceManualResetTarget<'a, S>),
    Ready(SourceManualResetAction<'a, S>),
}
async fn policy<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
) -> Result<(), GatewayError> {
    let policy: ResetConfig = tx.read(&keys::reset_config()).await?.unwrap_or_default();
    let reason = if !policy.enabled {
        Some("feature_disabled")
    } else if policy.mode == ResetMode::Disabled {
        Some("mode_disabled")
    } else if policy.max_per_account_per_day != 1 {
        Some("per_account_day_limit_invalid")
    } else if policy.max_per_account_per_window != 1 {
        Some("per_account_window_limit_invalid")
    } else {
        None
    };
    if let Some(reason) = reason {
        return Err(error(409, reason));
    }
    Ok(())
}
async fn current<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    phase: &SourceActorCurrentData,
    hash: &str,
) -> Result<(RoutingView, String), GatewayError> {
    policy(tx).await?;
    for check in phase.revision_checks() {
        tx.observe_revision(&check.key).await?;
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(actor_unavailable());
        }
    }
    if phase.scope() != NativeActorScope::Admin {
        return Err(error(403, "admin_auth_required"));
    }
    let raw: Value = tx
        .read(&codex_source_pool_key())
        .await?
        .ok_or_else(|| error(409, "codex_account_changed"))?;
    let source =
        parse_codex_source_pool(&raw)?.ok_or_else(|| error(409, "codex_account_changed"))?;
    let (slot, account) = source
        .accounts
        .iter()
        .enumerate()
        .find(|(_, a)| a.identity().administrative_account_hash.0 == hash)
        .ok_or_else(|| error(409, "codex_account_changed"))?;
    if codex_access_expiry_ms(&account.access_token).is_some_and(|v| v <= tx.now as f64) {
        return Err(error(409, "codex_auth_invalid"));
    }
    let view = view::read_view(tx).await?;
    let identity = account.identity();
    if view.pool.pool_versionstamp.0
        != tx
            .revision(&codex_source_pool_key())
            .ok_or_else(|| error(409, "codex_account_changed"))?
        || view.pool.accounts.get(slot).is_none_or(|a| {
            a.account_id_hash != identity.routing_account_hash
                || a.credential_version != identity.credential_version
                || a.admin_account_hash != identity.administrative_account_hash
        })
    {
        return Err(error(409, "codex_account_changed"));
    }
    if !view.pool.accounts[slot].enabled {
        return Err(error(409, "codex_provider_disabled"));
    }
    Ok((view, account.account_id.clone()))
}
fn slot(view: &RoutingView, hash: &str) -> Result<usize, GatewayError> {
    view.pool
        .accounts
        .iter()
        .position(|a| a.admin_account_hash.0 == hash)
        .ok_or_else(|| error(409, "codex_account_changed"))
}
fn checked_target<S>(
    view: &RoutingView,
    account: &str,
    target: &SourceManualResetTarget<'_, S>,
) -> Result<usize, GatewayError> {
    let index = slot(view, &target.hash)?;
    if account != target.account_id
        || view.pool.accounts[index].credential_version != target.binding.credential_version
    {
        return Err(error(409, "codex_account_changed"));
    }
    Ok(index)
}
async fn read_intent<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    hash: &str,
) -> Result<Option<ManualIntent>, GatewayError> {
    tx.read::<Value>(&intent_key(hash))
        .await?
        .as_ref()
        .map(parse_manual_intent)
        .transpose()
}
fn observation<S>(
    row: &CapacityObservation,
    target: &SourceManualResetTarget<'_, S>,
    now: i64,
) -> Result<(), GatewayError> {
    if !capacity_rules::manual::manual_observation_is_current(row, &target.binding, now) {
        return Err(error(409, "codex_quota_unavailable"));
    }
    Ok(())
}
fn deadline(row: &CapacityObservation, now: i64) -> Result<i64, GatewayError> {
    capacity_rules::manual::manual_reset_deadline(row, now)
        .ok_or_else(|| error(409, "codex_quota_identity_unavailable"))
}
async fn manual_budget<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    hash: &str,
) -> Result<AccountDayBudget, GatewayError> {
    let day = gateway_authority_reset_records::utc_day(tx.now)?;
    let budget: AccountDayBudget =
        tx.read(&keys::reset_day(hash, &day))
            .await?
            .unwrap_or(AccountDayBudget {
                v: 1,
                day: day.clone(),
                submission_count: 0,
                updated_at_ms: tx.now,
            });
    if budget.v != 1 || budget.day != day || budget.submission_count != 0 {
        return Err(error(429, "account_day_limit_reached"));
    }
    Ok(budget)
}
pub struct SourceManualResetService<'a, S, I, H> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
    actors: ActorService<'a, S, H>,
}
impl<'a, S: AuthorityStore, I: TokenSource, H: AuthHost + PassportDate>
    SourceManualResetService<'a, S, I, H>
{
    pub fn new(
        store: &'a S,
        tokens: &'a I,
        clock: Option<&'a dyn AuthorityClock>,
        host: &'a H,
        config: &'a AuthConfig,
    ) -> Self {
        Self {
            store,
            tokens,
            clock,
            actors: ActorService::new(store, host, config),
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
                gateway_authority_contracts::unavailable(
                    "authority_clock_unavailable",
                    "Time-sensitive authority actions require a trusted current clock.",
                )
            })?
            .now_ms()?;
        validate_time(now)?;
        if now < started_at {
            return Err(gateway_authority_contracts::unavailable(
                "invalid_clock",
                "Authority clock moved backwards.",
            ));
        }
        Ok(now)
    }

    async fn phase(
        &self,
        target: &SourceManualResetTarget<'_, S>,
    ) -> Result<SourceActorCurrentData, GatewayError> {
        if !std::ptr::eq(self.store, target.store) {
            return Err(error(403, "admin_auth_required"));
        }
        self.actors
            .check_stored_source_phase(
                &target.actor,
                &target.context,
                NativeActorScope::Admin,
                self.fresh_time(0)?,
            )
            .await
    }
    pub async fn prepare_source_manual_reset(
        &self,
        actor: &SourceNativeRenewalActor<'_, S>,
        hash: &str,
    ) -> Result<SourceManualResetPreparation<'a, S>, GatewayError> {
        if actor.scope() != NativeActorScope::Admin
            || hash.len() != 64
            || !hash
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        {
            return Err(error(403, "admin_auth_required"));
        }
        for _ in 0..3 {
            let phase = self
                .actors
                .prepare_source_phase(actor, NativeActorScope::Admin, self.fresh_time(0)?)
                .await?;
            let mut tx = self.transaction(self.fresh_time(0)?)?;
            let (view, account) = current(&mut tx, phase.current_data(), hash).await?;
            let index = slot(&view, hash)?;
            let target = SourceManualResetTarget {
                actor: phase.actor_data().clone(),
                context: phase.context().clone(),
                store: self.store,
                account_id: account,
                hash: hash.into(),
                binding: view.policy().binding(index),
            };
            let prior = read_intent(&mut tx, hash).await?;
            let result = if let Some(intent) = prior.filter(|r| r.promoted_at_ms.is_none()) {
                if intent.credential_version != target.binding.credential_version.0 {
                    return Err(error(409, "codex_account_changed"));
                }
                SourceManualResetPreparation::Ready(SourceManualResetAction { target, intent })
            } else {
                let existing = read_intent(&mut tx, hash).await?;
                if existing.is_some_and(|r| {
                    gateway_authority_reset_records::utc_day(r.created_at_ms).ok()
                        == gateway_authority_reset_records::utc_day(tx.now).ok()
                }) {
                    return Err(error(409, "codex_reset_already_completed"));
                }
                manual_budget(&mut tx, hash).await?;
                SourceManualResetPreparation::NeedsUsage(target)
            };
            lifetime(
                phase.actor_data(),
                phase.current_data().minimum_expiry(),
                self.fresh_time(tx.now)?,
            )?;
            if tx.commit().await? {
                return Ok(result);
            }
        }
        Err(error(409, "codex_reset_concurrent_update"))
    }
    pub async fn begin_source_manual_reset(
        &self,
        target: &SourceManualResetTarget<'_, S>,
        usage: &CapacityObservation,
    ) -> Result<SourceManualResetAction<'a, S>, GatewayError> {
        let phase = self.phase(target).await?;
        let mut tx = self.transaction(self.fresh_time(0)?)?;
        let (view, account) = current(&mut tx, &phase, &target.hash).await?;
        let index = checked_target(&view, &account, target)?;
        let prior = read_intent(&mut tx, &target.hash).await?;
        if prior.as_ref().is_some_and(|r| r.promoted_at_ms.is_none()) {
            return Err(error(409, "codex_reset_concurrent_update"));
        }
        manual_budget(&mut tx, &target.hash).await?;
        observation(usage, target, self.fresh_time(tx.now)?)?;
        let quota = deadline(usage, tx.now)?;
        if prior.as_ref().is_some_and(|r| {
            r.quota_reset_at_ms == quota
                || gateway_authority_reset_records::utc_day(r.created_at_ms).ok()
                    == gateway_authority_reset_records::utc_day(tx.now).ok()
        }) {
            return Err(error(409, "codex_reset_already_completed"));
        }
        let state = &view.state.slots[index];
        if state.invalid_credential_version.as_ref() == Some(&target.binding.credential_version) {
            return Err(error(409, "codex_auth_invalid"));
        }
        let intent = ManualIntent {
            v: 1,
            account_id_hash: target.hash.clone(),
            credential_version: target.binding.credential_version.0.clone(),
            quota_reset_at_ms: quota,
            routing_generation: state.generation,
            created_at_ms: self.fresh_time(tx.now)?,
            promoted_at_ms: None,
            quota_signal_at_ms: state.quota_signal_observed_at_ms,
        };
        tx.put(&intent_key(&target.hash), &intent, None)?;
        if !tx.commit().await? {
            return Err(error(409, "codex_reset_concurrent_update"));
        }
        Ok(SourceManualResetAction {
            target: SourceManualResetTarget {
                actor: target.actor.clone(),
                context: target.context.clone(),
                store: self.store,
                account_id: target.account_id.clone(),
                hash: target.hash.clone(),
                binding: view.policy().binding(index),
            },
            intent,
        })
    }
    async fn manual_fences(
        &self,
        tx: &mut Transaction<'_, S, I>,
        action: &SourceManualResetAction<'_, S>,
        phase: &SourceActorCurrentData,
        spending: bool,
    ) -> Result<(RoutingView, usize), GatewayError> {
        let (view, account) = current(tx, phase, &action.target.hash).await?;
        let index = checked_target(&view, &account, &action.target)?;
        if read_intent(tx, &action.target.hash).await?.as_ref() != Some(&action.intent)
            || action.intent.promoted_at_ms.is_some()
        {
            return Err(error(409, "codex_reset_already_completed"));
        }
        let now = self.fresh_time(tx.now)?;
        lifetime(&action.target.actor, phase.minimum_expiry(), now)?;
        tx.now = now;
        if spending {
            let state = &view.state.slots[index];
            if now < action.intent.created_at_ms
                || now - action.intent.created_at_ms >= 30_000
                || action.intent.quota_reset_at_ms <= now
                || state.generation != action.intent.routing_generation
                || state.quota_signal_observed_at_ms != action.intent.quota_signal_at_ms
                || u64::try_from(index).ok() != Some(action.target.binding.slot)
            {
                return Err(error(409, "codex_reset_concurrent_update"));
            }
        }
        Ok((view, index))
    }
    pub async fn claim_source_manual_reset(
        &self,
        action: &SourceManualResetAction<'_, S>,
    ) -> Result<ResetClaim, GatewayError> {
        for _ in 0..4 {
            let phase = self.phase(&action.target).await?;
            let mut tx = self.transaction(self.fresh_time(0)?)?;
            self.manual_fences(&mut tx, action, &phase, false).await?;
            let invocation = action.invocation()?;
            let key =
                keys::reset_redemption(&action.target.hash, &invocation.identity.quota_generation);
            let existing: Option<ResetRecord> = tx.read(&key).await?;
            if let Some(row) = existing.as_ref() {
                if row.v != 1 || row.identity != invocation.identity {
                    return Err(error(409, "redemption_record_context_mismatch"));
                }
                match row.state {
                    ResetState::Verified => return Ok(ResetClaim::Verified(row.clone())),
                    ResetState::Rejected => return Ok(ResetClaim::Rejected(row.clone())),
                    ResetState::Submitted | ResetState::Unknown => {
                        return Ok(ResetClaim::InProgress(row.clone()));
                    }
                    ResetState::Claimed if row.lease_expires_at_ms > tx.now => {
                        return Ok(ResetClaim::InProgress(row.clone()));
                    }
                    _ => {}
                }
            }
            self.manual_fences(&mut tx, action, &phase, true).await?;
            let row = ResetRecord {
                v: 1,
                identity: invocation.identity.clone(),
                routing_generation: action.intent.routing_generation,
                state: ResetState::Claimed,
                owner_token: self.tokens.next_token()?,
                fence: existing.as_ref().map_or(Ok(1), |r| {
                    gateway_authority_contracts::next_generation(r.fence)
                })?,
                lease_expires_at_ms: add(tx.now, PROBE_LEASE_MS)?,
                provider_receipt_id: None,
                created_at_ms: existing.as_ref().map_or(tx.now, |r| r.created_at_ms),
                updated_at_ms: tx.now,
                submitted_at_ms: None,
                verified_at_ms: None,
                last_error_code: None,
            };
            tx.put(&key, &row, None)?;
            if tx.commit().await? {
                return Ok(ResetClaim::Submit {
                    record: row,
                    invocation,
                });
            }
        }
        Err(error(409, "codex_reset_concurrent_update"))
    }
    pub async fn prepare_source_manual_reset_submission(
        &self,
        action: &SourceManualResetAction<'_, S>,
        expected: &ResetRecord,
    ) -> Result<ResetRecord, GatewayError> {
        self.manual_submit(action, expected, false).await
    }
    pub async fn renew_source_manual_reset_before_redeem(
        &self,
        action: &SourceManualResetAction<'_, S>,
        expected: &ResetRecord,
    ) -> Result<ResetRecord, GatewayError> {
        self.manual_submit(action, expected, true).await
    }
    async fn manual_submit(
        &self,
        action: &SourceManualResetAction<'_, S>,
        expected: &ResetRecord,
        renew: bool,
    ) -> Result<ResetRecord, GatewayError> {
        for _ in 0..4 {
            let phase = self.phase(&action.target).await?;
            let mut tx = self.transaction(self.fresh_time(0)?)?;
            self.manual_fences(&mut tx, action, &phase, true).await?;
            let invocation = action.invocation()?;
            let key =
                keys::reset_redemption(&action.target.hash, &invocation.identity.quota_generation);
            let mut row: ResetRecord = tx
                .read(&key)
                .await?
                .ok_or_else(|| error(409, "stale_owner"))?;
            gateway_authority_reset_records::check_record(&row, expected)?;
            if row.identity != invocation.identity
                || row.state
                    != if renew {
                        ResetState::Submitted
                    } else {
                        ResetState::Claimed
                    }
                || row.lease_expires_at_ms <= tx.now
                || gateway_authority_reset_records::utc_day(row.created_at_ms)?
                    != gateway_authority_reset_records::utc_day(tx.now)?
            {
                return Err(error(409, "stale_owner"));
            }
            if !renew {
                let mut budget = manual_budget(&mut tx, &action.target.hash).await?;
                budget.submission_count = 1;
                budget.updated_at_ms = tx.now;
                tx.put(
                    &keys::reset_day(&action.target.hash, &budget.day),
                    &budget,
                    None,
                )?;
                row.state = ResetState::Submitted;
                row.submitted_at_ms = Some(tx.now);
            }
            row.lease_expires_at_ms = add(tx.now, PROBE_LEASE_MS)?;
            row.updated_at_ms = tx.now;
            self.manual_fences(&mut tx, action, &phase, true).await?;
            tx.put(&key, &row, None)?;
            if tx.commit().await? {
                return Ok(row);
            }
        }
        Err(error(409, "codex_reset_concurrent_update"))
    }
    pub async fn promote_source_manual_reset(
        &self,
        action: &SourceManualResetAction<'_, S>,
        verified: &ResetRecord,
        usage: &CapacityObservation,
    ) -> Result<ManualResetPromotion, GatewayError> {
        for _ in 0..3 {
            let phase = self.phase(&action.target).await?;
            let mut tx = self.transaction(self.fresh_time(0)?)?;
            let (mut view, index) = self.manual_fences(&mut tx, action, &phase, false).await?;
            observation(usage, &action.target, tx.now)?;
            let mut current_usage = usage.clone();
            current_usage.slot =
                u8::try_from(index).map_err(|_| error(409, "codex_account_changed"))?;
            let invocation = action.invocation()?;
            let row: ResetRecord = tx
                .read(&keys::reset_redemption(
                    &action.target.hash,
                    &invocation.identity.quota_generation,
                ))
                .await?
                .ok_or_else(|| error(409, "codex_reset_unverified"))?;
            gateway_authority_reset_records::check_record(&row, verified)?;
            if row.state != ResetState::Verified || row.identity != invocation.identity {
                return Err(error(409, "codex_reset_unverified"));
            }
            let models = capacity_rules::manual::manual_recovered_models(usage);
            if models.is_empty() {
                return Err(error(409, "codex_reset_propagation_pending"));
            }
            let state = &view.state.slots[index];
            if state.invalid_credential_version.as_ref()
                == Some(&action.target.binding.credential_version)
                || state
                    .probe_lease
                    .as_ref()
                    .is_some_and(|l| l.expires_at_ms > tx.now)
                || state
                    .quota_signal_observed_at_ms
                    .is_some_and(|at| at >= usage.snapshot_at_ms)
                || view.capacity.observations.iter().any(|r| {
                    r.account_id_hash == usage.account_id_hash
                        && r.snapshot_at_ms > usage.snapshot_at_ms
                })
            {
                return Err(error(409, "codex_reset_concurrent_update"));
            }
            for model in &models {
                if let Some(next) = capacity_rules::apply_capacity_observation(
                    &view.state,
                    &current_usage,
                    tx.now,
                    Some(model),
                    tx.now,
                )? {
                    view.state = next;
                }
            }
            if models.iter().any(|model| {
                view.state.slots[index]
                    .quota_block_for(
                        gateway_authority_routing_evidence::models::model_quota_class(model),
                    )
                    .is_some_and(|b| b.blocked_until_ms > tx.now)
            }) {
                return Err(error(409, "codex_reset_concurrent_update"));
            }
            let capacity = gateway_authority_capacity_policy::plan_capacity_observation_store(
                tx.revision(&keys::capacity()),
                &view.capacity,
                std::slice::from_ref(&current_usage),
                tx.now,
            )
            .capacity;
            let active = ActiveSelection {
                v: 1,
                binding: view.policy().binding(index),
                generation: gateway_authority_contracts::next_generation(
                    view.active.as_ref().map_or(0, |a| a.generation),
                )?,
                transition_reason: Some(ActiveTransitionReason::ManualReset),
                updated_at_ms: tx.now,
            };
            let mut intent = action.intent.clone();
            intent.promoted_at_ms = Some(tx.now);
            policy(&mut tx).await?;
            lifetime(
                &action.target.actor,
                phase.minimum_expiry(),
                self.fresh_time(tx.now)?,
            )?;
            tx.put(&keys::routing(), &view.state, None)?;
            tx.put(&keys::capacity(), &capacity, None)?;
            tx.put(&keys::active(), &active, None)?;
            tx.put(&intent_key(&action.target.hash), &intent, None)?;
            if tx.commit().await? {
                return Ok(ManualResetPromotion {
                    reset_status: "verified".into(),
                    active_account_id_hash: action.target.hash.clone(),
                });
            }
        }
        Err(error(409, "codex_reset_concurrent_update"))
    }
}
fn lifetime(
    actor: &StoredNativeActorV2,
    minimum: Option<i64>,
    now: i64,
) -> Result<(), GatewayError> {
    validate_time(now)?;
    if actor
        .source_valid_until_ms
        .is_some_and(|expiry| expiry <= now)
        || actor.source_expiry().is_some_and(|expiry| expiry <= now)
        || minimum.is_some_and(|expiry| expiry <= now)
    {
        return Err(actor_unavailable());
    }
    Ok(())
}

fn actor_unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Original-request native actor authority is unavailable or changed.",
        "native_actor_unavailable",
    )
}
