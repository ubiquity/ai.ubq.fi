//! Source-derived authority transitions. All provider/native I/O remains outside transactions.
//! The store must atomically commit every check and mutation, durably, before returning.
//! Independent records preserve history without a bounded whole-ledger snapshot.

#[cfg(all(feature = "native-import-fixture", not(debug_assertions)))]
compile_error!("native-import-fixture is forbidden in non-debug production builds");
/// Denial marker only. Native service entrypoints must refuse when this is true.
pub const NATIVE_IMPORT_FIXTURE_ENABLED: bool = cfg!(feature = "native-import-fixture");

pub mod cache_experiment;
pub mod full_pool_reset;
pub mod manual_reset;
pub mod native_control;
pub mod native_live;
pub mod paid;
pub mod paid_backfill;
mod paid_control;
mod probes;
pub mod reset;
mod reset_recovery;
pub mod reset_types;
pub mod routing;
mod source_models;
pub mod store;

use gateway_authority_contracts::{
    DAY_MS, MINUTE_MS, PAID_RETENTION_MS, PROBE_LEASE_MS, QUOTA_RETENTION_MS, REQUEST_LEASE_MS,
    add, generation, invalid, next_generation, nonnegative, positive, quota_error, retention, text,
    unavailable, validate_time,
};
