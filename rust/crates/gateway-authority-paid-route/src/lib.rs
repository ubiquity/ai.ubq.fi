//! Checked paid receipt mint, original binding and current-source validation.
use gateway_core::identity::RequestContext;
pub mod authorization;
mod backend;
use backend::ReadTransaction;
use gateway_authority_contracts::{AuthorityClock, unavailable, validate_time};
use gateway_authority_keys as keys;
use gateway_authority_paid_control_records::{PaidRequestContext, PaidRoutingPolicy};
use gateway_authority_paid_records::{PaidAttempt, PaidRequest, PaidReservation};
use gateway_core::{state::AuthorityStore, state::RevisionCheck};
use gateway_schema::aliases::SourceStringCollator;
use gateway_source::error::GatewayError;
fn control_error(message: &str) -> GatewayError {
    unavailable("paid_control_unavailable", message)
}
/// Exact request capability. There is no public constructor or client deserializer.
/// ```compile_fail
/// use gateway_authority_paid_route::PaidRoutingReceipt;
/// let forged: PaidRoutingReceipt = serde_json::from_str("{}").unwrap();
/// ```
#[derive(Clone)]
pub struct PaidRoutingReceipt {
    request: PaidRequest,
    context: PaidRequestContext,
    checks: Vec<RevisionCheck>,
    original_request: Option<gateway_core::identity::RequestContext>,
    existing_paid: Option<gateway_authority_paid_records::PaidReservation>,
    owner_id: String,
    owner_epoch: String,
    configuration_generation: String,
    credential_binding: String,
}

impl PaidRoutingReceipt {
    pub fn owner_id(&self) -> &str {
        &self.owner_id
    }
    pub fn owner_epoch(&self) -> &str {
        &self.owner_epoch
    }
    pub fn configuration_generation(&self) -> &str {
        &self.configuration_generation
    }
    pub fn credential_binding(&self) -> &str {
        &self.credential_binding
    }
}

impl std::fmt::Debug for PaidRoutingReceipt {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PaidRoutingReceipt")
            .field("request_id", &self.request.request_id)
            .field("attempt_id", &self.request.attempt_id)
            .field("model", &self.request.model)
            .field("provider", &self.request.provider)
            .finish_non_exhaustive()
    }
}

impl PaidRoutingReceipt {
    pub fn has_original_request(&self) -> bool {
        self.original_request.is_some()
    }
    pub fn requires_existing(&self) -> bool {
        self.existing_paid.is_some()
    }
    pub fn matches_existing(&self, existing: &PaidReservation) -> bool {
        self.existing_paid
            .as_ref()
            .is_none_or(|expected| expected == existing)
    }
}
/// Read DATA, never an exposure or mutation grant.
pub struct CurrentPaidRoute {
    policy: PaidRoutingPolicy,
    checks: Vec<RevisionCheck>,
    now: i64,
}
impl CurrentPaidRoute {
    pub fn policy(&self) -> &PaidRoutingPolicy {
        &self.policy
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
    pub fn observed_at_ms(&self) -> i64 {
        self.now
    }
}

pub struct PaidRouteService<'a, S> {
    store: &'a S,
    clock: Option<&'a dyn AuthorityClock>,
    collator: &'a dyn SourceStringCollator,
}
impl<'a, S: AuthorityStore> PaidRouteService<'a, S> {
    pub fn new(
        store: &'a S,
        clock: Option<&'a dyn AuthorityClock>,
        collator: &'a dyn SourceStringCollator,
    ) -> Self {
        Self {
            store,
            clock,
            collator,
        }
    }
    fn transaction(&self, now: i64) -> Result<ReadTransaction<'_, S>, GatewayError> {
        ReadTransaction::new(self.store, now)
    }
    fn source_string_collator(&self) -> &dyn SourceStringCollator {
        self.collator
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
    pub async fn current_data(
        &self,
        request: &PaidRequest,
        receipt: &PaidRoutingReceipt,
        now: i64,
    ) -> Result<CurrentPaidRoute, GatewayError> {
        let mut tx = self.transaction(now)?;
        let policy = self
            .require_paid_route(&mut tx, request, receipt, now)
            .await?;
        Ok(CurrentPaidRoute {
            policy,
            checks: tx.checks(),
            now: tx.now,
        })
    }

    async fn require_paid_route(
        &self,
        tx: &mut ReadTransaction<'_, S>,
        request: &PaidRequest,
        receipt: &PaidRoutingReceipt,
        now: i64,
    ) -> Result<PaidRoutingPolicy, GatewayError> {
        if &receipt.request != request {
            return Err(control_error(
                "Paid routing receipt does not belong to this exact request/attempt.",
            ));
        }
        tx.now = self.fresh_time(tx.now.max(now))?;
        for check in &receipt.checks {
            tx.observe_revision(&check.key).await?;
            if !tx.expect(&check.key, &check.expected_revision) {
                return Err(control_error("Paid routing receipt source state changed."));
            }
        }
        let _ = route_current(tx, request, &receipt.context, self.source_string_collator()).await?;
        tx.now = self.fresh_time(tx.now)?;
        let route =
            route_current(tx, request, &receipt.context, self.source_string_collator()).await?;
        if route.owner_id() != receipt.owner_id
            || route.owner_epoch() != receipt.owner_epoch
            || route.configuration_generation() != receipt.configuration_generation
            || route.credential_binding() != receipt.credential_binding
        {
            return Err(control_error("Paid receipt credential custody changed."));
        }
        if let Some(original) = &receipt.original_request {
            tx.now = self.fresh_time(tx.now)?;
            require_original(tx, request, original, receipt.existing_paid.as_ref()).await?;
            tx.now = self.fresh_time(tx.now)?;
            require_original(tx, request, original, receipt.existing_paid.as_ref()).await?;
        }
        Ok(route.policy().clone())
    }
    async fn bind_original(
        &self,
        receipt: &PaidRoutingReceipt,
        original: &RequestContext,
        existing: Option<&PaidReservation>,
        now: i64,
    ) -> Result<PaidRoutingReceipt, GatewayError> {
        for _ in 0..128 {
            let mut tx = self.transaction(self.fresh_time(now)?)?;
            self.require_paid_route(&mut tx, &receipt.request, receipt, now)
                .await?;
            require_original(&mut tx, &receipt.request, original, existing).await?;
            if let Some(expected) = existing {
                let key = keys::paid_attempt(&expected.key_id.0, &expected.attempt_id.0);
                let row: PaidAttempt = tx
                    .read_source_control(&key)
                    .await?
                    .ok_or_else(|| control_error("Existing paid history is missing or expired."))?;
                if &row.reservation != expected {
                    return Err(control_error("Active paid reservation changed."));
                }
            }
            // Recheck time against this same cached snapshot, then CAS its exact revisions.
            tx.now = self.fresh_time(tx.now)?;
            require_original(&mut tx, &receipt.request, original, existing).await?;
            let mut bound = receipt.clone();
            bound.original_request = Some(original.clone());
            bound.existing_paid = existing.cloned();
            bound.checks = tx.checks();
            if tx.commit().await? {
                return Ok(bound);
            }
        }
        Err(control_error(
            "Original request ownership changed repeatedly.",
        ))
    }
}

