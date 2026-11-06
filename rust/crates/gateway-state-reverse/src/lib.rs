//! Native-only reverse of the Worker state transfer: a verified export archive
//! becomes a read-only query store, and the existing latest inverse with its
//! native custody checks writes a NEW private NativeCapture v2. The Bun KV
//! destination is written separately, only new, by the TypeScript restorer.
#[cfg(all(unix, not(target_arch = "wasm32")))]
pub mod archive;
#[cfg(all(unix, not(target_arch = "wasm32")))]
pub mod reverse;

#[cfg(all(unix, not(target_arch = "wasm32")))]
pub(crate) fn refused(message: impl Into<String>) -> gateway_core::error::GatewayError {
    gateway_core::error::GatewayError::new(409, message, Some("state_reverse_refused"))
}
