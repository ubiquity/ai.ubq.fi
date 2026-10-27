use crate::response_body::ExtrasResponseBody;
use gateway_auth_contracts::types::AuthPrincipal;
use gateway_core::{identity::RequestContext, lifecycle::SemanticTerminal, model::ModelId};
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_provider_images::extras::MultipartFile;
use gateway_provider_protocol::{types::PrepareOptions, types::PreparedRequest};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::Value;
use std::{collections::BTreeMap, future::Future};

pub type Headers = BTreeMap<String, String>;
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ImageKind {
    Generations,
    Edits,
}
impl ImageKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Generations => "generations",
            Self::Edits => "edits",
        }
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ExtrasRoute {
    Embeddings,
    JobCreate,
    JobGet(String),
    Images(ImageKind),
    SystemOne,
    Decisions,
}
impl ExtrasRoute {
    pub fn label(&self) -> &'static str {
        match self {
            Self::Embeddings => "embeddings",
            Self::JobCreate => "embeddings.jobs.create",
            Self::JobGet(_) => "embeddings.jobs.get",
            Self::Images(ImageKind::Generations) => "images.generations",
            Self::Images(ImageKind::Edits) => "images.edits",
            Self::SystemOne => "systemone",
            Self::Decisions => "decisions",
        }
    }
}
/// The HTTP host has already authenticated and bounded the actual bytes. Multipart
/// preserves scalar fields as JSON and file metadata/bytes separately; no URL proxy.
pub struct ExtrasRequest<'a> {
    pub route: ExtrasRoute,
    pub body: SourceJson,
    pub multipart: Option<Vec<MultipartFile>>,
    pub raw_body_bytes: usize,
    pub idempotency_key: Option<String>,
    pub principal: &'a AuthPrincipal,
    pub context: &'a RequestContext,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ExtrasProvider {
    Voyage,
    SystemOne,
    Decisions,
}
/// Factory-only prepared wire request: only the fixed source Voyage/SystemOne
/// factories construct this type; transport cannot receive an arbitrary URL.
pub struct PreparedCall {
    prepared: PreparedRequest,
    deadline_ms: i64,
}
impl PreparedCall {
    pub fn prepare(
        provider: ExtrasProvider,
        body: &SourceJson,
        options: &PrepareOptions,
        deadline_ms: i64,
    ) -> Result<Self, SourceRequestError> {
        let prepared = match provider {
            ExtrasProvider::Voyage => {
                gateway_provider_services::extras::prepare_source_embeddings(body, options)?
            }
            ExtrasProvider::SystemOne => {
                gateway_provider_services::extras::prepare_source_systemone(body, options)?
            }
            ExtrasProvider::Decisions => {
                gateway_provider_services::extras::prepare_source_decisions(body, options)?
            }
        };
        Ok(Self {
            prepared,
            deadline_ms,
        })
    }

