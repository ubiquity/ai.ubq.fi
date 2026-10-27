//! Complete replay reservation, staging, publication, reaping and charge release effects.
pub mod index;
mod replay_cleanup;
pub mod replay_retention;
mod replay_values;
mod store;
use gateway_observability_replay_state::{ReplayBootstrapState, ReplayMetadata};
pub struct ReplayRetention<'a, S, H> {
    store: &'a S,
    host: &'a H,
    state: ReplayBootstrapState,
}
impl<'a, S, H> ReplayRetention<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H, state: ReplayBootstrapState) -> Self {
        Self { store, host, state }
    }
    fn metadata(&self) -> ReplayMetadata<'_, S, H> {
        ReplayMetadata::new(self.store, self.host, self.state.clone())
    }
}
