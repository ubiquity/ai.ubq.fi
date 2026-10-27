use gateway_source::error::GatewayError;
pub fn unauthorized() -> GatewayError {
    GatewayError::new(401, "Unauthorized", Some("invalid_api_key"))
}
pub fn invalid(message: &str) -> GatewayError {
    GatewayError::new(400, message, Some("invalid_request_error"))
}
pub fn conflict(message: &str) -> GatewayError {
    GatewayError::new(409, message, Some("invalid_request_error"))
}
