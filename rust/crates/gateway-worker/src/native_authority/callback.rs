//! Trusted native configuration and entropy adapter for the shared child-join protocol.
use crate::native_host::NativeBindings;
use gateway_core::error::GatewayError;
use gateway_worker_authority::client::RemoteAuthorityStore;
pub async fn join(
    store: &RemoteAuthorityStore,
    native: &NativeBindings,
    bytes: &[u8],
) -> Result<RemoteAuthorityStore, GatewayError> {
    if matches!(
        store.access(),
        gateway_authority_lifecycle::model::Access::Legacy
    ) {
        return Ok(store.clone());
    }
    let id = gateway_auth::crypto::sha256_hex(crate::transport::random::opaque_token()?.as_bytes());
    let capability =
        gateway_auth::crypto::sha256_hex(crate::transport::random::opaque_token()?.as_bytes());
    gateway_worker_authority::callbacks::join(
        store,
        native.transport_identity()?,
        bytes,
        id,
        capability,
    )
    .await
}
