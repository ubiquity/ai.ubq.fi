//! Complete provider-health effects and data-only request-independent coordination.
mod completion;
pub mod health;
pub mod providers;
mod store;
#[derive(Clone, Default)]
pub struct HealthState {
    writer: health::HealthWriter,
    active: providers::ActiveHealth,
}
pub struct ProviderHealth<'a, S, H> {
    store: &'a S,
    host: &'a H,
    health: health::HealthWriter,
    active_health: providers::ActiveHealth,
}
impl<'a, S, H> ProviderHealth<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H, state: HealthState) -> Self {
        Self {
            store,
            host,
            health: state.writer,
            active_health: state.active,
        }
    }
}
