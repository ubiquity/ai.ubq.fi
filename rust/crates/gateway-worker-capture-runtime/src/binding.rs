//! Fixed resources projected by the actual request adapter; no authority is minted.
use crate::{host::CaptureHost, identity::CaptureBuild, runtime::CaptureRuntime};
use gateway_contracts::identity::RequestId;
use gateway_observability::{ObservabilityRuntime, ObservabilityState};
use gateway_observability_replay_model::replay_crypto::ReplayBody;
use gateway_worker_authority_binding::authority::RemoteAuthorityStore;
use gateway_worker_platform::cancellation::Cancellation;
use std::rc::Rc;
use worker::{Context, Env};

pub struct CaptureResources {
    env: Env,
    cancellation: Rc<Cancellation>,
    context: Rc<Context>,
}
impl CaptureResources {
    pub fn new(env: Env, cancellation: Rc<Cancellation>, context: Rc<Context>) -> Self {
        Self {
            env,
            cancellation,
            context,
        }
    }
}
impl CaptureRuntime {
    pub fn bind(
        request_id: &RequestId,
        original_body: &[u8],
        store: RemoteAuthorityStore,
        resources: CaptureResources,
        state: ObservabilityState,
        build: CaptureBuild,
    ) -> Self {
        let CaptureResources {
            env,
            cancellation,
            context: _context,
        } = resources;
        let host = CaptureHost::new(env, cancellation, build);
        let request_id = request_id.0.clone();
        let original_body = ReplayBody::from_vec(original_body.to_vec());
        Self {
            inner: ObservabilityRuntime::with_state(store, host, state),
            request_id,
            original_body,
        }
    }
}
