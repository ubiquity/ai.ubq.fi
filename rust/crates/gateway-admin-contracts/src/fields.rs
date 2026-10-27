//! Source-compatible scalar and query field parsers; decoded values carry no authority.
use crate::source_input as source_body;
use gateway_auth::types::invalid;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use gateway_contracts::identity::MAX_SAFE_INTEGER;
use gateway_profile_values::coercion::js_number;
use serde_json::Value;
use std::collections::BTreeMap;

pub fn first<'a>(query: &'a BTreeMap<String, Vec<String>>, name: &str) -> Option<&'a str> {
    query
        .get(name)
        .and_then(|values| values.first())
        .map(String::as_str)
}
pub fn text(value: Option<&Value>) -> Option<String> {
    value
        .and_then(Value::as_str)
        .map(gateway_schema::util::js_trim)
        .filter(|s| !s.is_empty())
        .map(str::to_owned)
}
/// Source field view: opaque strings remain UTF16 data until a finite field consumes them.
pub fn source_text(value: Option<&SourceJson>) -> Option<JsString16> {
    value
        .and_then(source_body::text)
        .map(|text| source_body::trim(&text))
        .filter(|text| !text.as_units().is_empty())
}
pub fn source_integer(value: &SourceJson) -> Option<i64> {
    let n = if let Some(number) = source_body::number(value) {
        number
    } else {
        let text = source_text(Some(value))?;
        // An unpaired unit cannot be in a valid finite Number spelling.
        // Only this consumed numeric string is projected, never the body.
        let text = text.try_utf8().ok()?;
        js_number(&Value::String(text))
    };
    (n.is_finite() && n.abs() <= MAX_SAFE_INTEGER as f64).then_some(n.trunc() as i64)
}
pub fn source_limit(value: &SourceJson) -> Option<i64> {
    if source_text(Some(value)).is_some_and(|text| {
        gateway_source::js_string_case::lowercase_source(&text)
            .as_units()
            .iter()
            .copied()
            .eq("unlimited".encode_utf16())
    }) {
        return Some(-1);
    }
    source_integer(value).filter(|number| *number >= -1)
}
pub fn source_positive(value: &SourceJson) -> Option<i64> {
    source_integer(value).filter(|number| *number > 0)
}
/// Existing Number coercion for a consumed numeric field only. JSON strings or
/// array strings with unpaired units cannot form a finite numeric grammar;
/// they reach the original numeric validation error, not a body-wide refusal.
pub fn source_coerced_number(value: &SourceJson) -> f64 {
    source_body::number(value).unwrap_or_else(|| {
        value
            .deserialize::<Value>()
            .ok()
            .as_ref()
            .map(js_number)
            .unwrap_or(f64::NAN)
    })
}
/// Source Object.keys semantics for the already validated admin record/array.
pub fn source_entries(value: &SourceJson) -> Option<Vec<(JsString16, SourceJson)>> {
    gateway_source::source_serialization::entries(value).or_else(|| {
        value.array_items().ok().map(|items| {
            items
                .into_iter()
                .enumerate()
                .map(|(index, value)| (JsString16::from_utf8(&index.to_string()), value))
                .collect()
        })
    })
}
pub fn integer(value: &Value) -> Option<i64> {
    if !value.is_number() && !value.is_string() {
        return None;
    }
    let n = js_number(value);
    (n.is_finite() && n.abs() <= MAX_SAFE_INTEGER as f64).then_some(n.trunc() as i64)
}

pub fn limit(value: &Value) -> Option<i64> {
    if value.as_str().is_some_and(|s| {
        s.trim_matches(gateway_schema::util::js_whitespace)
            .eq_ignore_ascii_case("unlimited")
    }) {
        return Some(-1);
    }
    integer(value).filter(|v| *v >= -1)
}
pub fn positive(value: &Value) -> Option<i64> {
    integer(value).filter(|v| *v > 0)
}
pub fn truth(value: Option<&str>) -> bool {
    value.is_some_and(|s| {
        ["1", "true", "yes", "on"].contains(
            &s.trim_matches(gateway_schema::util::js_whitespace)
                .to_lowercase()
                .as_str(),
        )
    })
}
pub fn digits(value: &str) -> bool {
    !value.is_empty() && value.bytes().all(|b| b.is_ascii_digit())
}
pub fn bounded_positive(value: Option<&str>, default: u32, max: u32) -> Result<u32, GatewayError> {
    let Some(value) = value else {
        return Ok(default);
    };
    if !digits(value.trim_matches(gateway_schema::util::js_whitespace)) {
        return Err(invalid("limit must be a positive integer"));
    }
    let value = value
        .trim_matches(gateway_schema::util::js_whitespace)
        .parse::<u64>()
        .ok()
        .filter(|v| *v > 0 && *v <= MAX_SAFE_INTEGER as u64)
        .ok_or_else(|| invalid("limit must be a positive integer"))?;
    Ok(value.min(u64::from(max)) as u32)
}
pub fn uuid(value: &str) -> bool {
    value.len() == 36
        && value.bytes().enumerate().all(|(i, b)| {
            if [8, 13, 18, 23].contains(&i) {
                b == b'-'
            } else {
                b.is_ascii_hexdigit()
            }
        })
}

pub fn boolean(value: Option<&str>) -> Option<bool> {
    let s = value?
        .trim_matches(gateway_schema::util::js_whitespace)
        .to_lowercase();
    if ["1", "true", "yes", "on"].contains(&s.as_str()) {
        Some(true)
    } else if ["0", "false", "no", "off"].contains(&s.as_str()) {
        Some(false)
    } else {
        None
    }
}
pub fn cursor(value: Option<&str>, empty: bool) -> Result<Option<String>, GatewayError> {
    let Some(value) = value else { return Ok(None) };
    if value.is_empty() && empty {
        return Ok(None);
    }
    let unpadded = value.trim_end_matches('=');
    if value.len() > 2048
        || unpadded.is_empty()
        || value.len() - unpadded.len() > 2
        || !unpadded
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
    {
        return Err(invalid("Invalid capture cursor"));
    }
    Ok(Some(value.into()))
}
