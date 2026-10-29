//! Private optional stale-reference cleanup; no public mutation interface.
use gateway_core::{
    state::AtomicTransition, state::AuthorityStore, state::CommitOutcome, state::RecordMutation,
};
use gateway_observability_contracts::batch::normalize_changes;
use gateway_source::error::GatewayError;
pub(crate) async fn commit<S: AuthorityStore>(
    store: &S,
    checks: Vec<gateway_core::state::RevisionCheck>,
    mutations: Vec<RecordMutation>,
) -> Result<bool, GatewayError> {
    let Some((checks, mutations)) = normalize_changes(checks, mutations) else {
        return Ok(false);
    };
    // Unknown committed outcomes remain errors; do not replay their mutation.
    Ok(store.commit(AtomicTransition { checks, mutations }).await? == CommitOutcome::Committed)
}
