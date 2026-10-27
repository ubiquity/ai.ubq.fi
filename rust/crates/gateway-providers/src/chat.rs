use gateway_provider_source::source as s;
use gateway_provider_source::types::ChatProfile;
use gateway_provider_protocol::types::Elision;
use gateway_provider_protocol::types::invalid;
use gateway_core::error::GatewayError;
use serde_json::Value;
use gateway_provider_protocol::chat::ChatProjection;

pub fn responses_messages(
    raw: &Value,
) -> Result<(Vec<gateway_core::source_json::SourceJson>, Vec<Elision>), GatewayError> {
    gateway_provider_history::source_chat::messages(&s::scalar_json(raw)?)
        .map(|(messages, elisions, _)| (messages, elisions)).map_err(s::scalar_error)
}


pub fn project_responses(
    raw: &Value,
    model: &str,
    stream: bool,
    profile: ChatProfile,
) -> Result<ChatProjection, GatewayError> {
    let projection = gateway_provider_chat::source_chat::project(
        &s::scalar_json(raw)?,
        &gateway_core::model::ModelId::from_scalar(model),
        stream,
        profile,
    )
    .map_err(s::scalar_error)?;
    Ok(ChatProjection {
        body: projection.body,
        tool_names: projection.names,
        elisions: projection.elisions,
        warnings: projection.warnings,
    })
}


pub fn project_effort(value: &str, _profile: ChatProfile) -> Result<String, GatewayError> { Ok(value.into()) }


pub fn thinking_active(effort: Option<&str>, thinking: &Value) -> bool {
    if let Some(effort) = effort.map(str::trim).filter(|effort| !effort.is_empty()) {
        return !effort.eq_ignore_ascii_case("none");
    }
    thinking
        .get("type")
        .and_then(Value::as_str)
        .is_none_or(|kind| !kind.trim().eq_ignore_ascii_case("disabled"))
}


pub fn validate_thinking_choice(body: &Value, field: &str) -> Result<(), GatewayError> {
    let effort = body["reasoning_effort"].as_str();
    let thinking = thinking_active(effort, &body["thinking"]);
    if thinking && (body["tool_choice"] == "required" || body["tool_choice"].is_object()) {
        let choice = body["tool_choice"]
            .as_str()
            .map(str::to_owned)
            .unwrap_or_else(|| {
                format!(
                    "function:{}",
                    body["tool_choice"]["function"]["name"]
                        .as_str()
                        .unwrap_or("")
                )
            });
        return Err(invalid(
            format!(
                "tool_choice '{choice}' is not supported while {field} keeps DeepSeek thinking mode active; set {field} to 'none' or use tool_choice 'auto'"
            ),
            "tool_choice",
        ));
    }
    Ok(())
}