async fn route_current<S: AuthorityStore>(
    tx: &mut ReadTransaction<'_, S>,
    request: &PaidRequest,
    context: &PaidRequestContext,
    collator: &dyn SourceStringCollator,
) -> Result<gateway_authority_paid_evidence::PaidRouteEvidence, GatewayError> {
    let now = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        gateway_authority_paid_evidence::capture_route(&reader, request, context, now, collator)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(control_error("Paid source provenance changed."));
        }
    }
    Ok(evidence)
}
async fn require_original<S: AuthorityStore>(
    tx: &mut ReadTransaction<'_, S>,
    request: &PaidRequest,
    original: &gateway_core::identity::RequestContext,
    existing: Option<&PaidReservation>,
) -> Result<(), GatewayError> {
    let now = tx.now;
    let checks = {
        let reader = tx.routing_read_store();
        gateway_authority_paid_evidence::capture_original(&reader, request, original, existing, now)
            .await?
    };
    for check in checks {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(control_error(
                "Original request ownership is missing, expired, finalizing or changed.",
            ));
        }
    }
    Ok(())
}
pub trait PaidRouteFactory {
    type Store: AuthorityStore;
    fn paid_route(&self) -> PaidRouteService<'_, Self::Store>;
}
pub trait PaidRouteOperations: PaidRouteFactory {
    fn authorize_paid_route(
        &self,
        request: &PaidRequest,
        context: &PaidRequestContext,
        now: i64,
    ) -> impl std::future::Future<Output = Result<PaidRoutingReceipt, GatewayError>> {
        async move {
            let service = self.paid_route();
            for _ in 0..128 {
                let mut tx = service.transaction(service.fresh_time(now)?)?;
                let _ = route_current(&mut tx, request, context, service.source_string_collator())
                    .await?;
                tx.now = service.fresh_time(tx.now)?;
                let route =
                    route_current(&mut tx, request, context, service.source_string_collator())
                        .await?;
                let receipt = PaidRoutingReceipt {
                    request: request.clone(),
                    context: context.clone(),
                    checks: tx.checks(),
                    original_request: None,
                    existing_paid: None,
                    owner_id: route.owner_id().to_owned(),
                    owner_epoch: route.owner_epoch().to_owned(),
                    configuration_generation: route.configuration_generation().to_owned(),
                    credential_binding: route.credential_binding().to_owned(),
                };
                if tx.commit().await? {
                    return Ok(receipt);
                }
            }
            Err(control_error(
                "Paid routing source state changed repeatedly.",
            ))
        }
    }
    /// Bind the exact authenticated original context to existing same-store ownership.
    /// Input data or a route-only receipt cannot authorize paid exposure by themselves.
    fn bind_paid_original_request(
        &self,
        receipt: &PaidRoutingReceipt,
        original: &RequestContext,
        now: i64,
    ) -> impl std::future::Future<Output = Result<PaidRoutingReceipt, GatewayError>> {
        async move {
            let context = self.paid_route();
            context.bind_original(receipt, original, None, now).await
        }
    }
    /// Recheck only the exact already-active paid reservation; this never grants fresh exposure.
    fn bind_paid_original_dispatch(
        &self,
        receipt: &PaidRoutingReceipt,
        original: &RequestContext,
        existing: &PaidReservation,
        now: i64,
    ) -> impl std::future::Future<Output = Result<PaidRoutingReceipt, GatewayError>> {
        async move {
            let context = self.paid_route();
            context
                .bind_original(receipt, original, Some(existing), now)
                .await
        }
    }
}
impl<T: PaidRouteFactory> PaidRouteOperations for T {}
