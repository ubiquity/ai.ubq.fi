//! Complete offline legacy renewal/import custody DATA validation.
//! Successful validation never authenticates or constructs an operation capability.
mod import;
pub mod renewal;

pub const OWNER_RENEWAL_MAX_MS: i64 = 45_000;
