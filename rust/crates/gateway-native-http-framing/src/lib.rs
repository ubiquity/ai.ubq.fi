//! Closed native HTTP framing DATA and already-owned TCP I/O, with no authentication/context authority.
#[cfg(unix)]
mod error;
#[cfg(unix)]
pub mod import_head;
#[cfg(unix)]
pub mod request;
#[cfg(unix)]
pub mod response;
