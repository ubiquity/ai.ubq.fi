//! Complete finite capacity event, historical derivation and remote observation operations.
pub mod capacity_events;
mod history;
pub mod remote_capacity;
mod store;
pub struct CapacityEvents<'a, S, H> {
    store: &'a S,
    host: &'a H,
}
impl<'a, S, H> CapacityEvents<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H) -> Self {
        Self { store, host }
    }
}
