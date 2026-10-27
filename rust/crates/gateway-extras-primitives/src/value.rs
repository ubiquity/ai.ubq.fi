use gateway_auth_primitives::crypto::{sha256_hex, uuid_from_random};
use gateway_core::{
    state::{RecordKey, StoredRecord},
    tuple_key::TupleKey,
};
use gateway_extras_contract::errors::invalid_state;
use gateway_extras_contract::types::ExtrasHost;
use gateway_source::error::GatewayError;
use serde::de::DeserializeOwned;
use serde_json::Value;

pub fn key(parts: Value) -> RecordKey {
    TupleKey::from_json_parts(parts.as_array().expect("source tuple array"))
        .expect("source scalar tuple")
        .to_record_key()
}

pub fn now<H: ExtrasHost>(host: &H) -> Result<i64, GatewayError> {
    let n = host.now_ms();
    if (0..=gateway_core::identity::MAX_SAFE_INTEGER).contains(&n) {
        Ok(n)
    } else {
        Err(invalid_state())
    }
}
pub fn uuid<H: ExtrasHost>(host: &H) -> Result<String, GatewayError> {
    uuid_from_random(&host.random_bytes(16)?)
}
pub fn hash(text: &str) -> String {
    sha256_hex(text.as_bytes())
}
pub fn decoded<T: DeserializeOwned>(
    row: Option<&StoredRecord>,
    at: i64,
) -> Result<Option<T>, GatewayError> {
    let Some(row) = row else { return Ok(None) };
    row.validate_version()?;
    if row.expires_at_ms.is_some_and(|x| x <= at) {
        return Ok(None);
    }
    serde_json::from_value(row.json_value()?.clone())
        .map(Some)
        .map_err(|_| invalid_state())
}
pub fn retry_seconds(ms: u64) -> u64 {
    ms.div_ceil(1000).max(1)
}

pub fn js_whitespace(c: char) -> bool {
    matches!(c,'\u{0009}'..='\u{000d}'|' '|'\u{00a0}'|'\u{1680}'|'\u{2000}'..='\u{200a}'|'\u{2028}'|'\u{2029}'|'\u{202f}'|'\u{205f}'|'\u{3000}'|'\u{feff}')
}

/// Existing source TextEncoder boundary for hashes/encrypted embedding input, not a payload projection.
pub fn source_utf8(text: &gateway_source::js_string::JsString16) -> String {
    String::from_utf16_lossy(text.as_units())
}
pub fn hash_source(text: &gateway_source::js_string::JsString16) -> String {
    hash(&source_utf8(text))
}
