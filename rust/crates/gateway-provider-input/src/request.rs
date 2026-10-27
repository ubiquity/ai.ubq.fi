use gateway_provider_source::source as s;
use gateway_provider_protocol::types::Headers;
use gateway_provider_protocol::types::ClientSurface;
use gateway_core::error::GatewayError;
use gateway_core::model::ModelId;
use gateway_core::source_json::SourceJson;
use serde_json::Value;

pub fn bearer_headers(secret: &str, accept: &str) -> Headers {
    Headers::from([
        ("Authorization".into(), format!("Bearer {}", secret.trim())),
        ("Content-Type".into(), "application/json".into()),
        ("Accept".into(), accept.into()),
    ])
}


pub fn codex_replay_input(input: &Value) -> Result<SourceJson, gateway_core::error::GatewayError> {
    Ok(crate::source_controls::replay(&s::scalar_json(input)?)?)
}


pub fn build_codex_body(raw: &Value) -> Result<SourceJson, GatewayError> {
    crate::source_controls::codex(&s::scalar_json(raw)?)
        .map_err(s::scalar_error)
}


pub fn chat_request_to_responses(raw: &Value) -> Result<SourceJson, GatewayError> {
    crate::source_controls::ordinary(
        &s::scalar_json(raw)?,
        ClientSurface::Chat,
        &ModelId::from_scalar(raw["model"].as_str().unwrap_or("")),
        None,
    )
    .map_err(s::scalar_error)
}


pub fn project_cerebras_schema(value: &Value) -> Result<SourceJson, gateway_core::error::GatewayError> {
    Ok(gateway_provider_chat_controls::projection::cerebras_schema(&s::scalar_json(value)?)?)
}
