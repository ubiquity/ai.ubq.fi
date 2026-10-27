use gateway_provider_source::source as s;
use gateway_provider_tool_grammar::source_tools::ToolMap;
use gateway_core::error::GatewayError;
use gateway_core::js_string::JsString16;
use gateway_core::model::ModelId;
use gateway_core::provider::UpstreamProvider;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization;
use serde::Deserialize;
use serde::Serialize;
use serde_json::Value;
use std::collections::BTreeMap;
use gateway_provider_source::types::ChatProfile;

pub type Headers = BTreeMap<String, String>;


#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum ClientSurface {
    Responses,
    Chat,
    SystemOne,
    Decisions,
    Embeddings,
}


#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum ResponseDecode {
    Responses,
    Chat,
    ChatToResponses(ChatProfile),
    ResponsesToChat,
    AnthropicToResponses,
    AnthropicChatToResponses,
    Voyage,
    SystemOne,
    Decisions,
}



/// Secrets are supplied by the host-owned credential capability; never discovered here.
#[derive(Clone, Default)]
pub struct AuthMaterial {
    pub secret: String,
    pub account_id: Option<String>,
    pub api_key: bool,
    pub workspace_id: Option<String>,
}


impl std::fmt::Debug for AuthMaterial {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("AuthMaterial")
            .field("secret", &"[redacted]")
            .field("api_key", &self.api_key)
            .finish_non_exhaustive()
    }
}


/// Immutable Rust-only selected-model observations, never request JSON or authority.
#[derive(Clone, Debug)]
pub struct OrdinaryPreparationContext {
    prompt_cache: Option<SourceJson>,
    reasoning: gateway_schema::reasoning::ModelReasoning,
    default_effort: JsString16,
}


impl OrdinaryPreparationContext {
    pub fn from_metadata(
        metadata: &gateway_schema::catalog::RequestModelMetadata,
        default_effort: &JsString16,
    ) -> Self {
        Self {
            prompt_cache: metadata.prompt_cache(),
            reasoning: metadata.reasoning.clone(),
            default_effort: default_effort.clone(),
        }
    }
    pub fn prompt_cache(&self) -> Option<&SourceJson> {
        self.prompt_cache.as_ref()
    }
    pub fn reasoning(&self) -> &gateway_schema::reasoning::ModelReasoning {
        &self.reasoning
    }
    pub fn default_effort(&self) -> &JsString16 {
        &self.default_effort
    }
}


#[derive(Clone, Debug)]
pub struct PrepareOptions {
    pub auth: AuthMaterial,
    pub client_version: String,
    pub native_session_id: Option<String>,
    pub response_id: String,
    pub created_at: i64,
    /// Host-supplied discovery evidence; absence must not invent provider support.
    pub allowed_models: Vec<ModelId>,
    pub catalog_max_output_tokens: Option<u64>,
    pub native_reasoning_efforts: Option<Vec<JsString16>>,
    pub supports_native_effort: bool,
    pub supports_disabled_thinking: bool,
    pub supports_parallel_tools: Option<bool>,
    /// Random per request unless a verified native-session identity is supplied.
    pub conversation_id: Option<String>,
    pub reasoning_summary: Option<JsString16>,
    /// Restored by ReplayStore in the same authenticated context, never by client input.
    pub restored_native_messages: Option<Vec<SourceJson>>,
    /// Runtime callers supply Some even for unknown metadata. None is the legacy pure adapter contract.
    pub ordinary_preparation: Option<OrdinaryPreparationContext>,
}


impl Default for PrepareOptions {
    fn default() -> Self {
        Self {
            auth: AuthMaterial::default(),
            client_version: "0.100.0".into(),
            native_session_id: None,
            response_id: "resp_gateway".into(),
            created_at: 0,
            allowed_models: Vec::new(),
            catalog_max_output_tokens: None,
            native_reasoning_efforts: None,
            supports_native_effort: false,
            supports_disabled_thinking: false,
            supports_parallel_tools: None,
            conversation_id: None,
            reasoning_summary: None,
            restored_native_messages: None,
            ordinary_preparation: None,
        }
    }
}


#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DecodePlan {
    pub provider: UpstreamProvider,
    pub surface: ClientSurface,
    pub decode: ResponseDecode,
    pub model: ModelId,
    pub upstream_model: ModelId,
    pub response_id: String,
    pub created_at: i64,
    pub stream: bool,
    pub request: SourceJson,
    pub tool_names: ToolMap,
    pub reasoning_summary: Option<JsString16>,
}


#[derive(Clone)]
pub struct PreparedRequest {
    pub provider: UpstreamProvider,
    pub url: String,
    pub method: String,
    pub headers: Headers,
    pub body: SourceJson,
    pub decode: DecodePlan,
    pub warnings: Vec<String>,
}


impl std::fmt::Debug for PreparedRequest {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PreparedRequest")
            .field("provider", &self.provider)
            .field("url", &self.url)
            .field("method", &self.method)
            .field("header_names", &self.headers.keys().collect::<Vec<_>>())
            .field("body", &"[request body omitted]")
            .field("decode", &self.decode.decode)
            .finish()
    }
}


#[derive(Clone)]
pub struct DecodedResponse {
    pub status: u16,
    pub headers: Headers,
    pub body: SourceJson,
    pub events: Vec<gateway_provider_events::event::ProviderEvent>,
    pub terminal: Option<gateway_core::lifecycle::SemanticTerminal>,
    pub usage: Option<Value>,
    pub native_replay: Option<Value>,
    pub raw_body: Option<Vec<u8>>,
}


#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Elision {
    pub path: String,
    pub call_id: Option<JsString16>,
    pub kind: String,
    pub original_bytes: usize,
    pub forwarded_bytes: usize,
    pub omitted_bytes: usize,
}


pub fn invalid(message: impl Into<String>, param: &str) -> GatewayError {
    GatewayError::new(400, message, Some("invalid_request_error")).with_param(Some(param))
}


pub fn header<'a>(headers: &'a Headers, name: &str) -> Option<&'a str> {
    headers
        .iter()
        .find(|(key, _)| key.eq_ignore_ascii_case(name))
        .map(|(_, v)| v.as_str())
}



impl std::fmt::Debug for DecodedResponse {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("DecodedResponse")
            .field("status", &self.status)
            .field("terminal", &self.terminal)
            .field("event_count", &self.events.len())
            .field("native_replay", &self.native_replay.is_some())
            .finish_non_exhaustive()
    }
}


pub fn prepared_post_data(provider: UpstreamProvider, url: String, headers: Headers, body: SourceJson, decode: DecodePlan) -> PreparedRequest {
    PreparedRequest { provider, url, method: "POST".into(), headers, body, decode, warnings: Vec::new() }
}
