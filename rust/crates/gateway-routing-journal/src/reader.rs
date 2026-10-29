//! The complete expiry-aware stored journal read; never an admission or mutation grant.
use crate::{record::Journal, unavailable};
use gateway_core::{
    source_control::decode_source_control_record,
    state::{AuthorityStore, StoredRecord},
};
use gateway_kv_codec::{
    key::RecordKey,
    tuple_key::{ScalarKeyPart, TupleKey},
};
use gateway_source::error::GatewayError;

pub fn session_key(id: &str) -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("routing_runtime"),
        ScalarKeyPart::String("v1"),
        ScalarKeyPart::String("session"),
        ScalarKeyPart::String(id),
    ])
    .to_record_key()
}

pub async fn load<S: AuthorityStore>(
    store: &S,
    id: &str,
) -> Result<(StoredRecord, Journal), GatewayError> {
    let row = store
        .read(&session_key(id))
        .await?
        .ok_or_else(|| unavailable("Routing session is missing."))?;
    let value = Journal::decode(&decode_source_control_record(&row)?)?;
    if value.context.request_id.0 != id {
        return Err(unavailable("Routing session identity mismatch."));
    }
    Ok((row, value))
}
