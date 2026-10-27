//! Complete checked paid operations; every existing effect retains one atomic transition.
use gateway_authority_paid_control_records::PaidRoutingPolicy;
mod backend;
use backend::Transaction;
use gateway_authority_api_protocol::ApiKeyPolicy;
use gateway_authority_contracts::{AuthorityClock, PAID_RETENTION_MS, TokenSource, add, invalid};
use gateway_authority_contracts::{nonnegative, positive, quota_error, retention, text};
use gateway_authority_contracts::{unavailable, validate_time};
use gateway_authority_keys as keys;
use gateway_authority_paid_records::validate_paid_window;
use gateway_authority_paid_records::{PaidAdmission, PaidAttempt, PaidRequest, arm_gate};
use gateway_authority_paid_records::{PaidReservation, PaidWindow, PendingPaid};
use gateway_authority_paid_records::{ReconciliationGate, pending_id, validate_attempt};
use gateway_authority_paid_route::PaidRoutingReceipt;
use gateway_authority_paid_route::{PaidRouteService, authorization::PaidAuthorization};
use gateway_authority_routing_selection::types::CohortProof;
use gateway_contracts::{identity::MicrocreditLimit, lifecycle::PaidTerminalState};
use gateway_contracts::{lifecycle::BillingState, lifecycle::DispatchState};
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
async fn read_gate<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
) -> Result<ReconciliationGate, GatewayError> {
    Ok(tx.read(&keys::paid_gate()).await?.unwrap_or_default())
}
pub(crate) async fn validate_cohort<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    proof: &CohortProof,
    quota_only: bool,
) -> Result<bool, GatewayError> {
    for check in proof.revision_fences() {
        if check.key == keys::models() {
            let _: Option<gateway_source::source_json::SourceJson> =
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
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(proof.matches_current_evidence(&evidence, quota_only))
}
struct PaidWindowDraft {
    limit: MicrocreditLimit,
    reset: i64,
    version: String,
    window_key: gateway_kv_codec::key::RecordKey,
    window: Option<PaidWindow>,
    exposure: i64,
}
pub struct PaidAdmissionService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
    collator: &'a dyn gateway_schema::aliases::SourceStringCollator,
}
impl<'a, S: AuthorityStore, I: TokenSource> PaidAdmissionService<'a, S, I> {
    pub fn new(
        store: &'a S,
        tokens: &'a I,
        clock: Option<&'a dyn AuthorityClock>,
        collator: &'a dyn gateway_schema::aliases::SourceStringCollator,
    ) -> Self {
        Self {
            store,
            tokens,
            clock,
            collator,
        }
    }
    fn transaction(&self, now: i64) -> Result<Transaction<'_, S, I>, GatewayError> {
        Transaction::new(self.store, self.tokens, now)
    }
    fn source_string_collator(&self) -> &dyn gateway_schema::aliases::SourceStringCollator {
        self.collator
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
    pub(crate) async fn require_claude_proof(
        &self,
        tx: &mut Transaction<'_, S, I>,
        proof: &gateway_authority_claude_proof::ClaudeCohortProof,
        model: &gateway_contracts::model::ModelId,
        now: i64,
    ) -> Result<(), GatewayError> {
        let fail = || {
            crate::unavailable(
                "claude_code_upstream_unreachable",
                "Claude full-pool proof is stale, expired, or not applicable.",
            )
        };
        gateway_authority_claude_proof::require_model(proof, model)?;
        tx.now = self.fresh_time(tx.now.max(now))?;
        let observed_now = tx.now;
        let _ = {
            let reader = tx.routing_read_store();
            gateway_authority_claude_proof::capture_evidence(&reader, model, observed_now).await?
        };
        for check in proof.revision_checks() {
            if !tx.expect(&check.key, &check.expected_revision) {
                return Err(fail());
            }
        }
        tx.now = self.fresh_time(tx.now)?;
        let observed_now = tx.now;
        let evidence = {
            let reader = tx.routing_read_store();
            gateway_authority_claude_proof::capture_evidence(&reader, model, observed_now).await?
        };
        gateway_authority_claude_proof::require_current(proof, &evidence)?;
        Ok(())
    }
    pub(crate) async fn require_paid_route(
        &self,
        tx: &mut Transaction<'_, S, I>,
        request: &PaidRequest,
        receipt: &PaidRoutingReceipt,
        now: i64,
    ) -> Result<PaidRoutingPolicy, GatewayError> {
        let started_at = tx.now.max(now);
        let evidence = {
            let reader = tx.routing_read_store();
            PaidRouteService::new(&reader, self.clock, self.source_string_collator())
                .current_data(request, receipt, started_at)
                .await?
        };
        for check in evidence.revision_checks() {
            if !tx.expect(&check.key, &check.expected_revision) {
                return Err(crate::unavailable(
                    "paid_control_unavailable",
                    "Paid routing receipt source state changed.",
                ));
            }
        }
        tx.now = evidence.observed_at_ms();
        Ok(evidence.policy().clone())
    }

    async fn read_paid_authorization(
        &self,
        tx: &mut Transaction<'_, S, I>,
        input: &PaidRequest,
        authorization: &PaidAuthorization,
        receipt: &PaidRoutingReceipt,
        now: i64,
    ) -> Result<ApiKeyPolicy, GatewayError> {
        let policy: ApiKeyPolicy = tx
            .read_source_control(&keys::api_policy(&input.key_id.0))
            .await?
            .ok_or_else(|| unavailable("paid_fallback_unavailable", "Paid policy is missing."))?;
        policy.validate(&input.key_id, now)?;
        if !policy.paid_fallback_enabled
            || !policy
                .paid_fallback_models
                .iter()
                .any(|model| model == &input.model)
        {
            return Err(unavailable(
                "paid_fallback_unavailable",
                "Paid fallback is disabled or this model is not allowed.",
            ));
        }
        let routing = self.require_paid_route(tx, input, receipt, now).await?;
        match authorization {
            PaidAuthorization::DirectPaidModel => {
                if routing.codex_enabled && routing.codex_models.contains(&input.model) {
                    return Err(unavailable(
                        "codex_routing_unavailable",
                        "A Codex model requires current full-pool proof before paid fallback.",
                    ));
                }
            }
            PaidAuthorization::SubscriptionUnavailable(proof) => {
                if !proof.full_cohort_unavailable()
                    || proof.account_bindings().is_empty()
                    || proof.model() != &input.model
                {
                    return Err(unavailable(
                        "codex_routing_unavailable",
                        "Full subscription-pool proof is missing.",
                    ));
                }
                if !validate_cohort(tx, proof, false).await? {
                    return Err(unavailable(
                        "codex_routing_unavailable",
                        "Subscription-pool proof is stale or expired.",
                    ));
                }
            }
            PaidAuthorization::ClaudeSubscriptionUnavailable(proof) => {
                if routing.codex_enabled && routing.codex_models.contains(&input.model) {
                    return Err(unavailable(
                        "codex_routing_unavailable",
                        "Claude evidence cannot replace current Codex full-pool proof.",
                    ));
                }
                self.require_claude_proof(tx, proof, &input.model, now)
                    .await?;
            }
        }
        let deletion: Option<serde_json::Value> =
            tx.read(&keys::paid_deletion(&input.key_id.0)).await?;
        if deletion.is_some() {
            return Err(unavailable(
                "paid_fallback_unavailable",
                "Paid history deletion is in progress.",
            ));
        }
        Ok(policy)
    }
    async fn prepare_paid_attempt(
        &self,
        tx: &mut Transaction<'_, S, I>,
        input: &PaidRequest,
        policy: &ApiKeyPolicy,
        receipt: &PaidRoutingReceipt,
        now: i64,
    ) -> Result<PaidAdmission, GatewayError> {
        let key = keys::paid_attempt(&input.key_id.0, &input.attempt_id.0);
        if let Some(existing) = tx.read_source_control::<PaidAttempt>(&key).await? {
            if !receipt.matches_existing(&existing.reservation) {
                return Err(invalid("Existing-only paid reservation changed."));
            }
            if existing.reservation.request_id != input.request_id
                || existing.reservation.provider != input.provider
                || existing.model != input.model
                || existing.route != input.route
                || matches!(
                    existing.billing_state,
                    BillingState::Settled | BillingState::NotBilled
                )
            {
                return Err(invalid("Paid attempt identity is consumed or conflicts."));
            }
            validate_attempt(&existing, &existing.reservation)?;
            return Ok(PaidAdmission::Existing(existing.reservation));
        }

        if receipt.requires_existing() {
            return Err(unavailable(
                "paid_control_unavailable",
                "Existing paid history is missing or expired; dispatch cannot create exposure.",
            ));
        }
        let draft = Self::prepare_paid_window(tx, input, policy, now).await?;
        self.stage_paid_attempt(tx, input, policy, &key, draft, now)
            .await
    }
    async fn prepare_paid_window(
        tx: &mut Transaction<'_, S, I>,
        input: &PaidRequest,
        policy: &ApiKeyPolicy,
        now: i64,
    ) -> Result<PaidWindowDraft, GatewayError> {
        let limit = MicrocreditLimit::try_from(policy.paid_fallback_limit_microcredits)
            .map_err(|_| invalid("Paid limit is invalid."))?;
        positive(policy.paid_fallback_quota_per_credit)?;
        let (_, reset) = policy.current_window(input.created_at_ms)?;
        let version = format!(
            "{}:{}",
            policy.window_ms, policy.paid_fallback_pricing_checked_at_ms
        );
        let window_key = keys::paid_window(&input.key_id.0, reset);
        let mut window: Option<PaidWindow> = tx.read(&window_key).await?;
        let exposure = match limit {
            MicrocreditLimit::Unlimited => 0,
            MicrocreditLimit::Bounded(_) => {
                let maximum = *policy
                    .paid_fallback_max_exposure_microcredits
                    .get(&input.model)
                    .ok_or_else(|| invalid("Paid model exposure is missing."))?;
                positive(maximum)?;
                let row = window.get_or_insert(PaidWindow {
                    v: 3,
                    key_id: input.key_id.clone(),
                    policy_version: version.clone(),
                    window_reset_at_ms: reset,
                    limit_microcredits: policy.paid_fallback_limit_microcredits,
                    settled_microcredits: 0,
                    reserved_microcredits: 0,
                    pending_count: 0,
                    updated_at_ms: now,
                });
                validate_paid_window(row, &input.key_id, reset)?;
                row.policy_version = version.clone();
                row.limit_microcredits = policy.paid_fallback_limit_microcredits;
                let used = add(row.settled_microcredits, row.reserved_microcredits)?;
                if used >= row.limit_microcredits {
                    return Err(quota_error(
                        "Paid fallback limit exceeded.",
                        "paid_fallback_limit_exceeded",
                    ));
                }
                maximum.min(row.limit_microcredits - used)
            }
        };
        Ok(PaidWindowDraft {
            limit,
            reset,
            version,
            window_key,
            window,
            exposure,
        })
    }
    async fn stage_paid_attempt(
        &self,
        tx: &mut Transaction<'_, S, I>,
        input: &PaidRequest,
        policy: &ApiKeyPolicy,
        key: &gateway_kv_codec::key::RecordKey,
        draft: PaidWindowDraft,
        now: i64,
    ) -> Result<PaidAdmission, GatewayError> {
        let PaidWindowDraft {
            limit,
            reset,
            version,
            window_key,
            window,
            exposure,
        } = draft;
        let reservation = PaidReservation {
            key_id: input.key_id.clone(),
            request_id: input.request_id.clone(),
            attempt_id: input.attempt_id.clone(),
            provider: input.provider,
            window_reset_at_ms: reset,
            created_at_ms: input.created_at_ms,
            reserved_microcredits: exposure,
        };
        let row = PaidAttempt {
            v: 3,
            reservation: reservation.clone(),
            policy_version: version,
            model: input.model.clone(),
            route: input.route.clone(),
            path: input.path.clone(),
            stream: input.stream,
            reasoning: input.reasoning.clone(),
            quota_per_credit: policy.paid_fallback_quota_per_credit,
            provider_request_id: None,
            dispatch_state: DispatchState::Dispatched,
            terminal_state: PaidTerminalState::Pending,
            billing_state: BillingState::Pending,
            provider_quota: None,
            input_tokens: None,
            cached_input_tokens: None,
            output_tokens: None,
            spend_microcredits: None,
            transport_started: false,
            reconciliation_attempts: 0,
            last_reconciliation_at_ms: None,
            dispatched_at_ms: Some(now),
            terminal_at_ms: None,
            settled_at_ms: None,
            updated_at_ms: now,
            usage_rollup_at_ms: None,
        };
        let pending = PendingPaid {
            key_id: input.key_id.clone(),
            attempt_id: input.attempt_id.clone(),
            created_at_ms: now,
            next_reconciliation_at_ms: now,
        };
        let pending_key = keys::paid_pending(&input.key_id.0, &input.attempt_id.0);
        let _: Option<PendingPaid> = tx.read(&pending_key).await?;
        let mut gate = read_gate(tx).await?;
        gate.pending.insert(
            pending_id(&input.key_id, &input.attempt_id),
            pending.clone(),
        );
        arm_gate(&mut gate);
        tx.put_source_control(
            key,
            &row,
            Some(retention(input.created_at_ms, PAID_RETENTION_MS)?),
        )?;
        tx.put(&pending_key, &pending, None)?;
        tx.put(&keys::paid_gate(), &gate, None)?;
        if let MicrocreditLimit::Bounded(_) = limit {
            let mut window = window.ok_or_else(|| invalid("Paid window is missing."))?;
            window.reserved_microcredits = add(window.reserved_microcredits, exposure)?;
            window.pending_count = add(window.pending_count, 1)?;
            window.updated_at_ms = now;
            tx.put(
                &window_key,
                &window,
                Some(retention(reset, PAID_RETENTION_MS)?),
            )?;
        }
        Ok(PaidAdmission::Reserved(reservation))
    }
    async fn recheck_paid_authorization(
        &self,
        tx: &mut Transaction<'_, S, I>,
        input: &PaidRequest,
        authorization: &PaidAuthorization,
        receipt: &PaidRoutingReceipt,
        now: i64,
    ) -> Result<(), GatewayError> {
        if let PaidAuthorization::ClaudeSubscriptionUnavailable(proof) = authorization {
            self.require_claude_proof(tx, proof, &input.model, now)
                .await?;
        }
        tx.now = self.fresh_time(tx.now.max(now))?;
        if let PaidAuthorization::SubscriptionUnavailable(proof) = authorization
            && !validate_cohort(tx, proof, false).await?
        {
            return Err(unavailable(
                "codex_routing_unavailable",
                "Subscription-pool proof expired before paid reservation.",
            ));
        }
        self.require_paid_route(tx, input, receipt, now).await?;
        Ok(())
    }
}
pub trait PaidAdmissionFactory {
    type Store: AuthorityStore;
    type Tokens: TokenSource;
    fn paid_admission(&self) -> PaidAdmissionService<'_, Self::Store, Self::Tokens>;
}
pub trait PaidAdmissionOperations: PaidAdmissionFactory {
    fn reserve_paid(
        &self,
        input: &PaidRequest,
        authorization: &PaidAuthorization,
        receipt: &PaidRoutingReceipt,
        now: i64,
    ) -> impl std::future::Future<Output = Result<PaidAdmission, GatewayError>> {
        async move {
            let context = self.paid_admission();
            if !receipt.has_original_request() {
                return Err(unavailable(
                    "paid_control_unavailable",
                    "Original request ownership is required before paid reservation.",
                ));
            }
            text(&input.key_id.0)?;
            text(&input.request_id.0)?;
            text(&input.attempt_id.0)?;
            if gateway_schema::reasoning::js_trim_source(input.model.as_js_string())
                .as_units()
                .is_empty()
            {
                return Err(invalid("Paid model is missing."));
            }
            text(&input.route)?;
            text(&input.path)?;
            nonnegative(input.created_at_ms)?;
            if input.created_at_ms > now {
                return Err(invalid("Paid request creation time is in the future."));
            }
            for _ in 0..128 {
                let mut tx = context.transaction(now)?;
                let policy = context
                    .read_paid_authorization(&mut tx, input, authorization, receipt, now)
                    .await?;
                let admission = context
                    .prepare_paid_attempt(&mut tx, input, &policy, receipt, now)
                    .await?;
                context
                    .recheck_paid_authorization(&mut tx, input, authorization, receipt, now)
                    .await?;
                if tx.commit().await? {
                    return Ok(admission);
                }
            }
            Err(unavailable(
                "paid_fallback_unavailable",
                "Paid admission changed concurrently.",
            ))
        }
    }
}
impl<T: PaidAdmissionFactory> PaidAdmissionOperations for T {}
