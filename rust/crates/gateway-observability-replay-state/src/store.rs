//! Private effect glue; callers own the complete named operation and its keys/checks.
use gateway_core::{
    state::AtomicTransition, state::AuthorityStore, state::CommitOutcome, state::RecordKey,
    state::RecordMutation, state::StoredRecord,
};
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

pub(crate) fn replay_mutation<
    H: gateway_observability_contracts::types::ObservabilityHost,
    T: serde::Serialize,
>(
    host: &H,
    key: gateway_core::state::RecordKey,
    previous: Option<&gateway_core::state::StoredRecord>,
    value: &T,
    expiry: Option<i64>,
) -> Result<gateway_core::state::RecordMutation, gateway_source::error::GatewayError> {
    let document =
        gateway_observability_replay_records::accounting::merged_document(previous, value)?;
    crate::replay_values::put_json(host, key, &document, expiry)
}
pub(crate) fn ledger_mutation<H: gateway_observability_contracts::types::ObservabilityHost>(
    host: &H,
    previous: Option<&gateway_core::state::StoredRecord>,
    value: &gateway_observability_replay_records::accounting::LedgerData,
) -> Result<gateway_core::state::RecordMutation, gateway_source::error::GatewayError> {
    if !value.valid() {
        return Err(gateway_observability_contracts::data::error(
            "Sentinel replay budget counter overflow",
        ));
    }
    replay_mutation(
        host,
        gateway_observability_replay_model::replay_model::key("budget"),
        previous,
        value,
        None,
    )
}
