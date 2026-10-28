//! Paid policy and key-deletion fencing over the existing authority.
pub mod paid;

pub struct PaidAdmin<'a, S, H, C> {
    pub store: &'a S,
    pub host: &'a H,
    pub capabilities: &'a C,
}
