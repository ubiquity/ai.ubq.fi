use gateway_core::error::GatewayError;
pub(crate) fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Invalid native HTTP request",
        Some("native_request_invalid"),
    )
}
pub(crate) fn too_large() -> GatewayError {
    GatewayError::new(
        413,
        "Native request body is too large",
        Some("native_body_too_large"),
    )
}
pub(crate) fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native operation is unavailable",
        "native_operation_unavailable",
    )
}
