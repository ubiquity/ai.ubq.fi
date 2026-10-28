//! Captured and intermediate DATA with complete source/native record validation.
pub mod capture;
pub mod claude;
pub mod custody;
pub mod error;
pub mod key_source;
pub mod manifest;
pub mod native_capture;
pub mod record_key_wire;
pub mod staged;

use gateway_core::state::StoredRecord;
use gateway_kv_codec::key::RecordKey;
use gateway_kv_codec::tuple_key::TupleKey;
use gateway_profile_values::coercion::encode_base64;
use gateway_source::error::GatewayError;
use gateway_source::source_json::SourceJson;
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Transform {
    Identity,
    CatalogBytes,
    ApiWindow,
    ApiRequest,
    KernelWindow,
    KernelRequest,
    PaidRequest,
    PaidPending,
    PaidGate,
}
#[derive(Clone, Deserialize, Serialize)]
pub struct MappingData {
    #[serde(with = "source_storage")]
    pub source: capture::SourceEntry,
    // Only non-secret transformed source rows need an original value sidecar.
    pub transform: Transform,
    pub baseline_digest: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub native_origin: Option<native_capture::NativeOrigin>,
}
#[derive(Clone, Deserialize, Serialize)]
pub struct RowData {
    #[serde(with = "crate::record_key_wire")]
    pub key: RecordKey,
    pub parts: Vec<Value>,
    pub record: StoredRecord,
    pub mapping: MappingData,
}
pub fn invalid(message: impl Into<String>) -> GatewayError {
    GatewayError::new(409, message, Some("migration_state_invalid"))
}
pub fn unavailable(message: impl Into<String>) -> GatewayError {
    GatewayError::unavailable(message, "migration_schema_unavailable")
}
pub fn starts(parts: &[Value], prefix: &[&str]) -> bool {
    parts.len() >= prefix.len()
        && parts
            .iter()
            .zip(prefix)
            .all(|(a, b)| a.as_str() == Some(*b))
}
pub fn private_source_key(p: &[Value]) -> bool {
    starts(p, &["uos_ai", "migration"])
        || starts(p, &["uos_ai", "authority"])
        || starts(p, &["uos_ai", "auth", "session_revision_index"])
        || starts(p, &["uos_ai", "native_live"])
        || starts(p, &["uos_ai", "routing_runtime"])
        || starts(
            p,
            &["uos_ai", "codex_live_calls", "v1", "pending_retirement"],
        )
}
pub fn digest(record: &StoredRecord) -> Result<String, GatewayError> {
    Ok(gateway_auth_primitives::crypto::sha256_hex(
        &serde_json::to_vec(record).map_err(|_| invalid("Record cannot be encoded"))?,
    ))
}
pub fn json<T: Serialize>(value: &T) -> Result<Value, GatewayError> {
    serde_json::to_value(value).map_err(|_| invalid("State schema cannot be encoded"))
}
pub fn decode<T: serde::de::DeserializeOwned>(value: &Value) -> Result<T, GatewayError> {
    serde_json::from_value(value.clone())
        .map_err(|_| invalid("Source state does not match its explicit schema"))
}
pub fn parts(key: &RecordKey) -> Result<Vec<Value>, GatewayError> {
    use gateway_kv_codec::tuple_key::DecodedKeyPart;
    TupleKey::from_record_key(key)?
        .decoded_parts()?
        .into_iter()
        .map(|part| match part {
            DecodedKeyPart::String(v) => Ok(match String::from_utf8(v) {
                Ok(value) => Value::String(value),
                Err(error) => serde_json::json!({"native_string_bytes":error.into_bytes()}),
            }),
            DecodedKeyPart::Number(v) if v.is_finite() => {
                let v = if v == 0.0 { 0.0 } else { v };
                gateway_kv_codec::value::PortableValue::number(v)
                    .to_json_lossless()
                    .ok_or_else(|| invalid("Finite source key number cannot be projected"))
            }
            DecodedKeyPart::Boolean(v) => Ok(Value::Bool(v)),
            DecodedKeyPart::Bytes(bytes)=>Ok(serde_json::json!({"type":"Uint8Array","value":encode_base64(&bytes)})),
            DecodedKeyPart::BigInt{negative,magnitude}=>Ok(serde_json::json!({"type":"bigint","value":crate::key_source::decimal_key(negative,&magnitude)})),
            DecodedKeyPart::Number(value)=>Ok(serde_json::json!({"type":"number","value":if value.is_nan(){"NaN"}else if value.is_sign_negative(){"-Infinity"}else{"Infinity"}})),
        })
        .collect()
}

// Private metadata stores source JSON as an explicit raw document string. It never
// treats a surrogate as scalar text or reinterprets a source object as codec tags.
mod source_storage {
    use super::*;
    pub fn serialize<S: serde::Serializer>(
        entry: &capture::SourceEntry,
        serializer: S,
    ) -> Result<S::Ok, S::Error> {
        let raw = serde_json::to_string(entry).map_err(serde::ser::Error::custom)?;
        serializer.serialize_str(&raw)
    }
    pub fn deserialize<'de, D: serde::Deserializer<'de>>(
        deserializer: D,
    ) -> Result<capture::SourceEntry, D::Error> {
        let raw = String::deserialize(deserializer)?;
        serde_json::from_str(&raw).map_err(serde::de::Error::custom)
    }
}

/// Checked row data from the complete source/native validator, never live authority.
/// Raw serialized RowData cannot create this carrier or a prepared adoption.
pub struct ValidatedRow(RowData);
impl ValidatedRow {
    pub fn as_data(&self) -> &RowData {
        &self.0
    }
    pub fn into_data(self) -> RowData {
        self.0
    }
}
pub fn quota_family(parts: &[Value]) -> bool {
    starts(parts, &["uos_ai", "claude_code_quota"])
}
pub const PRIVATE_WIRE_MAX_BYTES: usize = 32 * 1024 * 1024;
