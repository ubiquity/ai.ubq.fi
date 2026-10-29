use crate::coercion::{decode_base64, encode_base64, js_number, js_string, normalize_bigint};
use crate::date::{CanonicalDates, SourceDateHost, format_iso};
use crate::scalar::{error, json_number};

use gateway_source::error::GatewayError;
use gateway_kv_codec::value::PortableValue;
use serde_json::{Value, json};

pub fn binary_alignment(kind: &str) -> Option<usize> {
    match kind {
        "ArrayBuffer" | "DataView" | "Int8Array" | "Uint8Array" | "Uint8ClampedArray" => Some(1),
        "Int16Array" | "Uint16Array" => Some(2),
        "Int32Array" | "Uint32Array" | "Float32Array" => Some(4),
        "Float64Array" | "BigInt64Array" | "BigUint64Array" => Some(8),
        _ => None,
    }
}

pub fn regexp(raw: &str) -> Result<(String, String), GatewayError> {
    let split = raw.rfind('/');
    let flags = split.map(|p| &raw[p + 1..]).unwrap_or(raw);
    let source = if let Some(p) = split {
        raw.get(1..p).unwrap_or("").to_owned()
    } else {
        raw.chars()
            .skip(1)
            .take(raw.chars().count().saturating_sub(2))
            .collect()
    };
    let mut seen = std::collections::BTreeSet::new();
    for flag in flags.chars() {
        if !"dgimsuvy".contains(flag) || !seen.insert(flag) {
            return Err(error("Invalid RegExp flags."));
        }
    }
    if seen.contains(&'u') && seen.contains(&'v') {
        return Err(error("RegExp u and v flags are mutually exclusive."));
    }
    let syntax = regress::Flags {
        icase: seen.contains(&'i'),
        multiline: seen.contains(&'m'),
        dot_all: seen.contains(&'s'),
        unicode: seen.contains(&'u'),
        unicode_sets: seen.contains(&'v'),
        ..Default::default()
    };
    regress::Regex::with_flags(&source, syntax)
        .map_err(|_| error("Invalid ECMAScript RegExp pattern."))?;
    let mut canonical = String::new();
    let mut escaped = false;
    for c in source.chars() {
        match c {
            '/' if !escaped => canonical.push_str("\\/"),
            '\n' => canonical.push_str("\\n"),
            '\r' => canonical.push_str("\\r"),
            '\u{2028}' => canonical.push_str("\\u2028"),
            '\u{2029}' => canonical.push_str("\\u2029"),
            _ => canonical.push(c),
        }
        if c == '\\' {
            escaped = !escaped
        } else {
            escaped = false;
        }
    }
    if canonical.is_empty() {
        canonical = "(?:)".into();
    }
    let flags = "dgimsuvy".chars().filter(|f| seen.contains(f)).collect();
    Ok((canonical, flags))
}
pub fn same_primitive(a: &PortableValue, b: &PortableValue) -> bool {
    match (a, b) {
        (PortableValue::Undefined, PortableValue::Undefined)
        | (PortableValue::Null, PortableValue::Null) => true,
        (PortableValue::Boolean(a), PortableValue::Boolean(b)) => a == b,
        (PortableValue::String(a), PortableValue::String(b))
        | (PortableValue::BigInt(a), PortableValue::BigInt(b)) => a == b,
        (PortableValue::NumberBits(_), PortableValue::NumberBits(_)) => a
            .number_value()
            .zip(b.number_value())
            .is_some_and(|(a, b)| a == b || (a.is_nan() && b.is_nan())),
        _ => false,
    }
}
pub fn collection_key(value: PortableValue) -> PortableValue {
    if value.number_value() == Some(0.0) {
        PortableValue::number(0.0)
    } else {
        value
    }
}

pub fn from_profile(input: &Value) -> Result<PortableValue, GatewayError> {
    from_profile_with_host(input, &CanonicalDates)
}

