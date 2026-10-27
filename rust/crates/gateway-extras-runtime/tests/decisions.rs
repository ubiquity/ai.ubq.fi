use gateway_auth_contracts::types::{AuthMethod, AuthPrincipal};
use gateway_auth_primitives::api_keys::ApiKeyPolicy;
use gateway_core::{
    identity::{PrincipalId, RequestContext, RequestId},
    model::ModelId,
};
use gateway_extras_contract::types::{
    DispatchFailure, ExtrasDispatch, ExtrasHost, ExtrasProvider, ExtrasRequest, ExtrasRoute,
    Headers, ImageKind, KernelDisposition, UpstreamResponse,
};
use gateway_extras_runtime::ExtrasRuntime;
use gateway_extras_runtime_test_support::fixture::Memory;
use gateway_providers::types::PrepareOptions;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    future::Future,
    rc::Rc,
    task::{Context, Poll, Waker},
};
type TestResult = Result<(), Box<dyn std::error::Error>>;
const MODEL: &str = "baseten/elephant-1:free";
fn run<F: Future>(future: F) -> Result<F::Output, Box<dyn std::error::Error>> {
    let mut future = std::pin::pin!(future);
    match future
        .as_mut()
        .poll(&mut Context::from_waker(Waker::noop()))
    {
        Poll::Ready(value) => Ok(value),
        Poll::Pending => Err("unexpected pending controlled call".into()),
    }
}
struct Call {
    url: String,
    deadline: i64,
    body: SourceJson,
}
struct Host {
    models: RefCell<Vec<ModelId>>,
    calls: RefCell<Vec<Call>>,
    discoveries: Cell<usize>,
    configured: Cell<bool>,
    cancelled: Cell<bool>,
    reply: RefCell<Result<UpstreamResponse, DispatchFailure>>,
}
impl Default for Host {
    fn default() -> Self {
        Self { models:RefCell::new(vec![MODEL.into()]),calls:RefCell::default(),discoveries:Cell::new(0),
            configured:Cell::new(true),cancelled:Cell::new(false),
            reply:RefCell::new(Ok(UpstreamResponse { status:200,headers:Headers::new(),
                body:br#"{"answers":{"go":true},"usage":{"input_tokens":2,"output_tokens":3,"cost":0.1}}"#.to_vec(),semantic_terminal:None,usage:None })) }
    }
}
fn unavailable() -> GatewayError {
    GatewayError::new(500, "unused fixture capability", None)
}
impl ExtrasHost for Host {
    fn now_ms(&self) -> i64 {
        1_000_000
    }
    fn random_bytes(&self, _length: usize) -> Result<Vec<u8>, GatewayError> {
        Err(unavailable())
    }
    fn cancelled(&self) -> bool {
        self.cancelled.get()
    }
    async fn sleep_ms(&self, _millis: u64) -> Result<(), GatewayError> {
        Err(unavailable())
    }
    async fn decision_models(&self) -> Result<Vec<ModelId>, GatewayError> {
        self.discoveries.set(self.discoveries.get() + 1);
        Ok(self.models.borrow().clone())
    }
    async fn provider_options(
        &self,
        provider: ExtrasProvider,
    ) -> Result<Option<PrepareOptions>, GatewayError> {
        assert_eq!(provider, ExtrasProvider::Decisions);
        Ok(self.configured.get().then(|| PrepareOptions {
            auth: gateway_providers::types::AuthMaterial {
                secret: "fixture-decision-key".into(),
                ..Default::default()
            },
            ..Default::default()
        }))
    }
    async fn image_base_model(&self) -> Result<Option<ModelId>, GatewayError> {
        Err(unavailable())
    }
    async fn dispatch(
        &self,
        operation: ExtrasDispatch,
    ) -> Result<UpstreamResponse, DispatchFailure> {
        let ExtrasDispatch::Decisions(call) = operation else {
            return Err(DispatchFailure::gateway(unavailable(), true));
        };
        let request = call.prepared();
        assert_eq!(request.method, "POST");
        assert!(!request.decode.stream);
        self.calls.borrow_mut().push(Call {
            url: request.url.clone(),
            deadline: call.deadline_ms(),
            body: request.body.clone(),
        });
        self.reply.borrow().clone()
    }
    async fn begin_image_fanout(
        &self,
        _kind: ImageKind,
        _count: usize,
    ) -> Result<(), GatewayError> {
        Err(unavailable())
    }
    fn abort_image_siblings(&self) {}
    async fn image_child_finished(&self, _index: usize) -> Result<(), GatewayError> {
        Err(unavailable())
    }
    async fn finish_image_fanout(&self) -> Result<(), GatewayError> {
        Err(unavailable())
    }
    async fn aes_gcm_encrypt(
        &self,
        _key: &[u8; 32],
        _iv: &[u8; 12],
        _plaintext: &[u8],
    ) -> Result<Vec<u8>, GatewayError> {
        Err(unavailable())
    }
    async fn aes_gcm_decrypt(
        &self,
        _key: &[u8; 32],
        _iv: &[u8],
        _bytes: &[u8],
    ) -> Result<Vec<u8>, GatewayError> {
        Err(unavailable())
    }
}
fn body() -> Value {
    json!({"model":MODEL,"state":{"screen":[]},"questions":{"go":{"type":"noul","instructions":"go?"}},"trace":{"id":"fixture"}})
}
fn request(body: Value) -> Result<ExtrasRequest<'static>, GatewayError> {
    // The fixture principal owns this one request; it has no paid entitlement.
    static AUTH: std::sync::LazyLock<AuthPrincipal> = std::sync::LazyLock::new(|| AuthPrincipal {
        token: None,
        method: AuthMethod::KvApiKey {
            key_id: "fixture".into(),
            policy: ApiKeyPolicy {
                token_hash: "fixture".into(),
                key_id: "fixture".into(),
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
    });
    static CONTEXT: std::sync::LazyLock<RequestContext> =
        std::sync::LazyLock::new(|| RequestContext {
            request_id: RequestId("decision-original".into()),
            principal: PrincipalId::from_scalar("api-key:fixture".into()),
            key_id: None,
            route: "decisions".into(),
            started_at_ms: 1_000_000,
        });
    Ok(ExtrasRequest {
        route: ExtrasRoute::Decisions,
        body: SourceJson::from_serializable(&body)?,
        multipart: None,
        raw_body_bytes: 1024,
        idempotency_key: None,
        principal: &AUTH,
        context: &CONTEXT,
    })
}
fn runtime() -> ExtrasRuntime<Memory, Host> {
    ExtrasRuntime::new(Memory::new(Rc::new(Cell::new(1_000_000))), Host::default())
}
#[test]
fn actual_extras_dispatch_uses_one_fixed_decisions_call_and_terminal_usage() -> TestResult {
    let runtime = runtime();
    let result = run(runtime.handle(request(body())?))??;
    assert_eq!(result.status, 200);
    assert_eq!(result.kernel, KernelDisposition::Completed);
    assert_eq!(
        result.headers.get("x-uos-upstream").map(String::as_str),
        Some("openrouter")
    );
    assert_eq!(
        result.body.deserialize::<Value>()?["answers"],
        json!({"go":true})
    );
    assert_eq!(
        result.usage.as_ref().map(|v| &v["total_tokens"]),
        Some(&json!(5.0))
    );
    let calls = runtime.host().calls.borrow();
    assert_eq!(calls.len(), 1);
    assert_eq!(calls[0].url, "https://openrouter.ai/api/alpha/decisions");
    assert_eq!(calls[0].deadline, 1_060_000);
    assert_eq!(calls[0].body.deserialize::<Value>()?, body());
    Ok(())
}
#[test]
fn invalid_disabled_unknown_and_unconfigured_requests_never_dispatch() -> TestResult {
    let runtime = runtime();
    let result = run(runtime.handle(request(json!({"model":3}))?))??;
    assert_eq!(result.status, 400);
    assert_eq!(runtime.host().discoveries.get(), 0);
    runtime.store().seed(
        json!(["uos_ai", "provider_selection", "v1"]),
        json!({"provider_ids":["codex"],"updated_at_ms":1_000_000}),
        None,
    );
    let result = run(runtime.handle(request(body())?))??;
    assert_eq!(result.status, 503);
    assert_eq!(runtime.host().discoveries.get(), 0);
    runtime.store().seed(
        json!(["uos_ai", "provider_selection", "v1"]),
        json!({"provider_ids":["openrouter"],"updated_at_ms":1_000_000}),
        None,
    );
    runtime.host().models.borrow_mut().clear();
    assert_eq!(run(runtime.handle(request(body())?))??.status, 400);
    runtime.host().models.borrow_mut().push(MODEL.into());
    runtime.host().configured.set(false);
    assert_eq!(run(runtime.handle(request(body())?))??.status, 503);
    assert!(runtime.host().calls.borrow().is_empty());
    Ok(())
}
#[test]
fn upstream_status_and_invalid_answers_retain_failed_terminal_without_retry() -> TestResult {
    for (upstream, expected) in [(400, 400), (413, 413), (429, 429), (401, 502), (503, 502)] {
        let runtime = runtime();
        if let Ok(reply) = runtime.host().reply.borrow_mut().as_mut() {
            reply.status = upstream;
        }
        let result = run(runtime.handle(request(body())?))??;
        assert_eq!(result.status, expected);
        assert!(result.semantic_terminal.is_none());
        assert!(matches!(
            result.kernel,
            KernelDisposition::Incomplete("decisions_failed")
        ));
        assert_eq!(runtime.host().calls.borrow().len(), 1);
        assert_eq!(result.upstream_errors.len(), 1);
    }
    let runtime = runtime();
    if let Ok(reply) = runtime.host().reply.borrow_mut().as_mut() {
        reply.body = b"{\"answers\":[]}".to_vec();
    }
    let result = run(runtime.handle(request(body())?))??;
    assert_eq!(result.status, 502);
    assert!(result.usage.is_none());
    Ok(())
}
#[test]
fn caller_cancellation_and_predispatch_quota_failure_remain_distinct() -> TestResult {
    let runtime = runtime();
    runtime.host().cancelled.set(true);
    *runtime.host().reply.borrow_mut() = Err(DispatchFailure::transport(unavailable()));
    assert_eq!(run(runtime.handle(request(body())?))??.status, 499);
    runtime.host().cancelled.set(false);
    let mut failure =
        DispatchFailure::gateway(GatewayError::new(429, "quota", Some("quota")), true);
    failure.quota_refusal = true;
    failure.headers.insert("retry-after".into(), "5".into());
    *runtime.host().reply.borrow_mut() = Err(failure);
    let result = run(runtime.handle(request(body())?))??;
    assert_eq!(result.status, 429);
    assert_eq!(
        result.headers.get("retry-after").map(String::as_str),
        Some("5")
    );
    assert!(result.health.is_empty());
    Ok(())
}
#[test]
fn byte_cap_and_jev_alias_use_the_decisions_contract() -> TestResult {
    let runtime = runtime();
    let mut req = request(body())?;
    req.raw_body_bytes = 32 * 1024 * 1024 + 1;
    assert_eq!(run(runtime.handle(req))??.status, 413);
    assert!(runtime.host().calls.borrow().is_empty());
    *runtime.host().models.borrow_mut() = vec!["~typesafe/jev-latest".into()];
    let mut raw = body();
    raw["model"] = json!("typesafe/jev-latest");
    assert_eq!(run(runtime.handle(request(raw)?))??.status, 200);
    assert_eq!(
        runtime.host().calls.borrow()[0]
            .body
            .deserialize::<Value>()?["model"],
        "~typesafe/jev-latest"
    );
    Ok(())
}
