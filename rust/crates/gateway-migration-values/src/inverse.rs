//! Pure latest-source value/key transformations; no store or commit capability.
use gateway_core::{
    error::GatewayError,
    source_json::SourceJson,
    state::{RecordKey, StoredRecord},
    tuple_key::{DecodedKeyPart, TupleKey},
    value::PortableValue,
};
use gateway_migration_data::key_source::decimal_key;
use gateway_migration_data::{
    Transform, capture::SourceEntry, decode, invalid, parts, starts, unavailable,
};
use gateway_profile_source::codec::to_source_profile as to_profile;
use gateway_profile_values::coercion::encode_base64;
use serde_json::{Value, json};
pub fn source_profile_key(key: &RecordKey) -> Result<Vec<SourceJson>, GatewayError> {
    TupleKey::from_record_key(key)?
        .decoded_parts()?
        .into_iter()
        .map(|part| match part {
            DecodedKeyPart::String(bytes) => {
                let text = String::from_utf8(bytes).map_err(|_| {
                    unavailable("New non-UTF8 source key needs exact typed encoding")
                })?;
                to_profile(&PortableValue::String(text))
            }
            DecodedKeyPart::Number(number) => to_profile(&PortableValue::number(number)),
            DecodedKeyPart::Boolean(value) => to_profile(&PortableValue::Boolean(value)),
            DecodedKeyPart::Bytes(bytes) => SourceJson::from_serializable(
                &json!({"type":"Uint8Array","value":encode_base64(&bytes)}),
            ),
            DecodedKeyPart::BigInt {
                negative,
                magnitude,
            } => to_profile(&PortableValue::BigInt(decimal_key(negative, &magnitude))),
        })
        .collect()
}

fn overlay(original: &Value, current: &Value) -> Result<Value, GatewayError> {
    let mut out = original.as_object().cloned().unwrap_or_default();
    for (k, v) in current
        .as_object()
        .ok_or_else(|| invalid("Transformed runtime record is not an object"))?
    {
        out.insert(k.clone(), v.clone());
    }
    Ok(Value::Object(out))
}
fn remove(value: &mut Value, fields: &[&str]) {
    if let Some(object) = value.as_object_mut() {
        for field in fields {
            object.remove(*field);
        }
    }
}
pub fn inverse_value_for_key(
    transform: Transform,
    key: &RecordKey,
    record: &StoredRecord,
    source: Option<&SourceEntry>,
) -> Result<PortableValue, GatewayError> {
    if transform == Transform::PaidRequest {
        return gateway_migration_projection::paid_controls::inverse(key, record, source);
    }
    if starts(
        &parts(key)?,
        &["uos_ai", "paid_fallback", "v3", "usage_rollup"],
    ) {
        gateway_migration_projection::rollup_source::validate(key, record)?;
    }
    inverse_value(transform, record, source)
}

pub fn inverse_value(
    transform: Transform,
    record: &StoredRecord,
    source: Option<&SourceEntry>,
) -> Result<PortableValue, GatewayError> {
    if transform == Transform::PaidRequest {
        return Err(invalid("Paid inverse requires its actual source key"));
    }
    if transform == Transform::Identity {
        return record.portable_value();
    }
    if transform == Transform::CatalogBytes {
        let bytes: Vec<u8> = decode(record.json_value()?)?;
        let len = bytes.len() as u64;
        return Ok(PortableValue::Binary {
            kind: "Uint8Array".into(),
            bytes,
            byte_offset: 0,
            byte_length: len,
        });
    }
    let current = record.json_value()?;
    let original = source
        .map(|s| gateway_profile_source::codec::from_source_profile(&s.value))
        .transpose()?
        .and_then(|v| v.to_json_lossless())
        .unwrap_or(json!({}));
    let mut out = overlay(&original, current)?;
    match transform {
        Transform::ApiWindow | Transform::KernelWindow => remove(&mut out, &["reservations"]),
        Transform::ApiRequest => remove(&mut out, &["transport_started"]),
        Transform::KernelRequest => {
            let state = current["state"].as_str().unwrap_or("reserved");
            out["state"] = json!(state);
            out["committed_at_ms"] = if state == "committed" {
                current["terminal_at_ms"].clone()
            } else {
                Value::Null
            };
            out["released_at_ms"] = if state == "released" {
                current["terminal_at_ms"].clone()
            } else {
                Value::Null
            };
            remove(&mut out, &["terminal_at_ms"]);
        }
        Transform::PaidRequest => {
            return Err(invalid("Paid inverse requires its actual source key"));
        }
        Transform::PaidPending => remove(&mut out, &["key_id", "attempt_id"]),
        Transform::PaidGate => remove(&mut out, &["pending"]),
        Transform::Identity | Transform::CatalogBytes => {}
    }
    if matches!(
        transform,
        Transform::KernelWindow | Transform::KernelRequest
    ) {
        out["scope"] = json!(if out.get("repo").is_some_and(Value::is_string) {
            "repo"
        } else {
            "org"
        });
    }
    if matches!(
        transform,
        Transform::KernelWindow | Transform::KernelRequest
    ) && out["repo"].is_null()
    {
        remove(&mut out, &["repo"]);
    }
    Ok(PortableValue::from_json(&out))
}
pub fn transform_for_new(parts: &[Value]) -> Transform {
    if starts(parts, &["uos_ai", "api_key_usage", "v3", "window"]) {
        Transform::ApiWindow
    } else if starts(parts, &["uos_ai", "api_key_usage", "v3", "request"]) {
        Transform::ApiRequest
    } else if starts(parts, &["uos_ai", "paid_fallback", "v3", "request"]) {
        Transform::PaidRequest
    } else if starts(parts, &["uos_ai", "paid_fallback", "v3", "pending"]) {
        Transform::PaidPending
    } else if starts(
        parts,
        &["uos_ai", "paid_fallback", "v3", "reconciliation_gate"],
    ) {
        Transform::PaidGate
    } else if starts(parts, &["uos_ai", "kernel_quota", "v2"])
        && parts
            .get(3)
            .and_then(Value::as_str)
            .is_some_and(|s| s.ends_with("_window"))
    {
        Transform::KernelWindow
    } else if starts(parts, &["uos_ai", "kernel_quota", "v2"])
        && parts
            .get(3)
            .and_then(Value::as_str)
            .is_some_and(|s| s.ends_with("_reservation"))
    {
        Transform::KernelRequest
    } else {
        Transform::Identity
    }
}
