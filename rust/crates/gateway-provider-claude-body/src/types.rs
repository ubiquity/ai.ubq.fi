use gateway_core::js_string::JsString16;
use gateway_core::model::ModelId;
use gateway_core::source_json::SourceJson;
use gateway_provider_protocol::types::ClientSurface;
use gateway_provider_protocol::types::OrdinaryPreparationContext;
use gateway_provider_protocol::types::PrepareOptions;
use gateway_provider_protocol::types::DecodePlan;

/// Closed selected-credential kind data, never a credential or dispatch grant.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ClaudeCredentialKind {
    ApiKey,
    Oauth,
}


/// Inputs consumed only by the ordinary source prelude/coordinator, before account capability lookup.
#[derive(Clone, Debug, Default)]
pub struct OrdinaryPhaseOptions {
    pub allowed_models: Vec<ModelId>,
    pub ordinary_preparation: Option<OrdinaryPreparationContext>,
}


/// One already-normalized Claude ordinary outcome. Cloning retains immutable data
/// only: no auth, original transcript, native proof, serialization or transport grant.
#[derive(Clone, Debug)]
pub struct PreparedClaudeOrdinary {
    pub(crate) normalized: SourceJson,
    pub(crate) surface: ClientSurface,
    pub(crate) requested: ModelId,
    pub(crate) model: ModelId,
    pub(crate) client_stream: bool,
    pub(crate) effective_effort: Option<JsString16>,
}


impl PreparedClaudeOrdinary {
    pub fn effective_effort(&self) -> Option<&JsString16> {
        self.effective_effort.as_ref()
    }
    pub fn surface(&self) -> ClientSurface {
        self.surface
    }
    pub fn model(&self) -> &ModelId {
        &self.model
    }
}


/// Claude body/decode metadata only. No auth, secret or workspace material.
#[derive(Clone, Debug)]
pub struct ClaudeBodyOptions {
    pub kind: ClaudeCredentialKind,
    pub response_id: String,
    pub created_at: i64,
    pub allowed_models: Vec<ModelId>,
    pub native_reasoning_efforts: Option<Vec<JsString16>>,
    pub supports_disabled_thinking: bool,
    pub reasoning_summary: Option<JsString16>,
    pub restored_native_messages: Option<Vec<SourceJson>>,
    pub ordinary_preparation: Option<OrdinaryPreparationContext>,
}


impl ClaudeBodyOptions {
    pub fn new(kind: ClaudeCredentialKind) -> Self {
        Self {
            kind,
            response_id: "resp_gateway".into(),
            created_at: 0,
            allowed_models: Vec::new(),
            native_reasoning_efforts: None,
            supports_disabled_thinking: false,
            reasoning_summary: None,
            restored_native_messages: None,
            ordinary_preparation: None,
        }
    }
    pub(crate) fn from_scalar(options: &PrepareOptions) -> Self {
        Self {
            kind: if options.auth.api_key {
                ClaudeCredentialKind::ApiKey
            } else {
                ClaudeCredentialKind::Oauth
            },
            response_id: options.response_id.clone(),
            created_at: options.created_at,
            allowed_models: options.allowed_models.clone(),
            native_reasoning_efforts: options.native_reasoning_efforts.clone(),
            supports_disabled_thinking: options.supports_disabled_thinking,
            reasoning_summary: options.reasoning_summary.clone(),
            restored_native_messages: options.restored_native_messages.clone(),
            ordinary_preparation: options.ordinary_preparation.clone(),
        }
    }
}


/// Factory-owned canonical body data. This is not a Ready request or I/O token.
#[derive(Clone, Debug)]
pub struct PreparedClaudeBody {
    pub(crate) body: SourceJson,
    pub(crate) decode: DecodePlan,
    pub(crate) warnings: Vec<String>,
    pub(crate) stream: bool,
    pub(crate) kind: ClaudeCredentialKind,
}


impl PreparedClaudeBody {
    pub fn body(&self) -> &SourceJson {
        &self.body
    }
    pub fn decode(&self) -> &DecodePlan {
        &self.decode
    }
    pub fn warnings(&self) -> &[String] {
        &self.warnings
    }
    pub fn stream(&self) -> bool {
        self.stream
    }
    pub fn kind(&self) -> ClaudeCredentialKind {
        self.kind
    }
    pub fn url(&self) -> &'static str {
        "https://api.anthropic.com/v1/messages"
    }
    pub fn method(&self) -> &'static str {
        "POST"
    }
}
