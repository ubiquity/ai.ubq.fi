//! Existing finite authority host and checked scalar contracts; no store or operation grant.
use gateway_contracts::{identity::MAX_SAFE_INTEGER, identity::UnixMillis};
use gateway_source::error::GatewayError;

pub const MINUTE_MS: i64 = 60_000;
pub const DAY_MS: i64 = 86_400_000;
pub const QUOTA_RETENTION_MS: i64 = 7 * DAY_MS;
pub const PAID_RETENTION_MS: i64 = 365 * DAY_MS;
pub const REQUEST_LEASE_MS: i64 = 5 * MINUTE_MS;
pub const PROBE_LEASE_MS: i64 = 30_000;

pub fn unavailable(code: &str, message: &str) -> GatewayError {
    GatewayError::unavailable(message, code)
}

pub fn invalid(message: &str) -> GatewayError {
    unavailable("authority_state_invalid", message)
}

pub fn quota_error(message: &str, code: &str) -> GatewayError {
    GatewayError::new(429, message, Some(code))
}

pub fn validate_time(now: UnixMillis) -> Result<(), GatewayError> {
    nonnegative(now).map(|_| ())
}

pub fn nonnegative(value: i64) -> Result<i64, GatewayError> {
    if (0..=MAX_SAFE_INTEGER).contains(&value) {
        Ok(value)
    } else {
        Err(invalid("Expected a nonnegative safe integer."))
    }
}

pub fn positive(value: i64) -> Result<i64, GatewayError> {
    if value > 0 {
        nonnegative(value)
    } else {
        Err(invalid("Expected a positive safe integer."))
    }
}

pub fn add(left: i64, right: i64) -> Result<i64, GatewayError> {
    nonnegative(
        left.checked_add(right)
            .ok_or_else(|| invalid("Authority counter overflow."))?,
    )
}

pub fn generation(value: u64) -> Result<u64, GatewayError> {
    if value <= MAX_SAFE_INTEGER as u64 {
        Ok(value)
    } else {
        Err(invalid("Invalid authority generation."))
    }
}

pub fn next_generation(value: u64) -> Result<u64, GatewayError> {
    generation(
        value
            .checked_add(1)
            .ok_or_else(|| invalid("Authority generation overflow."))?,
    )
}

pub fn text(value: &str) -> Result<(), GatewayError> {
    if !value.trim().is_empty() && value.len() <= 4096 {
        Ok(())
    } else {
        Err(invalid("Missing or oversized authority identity."))
    }
}

pub fn retention(anchor: i64, duration: i64) -> Result<i64, GatewayError> {
    add(anchor, duration)
}

/// Platform-supplied cryptographically unique opaque tokens. Revisions are never parsed.
/// A reset owner or probe token must remain unpredictable to other callers.
pub trait TokenSource {
    fn next_token(&self) -> Result<String, GatewayError>;
}

/// Time-sensitive authority actions recheck time after awaited reads and before spending.
pub trait AuthorityClock: Send + Sync {
    fn now_ms(&self) -> Result<i64, GatewayError>;
}
