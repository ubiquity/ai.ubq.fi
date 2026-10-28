//! Ordinary-request data views; no provider dispatch or authority is implemented here.
use gateway_schema_values::{
    request::{SourceField, SourceModelRequest},
    source_view::js_trim_source as trim,
};
use gateway_source::error::GatewayError;
macro_rules! json {
    ({$($key:literal:$value:expr),*$(,)?})=>{gateway_schema_values::value::Value::object(vec![$((gateway_source::js_string::JsString16::from_utf8($key),gateway_schema_values::value::Value::from_serializable(&$value).expect("Source normalization fields serialize"))),*])};
    ($value:expr)=>{gateway_schema_values::value::Value::from_serializable(&$value).expect("Source normalization value serializes")};
}
pub mod cache_capability;
pub(crate) mod cache_metadata;
pub mod fields;
pub mod ordinary;
pub mod reasoning;
use self::{
    fields::validate_request_fields, ordinary::OrdinarySourceRequest, reasoning::parse_reasoning,
};
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_source::{js_string::JsString16, source_json::SourceJson};

pub const MAX_ACCEPTED_JSON_BODY_BYTES: usize = 32 * 1024 * 1024;

/// Private construction retains one immutable document; this is data, not admission.
#[derive(Clone)]
pub struct SourceRequestDocument {
    original: SourceJson,
}
impl std::fmt::Debug for SourceRequestDocument {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        out.debug_struct("SourceRequestDocument")
            .field("bytes", &self.original.as_bytes().len())
            .finish()
    }
}
impl SourceRequestDocument {
    pub fn new(original: SourceJson) -> Result<Self, SourceRequestError> {
        original
            .object_entries()
            .map_err(|_| SourceRequestError::scalar("Invalid JSON body", None))?;
        Ok(Self { original })
    }
    pub fn original_source(&self) -> &SourceJson {
        &self.original
    }
    pub fn for_surface(
        &self,
        surface: gateway_schema_values::request::RequestSurface,
    ) -> OrdinarySourceRequest {
        OrdinarySourceRequest::new(self.clone(), surface)
    }
    pub fn validate_fields(
        &self,
        surface: gateway_schema_values::request::RequestSurface,
        extensions: &gateway_schema_values::request::RequestExtensions,
    ) -> Result<(), SourceRequestError> {
        validate_request_fields(&self.original, surface, extensions)
    }
    pub fn reasoning(&self) -> Result<Option<SourceJson>, SourceRequestError> {
        parse_reasoning(member(&self.original, "reasoning")?.as_ref())
    }
    pub fn model_request(&self) -> Result<SourceModelRequest, SourceRequestError> {
        model_request(member(&self.original, "model")?.as_ref())
    }
    pub fn stream(&self) -> Result<bool, SourceRequestError> {
        parse_stream(member(&self.original, "stream")?.as_ref())
    }
    pub fn instructions(&self) -> Result<SourceField<JsString16>, SourceRequestError> {
        parse_instructions(member(&self.original, "instructions")?.as_ref())
    }
}

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

impl SourceRequestDocument {
    pub fn normalize_responses_input(&self) -> Result<Vec<SourceJson>, SourceRequestError> {
        gateway_responses_normalization::normalized::normalize_responses_input(
            member(self.original_source(), "input")?.as_ref(),
        )
    }
    /// Explicit Codex/paid Chat phase, never an unconditional direct-provider gate.
    pub fn normalize_chat_for_codex(
        &self,
    ) -> Result<gateway_chat_normalization::normalized::SourceChatInput, SourceRequestError> {
        let value = member(self.original_source(), "messages")?
            .ok_or_else(|| SourceRequestError::scalar("messages must be an array", None))?;
        gateway_chat_normalization::normalized::normalize_chat_messages(&value)
    }
}
