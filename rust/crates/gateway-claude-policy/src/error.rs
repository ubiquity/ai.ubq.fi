use gateway_source::error::GatewayError;
pub fn unavailable(message: &str) -> GatewayError {
    GatewayError::unavailable(message, "claude_code_upstream_unreachable")
}
pub fn invalid(message: &str) -> GatewayError {
    GatewayError::new(400, message, Some("claude_code_request_invalid"))
}
