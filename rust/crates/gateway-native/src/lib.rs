//! Narrow native capabilities. No shell, arbitrary URL/path or generic JSON-RPC ingress.
#[cfg(unix)]
pub mod authority_http;
#[cfg(unix)]
pub mod connector_supervisor;
#[cfg(unix)]
mod daemon_startup;
#[cfg(unix)]
pub mod existing_account_import;
#[cfg(unix)]
mod import_custody_protocol;
#[cfg(unix)]
pub mod live_bridge;
#[cfg(unix)]
pub mod managed_publication_http;
#[cfg(unix)]
pub mod native_request_process;
#[cfg(unix)]
pub mod owner_renewal;
#[cfg(unix)]
mod private_https;

#[cfg(all(test, unix))]
mod checked_wire_context;
