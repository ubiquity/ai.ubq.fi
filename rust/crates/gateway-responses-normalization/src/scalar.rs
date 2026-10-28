//! Scalar compatibility is an explicit fallible boundary over the source engine.
use crate::normalized as source;
use gateway_source::error::GatewayError;
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
    source::normalize_response_content(&raw(value)?, param, role)
        .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)
        .and_then(scalar)
}

pub fn normalize_response_message(
    value: &Value,
    param: &str,
) -> Result<Value, gateway_source::error::GatewayError> {
    source::normalize_response_message(&raw(value)?, param)
        .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)
        .and_then(scalar)
}

pub fn is_gateway_failover_warning(value: &Value) -> bool {
    raw(value)
        .ok()
        .is_some_and(|value| source::is_gateway_failover_warning(&value))
}

pub fn normalize_function_output(
    value: &Value,
    param: &str,
) -> Result<Value, gateway_source::error::GatewayError> {
    source::normalize_function_output(&raw(value)?, param)
        .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)
        .and_then(scalar)
}

pub fn normalize_responses_input(
    value: Option<&Value>,
) -> Result<Vec<Value>, gateway_source::error::GatewayError> {
    let value = value.map(raw).transpose()?;
    source::normalize_responses_input(value.as_ref())
        .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)?
        .into_iter()
        .map(scalar)
        .collect()
}

pub fn normalize_breakpoint(
    value: Option<&Value>,
    param: &str,
) -> Result<Option<Value>, GatewayError> {
    let value = value
        .map(gateway_source::source_json::SourceJson::from_serializable)
        .transpose()?;
    source::normalize_breakpoint(value.as_ref(), param)
        .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)?
        .map(|value| value.deserialize())
        .transpose()
}
