use gateway_provider_protocol::types::PrepareOptions;
use gateway_provider_source::source as s;
use serde_json::Value;

pub fn try_native_messages(
    raw: &Value,
    options: &PrepareOptions,
) -> Result<Option<gateway_source::source_json::SourceJson>, gateway_source::error::GatewayError> {
    gateway_provider_claude_body::source_anthropic::native(&s::scalar_json(raw)?, options)
}

pub fn try_claude_chat(
    raw: &Value,
    options: &PrepareOptions,
) -> Result<
    Option<gateway_provider_protocol::chat::ChatProjection>,
    gateway_source::error::GatewayError,
> {
    let projection = represented!(gateway_provider_chat::source_anthropic::chat(
        &s::scalar_json(raw)?,
        options
    )?);
    Ok(Some(gateway_provider_protocol::chat::ChatProjection {
        body: projection.body,
        tool_names: projection.names,
        elisions: projection.elisions,
        warnings: projection.warnings,
    }))
}
