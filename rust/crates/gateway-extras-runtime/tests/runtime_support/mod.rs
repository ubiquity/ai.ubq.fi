use aes_gcm::{Aes256Gcm, KeyInit, Nonce, aead::Aead};
use gateway_auth_contracts::types::{AuthMethod, AuthPrincipal};
use gateway_auth_primitives::{api_keys::ApiKeyPolicy, crypto::Secret};
use gateway_core::{
    identity::{ApiKeyId, PrincipalId, RequestContext, RequestId},
    lifecycle::SemanticTerminal,
};
use gateway_extras_contract::types::{
    DispatchFailure, ExtrasDispatch, ExtrasHost, ExtrasProvider, ExtrasRequest, ExtrasRoute,
    Headers, ImageKind, UpstreamResponse,
};
use gateway_extras_runtime_test_support::fixture::{Memory, error};
use gateway_provider_protocol::types::PrepareOptions;
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    collections::VecDeque,
    future::Future,
    pin::Pin,
    rc::Rc,
    task::{Context, Poll, Waker},
};

pub(super) fn poll<F: Future>(future: Pin<&mut F>) -> Poll<F::Output> {
    future.poll(&mut Context::from_waker(Waker::noop()))
}
pub(super) fn run<F: Future>(future: F) -> F::Output {
    let mut f = Box::pin(future);
    for _ in 0..10_000 {
        if let Poll::Ready(result) = poll(f.as_mut()) {
            return result;
        }
    }
    panic!("fixture remained pending")
}
#[derive(Clone)]
pub(super) struct Host(pub(super) Rc<HostState>);
pub(super) struct HostState {
    pub(super) clock: Rc<Cell<i64>>,
    rng: Cell<u64>,
    pub(super) configured: Cell<bool>,
    pub(super) cancelled: Cell<bool>,
    pub(super) pending: Cell<bool>,
    pub(super) calls: RefCell<Vec<(String, Value)>>,
    pub(super) replies: RefCell<VecDeque<Result<UpstreamResponse, DispatchFailure>>>,
    pub(super) sleeps: RefCell<Vec<u64>>,
    pub(super) begins: Cell<usize>,
    pub(super) finished: Cell<usize>,
    pub(super) children: RefCell<Vec<usize>>,
    pub(super) aborted: Cell<bool>,
    pub(super) decrypts: Cell<usize>,
}
impl Host {
    fn new(clock: Rc<Cell<i64>>) -> Self {
        Self(Rc::new(HostState {
            clock,
            rng: Cell::new(1),
            configured: Cell::new(true),
            cancelled: Cell::new(false),
            pending: Cell::new(false),
            calls: RefCell::new(Vec::new()),
            replies: RefCell::new(VecDeque::new()),
            sleeps: RefCell::new(Vec::new()),
            begins: Cell::new(0),
            finished: Cell::new(0),
            children: RefCell::new(Vec::new()),
            aborted: Cell::new(false),
            decrypts: Cell::new(0),
        }))
    }
    pub(super) fn reply(&self, status: u16, body: Value) {
        self.0.replies.borrow_mut().push_back(Ok(UpstreamResponse {
            status,
            headers: Headers::new(),
            body: serde_json::to_vec(&body).unwrap(),
            semantic_terminal: None,
            usage: None,
        }));
    }
    pub(super) fn count(&self) -> usize {
        self.0.calls.borrow().len()
    }
}
impl ExtrasHost for Host {
    fn now_ms(&self) -> i64 {
        self.0.clock.get()
    }
    fn random_bytes(&self, len: usize) -> Result<Vec<u8>, GatewayError> {
        let n = self.0.rng.get();
        self.0.rng.set(n + 1);
        Ok((0..len)
            .map(|i| ((n >> ((i % 8) * 8)) as u8).wrapping_add(i as u8))
            .collect())
    }
    fn cancelled(&self) -> bool {
        self.0.cancelled.get()
    }
    async fn sleep_ms(&self, ms: u64) -> Result<(), GatewayError> {
        self.0.sleeps.borrow_mut().push(ms);
        self.0.clock.set(self.now_ms() + ms as i64);
        Ok(())
    }
    async fn provider_options(
        &self,
        _: ExtrasProvider,
    ) -> Result<Option<PrepareOptions>, GatewayError> {
        Ok(self.0.configured.get().then(|| PrepareOptions {
            auth: gateway_provider_protocol::types::AuthMaterial {
                secret: "fixture-not-a-credential".into(),
                ..Default::default()
            },
            ..Default::default()
        }))
    }
    async fn image_base_model(&self) -> Result<Option<gateway_core::model::ModelId>, GatewayError> {
        Ok(Some("gpt-fixture-host".into()))
    }
    async fn dispatch(
        &self,
        operation: ExtrasDispatch,
    ) -> Result<UpstreamResponse, DispatchFailure> {
        let (label, source) = match &operation {
            ExtrasDispatch::Voyage(c) => ("voyage".into(), &c.prepared().body),
            ExtrasDispatch::SystemOne(c) | ExtrasDispatch::Decisions(c) => {
                ("systemone".into(), &c.prepared().body)
            }
            ExtrasDispatch::ImageChild {
                request_id, body, ..
            } => (request_id.clone(), body),
        };
        let body = source
            .deserialize::<Value>()
            .map_err(DispatchFailure::transport)?;
        self.0.calls.borrow_mut().push((label, body.clone()));
        if self.0.pending.get() {
            std::future::pending::<()>().await;
        }
        if let Some(reply) = self.0.replies.borrow_mut().pop_front() {
            return reply;
        }
        let (body, semantic) = match operation {
            ExtrasDispatch::Voyage(_) => {
                let count = body["input"].as_array().map_or(1, Vec::len);
                let dims = body["output_dimension"]
                    .as_u64()
                    .ok_or_else(|| DispatchFailure::transport(error("fixture_dimensions")))?
                    as usize;
                (
                    json!({"data":(0..count).map(|i|json!({"embedding":vec![i as f64+1.0;dims]})).collect::<Vec<_>>(),"usage":{"total_tokens":count}}),
                    None,
                )
            }
            ExtrasDispatch::SystemOne(_) | ExtrasDispatch::Decisions(_) => (
                json!({"answers":{"go":true},"usage":{"input_tokens":2,"output_tokens":3,"cost":0.1}}),
                None,
            ),
            ExtrasDispatch::ImageChild { .. } => (
                json!({"created_at":123,"output":[{"type":"image_generation_call","result":"AA==","output_format":"png"}]}),
                Some(SemanticTerminal::Completed),
            ),
        };
        Ok(UpstreamResponse {
            status: 200,
            headers: Headers::from([("x-uos-upstream".into(), "fixture".into())]),
            body: serde_json::to_vec(&body)
                .map_err(|_| DispatchFailure::transport(error("fixture_json")))?,
            semantic_terminal: semantic,
            usage: None,
        })
    }
    async fn begin_image_fanout(&self, _: ImageKind, _: usize) -> Result<(), GatewayError> {
        self.0.begins.set(self.0.begins.get() + 1);
        Ok(())
    }
    fn abort_image_siblings(&self) {
        self.0.aborted.set(true)
    }
    async fn image_child_finished(&self, index: usize) -> Result<(), GatewayError> {
        self.0.children.borrow_mut().push(index);
        Ok(())
    }
    async fn finish_image_fanout(&self) -> Result<(), GatewayError> {
        self.0.finished.set(self.0.finished.get() + 1);
        Ok(())
    }
    async fn aes_gcm_encrypt(
        &self,
        key: &[u8; 32],
        iv: &[u8; 12],
        plaintext: &[u8],
    ) -> Result<Vec<u8>, GatewayError> {
        let cipher = Aes256Gcm::new_from_slice(key).map_err(|_| error("crypto"))?;
        let nonce = Nonce::from(*iv);
        cipher
            .encrypt(&nonce, plaintext)
            .map_err(|_| error("crypto"))
    }
    async fn aes_gcm_decrypt(
        &self,
        key: &[u8; 32],
        iv: &[u8],
        data: &[u8],
    ) -> Result<Vec<u8>, GatewayError> {
        self.0.decrypts.set(self.0.decrypts.get() + 1);
        let cipher = Aes256Gcm::new_from_slice(key).map_err(|_| error("crypto"))?;
        let iv: [u8; 12] = iv.try_into().map_err(|_| error("crypto"))?;
        let nonce = Nonce::from(iv);
        cipher.decrypt(&nonce, data).map_err(|_| error("crypto"))
    }
}
pub(super) fn principal(id: &str) -> AuthPrincipal {
    AuthPrincipal {
        token: Some(Secret::new("rotating-bearer".into())),
        method: AuthMethod::KvApiKey {
            key_id: id.into(),
            policy: ApiKeyPolicy {
                token_hash: "fixture".into(),
                key_id: id.into(),
                expires_at_ms: 0,
                usage_limit_requests: -1,
                window_ms: 60_000,
                window_start_ms: 0,
                usage_reset_at_ms: 0,
                policy_version: "v1".into(),
                paid_fallback_enabled: false,
                paid_fallback_limit_microcredits: 0,
                paid_fallback_spent_microcredits: 0,
                paid_fallback_reserved_microcredits: 0,
                paid_fallback_reservation_request_id: None,
            },
        },
        is_admin: false,
        is_super_admin: false,
    }
}
pub(super) fn context(id: &str) -> RequestContext {
    RequestContext {
        request_id: RequestId("original".into()),
        principal: PrincipalId::from_scalar(format!("api-key:{id}")),
        key_id: Some(ApiKeyId(id.into())),
        route: "fixture".into(),
        started_at_ms: 1_000_000,
    }
}
pub(super) fn request<'a>(
    route: ExtrasRoute,
    body: Value,
    auth: &'a AuthPrincipal,
    ctx: &'a RequestContext,
) -> ExtrasRequest<'a> {
    ExtrasRequest {
        route,
        body: gateway_source::source_json::SourceJson::from_serializable(&body).unwrap(),
        multipart: None,
        raw_body_bytes: 1,
        idempotency_key: None,
        principal: auth,
        context: ctx,
    }
}
pub(super) fn embedding(input: Value) -> Value {
    json!({"model":"voyage-4-large","input":input,"dimensions":256})
}
pub(super) fn setup() -> (Memory, Host, AuthPrincipal, RequestContext) {
    let clock = Rc::new(Cell::new(1_000_000));
    (
        Memory::new(clock.clone()),
        Host::new(clock),
        principal("a"),
        context("a"),
    )
}

pub(super) fn scalar_source(
    value: &gateway_extras_contract::response_body::ExtrasResponseBody,
) -> Result<Value, Box<dyn std::error::Error>> {
    Ok(serde_json::from_slice(value.as_bytes())?)
}
