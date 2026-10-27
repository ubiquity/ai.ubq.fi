//! Bounded dev-only native import state support. Proofs are created only by real checked operations in each test.
pub mod fixture;
#[cfg(unix)]
pub mod socket;
