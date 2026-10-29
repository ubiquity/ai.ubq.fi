//! One immutable checked request document; construction is DATA, never admission.
use crate::fields::validate_request_fields;
use crate::ordinary::OrdinarySourceRequest;
use crate::syntax::{member, model_request, parse_instructions, parse_reasoning, parse_stream};
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_schema_values::request::{SourceField, SourceModelRequest};
use gateway_source::{js_string::JsString16, source_json::SourceJson};
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
