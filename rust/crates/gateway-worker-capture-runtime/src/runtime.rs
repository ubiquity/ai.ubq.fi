//! Complete fixed capture operations over the actual bound store and platform resources.
use crate::host::CaptureHost;
use gateway_observability::ObservabilityRuntime;
use gateway_observability_ledger::ledger::{
    ErrorObservation, terminal_rollup::TerminalUsageRollup,
};
use gateway_observability_prompt::prompt::PromptEvent;
use gateway_observability_replay_capture::replay_producer::{self, PersistResult, ReplayInput};
use gateway_observability_replay_model::replay_crypto::{self, ReplayBody};
use gateway_observability_replay_observation::{
    observation::{ClientFailureObservation, FailureObservation},
    replay_trace::UpstreamTrace,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use gateway_worker_authority_binding::authority::RemoteAuthorityStore;
use serde_json::Value;

pub struct CaptureRuntime {
    pub(crate) inner: ObservabilityRuntime<RemoteAuthorityStore, CaptureHost>,
    pub(crate) request_id: String,
    pub(crate) original_body: ReplayBody,
}
pub enum CaptureInput {
    Ready(ReplayInput),
    Omitted(&'static str),
}
impl CaptureRuntime {
    pub async fn record_request_log_source(
        &self,
        key_id: &str,
        input: &SourceJson,
    ) -> Result<(), GatewayError> {
        self.inner
            .ledger()
            .record_request_log_source(key_id, input)
            .await
    }
    pub async fn update_request_log_source(
        &self,
        key_id: &str,
        created_at_ms: i64,
        request_id: &str,
        patch: &SourceJson,
    ) -> Result<(), GatewayError> {
        self.inner
            .ledger()
            .update_request_log_source(key_id, created_at_ms, request_id, patch)
            .await
    }
    pub async fn record_prompt_telemetry(&self, event: &PromptEvent, release: &str) -> Value {
        self.inner
            .prompt()
            .record_prompt_telemetry(event, release)
            .await
    }
    pub async fn record_prompt_analytics(&self, event: &PromptEvent, release: &str) -> Value {
        self.inner
            .prompt()
            .record_prompt_analytics(event, release)
            .await
    }
    pub async fn record_terminal_usage_rollup(
        &self,
        input: TerminalUsageRollup<'_>,
    ) -> Result<bool, GatewayError> {
        self.inner
            .ledger()
            .record_terminal_usage_rollup(input)
            .await
    }
    pub async fn record_admin_error(&self, input: &ErrorObservation) -> Result<bool, GatewayError> {
        self.inner.ledger().record_admin_error(input).await
    }
    pub async fn record_capture_omission(
        &self,
        request_id: &str,
        reason: &str,
    ) -> Result<bool, GatewayError> {
        self.inner
            .captures()
            .record_capture_omission(request_id, reason)
            .await
    }
    pub fn capture_input(
        &self,
        request: &worker::Request,
        upstream: UpstreamTrace,
    ) -> Result<CaptureInput, GatewayError> {
        if request.method() != worker::Method::Post {
            return Ok(CaptureInput::Omitted("non_post"));
        }
        if self.original_body.as_bytes().len() > replay_crypto::MAX_REQUEST_BYTES {
            return Ok(CaptureInput::Omitted("body_over_limit"));
        }
        let url = request
            .url()
            .map_err(|_| unavailable("Capture request URL is invalid."))?;
        let endpoint = format!(
            "{}{}",
            url.path(),
            url.query().map(|q| format!("?{q}")).unwrap_or_default()
        );
        let headers = request.headers().entries().collect();
        let request = replay_producer::ReplayRequest {
            endpoint,
            method: "POST".into(),
            body: ReplayBody::from_vec(self.original_body.as_bytes().to_vec()),
            content_type: request
                .headers()
                .get("content-type")
                .map_err(|_| unavailable("Capture request headers are invalid."))?,
            headers,
            request_id: self.request_id.clone(),
            upstream,
        };
        Ok(CaptureInput::Ready(replay_producer::ReplayInput::new(
            self.inner.host(),
            request,
        )?))
    }
    pub async fn persist_failure(
        &self,
        input: ReplayInput,
        observation: FailureObservation,
        client: ClientFailureObservation,
    ) -> Result<PersistResult, GatewayError> {
        let key = self.inner.host().replay_key();
        self.inner
            .captures()
            .persist_failure(input, observation, client, key.as_ref())
            .await
    }
}
fn unavailable(message: &str) -> GatewayError {
    GatewayError::unavailable(message, "observability_host_unavailable")
}
