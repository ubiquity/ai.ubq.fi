//! Private effect glue; callers own the complete named operation and its keys/checks.
use gateway_core::{
    state::AtomicTransition, state::AuthorityStore, state::CommitOutcome, state::RecordMutation,
    state::StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_observability_contracts::{batch::normalize_changes, data, types::ObservabilityHost};
use gateway_source::error::GatewayError;
use serde_json::Value;
pub(crate) fn put<H: ObservabilityHost>(
    host: &H,
    key: RecordKey,
    value: Value,
    expires_at_ms: Option<i64>,
) -> Result<RecordMutation, GatewayError> {
    Ok(RecordMutation::Put {
        key,
        record: StoredRecord::json(data::uuid(host)?, value, expires_at_ms),
    })
}
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
