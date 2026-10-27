//! Exact client-facing extras failures, without transport or ledger side effects.
use gateway_source::error::GatewayError;
pub fn invalid_state() -> GatewayError {
    GatewayError::unavailable(
        "Embeddings durable state is invalid.",
        "embeddings_state_invalid",
    )
}
pub fn invalid(message: impl Into<String>, param: Option<&str>) -> GatewayError {
    GatewayError::new(400, message, Some("invalid_request_error")).with_param(param)
}
pub fn server(message: impl Into<String>, code: &str) -> GatewayError {
    GatewayError::new(502, message, Some(code))
        .with_type("server_error")
        .with_param(None)
}
pub fn cancelled() -> GatewayError {
    GatewayError::new(499, "Request was cancelled.", Some("request_cancelled"))
        .with_type("server_error")
        .with_param(None)
}

pub fn rendering() -> GatewayError {
    GatewayError::unavailable(
        "The source extras response could not be rendered.",
        "extras_response_unavailable",
    )
}
