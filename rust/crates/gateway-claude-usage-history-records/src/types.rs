use gateway_core::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::{Deserialize, Serialize};
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum CredentialKind {
    ApiKey,
    Oauth,
}
/// Source ClaudeCodeAcceptedObservation, emitted only after its current-binding quota CAS.
/// History/health subscribers never write quota or credential state from this event.
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct ClaudeAcceptedObservation {
    pub v: u8,
    pub observation_id: String,
    pub account_id: String,
    pub account_cohort_id: String,
    pub credential_kind: CredentialKind,
    pub workspace_id: Option<JsString16>,
    pub source_observed_at_ms: Option<i64>,
    pub sampled_at_ms: i64,
    pub response_status: Option<u16>,
    pub state: ClaudeObservationState,
    pub observation: SourceJson,
}
impl ClaudeAcceptedObservation {
    pub fn from_source(value: &SourceJson) -> Result<Self, GatewayError> {
        value.deserialize()
    }
}
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ClaudeObservationState {
    Available,
    Unavailable,
}
