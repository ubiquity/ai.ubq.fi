//! Observed prompt cache metadata is distinct from the official request control schema.
use crate::metadata::{
    compact_capabilities_source, concrete_scope_source, merge_capabilities_source,
    normalize_capabilities_source, normalize_scope_source,
};
use {gateway_source::error::GatewayError, serde_json::Value};

fn scalar_source(value: &Value) -> Option<gateway_source::source_json::SourceJson> {
    gateway_source::source_json::SourceJson::from_serializable(value).ok()
}
fn scalar_result(value: Option<gateway_source::source_json::SourceJson>) -> Option<Value> {
    value?.deserialize().ok()
}
pub fn normalize_capabilities(value: Option<&Value>) -> Option<Value> {
    let value = value.and_then(scalar_source);
    scalar_result(normalize_capabilities_source(value.as_ref()))
}
pub fn compact_capabilities(value: Option<&Value>) -> Option<Value> {
    let value = value.and_then(scalar_source);
    scalar_result(compact_capabilities_source(value.as_ref()))
}
pub fn normalize_scope(value: &Value) -> Option<Value> {
    scalar_result(normalize_scope_source(&scalar_source(value)?))
}
pub fn merge_capabilities(previous: Option<&Value>, next: Option<&Value>) -> Option<Value> {
    let previous = previous.and_then(scalar_source);
    let next = next.and_then(scalar_source);
    scalar_result(merge_capabilities_source(previous.as_ref(), next.as_ref()))
}
pub fn concrete_scope(value: &Value, cycles: i64) -> bool {
    scalar_source(value).is_some_and(|value| concrete_scope_source(&value, cycles))
}
pub fn validate_known_unsupported(
    model: &str,
    record: Option<&Value>,
    body: &Value,
    input: &[Value],
    input_key: &str,
) -> Result<(), GatewayError> {
    let body = gateway_source::source_json::SourceJson::from_serializable(body)?;
    let input = input
        .iter()
        .map(gateway_source::source_json::SourceJson::from_serializable)
        .collect::<Result<Vec<_>, _>>()?;
    let prompt_cache = record
        .and_then(|record| record.get("prompt_cache"))
        .map(gateway_source::source_json::SourceJson::from_serializable)
        .transpose()?;
    crate::capability::validate_known_unsupported_prompt_cache(
        &gateway_source::js_string::JsString16::from_utf8(model),
        prompt_cache.as_ref(),
        &body,
        &input,
        input_key,
    )
    .map_err(gateway_local_refusal::source_error::SourceRequestError::into_scalar)
}
