//! Responses -> Chat-only request translation, preserving provider-specific boundaries.
use crate::{
    source_tools::ToolMap,
    types::{ChatProfile, invalid},
};
use gateway_core::error::GatewayError;
use gateway_prompt_admission::{
    admission::PromptAdmission, forwarding::Elision, summary::Exemption,
};
use serde_json::Value;
pub(crate) const CONTINUATION: &str = "When tools are available, a progress update does not complete a requested action. If required work remains and you can perform it, continue with the next appropriate tool call instead of ending with a status message. Provide a final answer when the requested work is complete, or when you need user input or are blocked. Never claim a tool action has been done unless its result is in the conversation.";
#[derive(Clone, Debug)]
pub struct ChatProjection {
    pub body: gateway_core::source_json::SourceJson,
    pub tool_names: ToolMap,
    pub elisions: Vec<Elision>,
    pub exemptions: Vec<Exemption>,
    pub admission: Option<PromptAdmission>,
    pub warnings: Vec<String>,
}
pub fn responses_messages(
    raw: &Value,
) -> Result<(Vec<gateway_core::source_json::SourceJson>, Vec<Elision>), GatewayError> {
    crate::source_chat::messages(&crate::source::generated(raw))
        .map(|(messages, elisions, _)| (messages, elisions))
        .map_err(crate::source::scalar_error)
}
pub fn project_responses(
    raw: &Value,
    model: &str,
    stream: bool,
    profile: ChatProfile,
) -> Result<ChatProjection, GatewayError> {
    let projection = crate::source_chat::project(
        &crate::source::generated(raw),
        &gateway_core::model::ModelId::from_scalar(model),
        stream,
        profile,
    )
    .map_err(crate::source::scalar_error)?;
    Ok(ChatProjection {
        body: projection.body,
        tool_names: projection.names,
        elisions: projection.elisions,
        exemptions: projection.exemptions,
        admission: projection.admission,
        warnings: projection.warnings,
    })
}
pub fn project_effort(value: &str, profile: ChatProfile) -> Result<String, GatewayError> {
    let effort = value.trim().to_lowercase();
    if profile == ChatProfile::Deepseek {
        return Ok(if effort == "ultra" {
            "max".into()
        } else {
            value.into()
        });
    }
    let allowed = match profile {
        ChatProfile::Lithos => {
            ["none", "minimal", "low", "medium", "high", "xhigh", "max"].as_slice()
        }
        ChatProfile::Cerebras => ["none", "low", "medium", "high"].as_slice(),
        _ => return Ok(value.trim().into()),
    };
    if !allowed.contains(&effort.as_str()) {
        return Err(invalid(
            format!("reasoning.effort '{value}' is not supported by {profile:?}"),
            "reasoning.effort",
        ));
    }
    Ok(effort)
}
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
