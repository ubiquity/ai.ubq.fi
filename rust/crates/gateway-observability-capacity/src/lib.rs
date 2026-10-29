//! Complete capacity capture, sampler lease and guarded snapshot persistence.
pub mod capacity;
mod capacity_store;
mod store;
use gateway_observability_capacity_events::CapacityEvents;
use std::{cell::Cell, rc::Rc};
#[derive(Clone, Default)]
pub struct CapacityState(Rc<Cell<i64>>);
pub struct CapacitySampler<'a, S, H> {
    store: &'a S,
    host: &'a H,
    sampled_bucket: Rc<Cell<i64>>,
}
impl<'a, S, H> CapacitySampler<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H, state: CapacityState) -> Self {
        Self {
            store,
            host,
            sampled_bucket: state.0,
        }
    }
    fn events(&self) -> CapacityEvents<'_, S, H> {
        CapacityEvents::new(self.store, self.host)
    }
}
