//! Complete fixed incident index/list/evidence operations with private atomic effects.
pub mod incidents;
mod replay_values;
mod store;
pub struct IncidentIndex<'a, S, H> {
    store: &'a S,
    host: &'a H,
}
impl<'a, S, H> IncidentIndex<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H) -> Self {
        Self { store, host }
    }
}
