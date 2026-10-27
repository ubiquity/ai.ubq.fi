//! Exact routing refusal data; no provider, authority or transport dependency.
use crate::closed_error_body::ClosedErrorBody;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use std::collections::BTreeMap;
#[derive(Clone, Debug)]
pub struct RoutingError {
    pub error: Box<GatewayError>,
    pub headers: BTreeMap<String, String>,
    pub source_body: Option<SourceRefusalBody>,
    local_aggregate_context_overflow: bool,
}

#[derive(Clone, Debug)]
pub enum SourceRefusalBody {
    Json(SourceJson),
    ClosedError(ClosedErrorBody),
    /// Exact source HTTP-constructor failure; never a quota or paid proof.
    InternalServerError,
}
impl SourceRefusalBody {
    pub fn content_type(&self) -> &'static str {
        match self {
            Self::Json(_) | Self::ClosedError(_) => "application/json",
            Self::InternalServerError => "text/plain; charset=utf-8",
        }
    }
    pub fn as_bytes(&self) -> &[u8] {
        match self {
            Self::Json(body) => body.as_bytes(),
            Self::ClosedError(body) => body.as_bytes(),
            Self::InternalServerError => b"Internal Server Error",
        }
    }
}
impl std::fmt::Display for RoutingError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        std::fmt::Display::fmt(self.error.as_ref(), formatter)
    }
}
impl std::error::Error for RoutingError {}
impl RoutingError {
    /// Same ordinary data as the former literal; this constructor never selects the local kind.
    pub fn new(
        error: Box<GatewayError>,
        headers: BTreeMap<String, String>,
        source_body: Option<SourceRefusalBody>,
    ) -> Self {
        Self {
            error,
            headers,
            source_body,
            local_aggregate_context_overflow: false,
        }
    }
    pub fn is_local_aggregate_context_overflow(&self) -> bool {
        self.local_aggregate_context_overflow
    }

    /// Cleanup is an internal diagnostic; the original wire status/body remain authoritative.
    pub fn with_cleanup_diagnostic(mut self, cleanup: &RoutingError) -> Self {
        if self.source_body.is_none() {
            self.source_body = Some(SourceRefusalBody::ClosedError(
                ClosedErrorBody::from_scalar(&self.error),
            ));
        }
        let code = cleanup
            .error
            .envelope
            .error
            .code
            .as_deref()
            .unwrap_or("unknown")
            .chars()
            .take(48)
            .collect::<String>();
        self.error.envelope.error.message = format!(
            "Original request failure; original-lifetime cleanup also failed (status {}, code {}).",
            cleanup.error.status, code,
        );
        self
    }
}
impl From<GatewayError> for RoutingError {
    fn from(error: GatewayError) -> Self {
        Self {
            error: Box::new(error),
            headers: BTreeMap::new(),
            source_body: None,
            local_aggregate_context_overflow: false,
        }
    }
}
impl From<crate::source_error::SourceRequestError> for RoutingError {
    fn from(error: crate::source_error::SourceRequestError) -> Self {
        Self {
            local_aggregate_context_overflow: error.is_local_aggregate_context_overflow(),
            error: Box::new(error.diagnostic().clone()),
            headers: BTreeMap::new(),
            source_body: Some(SourceRefusalBody::ClosedError(error.body().clone())),
        }
    }
}
