//! Narrow native capabilities. No shell, arbitrary URL/path or generic JSON-RPC ingress.
use gateway_core::error::GatewayError;
/// A compile-time fixture artifact is never an ordinary native service, even
/// when dependency feature unification enabled it without the native feature.
pub fn require_production_artifact() -> Result<(), GatewayError> {
    if gateway_authority::NATIVE_IMPORT_FIXTURE_ENABLED {
        return Err(GatewayError::unavailable(
            "Fixture-enabled artifacts cannot run native services",
            "native_fixture_artifact_forbidden",
        ));
    }
    Ok(())
}
pub mod access_publication;
#[cfg(unix)]
pub mod authority_http;
#[cfg(unix)]
#[doc(hidden)]
pub mod connector_fd;
#[cfg(unix)]
#[doc(hidden)]
pub mod connector_policy;
#[cfg(unix)]
#[doc(hidden)]
pub mod connector_protocol;
#[cfg(unix)]
pub mod connector_supervisor;
#[cfg(unix)]
#[doc(hidden)]
pub mod connector_worker;
pub mod daemon_config;
#[cfg(unix)]
mod daemon_startup;
#[cfg(unix)]
pub mod existing_account_import;
#[cfg(unix)]
pub mod http_server;
#[cfg(unix)]
mod import_custody_protocol;
#[cfg(unix)]
pub mod import_ingress;
#[cfg(unix)]
pub mod live_bridge;
#[cfg(unix)]
pub mod managed_publication_http;
pub mod native_import;
#[cfg(unix)]
pub mod native_request_process;
pub mod owner_control;
#[cfg(unix)]
pub mod owner_renewal;
#[cfg(unix)]
mod private_https;
#[cfg(unix)]
pub mod publication_http;
