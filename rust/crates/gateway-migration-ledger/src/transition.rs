use crate::rows::ObservedRow;
use gateway_core::{
    error::GatewayError,
    state::{RecordKey, RecordMutation, RevisionCheck, StoredRecord},
    tuple_key::TupleKey,
};
use serde_json::Value;

pub(crate) fn check(row: &ObservedRow) -> RevisionCheck {
    RevisionCheck {
        key: row.key.clone(),
        expected_revision: Some(row.record.revision.clone()),
    }
}
pub(crate) fn put(
    key: RecordKey,
    value: Value,
    expiry: Option<i64>,
    revision: &str,
) -> RecordMutation {
    RecordMutation::Put {
        key,
        record: StoredRecord::json(revision, value, expiry),
    }
}
pub(crate) fn key(parts: &[Value]) -> Result<RecordKey, GatewayError> {
    let key = TupleKey::from_json_parts(parts)?;
    key.validate_stored_key()?;
    Ok(key.to_record_key())
}
