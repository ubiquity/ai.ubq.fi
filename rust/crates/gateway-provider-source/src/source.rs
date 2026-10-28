//! Lossless source composition and typed observations; Core alone owns JSON ordering.
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_schema_values::scalar as schema_scalar;
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as json,
};

pub type Result<T> = std::result::Result<T, SourceRequestError>;

/// Only actual scalar Values enter this lift; ordinary source payloads retain their carrier.
pub fn scalar_json(value: &serde_json::Value) -> std::result::Result<SourceJson, GatewayError> {
    SourceJson::from_serializable(value)
}
pub fn null() -> std::result::Result<SourceJson, GatewayError> {
    scalar_json(&serde_json::Value::Null)
}
pub fn text(value: &str) -> std::result::Result<SourceJson, GatewayError> {
    json::quoted(&JsString16::from_utf8(value))
}
pub fn quoted(value: &JsString16) -> std::result::Result<SourceJson, GatewayError> {
    json::quoted(value)
}
pub fn object(fields: Vec<(&str, SourceJson)>) -> std::result::Result<SourceJson, GatewayError> {
    json::object(fields)
}
pub fn object_keys(
    fields: Vec<(JsString16, SourceJson)>,
) -> std::result::Result<SourceJson, GatewayError> {
    json::object_keys(fields)
}
pub fn array(values: &[SourceJson]) -> std::result::Result<SourceJson, GatewayError> {
    json::json_array(values)
}
pub fn present(value: &SourceJson, name: &str) -> Option<SourceJson> {
    json::member(value, name)
}
pub fn present_path(value: &SourceJson, names: &[&str]) -> Option<SourceJson> {
    names
        .iter()
        .try_fold(value.clone(), |value, name| present(&value, name))
}
fn ordered_fields(
    value: &SourceJson,
) -> std::result::Result<Vec<(JsString16, SourceJson)>, GatewayError> {
    // Reject a non-object with the actual Core object parser error before ordered observation.
    value.object_entries()?;
    json::entries(value).ok_or_else(|| {
        GatewayError::unavailable(
            "Invalid source JSON serialization.",
            "source_json_serialization_invalid",
        )
    })
}
pub fn set(
    value: &mut SourceJson,
    name: &str,
    replacement: SourceJson,
) -> std::result::Result<(), GatewayError> {
    let mut fields = ordered_fields(value)?;
    let key = JsString16::from_utf8(name);
    if let Some((_, current)) = fields.iter_mut().find(|(field, _)| *field == key) {
        *current = replacement;
    } else {
        fields.push((key, replacement));
    }
    let result = object_keys(fields)?;
    *value = result;
    Ok(())
}
pub fn remove(value: &mut SourceJson, name: &str) -> std::result::Result<(), GatewayError> {
    let key = JsString16::from_utf8(name);
    let mut fields = ordered_fields(value)?;
    fields.retain(|(field, _)| *field != key);
    let result = object_keys(fields)?;
    *value = result;
    Ok(())
}
pub fn canonical(value: &SourceJson) -> std::result::Result<SourceJson, GatewayError> {
    json::canonical(value)
}

pub fn is_null(value: &SourceJson) -> bool {
    value.as_str() == "null"
}

pub fn is(value: &SourceJson, expected: &str) -> bool {
    json::string(value).is_some_and(|s| s.as_units().iter().copied().eq(expected.encode_utf16()))
}

pub fn boolean(value: &SourceJson) -> Option<bool> {
    match value.as_str() {
        "true" => Some(true),
        "false" => Some(false),
        _ => None,
    }
}

pub fn string(value: &SourceJson) -> Option<JsString16> {
    json::string(value)
}

/// Structural control only, never ordinary payload text or an opaque argument.
pub fn control(value: &SourceJson) -> Option<String> {
    string(value).and_then(|value| value.try_utf8().ok())
}

pub fn concat(parts: &[&JsString16]) -> JsString16 {
    JsString16::from_units(
        parts
            .iter()
            .flat_map(|part| part.as_units().iter().copied())
            .collect(),
    )
}

pub fn trim(value: &JsString16) -> JsString16 {
    let units = value.as_units();
    let white =
        |unit: &u16| char::from_u32(u32::from(*unit)).is_some_and(schema_scalar::js_whitespace);
    let start = units.iter().take_while(|unit| white(unit)).count();
    let end = units.len()
        - units[start..]
            .iter()
            .rev()
            .take_while(|unit| white(unit))
            .count();
    JsString16::from_units(units[start..end].to_vec())
}

pub fn nonempty(value: &SourceJson) -> Option<JsString16> {
    string(value).filter(|value| !trim(value).as_units().is_empty())
}

pub fn invalid(message: impl Into<JsString16>, param: &str) -> SourceRequestError {
    SourceRequestError::invalid(message, Some(JsString16::from_utf8(param)))
}

pub fn join(parts: &[JsString16], separator: &str) -> JsString16 {
    let mut units = Vec::new();
    for (index, part) in parts.iter().enumerate() {
        if index > 0 {
            units.extend(separator.encode_utf16());
        }
        units.extend_from_slice(part.as_units());
    }
    JsString16::from_units(units)
}

pub fn starts(value: &JsString16, prefix: &str) -> bool {
    value
        .as_units()
        .starts_with(&prefix.encode_utf16().collect::<Vec<_>>())
}

/// Only scalar API adapters call this after receiving an actual serde_json::Value.
pub fn scalar_error(error: SourceRequestError) -> GatewayError {
    error.into_scalar()
}
