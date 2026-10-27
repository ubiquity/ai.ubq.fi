//! Fixed preparation/refresh read bridges; actual campaign effects have one defining owner.
use crate::store::Transaction;
use gateway_authority_contracts::TokenSource;
use gateway_cache_experiment_evidence::CacheEvidenceReader;
use gateway_cache_experiment_records::model::{Evidence, State, TargetBinding};
use gateway_core::{error::GatewayError, state::AuthorityStore};
pub(super) async fn records<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    target: &TargetBinding,
) -> Result<(Option<State>, Option<Evidence>), GatewayError> {
    let observed_at_ms = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        CacheEvidenceReader::new(&reader, observed_at_ms)?
            .records(target)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(gateway_authority_contracts::invalid(
                "Cache evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok((evidence.state().cloned(), evidence.evidence().cloned()))
}
pub(super) async fn select_target<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    targets: Vec<TargetBinding>,
) -> Result<TargetBinding, GatewayError> {
    let observed_at_ms = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        CacheEvidenceReader::new(&reader, observed_at_ms)?
            .select_target(targets)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(gateway_authority_contracts::invalid(
                "Cache evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(evidence.target().clone())
}
