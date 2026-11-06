//! Fixed native resources and their event-driven lifetime; existing callers retain refresh ownership.
#[cfg(unix)]
pub mod https;
#[cfg(unix)]
pub mod lifecycle;
#[cfg(unix)]
pub mod owner_socket;
