//! Event-owned source observability over the existing atomic authority.
//! No provider inference, credential mutation, scheduler, or second paid ledger.
pub mod api;
pub mod capacity;
pub mod capacity_events;
mod capacity_store;
pub mod claude_capacity;
pub mod incidents;
pub mod metered;
pub mod remote_capacity;
mod replay_bootstrap;
mod replay_cleanup;
mod replay_metadata;
pub mod replay_producer;
pub mod replay_read;
pub mod replay_retention;
pub mod replay_retention_schema;
pub mod replay_trace;
mod replay_values;
pub mod settings;
mod store;
pub mod telemetry;

use std::rc::Rc;

/// Isolate-owned coordination data only. It never retains a store, host, or I/O future.
#[derive(Clone, Default)]
pub struct ObservabilityState {
    health: gateway_observability_health::HealthState,
    sampled_bucket: Rc<std::cell::Cell<i64>>,
    replay_bootstrap: Rc<std::cell::RefCell<replay_bootstrap::OwnerIndex>>,
}

pub struct ObservabilityRuntime<S, H> {
    pub(crate) store: Rc<S>,
    pub(crate) host: Rc<H>,
    pub(crate) health: gateway_observability_health::HealthState,
    pub(crate) sampled_bucket: Rc<std::cell::Cell<i64>>,
    pub(crate) replay_bootstrap: Rc<std::cell::RefCell<replay_bootstrap::OwnerIndex>>,
}
impl<S, H> Clone for ObservabilityRuntime<S, H> {
    fn clone(&self) -> Self {
        Self {
            store: self.store.clone(),
            host: self.host.clone(),
            health: self.health.clone(),
            sampled_bucket: self.sampled_bucket.clone(),
            replay_bootstrap: self.replay_bootstrap.clone(),
        }
    }
}
impl<S, H> ObservabilityRuntime<S, H> {
    pub fn new(store: S, host: H) -> Self {
        Self::with_state(store, host, ObservabilityState::default())
    }
    pub fn with_state(store: S, host: H, state: ObservabilityState) -> Self {
        Self {
            store: Rc::new(store),
            host: Rc::new(host),
            health: state.health,
            sampled_bucket: state.sampled_bucket,
            replay_bootstrap: state.replay_bootstrap,
        }
    }
    pub fn shared_state(&self) -> ObservabilityState {
        ObservabilityState {
            health: self.health.clone(),
            sampled_bucket: self.sampled_bucket.clone(),
            replay_bootstrap: self.replay_bootstrap.clone(),
        }
    }
    pub fn store(&self) -> &S {
        &self.store
    }
    pub fn host(&self) -> &H {
        &self.host
    }
    pub fn ledger(&self) -> gateway_observability_ledger::RequestLedger<'_, S, H> {
        gateway_observability_ledger::RequestLedger::new(self.store.as_ref(), self.host.as_ref())
    }
    pub fn prompt(&self) -> gateway_observability_prompt::PromptAnalytics<'_, S, H> {
        gateway_observability_prompt::PromptAnalytics::new(self.store.as_ref(), self.host.as_ref())
    }
    pub fn health(&self) -> gateway_observability_health::ProviderHealth<'_, S, H> {
        gateway_observability_health::ProviderHealth::new(
            self.store.as_ref(),
            self.host.as_ref(),
            self.health.clone(),
        )
    }
}
