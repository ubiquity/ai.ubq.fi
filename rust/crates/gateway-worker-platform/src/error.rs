use gateway_source::error::GatewayError;

pub fn platform_error(message: &str, code: &str) -> GatewayError {
    GatewayError::new(502, message, Some(code)).with_type("server_error")
}

pub fn configuration_error() -> GatewayError {
    GatewayError::unavailable(
        "The upstream transport configuration is invalid.",
        "upstream_transport_configuration_invalid",
    )
}
