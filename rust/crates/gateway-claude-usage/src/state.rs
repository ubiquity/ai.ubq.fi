//! Source `ClaudeCodeUsageState`: the most recent direct OAuth usage read.
//! The direct reader is retired (meters come from response headers), but rows
//! it wrote are retained verbatim. Never an inference-routing guard.
use crate::json::safe_integer;
use gateway_core::js_string::JsString16;
use serde::{Deserialize, Serialize};
use serde_json::Value;

pub const SOURCE: &str = "oauth_usage";
pub const STATES: [&str; 9] = [
    "ok",
    "scope_missing",
    "auth_invalid",
    "rate_limited",
    "unsupported_credential",
    "unreachable",
    "upstream_error",
    "http_error",
    "invalid_response",
];

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct UsageState {
    pub source: String,
    pub state: String,
    pub status: Option<f64>,
    pub message: Option<JsString16>,
    pub attempted_at_ms: f64,
    pub observed_at_ms: Option<f64>,
    /// Kept only for a rate-limited read whose deadline follows its attempt.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub retry_at_ms: Option<f64>,
}

fn positive(value: Option<&Value>) -> Option<f64> {
    value?.as_f64().filter(|n| safe_integer(*n) && *n > 0.0)
}
/// The `message` leaf as the caller's codec read it: `None` is a non-string,
/// non-null (or absent) leaf, `Some(None)` is null. Unpaired units survive.
pub type MessageLeaf = Option<Option<JsString16>>;

/// Source `parseClaudeCodeUsageState`; any malformed member drops the whole read.
pub fn parse_usage(value: &Value, message: MessageLeaf) -> Option<UsageState> {
    let state = value.get("state")?.as_str()?;
    if value.get("source")?.as_str()? != SOURCE || !STATES.contains(&state) {
        return None;
    }
    let attempted = positive(value.get("attempted_at_ms"))?;
    let observed = match value.get("observed_at_ms")? {
        Value::Null => None,
        other => Some(positive(Some(other))?),
    };
    let status = match value.get("status")? {
        Value::Null => None,
        other => Some(
            other
                .as_f64()
                .filter(|n| safe_integer(*n) && (100.0..=599.0).contains(n))?,
        ),
    };
    let retry = positive(value.get("retry_at_ms"))
        .filter(|retry| state == "rate_limited" && *retry > attempted);
    Some(UsageState {
        source: SOURCE.into(),
        state: state.into(),
        status,
        message: message?.and_then(|m| m.sanitize_js_whitespace_prefix(240)),
        attempted_at_ms: attempted,
        observed_at_ms: observed,
        retry_at_ms: retry,
    })
}
/// The scalar-JSON codec's message leaf.
pub fn json_message(value: &Value) -> MessageLeaf {
    match value.get("message")? {
        Value::Null => Some(None),
        Value::String(text) => Some(Some(JsString16::from_utf8(text))),
        _ => None,
    }
}
