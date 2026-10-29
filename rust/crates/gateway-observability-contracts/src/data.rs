//! Ordinary source data helpers; no store, transaction or authority proof is exposed.
use crate::types::ObservabilityHost;
use gateway_auth_primitives::crypto::uuid_from_random;
use gateway_core::{state::RevisionCheck, state::StoredRecord};
use gateway_kv_codec::{key::RecordKey, tuple_key::DecodedKeyPart, tuple_key::TupleKey};
use gateway_source::error::GatewayError;
use serde_json::Value;
pub fn error(message: impl Into<String>) -> GatewayError {
    GatewayError::unavailable(message, "observability_state_invalid")
}
pub fn missing(name: &str) -> GatewayError {
    GatewayError::unavailable(
        format!("Required {name} capability is not configured"),
        "admin_capability_unavailable",
    )
}
pub fn invalid(message: impl Into<String>) -> GatewayError {
    GatewayError::new(400, message, Some("invalid_request_error"))
}
pub fn key(parts: Value) -> RecordKey {
    TupleKey::from_json_parts(parts.as_array().expect("source tuple array"))
        .expect("source scalar tuple")
        .to_record_key()
}
pub fn parts(key: &RecordKey) -> Result<Vec<Value>, GatewayError> {
    TupleKey::from_record_key(key)?
        .decoded_parts()?
        .into_iter()
        .map(|p| match p {
            DecodedKeyPart::String(v) => String::from_utf8(v)
                .map(Value::String)
                .map_err(|_| error("Invalid source key text")),
            DecodedKeyPart::Number(v) => serde_json::Number::from_f64(v)
                .map(Value::Number)
                .ok_or_else(|| error("Invalid source key number")),
            DecodedKeyPart::Boolean(v) => Ok(Value::Bool(v)),
            _ => Err(error("Unexpected source key kind")),
        })
        .collect()
}
pub fn check(key: &RecordKey, row: Option<&StoredRecord>) -> RevisionCheck {
    RevisionCheck {
        key: key.clone(),
        expected_revision: row.map(|r| r.revision.clone()),
    }
}
pub fn value(row: Option<&StoredRecord>, now: i64) -> Result<Option<&Value>, GatewayError> {
    row.filter(|r| !r.expires_at_ms.is_some_and(|x| x <= now))
        .map(StoredRecord::json_value)
        .transpose()
}
pub fn now<H: ObservabilityHost>(host: &H) -> Result<i64, GatewayError> {
    let now = host.now_ms();
    if (0..=gateway_contracts::identity::MAX_SAFE_INTEGER).contains(&now) {
        Ok(now)
    } else {
        Err(error("Invalid observation clock"))
    }
}
pub fn uuid<H: ObservabilityHost>(host: &H) -> Result<String, GatewayError> {
    uuid_from_random(&host.random_bytes(16)?)
}
