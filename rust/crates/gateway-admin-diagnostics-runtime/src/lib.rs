//! Administrative error history and source supervisor brief projection.
pub mod brief;
pub mod errors;

pub struct DiagnosticsAdmin<'a, S, H, C> {
    pub store: &'a S,
    pub host: &'a H,
    pub capabilities: &'a C,
}