pub fn from_profile_with_host(
    input: &Value,
    host: &dyn SourceDateHost,
) -> Result<PortableValue, GatewayError> {
    let kind = input
        .get("type")
        .and_then(Value::as_str)
        .ok_or_else(|| error("Malformed profile value."))?;
    let value = input.get("value");
    let raw = value.cloned().unwrap_or(Value::Null);
    let parsed = match kind {
        "undefined" => PortableValue::Undefined,
        "null" => PortableValue::Null,
        "boolean" | "string" => value
            .map(PortableValue::from_json)
            .unwrap_or(PortableValue::Undefined),
        "number" => PortableValue::number(value.map(js_number).unwrap_or(f64::NAN)),
        "bigint" | "KvU64" => {
            let text = value.map(js_string).unwrap_or_else(|| "undefined".into());
            let (_, decimal, _) = normalize_bigint(&text)?;
            if kind == "KvU64" {
                decimal
                    .parse::<u64>()
                    .map_err(|_| error("KvU64 is outside its unsigned 64-bit range."))?;
                PortableValue::KvU64(decimal)
            } else {
                PortableValue::BigInt(decimal)
            }
        }
        "Date" => PortableValue::Date {
            epoch_ms: host.date_value(value)?,
        },
        "RegExp" => {
            let (source, flags) =
                regexp(&value.map(js_string).unwrap_or_else(|| "undefined".into()))?;
            PortableValue::RegExp { source, flags }
        }
        "Array" => PortableValue::Array(
            raw.as_array()
                .ok_or_else(|| error("Malformed profile Array."))?
                .iter()
                .map(|item| from_profile_with_host(item, host))
                .collect::<Result<_, _>>()?,
        ),
        "object" => {
            let fields: Vec<(String, Value)> = match raw {
                Value::Object(v) => v.into_iter().collect(),
                Value::Array(v) => v
                    .into_iter()
                    .enumerate()
                    .map(|(i, v)| (i.to_string(), v))
                    .collect(),
                Value::Null => return Err(error("Malformed profile object.")),
                Value::String(s) => s
                    .chars()
                    .enumerate()
                    .map(|(i, c)| (i.to_string(), json!(c.to_string())))
                    .collect(),
                _ => vec![],
            };
            PortableValue::Object(
                fields
                    .into_iter()
                    .map(|(k, v)| Ok((k, from_profile_with_host(&v, host)?)))
                    .collect::<Result<_, GatewayError>>()?,
            )
        }
        "Map" => {
            let mut entries: Vec<(PortableValue, PortableValue)> = vec![];
            for pair in raw
                .as_array()
                .ok_or_else(|| error("Malformed profile Map."))?
            {
                let pair = pair
                    .as_array()
                    .filter(|v| v.len() >= 2)
                    .ok_or_else(|| error("Malformed profile Map entry."))?;
                let key = collection_key(from_profile_with_host(&pair[0], host)?);
                let item = from_profile_with_host(&pair[1], host)?;
                if let Some(existing) = entries.iter_mut().find(|(k, _)| same_primitive(k, &key)) {
                    existing.1 = item;
                } else {
                    entries.push((key, item));
                }
            }
            PortableValue::Map(entries)
        }
        "Set" => {
            let mut entries = vec![];
            for item in raw
                .as_array()
                .ok_or_else(|| error("Malformed profile Set."))?
            {
                let item = collection_key(from_profile_with_host(item, host)?);
                if !entries.iter().any(|v| same_primitive(v, &item)) {
                    entries.push(item);
                }
            }
            PortableValue::Set(entries)
        }
        "Error" | "EvalError" | "RangeError" | "ReferenceError" | "SyntaxError" | "TypeError"
        | "URIError" => {
            if raw.is_null() {
                return Err(error("Malformed profile Error detail."));
            }
            let message = raw.get("message").map(js_string).unwrap_or_default();
            let stack = raw
                .get("stack")
                .filter(|v| !v.is_null() && **v != false && **v != 0 && **v != "");
            let stack = stack
                .map(|v| {
                    v.as_str().map(str::to_owned).ok_or_else(|| {
                        error("A nonstring Error stack requires source normalization.")
                    })
                })
                .transpose()?;
            let cause = raw
                .get("cause")
                .filter(|v| !v.is_null() && **v != false && **v != 0 && **v != "")
                .map(|item| from_profile_with_host(item, host))
                .transpose()?
                .map(Box::new);
            PortableValue::Error {
                kind: kind.into(),
                message,
                stack,
                cause,
            }
        }
        kind if binary_alignment(kind).is_some() => {
            let bytes = decode_base64(&value.map(js_string).unwrap_or_else(|| "undefined".into()));
            let alignment = binary_alignment(kind).unwrap_or(1);
            if !bytes.len().is_multiple_of(alignment) {
                return Err(error("Profile typed-array backing buffer is misaligned."));
            }
            let length = bytes.len() as u64;
            PortableValue::Binary {
                kind: kind.into(),
                bytes,
                byte_offset: 0,
                byte_length: length,
            }
        }
        _ => return Err(error(format!("Unsupported profile value type: {kind}"))),
    };
    parsed.validate()?;
    Ok(parsed)
}

