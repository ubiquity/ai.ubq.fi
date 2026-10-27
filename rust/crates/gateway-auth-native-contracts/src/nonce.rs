use gateway_core::error::GatewayError;
use std::future::Future;
/// Atomically prune only expired nonces, refuse any live replay, and refuse new
/// requests at NATIVE_NONCE_CAP rather than evicting a live authentication fence.
/// Runtime implementations must persist fences in their owned control store;
/// no in-memory default is installed by the authentication library.
pub trait NativeNonceAuthority {
    fn consume_nonce(
        &self,
        key_id: &str,
        nonce: &str,
        valid_through_ms: i64,
        now_ms: i64,
    ) -> impl Future<Output = Result<(), GatewayError>>;
}
