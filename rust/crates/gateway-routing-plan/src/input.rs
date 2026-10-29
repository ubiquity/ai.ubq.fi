use gateway_auth_contracts::types::AuthPrincipal;
use gateway_catalog_input::catalog::CatalogInput;
use gateway_contracts::identity::RequestContext;
use gateway_local_refusal::routing_error::RoutingError;
use gateway_schema_values::request::RequestSurface;
use gateway_source::source_json::SourceJson;

/// Existing request metadata only. No caller supplies a replacement normalized body.
#[derive(Clone)]
pub struct LocalRequestMetadata {
    pub context: RequestContext,
    pub principal: AuthPrincipal,
    pub catalog: gateway_catalog_input::catalog::CatalogInput,
    pub runtime: Option<SourceJson>,
    pub client_version: String,
    pub native_session_id: Option<String>,
    pub retry_prohibited: bool,
}

#[derive(Clone, Debug)]
pub struct RoutingInput {
    pub context: RequestContext,
    pub principal: AuthPrincipal,
    pub raw: gateway_source::source_json::SourceJson,
    pub surface: ClientSurface,
    pub catalog: CatalogInput,
    pub runtime: Option<gateway_source::source_json::SourceJson>,
    pub client_version: String,
    pub native_session_id: Option<String>,
    /// Existing server-side request retry policy; never read from client JSON.
    pub retry_prohibited: bool,
}

impl RoutingInput {
    /// Neutral data construction only; validation remains in source-defined planning phases.
    pub fn from_source_request(
        metadata: LocalRequestMetadata,
        request: gateway_request_normalization::ordinary::OrdinarySourceRequest,
    ) -> Self {
        Self {
            context: metadata.context,
            principal: metadata.principal,
            raw: request.original_source().clone(),
            surface: match request.surface() {
                RequestSurface::Responses => ClientSurface::Responses,
                RequestSurface::ChatCompletions => ClientSurface::Chat,
            },
            catalog: metadata.catalog,
            runtime: metadata.runtime,
            client_version: metadata.client_version,
            native_session_id: metadata.native_session_id,
            retry_prohibited: metadata.retry_prohibited,
        }
    }
    pub fn original_source(&self) -> &gateway_source::source_json::SourceJson {
        &self.raw
    }
    pub fn source_member(&self, name: &str) -> Option<gateway_source::source_json::SourceJson> {
        gateway_source::source_serialization::member(&self.raw, name)
    }
    pub fn streamed(&self) -> bool {
        self.source_member("stream")
            .is_some_and(|value| value.as_str() == "true")
    }
    pub fn requested_reasoning(&self) -> Option<gateway_source::js_string::JsString16> {
        self.source_member("reasoning")
            .and_then(|value| gateway_source::source_serialization::member(&value, "effort"))
            .and_then(|value| gateway_source::source_serialization::string(&value))
            .or_else(|| {
                self.source_member("reasoning_effort")
                    .and_then(|value| gateway_source::source_serialization::string(&value))
            })
    }
}

pub type RoutingResult<T> = Result<T, RoutingError>;
