//! Complete checked paid operations; every existing effect retains one atomic transition.
mod backend;
use backend::Transaction;
use gateway_authority_contracts::{DAY_MS, PAID_RETENTION_MS, TokenSource, add, invalid};
use gateway_authority_contracts::{nonnegative, retention, text, unavailable};
use gateway_authority_keys as keys;
use gateway_authority_paid_records::arm_gate;
use gateway_authority_paid_records::{PaidAttempt, PaidCorrelation, PaidReservation, PaidRollup};
use gateway_authority_paid_records::{PaidUsage, PaidWindow, PendingPaid, ReconciliationGate};
use gateway_authority_paid_records::{ReconciliationLease, provider_label, rollup_shard};
use gateway_authority_paid_records::{pending_id, validate_attempt, validate_paid_window};
use gateway_core::{error::GatewayError, identity::ApiKeyId, state::AuthorityStore};
use gateway_core::{identity::PaidAttemptId, lifecycle::BillingState, lifecycle::DispatchState};
use gateway_core::{lifecycle::PaidTerminalState, provider::PaidProvider};
async fn read_gate<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
) -> Result<ReconciliationGate, GatewayError> {
    Ok(tx.read(&keys::paid_gate()).await?.unwrap_or_default())
}
async fn clear_nonbillable_marker<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    key_id: &ApiKeyId,
    attempt_id: &PaidAttemptId,
) -> Result<(), GatewayError> {
    let pending_key = keys::paid_pending(&key_id.0, &attempt_id.0);
    let pending: Option<PendingPaid> = tx.read(&pending_key).await?;
    let mut gate = read_gate(tx).await?;
    let indexed = gate
        .pending
        .remove(&pending_id(key_id, attempt_id))
        .is_some();
    if pending.is_some() {
        tx.delete(&pending_key)?;
    }
    if pending.is_some() || indexed {
        arm_gate(&mut gate);
        tx.put(&keys::paid_gate(), &gate, None)?;
    }
    Ok(())
}
fn correlated_spend(row: &PaidAttempt, usage: &PaidUsage) -> Result<Option<i64>, GatewayError> {
    let exact = match &usage.correlation {
        PaidCorrelation::ProviderRequestId(id)
            if row.reservation.provider == PaidProvider::Metered =>
        {
            if row.provider_request_id.as_ref() != Some(id) {
                return Ok(None);
            }
            None
        }
        PaidCorrelation::SurplusReservation {
            attempt_id,
            buyer_cost_micro,
        } if row.reservation.provider == PaidProvider::Surplus
            && attempt_id == &row.reservation.attempt_id =>
        {
            *buyer_cost_micro
        }
        _ => return Ok(None),
    };
    let spend = if let Some(exact) = exact {
        nonnegative(exact)?
    } else {
        let value = (usage.provider_quota * 1_000_000.0 / row.quota_per_credit as f64).round();
        if !value.is_finite()
            || value < 0.0
            || value > gateway_core::identity::MAX_SAFE_INTEGER as f64
        {
            return Err(invalid("Provider spend is outside the safe integer range."));
        }
        value as i64
    };
    Ok(Some(spend))
}
fn update_paid_rollup(
    mut rollup: PaidRollup,
    rollup_key: &gateway_core::state::RecordKey,
    reservation: &PaidReservation,
    usage: &PaidUsage,
    spend: i64,
    now: i64,
) -> Result<PaidRollup, GatewayError> {
    gateway_authority_paid_records::source_records::validate_rollup_identity(&rollup, rollup_key)?;
    rollup.request_count = add(rollup.request_count, 1)?;
    rollup.input_tokens = add(rollup.input_tokens, usage.input_tokens)?;
    rollup.cached_input_tokens = add(
        rollup.cached_input_tokens,
        usage.cached_input_tokens.unwrap_or(0),
    )?;
    rollup.output_tokens = add(rollup.output_tokens, usage.output_tokens)?;
    rollup.spend_microcredits += spend as f64;
    if !rollup.spend_microcredits.is_finite() || rollup.spend_microcredits < 0.0 {
        return Err(invalid("Paid rollup spend overflow."));
    }
    rollup.provider_quota += usage.provider_quota;
    if !rollup.provider_quota.is_finite() {
        return Err(invalid("Paid rollup quota overflow."));
    }
    rollup.first_request_at_ms = rollup.first_request_at_ms.min(reservation.created_at_ms);
    rollup.last_request_at_ms = rollup.last_request_at_ms.max(reservation.created_at_ms);
    rollup.updated_at_ms = now;
    Ok(rollup)
}
fn initial_paid_rollup(
    reservation: &PaidReservation,
    model: gateway_core::model::ModelId,
    bucket: i64,
    now: i64,
) -> PaidRollup {
    PaidRollup {
        v: 1,
        bucket_start_at_ms: bucket,
        model,
        provider: reservation.provider,
        request_count: 0,
        provider_quota: 0.0,
        input_tokens: 0,
        cached_input_tokens: 0,
        output_tokens: 0,
        spend_microcredits: 0.0,
        first_request_at_ms: reservation.created_at_ms,
        last_request_at_ms: reservation.created_at_ms,
        updated_at_ms: now,
    }
}
fn validate_paid_usage(usage: &PaidUsage) -> Result<(), GatewayError> {
    if !usage.provider_quota.is_finite() || usage.provider_quota < 0.0 {
        return Err(invalid("Provider usage is invalid."));
    }
    nonnegative(usage.input_tokens)?;
    nonnegative(usage.output_tokens)?;
    if let Some(cached) = usage.cached_input_tokens {
        nonnegative(cached)?;
    }
    Ok(())
}
pub struct PaidLedgerService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
}
impl<'a, S: AuthorityStore, I: TokenSource> PaidLedgerService<'a, S, I> {
    pub fn new(store: &'a S, tokens: &'a I) -> Self {
        Self { store, tokens }
    }
    fn transaction(&self, now: i64) -> Result<Transaction<'_, S, I>, GatewayError> {
        Transaction::new(self.store, self.tokens, now)
    }
}
pub trait PaidLedgerFactory {
    type Store: AuthorityStore;
    type Tokens: TokenSource;
    fn paid_ledger(&self) -> PaidLedgerService<'_, Self::Store, Self::Tokens>;
}
pub trait PaidLedgerOperations: PaidLedgerFactory {
    fn mark_paid_transport_started(
        &self,
        reservation: &PaidReservation,
        now: i64,
    ) -> impl std::future::Future<Output = Result<(), GatewayError>> {
        async move {
            let context = self.paid_ledger();
            for _ in 0..128 {
                let mut tx = context.transaction(now)?;
                let key = keys::paid_attempt(&reservation.key_id.0, &reservation.attempt_id.0);
                let mut row: PaidAttempt = tx
                    .read_source_control(&key)
                    .await?
                    .ok_or_else(|| invalid("Paid reservation is missing."))?;
                validate_attempt(&row, reservation)?;
                if row.dispatch_state != DispatchState::Dispatched
                    || row.billing_state == BillingState::NotBilled
                {
                    return Err(invalid("Paid attempt cannot dispatch."));
                }
                row.transport_started = true;
                tx.put_source_control(
                    &key,
                    &row,
                    Some(retention(reservation.created_at_ms, PAID_RETENTION_MS)?),
                )?;
                if tx.commit().await? {
                    return Ok(());
                }
            }
            Err(unavailable(
                "paid_fallback_unavailable",
                "Paid dispatch changed concurrently.",
            ))
        }
    }
    fn record_paid_lifecycle(
        &self,
        reservation: &PaidReservation,
        provider_request_id: Option<&str>,
        terminal: Option<PaidTerminalState>,
        now: i64,
    ) -> impl std::future::Future<Output = Result<(), GatewayError>> {
        async move {
            let context = self.paid_ledger();
            if let Some(id) = provider_request_id {
                text(id)?;
            }
            for _ in 0..128 {
                let mut tx = context.transaction(now)?;
                let key = keys::paid_attempt(&reservation.key_id.0, &reservation.attempt_id.0);
                let mut row: PaidAttempt = tx
                    .read_source_control(&key)
                    .await?
                    .ok_or_else(|| invalid("Paid attempt is missing."))?;
                validate_attempt(&row, reservation)?;
                if let Some(id) = provider_request_id
                    && row.provider_request_id.is_none()
                {
                    row.provider_request_id = Some(id.to_owned());
                }
                if row.terminal_state == PaidTerminalState::Pending
                    && let Some(terminal) = terminal
                {
                    row.terminal_state = terminal;
                    if terminal != PaidTerminalState::Pending {
                        row.terminal_at_ms = Some(now);
                    }
                }
                row.updated_at_ms = now;
                let pending_key =
                    keys::paid_pending(&reservation.key_id.0, &reservation.attempt_id.0);
                if let Some(mut pending) = tx.read::<PendingPaid>(&pending_key).await? {
                    pending.next_reconciliation_at_ms = now;
                    let mut gate = read_gate(&mut tx).await?;
                    gate.pending.insert(
                        pending_id(&reservation.key_id, &reservation.attempt_id),
                        pending.clone(),
                    );
                    arm_gate(&mut gate);
                    tx.put(&pending_key, &pending, None)?;
                    tx.put(&keys::paid_gate(), &gate, None)?;
                }
                tx.put_source_control(
                    &key,
                    &row,
                    Some(retention(reservation.created_at_ms, PAID_RETENTION_MS)?),
                )?;
                if tx.commit().await? {
                    return Ok(());
                }
            }
            Err(unavailable(
                "paid_fallback_unavailable",
                "Paid lifecycle changed concurrently.",
            ))
        }
    }
    fn release_paid_before_transport(
        &self,
        reservation: &PaidReservation,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.paid_ledger();
            for _ in 0..128 {
                let mut tx = context.transaction(now)?;
                let key = keys::paid_attempt(&reservation.key_id.0, &reservation.attempt_id.0);
                let mut row: PaidAttempt = tx
                    .read_source_control(&key)
                    .await?
                    .ok_or_else(|| invalid("Paid attempt is missing."))?;
                validate_attempt(&row, reservation)?;
                if row.billing_state != BillingState::Pending
                    || row.provider_request_id.is_some()
                    || row.transport_started
                    || row.dispatch_state == DispatchState::NotDispatched
                {
                    return Ok(false);
                }
                let pending_key =
                    keys::paid_pending(&reservation.key_id.0, &reservation.attempt_id.0);
                let _: Option<PendingPaid> = tx.read(&pending_key).await?;
                let window_key =
                    keys::paid_window(&reservation.key_id.0, reservation.window_reset_at_ms);
                let window: Option<PaidWindow> = tx.read(&window_key).await?;
                let mut gate = read_gate(&mut tx).await?;
                gate.pending
                    .remove(&pending_id(&reservation.key_id, &reservation.attempt_id));
                arm_gate(&mut gate);
                row.dispatch_state = DispatchState::NotDispatched;
                row.terminal_state = PaidTerminalState::Cancelled;
                row.billing_state = BillingState::NotBilled;
                row.spend_microcredits = Some(0);
                row.terminal_at_ms.get_or_insert(now);
                row.updated_at_ms = now;
                tx.put_source_control(
                    &key,
                    &row,
                    Some(retention(reservation.created_at_ms, PAID_RETENTION_MS)?),
                )?;
                tx.delete(&pending_key)?;
                tx.put(&keys::paid_gate(), &gate, None)?;
                if let Some(mut window) = window {
                    validate_paid_window(
                        &window,
                        &reservation.key_id,
                        reservation.window_reset_at_ms,
                    )?;
                    window.reserved_microcredits =
                        (window.reserved_microcredits - reservation.reserved_microcredits).max(0);
                    window.pending_count = (window.pending_count - 1).max(0);
                    window.updated_at_ms = now;
                    tx.put(
                        &window_key,
                        &window,
                        Some(retention(window.window_reset_at_ms, PAID_RETENTION_MS)?),
                    )?;
                }
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "paid_fallback_unavailable",
                "Paid release changed concurrently.",
            ))
        }
    }
    fn settle_paid(
        &self,
        reservation: &PaidReservation,
        usage: &PaidUsage,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.paid_ledger();
            validate_paid_usage(usage)?;
            for _ in 0..128 {
                let mut tx = context.transaction(now)?;
                let key = keys::paid_attempt(&reservation.key_id.0, &reservation.attempt_id.0);
                let Some(mut row) = tx.read_source_control::<PaidAttempt>(&key).await? else {
                    clear_nonbillable_marker(&mut tx, &reservation.key_id, &reservation.attempt_id)
                        .await?;
                    if tx.commit().await? {
                        return Ok(false);
                    }
                    continue;
                };
                validate_attempt(&row, reservation)?;
                let pending_key =
                    keys::paid_pending(&reservation.key_id.0, &reservation.attempt_id.0);
                let pending: Option<PendingPaid> = tx.read(&pending_key).await?;
                let mut gate = read_gate(&mut tx).await?;
                if matches!(
                    row.billing_state,
                    BillingState::Settled | BillingState::NotBilled
                ) {
                    if pending.is_some() {
                        tx.delete(&pending_key)?;
                        gate.pending
                            .remove(&pending_id(&reservation.key_id, &reservation.attempt_id));
                        arm_gate(&mut gate);
                        tx.put(&keys::paid_gate(), &gate, None)?;
                    }
                    if tx.commit().await? {
                        return Ok(false);
                    } else {
                        continue;
                    }
                }
                if pending.is_none() {
                    return Ok(false);
                }
                let Some(spend) = correlated_spend(&row, usage)? else {
                    return Ok(false);
                };
                let window_key =
                    keys::paid_window(&reservation.key_id.0, reservation.window_reset_at_ms);
                let window: Option<PaidWindow> = tx.read(&window_key).await?;
                let bucket = reservation.created_at_ms / 3_600_000 * 3_600_000;
                let model = gateway_core::model::ModelId::from_source(
                    gateway_schema::reasoning::js_trim_source(row.model.as_js_string()),
                );
                let rollup_key = keys::paid_rollup_model(
                    bucket,
                    &model,
                    provider_label(reservation.provider),
                    rollup_shard(&reservation.attempt_id.0),
                );
                let rollup: PaidRollup = tx
                    .read_source_control(&rollup_key)
                    .await?
                    .unwrap_or(initial_paid_rollup(reservation, model.clone(), bucket, now));
                let rollup =
                    update_paid_rollup(rollup, &rollup_key, reservation, usage, spend, now)?;
                row.billing_state = BillingState::Settled;
                row.provider_quota = Some(usage.provider_quota);
                row.input_tokens = Some(usage.input_tokens);
                row.cached_input_tokens = usage.cached_input_tokens;
                row.output_tokens = Some(usage.output_tokens);
                row.spend_microcredits = Some(spend);
                row.settled_at_ms.get_or_insert(now);
                row.last_reconciliation_at_ms = Some(now);
                row.updated_at_ms = now;
                row.usage_rollup_at_ms = Some(now);
                gate.pending
                    .remove(&pending_id(&reservation.key_id, &reservation.attempt_id));
                arm_gate(&mut gate);
                tx.put_source_control(
                    &key,
                    &row,
                    Some(retention(reservation.created_at_ms, PAID_RETENTION_MS)?),
                )?;
                tx.delete(&pending_key)?;
                tx.put(&keys::paid_gate(), &gate, None)?;
                tx.put_source_control(&rollup_key, &rollup, None)?;
                if let Some(mut window) = window {
                    validate_paid_window(
                        &window,
                        &reservation.key_id,
                        reservation.window_reset_at_ms,
                    )?;
                    window.settled_microcredits = add(window.settled_microcredits, spend)?;
                    window.reserved_microcredits =
                        (window.reserved_microcredits - reservation.reserved_microcredits).max(0);
                    window.pending_count = (window.pending_count - 1).max(0);
                    window.updated_at_ms = now;
                    tx.put(
                        &window_key,
                        &window,
                        Some(retention(window.window_reset_at_ms, PAID_RETENTION_MS)?),
                    )?;
                }
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "paid_fallback_unavailable",
                "Paid settlement changed concurrently.",
            ))
        }
    }
    /// Event-owned recovery only. No timer/alarm is introduced by this module.
    fn due_paid(
        &self,
        now: i64,
        limit: usize,
    ) -> impl std::future::Future<Output = Result<Vec<PendingPaid>, GatewayError>> {
        async move {
            let context = self.paid_ledger();
            let mut tx = context.transaction(now)?;
            let gate = read_gate(&mut tx).await?;
            let mut due: Vec<_> = gate
                .pending
                .into_values()
                .filter(|row| row.next_reconciliation_at_ms <= now)
                .collect();
            due.sort_by_key(|row| row.next_reconciliation_at_ms);
            due.truncate(limit);
            Ok(due)
        }
    }
    fn defer_paid(
        &self,
        reservation: &PaidReservation,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Option<i64>, GatewayError>> {
        async move {
            let context = self.paid_ledger();
            const DELAYS: [i64; 6] = [5_000, 30_000, 120_000, 600_000, 3_600_000, 21_600_000];
            for _ in 0..128 {
                let mut tx = context.transaction(now)?;
                let key = keys::paid_attempt(&reservation.key_id.0, &reservation.attempt_id.0);
                let Some(mut row) = tx.read_source_control::<PaidAttempt>(&key).await? else {
                    clear_nonbillable_marker(&mut tx, &reservation.key_id, &reservation.attempt_id)
                        .await?;
                    if tx.commit().await? {
                        return Ok(None);
                    }
                    continue;
                };
                validate_attempt(&row, reservation)?;
                let pending_key =
                    keys::paid_pending(&reservation.key_id.0, &reservation.attempt_id.0);
                let Some(mut pending) = tx.read::<PendingPaid>(&pending_key).await? else {
                    return Ok(None);
                };
                if matches!(
                    row.billing_state,
                    BillingState::Settled | BillingState::NotBilled
                ) {
                    return Ok(None);
                }
                row.reconciliation_attempts = add(row.reconciliation_attempts, 1)?;
                let delay = DELAYS[((row.reconciliation_attempts - 1).min(5)) as usize];
                if now - reservation.created_at_ms >= DAY_MS {
                    row.billing_state = BillingState::Unresolved;
                }
                row.last_reconciliation_at_ms = Some(now);
                row.updated_at_ms = now;
                pending.next_reconciliation_at_ms = add(now, delay)?;
                let mut gate = read_gate(&mut tx).await?;
                gate.pending.insert(
                    pending_id(&reservation.key_id, &reservation.attempt_id),
                    pending.clone(),
                );
                arm_gate(&mut gate);
                tx.put_source_control(
                    &key,
                    &row,
                    Some(retention(reservation.created_at_ms, PAID_RETENTION_MS)?),
                )?;
                tx.put(&pending_key, &pending, None)?;
                tx.put(&keys::paid_gate(), &gate, None)?;
                if tx.commit().await? {
                    return Ok(Some(delay));
                }
            }
            Err(unavailable(
                "paid_fallback_unavailable",
                "Paid reconciliation changed concurrently.",
            ))
        }
    }
    /// Expired raw evidence may leave a durable marker. Remove only that stale wake-up;
    /// never infer zero billing or refund an old window's reserved exposure.
    fn reconcile_paid_marker(
        &self,
        key_id: &ApiKeyId,
        attempt_id: &PaidAttemptId,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.paid_ledger();
            text(&key_id.0)?;
            text(&attempt_id.0)?;
            for _ in 0..128 {
                let mut tx = context.transaction(now)?;
                let row: Option<PaidAttempt> = tx
                    .read(&keys::paid_attempt(&key_id.0, &attempt_id.0))
                    .await?;
                if row.is_some_and(|row| {
                    !matches!(
                        row.billing_state,
                        BillingState::Settled | BillingState::NotBilled
                    )
                }) {
                    return Ok(false);
                }
                clear_nonbillable_marker(&mut tx, key_id, attempt_id).await?;
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "paid_fallback_unavailable",
                "Paid marker reconciliation changed concurrently.",
            ))
        }
    }
    fn claim_paid_reconciliation(
        &self,
        key_id: &ApiKeyId,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Option<ReconciliationLease>, GatewayError>> {
        async move {
            let context = self.paid_ledger();
            for _ in 0..128 {
                let mut tx = context.transaction(now)?;
                let key = keys::paid_lease(&key_id.0);
                if tx
                    .read::<ReconciliationLease>(&key)
                    .await?
                    .is_some_and(|lease| lease.expires_at_ms > now)
                {
                    return Ok(None);
                }
                let lease = ReconciliationLease {
                    token: context.tokens.next_token()?,
                    expires_at_ms: add(now, 60_000)?,
                };
                text(&lease.token)?;
                tx.put(&key, &lease, Some(lease.expires_at_ms))?;
                if tx.commit().await? {
                    return Ok(Some(lease));
                }
            }
            Err(unavailable(
                "paid_fallback_unavailable",
                "Reconciliation lease changed concurrently.",
            ))
        }
    }
    fn release_paid_reconciliation(
        &self,
        key_id: &ApiKeyId,
        expected: &ReconciliationLease,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.paid_ledger();
            for _ in 0..128 {
                let mut tx = context.transaction(now)?;
                let key = keys::paid_lease(&key_id.0);
                if tx.read::<ReconciliationLease>(&key).await?.as_ref() != Some(expected) {
                    return Ok(false);
                }
                tx.delete(&key)?;
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "paid_fallback_unavailable",
                "Reconciliation lease release changed concurrently.",
            ))
        }
    }
}
impl<T: PaidLedgerFactory> PaidLedgerOperations for T {}
