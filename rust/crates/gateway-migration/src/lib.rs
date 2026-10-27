//! Offline reversible source-state adoption through the one existing authority.
//! This library neither opens a source VM database nor grants runtime/publication ownership.
pub mod inverse;
pub mod native_custody;
#[cfg(all(unix, not(target_arch = "wasm32")))]
mod native_history_reconcile;
pub mod native_inverse;
#[cfg(all(unix, not(target_arch = "wasm32")))]
mod native_latest;
#[cfg(all(unix, not(target_arch = "wasm32")))]
pub mod native_operator;
#[cfg(all(unix, not(target_arch = "wasm32")))]
pub mod native_stage;
pub mod operational;
pub mod staged;

use gateway_core::{error::GatewayError, state::RecordKey};
use gateway_migration_data::{
    MappingData as Mapping, Transform, decode, digest, invalid, json, key, parts, starts,
    unavailable,
};
