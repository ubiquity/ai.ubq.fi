//! Mandatory real OS confinement. Unsupported enforcement fails closed.
pub mod confinement;
#[cfg(unix)]
pub mod file_observation;
