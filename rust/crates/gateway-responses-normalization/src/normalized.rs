//! Checked Responses/content source normalization; no scalar transcript mirror.
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_schema_values::value::Value;
use gateway_source::source_json::SourceJson;

fn output(value: Value) -> Result<SourceJson, SourceRequestError> {
    value.source().map_err(SourceRequestError::from_gateway)
}

fn values(values: Vec<Value>) -> Result<Vec<SourceJson>, SourceRequestError> {
    values.into_iter().map(output).collect()
}

pub fn normalize_response_content(
    value: &SourceJson,
    param: &str,
    role: &str,
) -> Result<SourceJson, SourceRequestError> {
    crate::engine::normalize_response_content(&Value::from_source(value.clone()), param, role)
        .and_then(output)
}

pub fn normalize_response_message(
    value: &SourceJson,
    param: &str,
) -> Result<SourceJson, SourceRequestError> {
    crate::engine::normalize_response_message(&Value::from_source(value.clone()), param)
        .and_then(output)
}

pub fn is_gateway_failover_warning(value: &SourceJson) -> bool {
    crate::engine::is_gateway_failover_warning(&Value::from_source(value.clone()))
}

pub fn normalize_function_output(
    value: &SourceJson,
    param: &str,
) -> Result<SourceJson, SourceRequestError> {
    crate::engine::normalize_function_output(&Value::from_source(value.clone()), param)
        .and_then(output)
}

pub fn normalize_responses_input(
    value: Option<&SourceJson>,
) -> Result<Vec<SourceJson>, SourceRequestError> {
    let value = value.map(|value| Value::from_source(value.clone()));
    crate::engine::normalize_responses_input(value.as_ref()).and_then(values)
}

pub fn normalize_breakpoint(
    value: Option<&SourceJson>,
    param: &str,
) -> Result<Option<SourceJson>, SourceRequestError> {
    let value = value.map(|value| Value::from_source(value.clone()));
    crate::controls::normalize_breakpoint(value.as_ref(), param)?
        .map(output)
        .transpose()
}
