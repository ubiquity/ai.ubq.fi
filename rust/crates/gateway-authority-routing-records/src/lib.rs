//! Persisted routing data and pure normalization; no authority, store, or host operations.
pub mod codec;
pub mod slot;
pub mod types;

use gateway_core::identity::MAX_SAFE_INTEGER;
use gateway_source::error::GatewayError;

fn invalid(message: &str) -> GatewayError {
    GatewayError::unavailable(message, "authority_state_invalid")
}

fn generation(value: u64) -> Result<u64, GatewayError> {
    if value <= MAX_SAFE_INTEGER as u64 {
        Ok(value)
    } else {
        Err(invalid("Invalid authority generation."))
    }
}

fn next_generation(value: u64) -> Result<u64, GatewayError> {
    generation(
        value
            .checked_add(1)
            .ok_or_else(|| invalid("Authority generation overflow."))?,
    )
}
