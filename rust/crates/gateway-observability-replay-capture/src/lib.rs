//! Complete request-owned accepted replay input and failure capture production.
pub mod replay_producer;
use gateway_observability_incidents::IncidentIndex;
use gateway_observability_replay_read::duplicate::ReplayDuplicates;
use gateway_observability_replay_retention::ReplayRetention;
use gateway_observability_replay_state::{ReplayBootstrapState, ReplayMetadata};
pub struct ReplayCapture<'a, S, H> {
    store: &'a S,
    host: &'a H,
    state: ReplayBootstrapState,
}
impl<'a, S, H> ReplayCapture<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H, state: ReplayBootstrapState) -> Self {
        Self { store, host, state }
    }
    fn replay_state(&self) -> ReplayMetadata<'_, S, H> {
        ReplayMetadata::new(self.store, self.host, self.state.clone())
    }
    fn retention(&self) -> ReplayRetention<'_, S, H> {
        ReplayRetention::new(self.store, self.host, self.state.clone())
    }
    fn incidents(&self) -> IncidentIndex<'_, S, H> {
        IncidentIndex::new(self.store, self.host)
    }
    fn duplicates(&self) -> ReplayDuplicates<'_, S, H> {
        ReplayDuplicates::new(self.store, self.host, self.state.clone())
    }
}
