use gateway_source::error::GatewayError;
pub fn failure(reason: &str) -> GatewayError {
    GatewayError::unavailable(reason, "native_live_transport_failed")
}
