//! Native-private disk latest inventory and complete source/control sink export.
#![cfg(all(unix, not(target_arch = "wasm32")))]
pub mod native_latest;
