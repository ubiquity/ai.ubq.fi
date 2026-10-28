//! Exact Worker error/header/correlation projections; no transport or admission authority.
use gateway_contracts::provider::UpstreamProvider;
use gateway_source::error::GatewayError;
use serde_json::json;
use worker::Response;

pub const CEREBRAS_RATE_LIMIT_HEADERS: &[&str] = &[
    "x-ratelimit-limit-requests-minute",
    "x-ratelimit-remaining-requests-minute",
    "x-ratelimit-reset-requests-minute",
    "x-ratelimit-limit-tokens-minute",
    "x-ratelimit-remaining-tokens-minute",
    "x-ratelimit-reset-tokens-minute",
    "x-ratelimit-limit-requests-day",
    "x-ratelimit-remaining-requests-day",
    "x-ratelimit-reset-requests-day",
    "x-ratelimit-limit-tokens-day",
    "x-ratelimit-remaining-tokens-day",
    "x-ratelimit-reset-tokens-day",
];

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

pub fn set_header(response: &mut Response, name: &str, value: &str) -> Result<(), GatewayError> {
    response
        .headers_mut()
        .set(name, value)
        .map_err(|_| invalid("Invalid response header."))
}

pub fn apply_warnings(response: &mut Response, warnings: &[String]) -> Result<(), GatewayError> {
    if warnings.is_empty() {
        return Ok(());
    }
    let mut values = response
        .headers()
        .get("x-uos-warning")
        .map_err(|_| invalid("Invalid provider warning header."))?
        .unwrap_or_default()
        .split(',')
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_owned)
        .collect::<Vec<_>>();
    for warning in warnings {
        if !values.contains(warning) {
            values.push(warning.clone());
        }
    }
    response
        .headers_mut()
        .set("x-uos-warning", &values.join(", "))
        .map_err(|_| invalid("Invalid provider warning header."))
}

pub fn copy_headers(
    response: &mut Response,
    headers: &gateway_provider_protocol::types::Headers,
) -> Result<(), GatewayError> {
    for (name, value) in headers {
        let lower = name.to_ascii_lowercase();
        if lower == "retry-after"
            || lower.starts_with("x-ratelimit-")
            || [
                "x-request-id",
                "request-id",
                "x-uos-warning",
                "x-should-retry",
            ]
            .contains(&lower.as_str())
        {
            set_header(response, name, value)?;
        }
    }
    Ok(())
}

pub fn correlation(
    provider: UpstreamProvider,
    headers: &gateway_provider_protocol::types::Headers,
) -> Option<&str> {
    let names: &[&str] = if provider == UpstreamProvider::Deepseek {
        &["x-request-id", "x-ds-request-id", "x-deepseek-request-id"]
    } else if provider == UpstreamProvider::Cerebras {
        &[
            "x-request-id",
            "x-api-request-id",
            "x-oneapi-request-id",
            "x-cerebras-request-id",
        ]
    } else {
        &["x-request-id", "request-id"]
    };
    names
        .iter()
        .find_map(|wanted| {
            headers
                .iter()
                .find(|(name, _)| name.eq_ignore_ascii_case(wanted))
                .map(|(_, value)| value.trim())
        })
        .filter(|id| {
            !id.is_empty() && id.len() <= 256 && !id.chars().any(|c| c <= '\u{1f}' || c == '\u{7f}')
        })
}

pub fn provider_correlation(
    response: &mut Response,
    provider: UpstreamProvider,
    headers: &gateway_provider_protocol::types::Headers,
) -> Result<(), GatewayError> {
    let id = correlation(provider, headers);
    for name in [
        "x-request-id",
        "x-api-request-id",
        "x-oneapi-request-id",
        "x-cerebras-request-id",
        "x-ds-request-id",
        "x-deepseek-request-id",
        "x-uos-provider-request-id",
        "request-id",
    ] {
        response
            .headers_mut()
            .delete(name)
            .map_err(|_| invalid("Invalid response header."))?;
    }
    if let Some(id) = id {
        set_header(response, "x-uos-provider-request-id", id)?;
    }
    Ok(())
}

pub fn interrupted() -> GatewayError {
    gateway_worker_platform::error::platform_error(
        "The upstream request was interrupted.",
        "upstream_request_interrupted",
    )
}
pub fn stream_failure(provider: UpstreamProvider) -> GatewayError {
    GatewayError::new(
        502,
        format!(
            "DeepSeek waterfall provider '{}' ended its stream before producing output.",
            gateway_provider_source::policy::direct_provider_label(provider)
        ),
        Some("deepseek_waterfall_stream_failure"),
    )
    .with_type("server_error")
}
pub fn invalid(message: &str) -> GatewayError {
    GatewayError::new(502, message, Some("upstream_transport_error"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cerebras_payment_required_body_forwards_only_source_detail_fields() {
        let input = br#"{"error":{"message":"Payment required","code":"payment_required","param":"secret","type":"vendor"},"secret":"never"}"#;
        let body = cerebras_error_body(402, input);
        let text = String::from_utf8(body.clone()).unwrap();
        assert_eq!(
            text,
            r#"{"error":{"message":"Payment required","type":"invalid_request_error","code":"payment_required"}}"#
        );
        assert!(!text.contains("vendor"));
        assert!(!text.contains("secret"));
        assert!(!text.contains("param"));
    }

    #[test]
    fn cerebras_payment_required_146_byte_source_shape() {
        let message = "x".repeat(65);
        let input = serde_json::to_vec(&json!({
            "error": {"message": message, "code": "payment_required"}
        }))
        .unwrap();
        let body = cerebras_error_body(402, &input);
        assert_eq!(body.len(), 146);
        assert_eq!(
            body,
            br#"{"error":{"message":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","type":"invalid_request_error","code":"payment_required"}}"#
        );
    }

    #[test]
    fn cerebras_status_types_and_bounds_follow_the_source() {
        let body = cerebras_error_body(429, br#"{"error":{"message":"slow down","code":"rate"}}"#);
        assert!(
            String::from_utf8(body)
                .unwrap()
                .contains("rate_limit_error")
        );
        let body = cerebras_error_body(503, br#"{"message":"down","code":"unavailable"}"#);
        assert!(String::from_utf8(body).unwrap().contains("server_error"));
        let oversized = serde_json::to_vec(&json!({
            "error": {"message": "m".repeat(2_000), "code": "c".repeat(400)}
        }))
        .unwrap();
        let body = String::from_utf8(cerebras_error_body(402, &oversized)).unwrap();
        assert!(!body.contains(&"m".repeat(1_001)));
        assert!(!body.contains(&"c".repeat(201)));
    }
}
