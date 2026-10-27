//! Profile NDJSON conversion; never loads a VM-specific value codec or executes JavaScript.

use gateway_source::error::GatewayError;
use gateway_kv_codec::{
    key::RecordKey, tuple_key::BigIntKey, tuple_key::KeyPart, tuple_key::TupleKey,
    value::PortableValue,
};
use gateway_profile_values::{
    codec::from_profile_with_host,
    coercion::{encode_base64, normalize_bigint},
    date::{CanonicalDates, SourceDateHost},
    scalar::error,
};
use serde_json::{Value, json};

#[derive(Clone, Debug)]
pub struct NormalizedEntry {
    pub key: RecordKey,
    pub parts: Vec<Value>,
    pub value: PortableValue,
    pub raw: Value,
}
pub fn normalize_import_line(line: &str) -> Result<Option<NormalizedEntry>, GatewayError> {
    normalize_import_line_with_host(line, &CanonicalDates)
}

pub fn normalize_import_line_with_host(
    line: &str,
    host: &dyn SourceDateHost,
) -> Result<Option<NormalizedEntry>, GatewayError> {
    if line.trim().is_empty() {
        return Ok(None);
    }
    let raw: Value =
        serde_json::from_str(line).map_err(|_| error("Malformed profile JSON line."))?;
    let input = raw
        .get("key")
        .and_then(Value::as_array)
        .ok_or_else(|| error("Malformed profile key."))?;
    let mut parts = vec![];
    let mut logical = vec![];
    for part in input {
        if !matches!(
            part["type"].as_str(),
            Some("bigint" | "boolean" | "number" | "string" | "Uint8Array")
        ) {
            return Err(error("Unsupported profile key type."));
        }
        let decoded = from_profile_with_host(part, host)?;
        let key = match decoded {
            PortableValue::String(v) => {
                logical.push(json!(v));
                KeyPart::String(v)
            }
            PortableValue::Boolean(v) => {
                logical.push(json!(v));
                KeyPart::Boolean(v)
            }
            PortableValue::NumberBits(_) => {
                let v = decoded
                    .number_value()
                    .ok_or_else(|| error("Invalid key number."))?;
                logical.push(if v.is_finite(){json!(v)}else{json!({"type":"number","value":if v.is_nan(){"NaN"}else if v.is_sign_negative(){"-Infinity"}else{"Infinity"}})});
                KeyPart::Number(v)
            }
            PortableValue::BigInt(v) => {
                let (negative, _, magnitude) = normalize_bigint(&v)?;
                logical.push(json!({"type":"bigint","value":v}));
                KeyPart::BigInt(BigIntKey::from_signed_magnitude(negative, magnitude)?)
            }
            PortableValue::Binary { kind, bytes, .. } if kind == "Uint8Array" => {
                logical.push(json!({"type":"Uint8Array","value":encode_base64(&bytes)}));
                KeyPart::Bytes(bytes)
            }
            _ => return Err(error("Unsupported decoded profile key type.")),
        };
        parts.push(key);
    }
    let tuple = TupleKey::from_parts(&parts)?;
    tuple.validate_stored_key()?;
    let value = from_profile_with_host(
        raw.get("value")
            .ok_or_else(|| error("Missing profile value."))?,
        host,
    )?;
    Ok(Some(NormalizedEntry {
        key: tuple.to_record_key(),
        parts: logical,
        value,
        raw,
    }))
}
