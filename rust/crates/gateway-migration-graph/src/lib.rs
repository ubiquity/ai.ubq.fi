//! Lossless native source graph and explicit identity-free tree wire.
pub mod transport;
mod tree;

use gateway_source::{error::GatewayError, source_json::SourceJson};
fn invalid(message: impl Into<String>) -> GatewayError {
    GatewayError::new(409, message, Some("migration_state_invalid"))
}