    pub fn prepared(&self) -> &PreparedRequest {
        &self.prepared
    }
    pub fn deadline_ms(&self) -> i64 {
        self.deadline_ms
    }
}
pub enum ExtrasDispatch {
    Voyage(PreparedCall),
    SystemOne(PreparedCall),
    Decisions(PreparedCall),
    /// This typed variant always uses the m14 Codex-only original child scope.
    ImageChild {
        index: usize,
        kind: ImageKind,
        request_id: String,
        body: SourceJson,
    },
}
#[derive(Clone, Debug)]
pub struct UpstreamResponse {
    pub status: u16,
    pub headers: Headers,
    pub body: Vec<u8>,
    /// Supplied only by the actual child decoder; status/EOF never invent it.
    pub semantic_terminal: Option<SemanticTerminal>,
    pub usage: Option<Value>,
}
#[derive(Clone, Debug)]
pub struct DispatchFailure {
    pub origin: DispatchFailureOrigin,
    pub error: Box<GatewayError>,
    pub headers: Headers,
    /// Quota failure is independent of upstream rejection or transport failure.
    pub quota_refusal: bool,
    pub upstream_status: Option<u16>,
    pub retry_after_ms: Option<u64>,
    pub before_transport: bool,
    pub raw_error_body: Option<Vec<u8>>,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DispatchFailureOrigin {
    Gateway,
    Transport,
    Upstream,
}
impl DispatchFailure {
    pub fn transport(error: GatewayError) -> Self {
        Self {
            origin: DispatchFailureOrigin::Transport,
            error: Box::new(error),
            headers: Headers::new(),
            quota_refusal: false,
            upstream_status: None,
            retry_after_ms: None,
            before_transport: false,
            raw_error_body: None,
        }
    }
    pub fn gateway(error: GatewayError, before_transport: bool) -> Self {
        Self {
            origin: DispatchFailureOrigin::Gateway,
            error: Box::new(error),
            headers: Headers::new(),
            quota_refusal: false,
            upstream_status: None,
            retry_after_ms: None,
            before_transport,
            raw_error_body: None,
        }
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum KernelDisposition {
    Completed,
    Incomplete(&'static str),
    FromAggregate,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HealthKind {
    Success,
    AuthInvalid,
    QuotaExhausted,
    UpstreamError,
    Reachable,
}
#[derive(Clone, Debug)]
pub struct HealthObservation {
    pub provider: ExtrasProvider,
    pub kind: HealthKind,
    pub upstream_status: Option<u16>,
}
#[derive(Clone, Debug)]
pub struct ExtrasResponse {
    pub status: u16,
    pub headers: Headers,
    pub body: ExtrasResponseBody,
    pub semantic_terminal: Option<SemanticTerminal>,
    pub usage: Option<Value>,
    pub kernel: KernelDisposition,
    pub failure_kind: Option<String>,
    /// Private telemetry/capture evidence; the HTTP host serializes only body.
    pub upstream_errors: Vec<DispatchFailure>,
    pub health: Vec<HealthObservation>,
}
impl ExtrasResponse {
    pub fn json(status: u16, body: Value) -> Self {
        Self::source_json(
            status,
            SourceJson::from_serializable(&body).expect("actual scalar response"),
        )
    }
    pub fn source_error(error: SourceRequestError) -> Self {
        Self::with_body(
            error.status,
            ExtrasResponseBody::ClosedError(error.body().clone()),
        )
    }
    pub fn source_json(status: u16, body: SourceJson) -> Self {
        Self::with_body(status, ExtrasResponseBody::Json(body))
    }
    fn with_body(status: u16, body: ExtrasResponseBody) -> Self {
        Self {
            status,
            body,
            headers: Headers::from([("content-type".into(), "application/json".into())]),
            semantic_terminal: None,
            usage: None,
            kernel: KernelDisposition::FromAggregate,
            failure_kind: None,
            upstream_errors: Vec::new(),
            health: Vec::new(),
        }
    }
    pub fn error(error: GatewayError) -> Self {
        Self::json(
            error.status,
            serde_json::to_value(error.envelope).expect("error envelope serializes"),
        )
    }
    pub fn voyage(mut self) -> Self {
        self.headers
            .insert("x-uos-upstream".into(), "voyage".into());
        self
    }
}
/// Per-original-request capability built by the Worker over m14's opaque shared
/// AdmissionHandle. Every dispatch MUST await its original pretransport gate.
/// No child may begin another admission, settle kernel, or authorize paid images.
/// All cryptographic operations are real AEAD; absence returns an explicit error.
pub trait ExtrasHost {
    fn now_ms(&self) -> i64;
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError>;
    fn cancelled(&self) -> bool;
    fn sleep_ms(&self, millis: u64) -> impl Future<Output = Result<(), GatewayError>>;
    /// Actual configured credential snapshot. Voyage resolves env first, KV second;
    /// SystemOne uses the existing OpenRouter credential. None means unconfigured.
    fn provider_options(
        &self,
        provider: ExtrasProvider,
    ) -> impl Future<Output = Result<Option<PrepareOptions>, GatewayError>>;
    fn decision_models(&self) -> impl Future<Output = Result<Vec<ModelId>, GatewayError>> {
        async { Ok(Vec::new()) }
    }
    fn image_base_model(&self) -> impl Future<Output = Result<Option<ModelId>, GatewayError>>;
    fn dispatch(
        &self,
        operation: ExtrasDispatch,
    ) -> impl Future<Output = Result<UpstreamResponse, DispatchFailure>>;
    fn begin_image_fanout(
        &self,
        kind: ImageKind,
        count: usize,
    ) -> impl Future<Output = Result<(), GatewayError>>;
    fn abort_image_siblings(&self);
    fn image_child_finished(&self, index: usize) -> impl Future<Output = Result<(), GatewayError>>;
    /// Waits for all logical children and cancels the original dispatch only if
    /// none reached transport. It does not finish the original kernel/terminal.
    fn finish_image_fanout(&self) -> impl Future<Output = Result<(), GatewayError>>;
    fn aes_gcm_encrypt(
        &self,
        key: &[u8; 32],
        iv: &[u8; 12],
        plaintext: &[u8],
    ) -> impl Future<Output = Result<Vec<u8>, GatewayError>>;
    fn aes_gcm_decrypt(
        &self,
        key: &[u8; 32],
        iv: &[u8],
        ciphertext_and_tag: &[u8],
    ) -> impl Future<Output = Result<Vec<u8>, GatewayError>>;
}
