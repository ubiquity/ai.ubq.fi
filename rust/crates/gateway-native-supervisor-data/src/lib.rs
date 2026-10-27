//! Constructor-owned sampled supervisor registry, selection/cache and real collection.
#[cfg(unix)]
mod collect;
#[cfg(unix)]
pub mod data;
