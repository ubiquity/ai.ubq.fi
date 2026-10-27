//! Offline native orchestration over the one existing migration authority.
//! Source/data preparation grants no runtime, custody or publication ownership.
#[cfg(all(unix, not(target_arch = "wasm32")))]
pub mod native_operator;
