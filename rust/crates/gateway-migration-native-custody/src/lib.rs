//! Fixed native custody observation and configured registration; no owner discovery.
pub mod native_custody;
#[cfg(all(unix, not(target_arch = "wasm32")))]
pub mod registry;
