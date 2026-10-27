use crate::store::Transaction;
use gateway_authority_contracts::{TokenSource, invalid};
use gateway_authority_reset_evidence::types::ResetCandidate;
use gateway_authority_reset_permit::types::AutomaticResetPermit;
use gateway_authority_routing_selection::types::CohortProof;
use gateway_core::{error::GatewayError, identity::RequestContext, state::AuthorityStore};
pub(crate) async fn capture_reconciliation_candidates<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    proof: &CohortProof,
    original: &RequestContext,
) -> Result<Vec<ResetCandidate>, GatewayError> {
    let now = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        gateway_authority_reset_evidence::capture_existing_facts(&reader, proof, original, now)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(evidence
        .facts()
        .members()
        .iter()
        .filter(|member| member.enabled)
        .map(|member| member.candidate.clone())
        .collect())
}
pub(crate) async fn validate_automatic_permit<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    permit: &AutomaticResetPermit,
    now: i64,
) -> Result<(), GatewayError> {
    tx.now = now;
    let evidence = {
        let reader = tx.routing_read_store();
        permit.current_facts(&reader, now).await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(())
}
