//! Kernel policy/window operations with their original reservation CAS.
pub mod kernel;

pub struct KernelAdmin<'a, S, H, C> {
    pub store: &'a S,
    pub host: &'a H,
    pub capabilities: &'a C,
}
