//! Source provider error envelopes that forward only bounded detail fields.
use serde_json::json;

/// Source Cerebras errors forward only message/code from a bounded body.
fn cerebras_error_detail(bytes: &[u8]) -> (String, String) {
    let fallback_message = "Cerebras upstream returned an error.".to_owned();
    let fallback_code = "cerebras_upstream_error".to_owned();
    let Ok(value) = serde_json::from_slice::<serde_json::Value>(bytes) else {
        return (fallback_message, fallback_code);
    };
    let error = value.get("error").filter(|value| value.is_object());
    let text = |name: &str, limit: usize| {
        error
            .and_then(|error| error.get(name))
            .or_else(|| value.get(name))
            .and_then(serde_json::Value::as_str)
            .map(|value| {
                value
                    .chars()
                    .take(limit)
                    .collect::<String>()
                    .trim()
                    .to_owned()
            })
            .filter(|value| !value.is_empty())
    };
    (
        text("message", 1_000).unwrap_or(fallback_message),
        text("code", 200).unwrap_or(fallback_code),
    )
}

fn cerebras_error_type(status: u16) -> &'static str {
    if status >= 500 {
        "server_error"
    } else if status == 429 {
        "rate_limit_error"
    } else {
        "invalid_request_error"
    }
}

pub fn cerebras_error_body(status: u16, bytes: &[u8]) -> Vec<u8> {
    let (message, code) = cerebras_error_detail(bytes);
    serde_json::to_vec(&json!({
        "error": {
            "message": message,
            "type": cerebras_error_type(status),
            "code": code,
        }
    }))
    .unwrap_or_else(|_| {
        br#"{"error":{"message":"Cerebras upstream returned an error.","type":"invalid_request_error","code":"cerebras_upstream_error"}}"#.to_vec()
    })
}
