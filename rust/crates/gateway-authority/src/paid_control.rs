use crate::{store::Authority, store::Transaction};
use gateway_authority_contracts::TokenSource;
use gateway_authority_paid_control_records::PaidRoutingPolicy;
use gateway_authority_paid_records::PaidRequest;
use gateway_authority_paid_route::{PaidRouteService, PaidRoutingReceipt};
use gateway_core::{error::GatewayError, state::AuthorityStore};
impl<S: AuthorityStore, I: TokenSource> Authority<S, I> {
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
            PaidRouteService::new(
                &reader,
                self.paid_route_clock(),
                self.source_string_collator(),
            )
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
}
