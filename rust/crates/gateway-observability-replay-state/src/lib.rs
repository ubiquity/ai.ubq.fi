//! Complete replay bootstrap and metadata admission with data-only shared owner index.
mod replay_bootstrap;
mod replay_metadata;
mod replay_values;
mod store;
use std::{cell::RefCell, rc::Rc};
#[derive(Clone, Default)]
pub struct ReplayBootstrapState(Rc<RefCell<replay_bootstrap::OwnerIndex>>);
pub struct ReplayMetadata<'a, S, H> {
    store: &'a S,
    host: &'a H,
    replay_bootstrap: Rc<RefCell<replay_bootstrap::OwnerIndex>>,
}
impl<'a, S, H> ReplayMetadata<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H, state: ReplayBootstrapState) -> Self {
        Self {
            store,
            host,
            replay_bootstrap: state.0,
        }
    }
}
