use crate::source as s;
use gateway_contracts::provider::UpstreamProvider;
use serde_json::Value;

pub fn response_has_answer(value: &Value) -> Result<bool, gateway_source::error::GatewayError> {
    Ok(crate::source_policy::response_has_answer(&s::scalar_json(
        value,
    )?))
}

pub fn item_has_answer(value: &Value) -> Result<bool, gateway_source::error::GatewayError> {
    Ok(crate::source_policy::item_has_answer(&s::scalar_json(
        value,
    )?))
}

pub fn semantic_event(value: &Value) -> Result<bool, gateway_source::error::GatewayError> {
    Ok(crate::source_policy::semantic_event(&s::scalar_json(
        value,
    )?))
}

pub fn reasoning_progress(value: &Value) -> Result<bool, gateway_source::error::GatewayError> {
    Ok(crate::source_policy::reasoning_progress(&s::scalar_json(
        value,
    )?))
}

pub fn chat_has_answer(value: &Value) -> bool {
    value
        .get("choices")
        .and_then(Value::as_array)
        .is_some_and(|choices| {
            choices.iter().any(|c| {
                let m = c.get("message").or_else(|| c.get("delta"));
                nonempty(m.and_then(|message| message.get("content")))
                    || nonempty(m.and_then(|message| message.get("refusal")))
                    || m.and_then(|message| message.get("tool_calls"))
                        .and_then(Value::as_array)
                        .is_some_and(|t| !t.is_empty())
            })
        })
}

fn nonempty(value: Option<&Value>) -> bool {
    value.and_then(Value::as_str).is_some_and(|v| !v.is_empty())
}

pub fn direct_provider_label(provider: UpstreamProvider) -> &'static str {
    match provider {
        UpstreamProvider::ChatgptCodex => "chatgpt_codex",
        UpstreamProvider::ClaudeCode => "claude_code",
        UpstreamProvider::Deepseek => "deepseek",
        UpstreamProvider::Cerebras => "cerebras",
        UpstreamProvider::Lithos => "lithos",
        UpstreamProvider::Openrouter => "openrouter",
        UpstreamProvider::RemovedProvider => "removed_provider",
        UpstreamProvider::Metered => "metered",
        UpstreamProvider::Surplus => "surplus",
        UpstreamProvider::Voyage => "voyage",
    }
}
