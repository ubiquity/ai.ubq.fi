//! Narrow native capabilities. No shell, arbitrary URL/path or generic JSON-RPC ingress.
#[cfg(unix)]
pub mod authority_http;
#[cfg(unix)]
pub mod connector_supervisor;
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
