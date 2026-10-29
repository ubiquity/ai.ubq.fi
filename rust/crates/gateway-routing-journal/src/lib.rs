//! Complete routing journal schema and reader DATA, without live effect authority.
pub mod reader;
pub mod record;

fn unavailable(message: &str) -> gateway_source::error::GatewayError {
    gateway_source::error::GatewayError::unavailable(message, "routing_session_unavailable")
}
