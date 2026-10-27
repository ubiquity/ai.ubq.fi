//! Co-owned private captured preparation and ordered bounded stage emission.
#[cfg(all(unix, not(target_arch = "wasm32")))]
pub mod native_stage;
pub mod plan;
