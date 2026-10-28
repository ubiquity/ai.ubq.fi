//! Scalar compatibility entrypoints delegate to the one lossless source engine.
use crate::source_request as source;
use serde_json::Value;
fn raw(
    value: &Value,
) -> Result<gateway_source::source_json::SourceJson, gateway_source::error::GatewayError> {
    gateway_source::source_json::SourceJson::from_serializable(value)
}
fn scalar(
    value: gateway_source::source_json::SourceJson,
) -> Result<Value, gateway_source::error::GatewayError> {
    value.deserialize()
}
pub fn normalize_response_content(
    value: &Value,
    param: &str,
    role: &str,
) -> Result<Value, gateway_source::error::GatewayError> {
    source::normalized::normalize_response_content(&raw(value)?, param, role)
        .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)
        .and_then(scalar)
}
pub fn normalize_response_message(
    value: &Value,
    param: &str,
) -> Result<Value, gateway_source::error::GatewayError> {
    source::normalized::normalize_response_message(&raw(value)?, param)
        .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)
        .and_then(scalar)
}
pub fn is_gateway_failover_warning(value: &Value) -> bool {
    raw(value)
        .ok()
        .is_some_and(|value| source::normalized::is_gateway_failover_warning(&value))
}
pub fn normalize_function_output(
    value: &Value,
    param: &str,
) -> Result<Value, gateway_source::error::GatewayError> {
    source::normalized::normalize_function_output(&raw(value)?, param)
        .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)
        .and_then(scalar)
}
pub fn normalize_responses_input(
    value: Option<&Value>,
) -> Result<Vec<Value>, gateway_source::error::GatewayError> {
    let value = value.map(raw).transpose()?;
    source::normalized::normalize_responses_input(value.as_ref())
        .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)?
        .into_iter()
        .map(scalar)
        .collect()
}
#[derive(Clone, Debug, PartialEq)]
pub struct ChatMessage {
    pub instruction: Option<String>,
    pub instruction_content: Option<Vec<Value>>,
    pub input: Vec<Value>,
}
pub fn normalize_chat_message(
    value: &Value,
    index: usize,
) -> Result<ChatMessage, gateway_source::error::GatewayError> {
    let value = source::normalized::normalize_chat_message(&raw(value)?, index)
        .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)?;
    Ok(ChatMessage {
        instruction: value
            .instruction
            .map(|value| value.try_utf8())
            .transpose()?,
        instruction_content: value
            .instruction_content
            .map(|values| values.into_iter().map(scalar).collect())
            .transpose()?,
        input: value
            .input
            .into_iter()
            .map(scalar)
            .collect::<Result<_, _>>()?,
    })
}
#[derive(Clone, Debug, PartialEq)]
pub struct ChatInput {
    pub input: Vec<Value>,
    pub instructions: Option<String>,
}
pub fn normalize_chat_messages(
    value: &Value,
) -> Result<ChatInput, gateway_source::error::GatewayError> {
    let value = source::normalized::normalize_chat_messages(&raw(value)?)
        .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)?;
    Ok(ChatInput {
        input: value
            .input
            .into_iter()
            .map(scalar)
            .collect::<Result<_, _>>()?,
        instructions: value
            .instructions
            .map(|value| value.try_utf8())
            .transpose()?,
    })
}
