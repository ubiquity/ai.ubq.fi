//! Complete checked reset effects; each source read set stays in its original final CAS.
mod backend;
use backend::Transaction;
use gateway_authority_contracts::{AuthorityClock, PROBE_LEASE_MS, TokenSource, add, invalid};
use gateway_authority_contracts::{nonnegative, text, unavailable, validate_time};
use gateway_authority_keys as keys;
use gateway_authority_reset_authorization::{ResetAuthorization, live_clock};
use gateway_authority_reset_evidence::types::ResetCandidate;
use gateway_authority_reset_records::{AccountDayBudget, ResetConfig, ResetInvocation};
use gateway_authority_reset_records::{ResetRecord, ResetState, check_record, utc_day};
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;

async fn authorization_fences<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    authorization: ResetAuthorization<'_>,
) -> Result<ResetInvocation, GatewayError> {
    let now = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        authorization.fence_data(&reader, now).await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(evidence.invocation().clone())
}
pub struct ResetSpendService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
}
impl<'a, S: AuthorityStore, I: TokenSource> ResetSpendService<'a, S, I> {
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
            .as_ref()
            .ok_or_else(|| {
                crate::unavailable(
                    "authority_clock_unavailable",
                    "Time-sensitive authority actions require a trusted current clock.",
                )
            })?
            .now_ms()?;
        validate_time(now)?;
        if now < started_at {
            return Err(crate::unavailable(
                "invalid_clock",
                "Authority clock moved backwards.",
            ));
        }
        Ok(now)
    }
    fn check_reset_authorization_deadline(
        &self,
        authorization: ResetAuthorization<'_>,
        anchor: i64,
    ) -> Result<(), GatewayError> {
        if let ResetAuthorization::FullPool(permit) = authorization {
            permit.check_deadline(self.fresh_time(anchor)?)?;
        }
        Ok(())
    }

    async fn prepare_reset_submission_guarded(
        &self,
        authorization: ResetAuthorization<'_>,
        expected: &ResetRecord,
        now: i64,
    ) -> Result<ResetRecord, GatewayError> {
        let candidate = authorization.candidate();
        for _ in 0..4 {
            let mut tx = self.transaction(now)?;
            let invocation = authorization_fences(&mut tx, authorization).await?;
            let config: ResetConfig = tx.read(&keys::reset_config()).await?.unwrap_or_default();
            if let Some(reason) = config.live_reason() {
                return Err(unavailable(reason, "Reset live policy changed."));
            }
            let key = keys::reset_redemption(
                &invocation.identity.account_id_hash,
                &invocation.identity.quota_generation,
            );
            let mut row: ResetRecord = tx
                .read(&key)
                .await?
                .ok_or_else(|| unavailable("stale_owner", "Reset claim is missing."))?;
            check_record(&row, expected)?;
            live_clock(&row, candidate, now)?;
            if row.state != ResetState::Claimed
                || row.routing_generation != candidate.binding.routing_generation
            {
                return Err(unavailable(
                    "stale_owner",
                    "Reset claim is no longer submit-capable.",
                ));
            }
            let day = utc_day(now)?;
            let day_key = keys::reset_day(&row.identity.account_id_hash, &day);
            let mut budget: AccountDayBudget =
                tx.read(&day_key).await?.unwrap_or(AccountDayBudget {
                    v: 1,
                    day: day.clone(),
                    submission_count: 0,
                    updated_at_ms: now,
                });
            if budget.v != 1 || budget.day != day {
                return Err(invalid("Reset account/day record is malformed."));
            }
            nonnegative(budget.submission_count)?;
            if budget.submission_count >= 1 {
                return Err(unavailable(
                    "account_day_limit_reached",
                    "This account already submitted a reset today.",
                ));
            }
            live_clock(&row, candidate, self.fresh_time(now)?)?;
            budget.submission_count = add(budget.submission_count, 1)?;
            budget.updated_at_ms = now;
            row.state = ResetState::Submitted;
            row.submitted_at_ms = Some(now);
            row.updated_at_ms = now;
            row.lease_expires_at_ms = add(now, PROBE_LEASE_MS)?;
            tx.put(&key, &row, None)?;
            tx.put(&day_key, &budget, None)?;
            self.check_reset_authorization_deadline(authorization, now)?;
            if tx.commit().await? {
                return Ok(row);
            }
        }
        Err(unavailable(
            "stale_owner",
            "Reset submission changed concurrently.",
        ))
    }

    async fn renew_reset_before_redeem_guarded(
        &self,
        authorization: ResetAuthorization<'_>,
        expected: &ResetRecord,
        now: i64,
    ) -> Result<ResetRecord, GatewayError> {
        let candidate = authorization.candidate();
        for _ in 0..4 {
            let mut tx = self.transaction(now)?;
            let invocation = authorization_fences(&mut tx, authorization).await?;
            let config: ResetConfig = tx.read(&keys::reset_config()).await?.unwrap_or_default();
            if let Some(reason) = config.live_reason() {
                return Err(unavailable(reason, "Reset live policy changed."));
            }
            let key = keys::reset_redemption(
                &invocation.identity.account_id_hash,
                &invocation.identity.quota_generation,
            );
            let mut row: ResetRecord = tx
                .read(&key)
                .await?
                .ok_or_else(|| unavailable("stale_owner", "Reset submission is missing."))?;
            check_record(&row, expected)?;
            live_clock(&row, candidate, now)?;
            if row.state != ResetState::Submitted {
                return Err(unavailable("stale_owner", "Reset is not submitted."));
            }
            live_clock(&row, candidate, self.fresh_time(now)?)?;
            row.lease_expires_at_ms = add(now, PROBE_LEASE_MS)?;
            row.updated_at_ms = now;
            tx.put(&key, &row, None)?;
            self.check_reset_authorization_deadline(authorization, now)?;
            if tx.commit().await? {
                return Ok(row);
            }
        }
        Err(unavailable(
            "stale_owner",
            "Reset renewal changed concurrently.",
        ))
    }
}
pub trait ResetSpendFactory {
    type Store: AuthorityStore;
    type Tokens: TokenSource;
    fn reset_spend(&self) -> ResetSpendService<'_, Self::Store, Self::Tokens>;
}
pub trait ResetSpendOperations: ResetSpendFactory {
    /// Durable claimed->submitted boundary consumes one account/day slot atomically.
    fn prepare_reset_submission(
        &self,
        candidate: &ResetCandidate,
        expected: &ResetRecord,
        now: i64,
    ) -> impl std::future::Future<Output = Result<ResetRecord, GatewayError>> {
        async move {
            let context = self.reset_spend();
            context
                .prepare_reset_submission_guarded(
                    ResetAuthorization::Legacy(candidate),
                    expected,
                    now,
                )
                .await
        }
    }
    /// Complete source audit permission, validated inside the existing shared spend transaction.
    fn prepare_full_pool_reset_submission(
        &self,
        permit: &gateway_authority_reset_permit::types::AutomaticResetPermit,
        expected: &ResetRecord,
    ) -> impl std::future::Future<Output = Result<ResetRecord, GatewayError>> {
        async move {
            let context = self.reset_spend();
            let now = context.fresh_time(0)?;
            context
                .prepare_reset_submission_guarded(
                    ResetAuthorization::FullPool(permit),
                    expected,
                    now,
                )
                .await
        }
    }
    /// Native owner calls immediately before redeem, then rechecks its local kill switch/clock.
    /// The returned lease is not a provider success receipt or permission to retry a lost consume.
    fn renew_reset_before_redeem(
        &self,
        candidate: &ResetCandidate,
        expected: &ResetRecord,
        now: i64,
    ) -> impl std::future::Future<Output = Result<ResetRecord, GatewayError>> {
        async move {
            let context = self.reset_spend();
            context
                .renew_reset_before_redeem_guarded(
                    ResetAuthorization::Legacy(candidate),
                    expected,
                    now,
                )
                .await
        }
    }
    /// Complete source audit permission, validated inside the existing shared spend transaction.
    fn renew_full_pool_reset_before_redeem(
        &self,
        permit: &gateway_authority_reset_permit::types::AutomaticResetPermit,
        expected: &ResetRecord,
    ) -> impl std::future::Future<Output = Result<ResetRecord, GatewayError>> {
        async move {
            let context = self.reset_spend();
            let now = context.fresh_time(0)?;
            context
                .renew_reset_before_redeem_guarded(
                    ResetAuthorization::FullPool(permit),
                    expected,
                    now,
                )
                .await
        }
    }
    /// Only the authenticated approved native provider boundary may supply observed outcomes.
    fn record_reset_outcome(
        &self,
        expected: &ResetRecord,
        state: ResetState,
        receipt: Option<&str>,
        error: Option<&str>,
        now: i64,
    ) -> impl std::future::Future<Output = Result<ResetRecord, GatewayError>> {
        async move {
            let context = self.reset_spend();
            if !matches!(
                state,
                ResetState::Unknown | ResetState::Verified | ResetState::Rejected
            ) {
                return Err(invalid("Invalid reset provider outcome."));
            }
            if let Some(receipt) = receipt {
                text(receipt)?;
                if receipt.len() > 512 {
                    return Err(invalid("Reset receipt is too large."));
                }
            }
            for _ in 0..4 {
                let mut tx = context.transaction(now)?;
                let key = keys::reset_redemption(
                    &expected.identity.account_id_hash,
                    &expected.identity.quota_generation,
                );
                let mut row: ResetRecord = tx
                    .read(&key)
                    .await?
                    .ok_or_else(|| unavailable("stale_owner", "Reset record is missing."))?;
                check_record(&row, expected)?;
                if matches!(row.state, ResetState::Verified | ResetState::Rejected) {
                    if row.state == state {
                        return Ok(row);
                    }
                    return Err(invalid("Reset provider terminal is already immutable."));
                }
                if !matches!(row.state, ResetState::Submitted | ResetState::Unknown) {
                    return Err(unavailable(
                        "stale_owner",
                        "Reset has not crossed submission.",
                    ));
                }
                if row.lease_expires_at_ms <= now {
                    return Err(unavailable("stale_owner", "Reset owner lease expired."));
                }
                row.state = state;
                row.updated_at_ms = now;
                if state == ResetState::Verified {
                    row.verified_at_ms = Some(now);
                }
                if row.provider_receipt_id.is_none() {
                    row.provider_receipt_id = receipt.map(str::to_owned);
                }
                row.last_error_code = error.map(|error| error.chars().take(120).collect());
                tx.put(&key, &row, None)?;
                if tx.commit().await? {
                    return Ok(row);
                }
            }
            Err(unavailable(
                "stale_owner",
                "Reset outcome changed concurrently.",
            ))
        }
    }
}
impl<T: ResetSpendFactory> ResetSpendOperations for T {}
