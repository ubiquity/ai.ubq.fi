//! The fixed OpenRouter Decisions wire capability over the original extras admission.
use gateway_core::source_json::SourceJson;
use gateway_extras_runtime::types::{DispatchFailure, ExtrasHost, Headers, UpstreamResponse};
use std::future::Future;

pub const DECISIONS_URL: &str = "https://openrouter.ai/api/alpha/decisions";
/// OPENROUTER_DECISIONS_TIMEOUT_MS; it covers headers and the bounded body.
pub const DECISIONS_TIMEOUT_MS: i64 = 60_000;

/// Factory-only wire request: only this crate constructs it, always for the
/// fixed Decisions URL, so transport cannot receive a caller-selected target.
pub struct DecisionsCall {
    pub(crate) headers: Headers,
    pub(crate) body: SourceJson,
    pub(crate) deadline_ms: i64,
}
impl DecisionsCall {
    pub fn url(&self) -> &'static str {
        DECISIONS_URL
    }
    pub fn headers(&self) -> &Headers {
        &self.headers
    }
    pub fn body(&self) -> &SourceJson {
        &self.body
    }
    pub fn deadline_ms(&self) -> i64 {
        self.deadline_ms
    }
}

/// The extras host plus one fixed dispatch. The dispatch MUST await the original
/// single-child pretransport gate exactly as the SystemOne dispatch does.
pub trait DecisionsHost: ExtrasHost {
    fn dispatch_decisions(
        &self,
        call: DecisionsCall,
    ) -> impl Future<Output = Result<UpstreamResponse, DispatchFailure>>;
}
