use gateway_provider_protocol::types::PrepareOptions;
use serde_json::Value;
use serde_json::json;

pub const CLAUDE_ATTRIBUTION: &str = "You are Claude Code, Anthropic's official CLI for Claude.";

pub fn claude_native_transport(body: &mut Value, options: &PrepareOptions) {
    if let Some(object) = body.as_object_mut() {
        object.remove("prompt_cache_key");
    }
    if !options.auth.api_key {
        let system = body
            .get("system")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        let mut attributed = vec![json!({"type":"text","text":CLAUDE_ATTRIBUTION})];
        attributed.extend(system);
        body["system"] = json!(attributed);
    }
}
