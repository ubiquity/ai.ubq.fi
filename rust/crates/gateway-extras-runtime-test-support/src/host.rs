//! Original runtime fixture host with real dev-only AES-GCM; no production consumer.
use crate::fixture::error;
use aes_gcm::{Aes256Gcm, KeyInit, Nonce, aead::Aead};
use gateway_contracts::lifecycle::SemanticTerminal;
use gateway_extras_contract::types::{
    DispatchFailure, ExtrasDispatch, ExtrasHost, ExtrasProvider, Headers, ImageKind,
    UpstreamResponse,
};
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
pub fn poll<F: Future>(future: Pin<&mut F>) -> Poll<F::Output> {
    future.poll(&mut Context::from_waker(Waker::noop()))
}
pub fn run<F: Future>(future: F) -> F::Output {
    let mut f = Box::pin(future);
    for _ in 0..10_000 {
        if let Poll::Ready(result) = poll(f.as_mut()) {
            return result;
        }
    }
    panic!("fixture remained pending")
}
#[derive(Clone)]
pub struct Host(pub Rc<HostState>);
pub struct HostState {
    pub clock: Rc<Cell<i64>>,
    pub rng: Cell<u64>,
    pub configured: Cell<bool>,
    pub cancelled: Cell<bool>,
    pub pending: Cell<bool>,
    pub calls: RefCell<Vec<(String, Value)>>,
    pub replies: RefCell<VecDeque<Result<UpstreamResponse, DispatchFailure>>>,
    pub sleeps: RefCell<Vec<u64>>,
    pub begins: Cell<usize>,
    pub finished: Cell<usize>,
    pub children: RefCell<Vec<usize>>,
    pub aborted: Cell<bool>,
    pub decrypts: Cell<usize>,
}
impl Host {
    pub fn new(clock: Rc<Cell<i64>>) -> Self {
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
    pub fn reply(&self, status: u16, body: Value) {
        self.0.replies.borrow_mut().push_back(Ok(UpstreamResponse {
            status,
            headers: Headers::new(),
            body: serde_json::to_vec(&body).unwrap(),
            semantic_terminal: None,
            usage: None,
        }));
    }
    pub fn count(&self) -> usize {
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
    async fn image_base_model(
        &self,
    ) -> Result<Option<gateway_contracts::model::ModelId>, GatewayError> {
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
