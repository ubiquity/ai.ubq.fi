//! Registry-bound local control transport and guarded renewal. No process launcher or caller-selected operation.
pub mod guard;
pub mod limits;
pub mod transport;
#[cfg(unix)]
pub mod unix;
