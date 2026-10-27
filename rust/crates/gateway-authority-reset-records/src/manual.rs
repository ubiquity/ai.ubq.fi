//! Ordinary manual intent and response data only; actor/currentness and promotion remain checked operations.
use gateway_core::{identity::MAX_SAFE_INTEGER, state::RecordKey};
use gateway_core::{tuple_key::ScalarKeyPart, tuple_key::TupleKey};
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};
use serde_json::Value;
pub fn intent_key(hash: &str) -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("codex_manual_reset"),
        ScalarKeyPart::String("v1"),
        ScalarKeyPart::String(hash),
    ])
    .to_record_key()
}
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ManualIntent {
    pub v: u8,
    pub account_id_hash: String,
    pub credential_version: String,
    pub quota_reset_at_ms: i64,
    pub routing_generation: u64,
    pub created_at_ms: i64,
    pub promoted_at_ms: Option<i64>,
    pub quota_signal_at_ms: Option<i64>,
}
pub fn parse_manual_intent(value: &Value) -> Result<ManualIntent, GatewayError> {
    if value.as_object().is_none_or(|v| v.len() != 8) {
        return Err(error(409, "codex_reset_intent_invalid"));
    }
    let row: ManualIntent = serde_json::from_value(value.clone())
        .map_err(|_| error(409, "codex_reset_intent_invalid"))?;
    let valid_hash = |s: &str| {
        s.len() == 64
            && s.bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    };
    if row.v != 1
        || !valid_hash(&row.account_id_hash)
        || !valid_hash(&row.credential_version)
        || !(0..=MAX_SAFE_INTEGER).contains(&row.quota_reset_at_ms)
        || !(0..=MAX_SAFE_INTEGER).contains(&row.created_at_ms)
        || row.routing_generation > MAX_SAFE_INTEGER as u64
        || row
            .promoted_at_ms
            .is_some_and(|v| v < row.created_at_ms || v > MAX_SAFE_INTEGER)
        || row
            .quota_signal_at_ms
            .is_some_and(|v| !(0..=MAX_SAFE_INTEGER).contains(&v))
    {
        return Err(error(409, "codex_reset_intent_invalid"));
    }
    Ok(row)
}
#[derive(Debug, Deserialize, Serialize)]
pub struct ManualResetPromotion {
    pub reset_status: String,
    pub active_account_id_hash: String,
}

fn error(status: u16, code: &str) -> GatewayError {
    GatewayError::new(
        status,
        "Manual reset requires current account, policy and verified quota state.",
        Some(code),
    )
}
