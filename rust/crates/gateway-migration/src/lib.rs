//! Offline reversible source-state adoption through the one existing authority.
//! This library neither opens a source VM database nor grants runtime/publication ownership.
#[cfg(all(unix, not(target_arch = "wasm32")))]
pub mod core_state;
pub mod engine;
mod indexes;
pub mod initial_owner;
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
mod native_spool;
#[cfg(all(unix, not(target_arch = "wasm32")))]
pub mod native_stage;
pub mod operational;
mod profile_bindings;
mod profile_coupling;
mod profile_reset;
pub mod profile_runtime;
mod profile_validate;
pub mod staged;

use gateway_core::{
    error::GatewayError,
    source_json::SourceJson,
    state::{RecordKey, StoredRecord},
    tuple_key::TupleKey,
};
use gateway_migration_data::{
    MappingData as Mapping, RowData as Row, Transform, decode, digest, invalid, json, key, parts,
    private_source_key, starts, unavailable,
};
use gateway_profile_values::coercion::encode_base64;
use serde::{Deserialize, Serialize};
use serde_json::Value;
