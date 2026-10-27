use crate::backend::Transaction;
use gateway_authority_contracts::{TokenSource, invalid};
use gateway_authority_reset_evidence::types::Facts;
use gateway_authority_routing_selection::types::CohortProof;
use gateway_core::{error::GatewayError, identity::RequestContext, state::AuthorityStore};
pub(super) async fn capture<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    proof: &CohortProof,
    original: &RequestContext,
) -> Result<Facts, GatewayError> {
    let now = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        gateway_authority_reset_evidence::capture_spend_facts(&reader, proof, original, now).await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(evidence.facts().clone())
}
