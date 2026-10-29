//! Real primitives for the closed capture service, with no native or administrative effects.
use crate::identity::CaptureBuild;
use gateway_observability_contracts::{host_types::BuildIdentity, types::ObservabilityHost};
use gateway_observability_replay_model::replay_crypto::ReplayKey;
use gateway_source::error::GatewayError;
use gateway_worker_platform::cancellation::Cancellation;
use std::{cmp::Ordering, rc::Rc};
use worker::Env;

pub(crate) struct CaptureHost {
    env: Env,
    cancellation: Rc<Cancellation>,
    build: CaptureBuild,
}
impl CaptureHost {
    pub(crate) fn new(env: Env, cancellation: Rc<Cancellation>, build: CaptureBuild) -> Self {
        Self {
            env,
            cancellation,
            build,
        }
    }
    pub(crate) fn replay_key(&self) -> Option<ReplayKey> {
        self.env
            .secret("SENTINEL_REPLAY_KEY")
            .map(|value| value.to_string())
            .or_else(|_| {
                self.env
                    .var("SENTINEL_REPLAY_KEY")
                    .map(|value| value.to_string())
            })
            .ok()
            .and_then(|raw| {
                ReplayKey::decode(raw.trim_matches(gateway_schema_values::scalar::js_whitespace))
                    .ok()
            })
    }
}
impl ObservabilityHost for CaptureHost {
    fn source_string_collator(
        &self,
    ) -> std::sync::Arc<dyn gateway_model_selection::aliases::SourceStringCollator> {
        gateway_worker_platform::source_collator::shared()
    }
    fn now_ms(&self) -> i64 {
        gateway_worker_storage::store::now_ms().unwrap_or(i64::MAX)
    }
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        gateway_worker_platform::random::random_bytes(length)
    }
    fn compare_labels(&self, left: &str, right: &str) -> Result<Ordering, GatewayError> {
        gateway_worker_platform::labels::compare_labels(left, right)
    }
    async fn wait_ms(&self, milliseconds: u32) -> Result<(), GatewayError> {
        if self.cancellation.signal().aborted() {
            return Err(GatewayError::unavailable(
                "Observational request cancelled.",
                "observability_request_cancelled",
            ));
        }
        worker::Delay::from(std::time::Duration::from_millis(u64::from(milliseconds))).await;
        if self.cancellation.signal().aborted() {
            return Err(GatewayError::unavailable(
                "Observational request cancelled.",
                "observability_request_cancelled",
            ));
        }
        Ok(())
    }
    fn build_identity(&self) -> Result<BuildIdentity, GatewayError> {
        let identity = BuildIdentity {
            git_sha: self.build.git_sha().into(),
            deployment_id: self.build.deployment_id().into(),
        };
        identity.validate()?;
        Ok(identity)
    }
}
