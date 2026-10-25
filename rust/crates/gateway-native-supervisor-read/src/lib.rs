//! Read-only supervisor state and fixed-schema inventory/metadata projections.
#[cfg(unix)]
pub mod inventory;
#[cfg(unix)]
pub mod metadata;
pub mod state;
