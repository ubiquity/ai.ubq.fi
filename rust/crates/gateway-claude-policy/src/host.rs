//! Fixed owner capabilities; none accepts a URL, caller proof, environment name or refresh token.
use gateway_auth_contracts::types::AuthHost;
use gateway_catalog_input::catalog::ProviderCatalog;
use gateway_catalog_provider::host::ProviderBinding;
use gateway_claude_roster::{
    claude::{AccountBinding, Bootstrap},
    claude_generation::StateGeneration,
};
use gateway_claude_usage_quota::observation::AcceptedObservation;
use gateway_contracts::model::ModelId;
use gateway_core::state::RevisionCheck;
use gateway_model_selection::selection::ProviderSelection;
use gateway_provider_protocol::types::Headers;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use std::{cmp::Ordering, future::Future};

#[derive(Clone, Debug, Default)]
pub struct PoolConfig {
    /// Existing explicitly injected bootstrap sources. No environment lookup occurs here.
    pub bootstrap: Bootstrap,
    pub allowed_key_ids: Vec<String>,
    pub generation: StateGeneration,
    /// Existing verified principal/native-session replay partition.
    pub replay_scope: String,
}

pub struct SelectionInput<'a> {
    pub key_id: &'a str,
    pub model: &'a ModelId,
    pub raw: &'a SourceJson,
    pub effective_effort: Option<JsString16>,
    /// The same server-owned source selection used for validation and the m14 plan.
    pub selection: Option<&'a ProviderSelection>,
}

#[derive(Debug)]
pub struct ResponseObservation {
    pub status: u16,
    pub headers: Headers,
    pub provider_request_id: Option<String>,
    /// Capture at actual header receipt, BEFORE awaiting any clone/body work.
    pub observed_at_ms: i64,
}

#[derive(Clone, Debug)]
pub struct HealthObservation {
    pub identity: String,
    pub event: String,
    pub status: Option<u16>,
    pub provider_request_id: Option<String>,
    pub checks: Vec<RevisionCheck>,
}

#[derive(Clone, Debug)]
pub struct WorkspaceResponse {
    pub status: u16,
    pub body: String,
}

pub trait ClaudePoolHost: AuthHost {
    /// The real source localeCompare primitive, not byte ordering.
    fn compare_principals(
        &self,
        left: &JsString16,
        right: &JsString16,
    ) -> Result<Ordering, GatewayError>;
    /// Source Date.parse for Retry-After's date form; invalid text returns None.
    fn parse_retry_after_date(&self, raw: &str) -> Option<i64>;
    /// Bind the existing source/native owner. Missing enforcement is explicit unavailability.
    fn credential_checks(
        &self,
        binding: &AccountBinding,
    ) -> impl Future<Output = Result<Vec<RevisionCheck>, GatewayError>>;
    /// Reuse m11's fixed Claude discovery with this exact m20 binding identity.
    fn catalog(
        &self,
        binding: &AccountBinding,
        identity: &ProviderBinding,
    ) -> impl Future<Output = Result<ProviderCatalog, GatewayError>>;
    /// Fixed GET https://api.anthropic.com/v1/organizations/workspaces, manual redirects,
    /// current account headers and cancellation. Parsing and owner update remain Rust below.
    fn workspace_list(
        &self,
        binding: &AccountBinding,
    ) -> impl Future<Output = Result<WorkspaceResponse, GatewayError>>;
    fn accepted_observation(
        &self,
        event: &AcceptedObservation,
    ) -> impl Future<Output = Result<(), GatewayError>>;
    fn health_observation(
        &self,
        event: &HealthObservation,
    ) -> impl Future<Output = Result<(), GatewayError>>;
}

pub fn revision<H: AuthHost>(host: &H) -> Result<String, GatewayError> {
    uuid_from_random(&host.random_bytes(16)?)
}
pub fn now<H: AuthHost>(host: &H) -> Result<i64, GatewayError> {
    let now = host.now_ms();
    if (1..=gateway_contracts::identity::MAX_SAFE_INTEGER).contains(&now) {
        Ok(now)
    } else {
        Err(crate::error::unavailable("Claude source clock is invalid."))
    }
}

use gateway_auth_primitives::crypto::uuid_from_random;