pub fn to_profile(value: &PortableValue) -> Result<Value, GatewayError> {
    value.validate()?;
    let (kind, data) = match value {
        PortableValue::Undefined => return Ok(json!({"type":"undefined"})),
        PortableValue::Null => ("null", Value::Null),
        PortableValue::Boolean(v) => ("boolean", json!(v)),
        PortableValue::String(v) => ("string", json!(v)),
        PortableValue::NumberBits(_) => {
            let n = value
                .number_value()
                .ok_or_else(|| error("Invalid number bits."))?;
            (
                "number",
                if n.is_finite() {
                    json_number(Some(n))
                } else {
                    json!(if n.is_nan() {
                        "NaN"
                    } else if n.is_sign_negative() {
                        "-Infinity"
                    } else {
                        "Infinity"
                    })
                },
            )
        }
        PortableValue::BigInt(v) => ("bigint", json!(v)),
        PortableValue::KvU64(v) => ("KvU64", json!(v)),
        PortableValue::Binary {
            kind,
            bytes,
            byte_length,
            ..
        } => return Ok(json!({"type":kind,"value":encode_base64(bytes),"byteLength":byte_length})),
        PortableValue::Date { epoch_ms } => ("Date", json!(epoch_ms.map(format_iso))),
        PortableValue::RegExp { source, flags } => ("RegExp", json!(format!("/{source}/{flags}"))),
        PortableValue::Array(v) => (
            "Array",
            Value::Array(v.iter().map(to_profile).collect::<Result<_, _>>()?),
        ),
        PortableValue::Set(v) => (
            "Set",
            Value::Array(v.iter().map(to_profile).collect::<Result<_, _>>()?),
        ),
        PortableValue::Object(v) => (
            "object",
            Value::Object(
                v.iter()
                    .map(|(k, v)| Ok((k.clone(), to_profile(v)?)))
                    .collect::<Result<_, GatewayError>>()?,
            ),
        ),
        PortableValue::Map(v) => (
            "Map",
            Value::Array(
                v.iter()
                    .map(|(k, v)| Ok(json!([to_profile(k)?, to_profile(v)?])))
                    .collect::<Result<_, GatewayError>>()?,
            ),
        ),
        PortableValue::StringUtf16(_)
        | PortableValue::ObjectUtf16(_)
        | PortableValue::RegExpUtf16 { .. }
        | PortableValue::ErrorUtf16 { .. } => {
            return Err(error(
                "ProfileV2 strings require the raw source-profile writer; Value cannot preserve them.",
            ));
        }
        PortableValue::Error {
            kind,
            message,
            stack,
            cause,
        } => {
            let mut detail = json!({"message":message});
            if let Some(stack) = stack {
                detail["stack"] = json!(stack);
            }
            if let Some(cause) = cause {
                detail["cause"] = to_profile(cause)?;
            }
            return Ok(json!({"type":kind,"value":detail}));
        }
    };
    Ok(json!({"type":kind,"value":data}))
}
