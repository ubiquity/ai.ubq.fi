//! Original HTTP-byte and source request syntax; no model policy or authority.
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_schema_values::{
    request::{SourceField, SourceModelRequest},
    source_view::js_trim_source as trim,
};
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
pub const MAX_ACCEPTED_JSON_BODY_BYTES: usize = 32 * 1024 * 1024;

pub fn parse_http_body(bytes: &[u8]) -> Result<SourceJson, SourceRequestError> {
    if bytes.is_empty() || bytes.len() > MAX_ACCEPTED_JSON_BODY_BYTES {
        return Err(SourceRequestError::scalar("Invalid JSON body", None));
    }
    // Source TextDecoder replacement happens once at the HTTP byte boundary,
    // never on escaped JSON string leaves or a UTF16 string value.
    let text = String::from_utf8_lossy(bytes);
    let text = text.strip_prefix('\u{feff}').unwrap_or(&text);
    SourceJson::parse(text).map_err(|_| SourceRequestError::scalar("Invalid JSON body", None))
}

pub(crate) fn member(
    value: &SourceJson,
    name: &str,
) -> Result<Option<SourceJson>, SourceRequestError> {
    Ok(value
        .object_entries()
        .map_err(|_| SourceRequestError::scalar("Invalid JSON body", None))?
        .into_iter()
        .rev()
        .find(|(key, _)| key.as_units().iter().copied().eq(name.encode_utf16()))
        .map(|(_, value)| value))
}

fn is_null(value: &SourceJson) -> bool {
    value.as_str() == "null"
}

fn string(value: &SourceJson) -> Option<JsString16> {
    value.deserialize::<JsString16>().ok()
}

pub fn model_request(value: Option<&SourceJson>) -> Result<SourceModelRequest, SourceRequestError> {
    let Some(value) = value.filter(|value| !is_null(value)) else {
        return Ok(SourceModelRequest::Default);
    };
    let Some(value) = string(value) else {
        return Err(SourceRequestError::scalar("model must be a string", None));
    };
    let value = trim(&value);
    Ok(if value.as_units().is_empty() {
        SourceModelRequest::Default
    } else {
        SourceModelRequest::Named(value)
    })
}

pub fn parse_stream(value: Option<&SourceJson>) -> Result<bool, SourceRequestError> {
    match value.map(SourceJson::as_str) {
        None | Some("false") => Ok(false),
        Some("true") => Ok(true),
        _ => Err(SourceRequestError::scalar(
            "stream must be a boolean",
            Some("stream"),
        )),
    }
}

pub fn parse_instructions(
    value: Option<&SourceJson>,
) -> Result<SourceField<JsString16>, SourceRequestError> {
    let Some(value) = value else {
        return Ok(SourceField::Missing);
    };
    if is_null(value) {
        return Ok(SourceField::Null);
    }
    string(value)
        .map(SourceField::Value)
        .ok_or_else(|| SourceRequestError::scalar("instructions must be a string", None))
}

pub fn resolve_model(
    value: Option<&SourceJson>,
    default: Option<&str>,
) -> Result<JsString16, SourceRequestError> {
    match model_request(value)? {
        SourceModelRequest::Named(value) => Ok(value),
        SourceModelRequest::Default => default.map(JsString16::from_utf8).ok_or_else(|| SourceRequestError::from_gateway(
            GatewayError::new(503, "Default model is unavailable: no configured default model or Codex model snapshot.", Some("server_error")))),
    }
}

pub(crate) fn scalar_value(
    value: Option<&serde_json::Value>,
) -> Result<Option<SourceJson>, GatewayError> {
    value.map(SourceJson::from_serializable).transpose()
}

pub fn parse_effort(
    value: Option<&SourceJson>,
    field: &str,
) -> Result<Option<JsString16>, SourceRequestError> {
    let Some(value) = value.filter(|v| v.as_str() != "null") else {
        return Ok(None);
    };
    let value = source::string(value).ok_or_else(|| {
        SourceRequestError::scalar(&format!("{field} must be a string"), Some(field))
    })?;
    let value = trim(&value);
    if value.as_units().is_empty() {
        return Err(SourceRequestError::scalar(
            &format!("{field} must be a non-empty string"),
            Some(field),
        ));
    }
    Ok(Some(value))
}

pub fn parse_reasoning(
    value: Option<&SourceJson>,
) -> Result<Option<SourceJson>, SourceRequestError> {
    let Some(value) = value.filter(|v| v.as_str() != "null") else {
        return Ok(None);
    };
    let mut entries = source::entries(value).ok_or_else(|| {
        SourceRequestError::scalar("reasoning must be an object", Some("reasoning"))
    })?;
    if let Some(index) = entries
        .iter()
        .position(|(key, _)| key.as_units().iter().copied().eq("effort".encode_utf16()))
    {
        match parse_effort(Some(&entries[index].1), "reasoning.effort").map_err(|error| {
            let message = error.body().message().clone();
            // Reuse the same exact message, but source attaches all reasoning syntax to its parent param.
            SourceRequestError::invalid(message, Some(JsString16::from_utf8("reasoning")))
        })? {
            Some(effort) => {
                entries[index].1 =
                    source::quoted(&effort).map_err(SourceRequestError::from_gateway)?
            }
            None => {
                entries.remove(index);
            }
        }
    }
    for field in ["summary", "generate_summary"] {
        if let Some(index) = entries
            .iter()
            .position(|(key, _)| key.as_units().iter().copied().eq(field.encode_utf16()))
        {
            if entries[index].1.as_str() == "null" {
                entries.remove(index);
            } else if source::string(&entries[index].1).is_none() {
                return Err(SourceRequestError::scalar(
                    &format!("reasoning.{field} must be a string"),
                    Some("reasoning"),
                ));
            }
        }
    }
    if entries.is_empty() {
        Ok(None)
    } else {
        source::object_keys(entries)
            .map(Some)
            .map_err(SourceRequestError::from_gateway)
    }
}
