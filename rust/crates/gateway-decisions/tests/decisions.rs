//! Port of tests/decisions.test.ts (cfe8458) handler cases; no live provider calls.
use gateway_auth::types::{AuthMethod, AuthPrincipal};
use gateway_core::{
    error::GatewayError,
    identity::{PrincipalId, RequestContext, RequestId},
    lifecycle::SemanticTerminal,
    model::ModelId,
    source_json::SourceJson,
};
use gateway_decisions::{
    handler::{decision_model_ids, handle, resolve_model, upstream_status},
    host::{DECISIONS_URL, DecisionsCall, DecisionsHost},
};
use gateway_extras_runtime::types::{
    DispatchFailure, ExtrasDispatch, ExtrasHost, ExtrasProvider, ExtrasRequest, ExtrasResponse,
    ExtrasRoute, Headers, HealthKind, ImageKind, KernelDisposition, UpstreamResponse,
};
use gateway_extras_runtime_test_support::fixture::{Memory, error};
use gateway_providers::types::{AuthMaterial, PrepareOptions};
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    collections::VecDeque,
    future::Future,
    pin::pin,
    rc::Rc,
    task::{Context, Poll, Waker},
};

type TestResult = Result<(), Box<dyn std::error::Error>>;
type Reply = Result<UpstreamResponse, DispatchFailure>;

fn run<F: Future>(future: F) -> Result<F::Output, GatewayError> {
    let mut future = pin!(future);
    for _ in 0..1_000 {
        if let Poll::Ready(output) = future
            .as_mut()
            .poll(&mut Context::from_waker(Waker::noop()))
        {
            return Ok(output);
        }
    }
    Err(error("fixture_pending"))
}

#[derive(Default)]
struct Wire {
    url: String,
    headers: Headers,
    body: Value,
}
struct Host {
    clock: Rc<Cell<i64>>,
    key: Option<String>,
    cancelled: Cell<bool>,
    replies: RefCell<VecDeque<Reply>>,
    calls: RefCell<Vec<Wire>>,
}
impl Host {
    fn new(clock: Rc<Cell<i64>>) -> Self {
        Self {
            clock,
            key: Some("or-test-key".into()),
            cancelled: Cell::new(false),
            replies: RefCell::new(VecDeque::new()),
            calls: RefCell::new(Vec::new()),
        }
    }
    fn reply(&self, status: u16, body: &Value) -> TestResult {
        self.replies.borrow_mut().push_back(Ok(UpstreamResponse {
            status,
            headers: Headers::new(),
            body: serde_json::to_vec(body)?,
            semantic_terminal: None,
            usage: None,
        }));
        Ok(())
    }
    fn models(&self) -> Vec<Value> {
        let calls = self.calls.borrow();
        calls
            .iter()
            .map(|wire| wire.body["model"].clone())
            .collect()
    }
}
impl ExtrasHost for Host {
    fn now_ms(&self) -> i64 {
        self.clock.get()
    }
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        Ok(vec![7; length])
    }
    fn cancelled(&self) -> bool {
        self.cancelled.get()
    }
    async fn sleep_ms(&self, _: u64) -> Result<(), GatewayError> {
        Err(error("fixture_sleep"))
    }
    async fn provider_options(
        &self,
        provider: ExtrasProvider,
    ) -> Result<Option<PrepareOptions>, GatewayError> {
        assert_eq!(provider, ExtrasProvider::SystemOne);
        Ok(self.key.clone().map(|secret| PrepareOptions {
            auth: AuthMaterial {
                secret,
                ..Default::default()
            },
            ..Default::default()
        }))
    }
    async fn image_base_model(&self) -> Result<Option<ModelId>, GatewayError> {
        Err(error("fixture_image"))
    }
    async fn dispatch(&self, _: ExtrasDispatch) -> Reply {
        Err(DispatchFailure::gateway(error("fixture_dispatch"), true))
    }
    async fn begin_image_fanout(&self, _: ImageKind, _: usize) -> Result<(), GatewayError> {
        Err(error("fixture_image"))
    }
    fn abort_image_siblings(&self) {}
    async fn image_child_finished(&self, _: usize) -> Result<(), GatewayError> {
        Err(error("fixture_image"))
    }
    async fn finish_image_fanout(&self) -> Result<(), GatewayError> {
        Err(error("fixture_image"))
    }
    async fn aes_gcm_encrypt(
        &self,
        _: &[u8; 32],
        _: &[u8; 12],
        _: &[u8],
    ) -> Result<Vec<u8>, GatewayError> {
        Err(error("fixture_crypto"))
    }
    async fn aes_gcm_decrypt(
        &self,
        _: &[u8; 32],
        _: &[u8],
        _: &[u8],
    ) -> Result<Vec<u8>, GatewayError> {
        Err(error("fixture_crypto"))
    }
}
impl DecisionsHost for Host {
    async fn dispatch_decisions(&self, call: DecisionsCall) -> Reply {
        assert!(call.deadline_ms() >= self.clock.get() + 60_000);
        let body = call
            .body()
            .deserialize::<Value>()
            .map_err(|e| DispatchFailure::gateway(e, true))?;
        self.calls.borrow_mut().push(Wire {
            url: call.url().into(),
            headers: call.headers().clone(),
            body,
        });
        let reply = self.replies.borrow_mut().pop_front();
        reply.unwrap_or_else(|| {
            Ok(UpstreamResponse {
                status: 200,
                headers: Headers::new(),
                body: decision_payload().to_string().into_bytes(),
                semantic_terminal: None,
                usage: None,
            })
        })
    }
}

fn catalogue() -> Vec<SourceJson> {
    [
        json!({"id":"vendor/text","output_modalities":["text"]}),
        json!({"id":"baseten/elephant-1:free","output_modalities":["decisions"]}),
        json!({"id":"~typesafe/jev-latest","output_modalities":["decisions"]}),
        json!({"id":"legacy/text-default"}),
    ]
    .iter()
    .filter_map(|row| SourceJson::from_serializable(row).ok())
    .collect()
}
fn decision_body() -> Value {
    json!({
        "model": "baseten/elephant-1:free",
        "state": ["Inspect the listing photo and ticket.", {"type":"image_url","image_url":{"url":"data:image/png;base64,AAAA"}}],
        "questions": {
            "is_bug": {"type":"noul","instructions":"Is the customer reporting a defect?","criteria":{"true":"Broken behavior.","false":"A question or feature request."}},
            "team": {"type":"choice","instructions":"Which team owns this?","criteria":{"payments":"Checkout or billing.","frontend":"Rendering or browser issues.","account":null}},
            "urgency": {"type":"score","instructions":"How urgent is this?","criteria":["Later","This week","Blocking revenue"]},
            "product": {"type":"box","instructions":"the product in the listing photo"}
        },
        "provider": {"allow_fallbacks": false},
        "session_id": "ticket-42",
        "trace": {"trace_id":"trace-42","generation_name":"triage"},
        "user": "operator-1"
    })
}
fn decision_payload() -> Value {
    json!({
        "id": "gen-dec-test",
        "model": "baseten/elephant-1-20261021",
        "provider": "BaseTen",
        "answers": {
            "is_bug": {"type":"noul","noul":0.97},
            "team": {"type":"choice","choice":"payments","confidence":0.8,"probabilities":{"payments":0.8,"frontend":0.2}},
            "urgency": {"type":"score","score":1.9,"confidence":0.95,"probabilities":{"0":0,"1":0.1,"2":0.9}},
            "product": {"type":"box","box":{"top":0.1,"bottom":0.9,"left":0.2,"right":0.8}}
        },
        "usage": {"input_tokens":321,"output_tokens":4,"cost":0}
    })
}
fn minimal(model: &str) -> Value {
    json!({"model":model,"state":"x","questions":{"yes":{"type":"noul","instructions":"Is x true?"}}})
}

struct Fixture {
    store: Memory,
    host: Host,
    principal: AuthPrincipal,
    context: RequestContext,
}
impl Fixture {
    fn new() -> Self {
        let clock = Rc::new(Cell::new(1_000_000));
        Self {
            store: Memory::new(clock.clone()),
            host: Host::new(clock),
            principal: AuthPrincipal {
                token: None,
                method: AuthMethod::Disabled,
                is_admin: false,
                is_super_admin: false,
            },
            context: RequestContext {
                request_id: RequestId("original".into()),
                principal: PrincipalId("anonymous".into()),
                key_id: None,
                route: "decisions".into(),
                started_at_ms: 1_000_000,
            },
        }
    }
    fn send_raw(&self, body: SourceJson) -> Result<ExtrasResponse, GatewayError> {
        let request = ExtrasRequest {
            route: ExtrasRoute::Decisions,
            body,
            multipart: None,
            raw_body_bytes: 1,
            idempotency_key: None,
            principal: &self.principal,
            context: &self.context,
        };
        run(handle(&self.store, &self.host, &request, &catalogue()))?
    }
    fn send(&self, body: &Value) -> Result<ExtrasResponse, GatewayError> {
        self.send_raw(SourceJson::from_serializable(body)?)
    }
}
fn body(response: &ExtrasResponse) -> Result<Value, GatewayError> {
    response.body.deserialize::<Value>()
}

#[test]
fn route_label_and_upstream_status_mapping_follow_source() {
    assert_eq!(ExtrasRoute::Decisions.label(), "decisions");
    for (upstream, client) in [
        (400, 400),
        (413, 413),
        (429, 429),
        (500, 502),
        (503, 502),
        (401, 502),
    ] {
        assert_eq!(upstream_status(upstream), client);
    }
}

#[test]
fn discovery_classifies_decision_rows_and_keeps_the_text_default() {
    let rows = catalogue();
    let ids: Vec<String> = decision_model_ids(&rows)
        .iter()
        .filter_map(|id| id.try_utf8().ok())
        .collect();
    assert_eq!(ids, ["baseten/elephant-1:free", "~typesafe/jev-latest"]);
    let model = |id: &str| resolve_model(&id.into(), &rows).and_then(|m| m.try_utf8().ok());
    assert_eq!(
        model("typesafe/jev-latest").as_deref(),
        Some("~typesafe/jev-latest")
    );
    assert_eq!(
        model("baseten/elephant-1:free").as_deref(),
        Some("baseten/elephant-1:free")
    );
    for text in ["vendor/text", "legacy/text-default", "unknown/model", ""] {
        assert_eq!(model(text), None);
    }
}

#[test]
fn decisions_forwards_the_documented_envelope_and_relays_typed_answers() -> TestResult {
    let fixture = Fixture::new();
    let response = fixture.send(&decision_body())?;
    assert_eq!(response.status, 200);
    let calls = fixture.host.calls.borrow();
    assert_eq!(calls.len(), 1);
    let wire = &calls[0];
    assert_eq!(wire.url, DECISIONS_URL);
    assert_eq!(
        wire.headers.get("authorization").map(String::as_str),
        Some("Bearer or-test-key")
    );
    let sent = &wire.body;
    let expected = decision_body();
    assert_eq!(sent["model"], "baseten/elephant-1:free");
    for key in [
        "state",
        "questions",
        "provider",
        "session_id",
        "trace",
        "user",
    ] {
        assert_eq!(sent[key], expected[key], "{key}");
    }
    let keys: Vec<&String> = sent
        .as_object()
        .map(|o| o.keys().collect())
        .unwrap_or_default();
    assert_eq!(
        keys,
        [
            "model",
            "state",
            "questions",
            "provider",
            "session_id",
            "trace",
            "user"
        ]
    );
    assert_eq!(
        response.headers.get("x-uos-upstream").map(String::as_str),
        Some("openrouter")
    );
    assert_eq!(body(&response)?, decision_payload());
    assert_eq!(
        response.semantic_terminal,
        Some(SemanticTerminal::Completed)
    );
    assert_eq!(response.kernel, KernelDisposition::Completed);
    let usage = response.usage.clone().unwrap_or_default();
    assert_eq!(usage["input_tokens"], 321.0);
    assert_eq!(usage["output_tokens"], 4.0);
    assert_eq!(usage["total_tokens"], 325.0);
    assert_eq!(usage["input_tokens_details"]["cached_tokens"], 0);
    assert_eq!(response.health.len(), 1);
    assert_eq!(response.health[0].kind, HealthKind::Success);
    Ok(())
}

#[test]
fn decisions_keeps_the_stable_jev_alias_and_rejects_text_or_unknown_ids() -> TestResult {
    let fixture = Fixture::new();
    assert_eq!(fixture.send(&minimal("typesafe/jev-latest"))?.status, 200);
    assert_eq!(fixture.host.models(), [json!("~typesafe/jev-latest")]);
    for model in ["vendor/text", "unknown/model", "baseten/elephant-1:free"] {
        let response = fixture.send(&minimal(model))?;
        let expected = if model == "baseten/elephant-1:free" {
            200
        } else {
            400
        };
        assert_eq!(response.status, expected);
        if expected == 400 {
            assert_eq!(body(&response)?["error"]["param"], "model");
            assert_eq!(
                response.kernel,
                KernelDisposition::Incomplete("decisions_failed")
            );
        }
    }
    assert_eq!(
        fixture.host.models(),
        [
            json!("~typesafe/jev-latest"),
            json!("baseten/elephant-1:free")
        ]
    );
    let padded = fixture.send(&minimal("  baseten/elephant-1:free\n"))?;
    assert_eq!(padded.status, 200);
    Ok(())
}

#[test]
fn decisions_validates_state_question_primitives_metadata_and_unsupported_keys() -> TestResult {
    let fixture = Fixture::new();
    let valid = minimal("baseten/elephant-1:free");
    let with = |key: &str, value: Value| {
        let mut body = valid.clone();
        body[key] = value;
        body
    };
    let question = |value: Value| with("questions", json!({ "q": value }));
    let mut missing_state = valid.clone();
    if let Some(object) = missing_state.as_object_mut() {
        object.remove("state");
    }
    let many: serde_json::Map<String, Value> = (0..25)
        .map(|i| (format!("q{i}"), json!({"type":"box","instructions":"x"})))
        .collect();
    let cases = [
        missing_state,
        with("state", json!(4)),
        with("state", Value::Null),
        with("questions", json!({})),
        with("questions", Value::Object(many)),
        with(
            "questions",
            json!({ "x".repeat(121): {"type":"box","instructions":"x"} }),
        ),
        with(
            "questions",
            json!({ "  ": {"type":"box","instructions":"x"} }),
        ),
        question(json!({"type":"unsupported","instructions":"x"})),
        question(json!({"type":"noul"})),
        question(json!({"type":"noul","instructions":"x","criteria":{"true":"yes"}})),
        question(json!({"type":"noul","instructions":"x","criteria":{"true":"y","false":4}})),
        question(json!({"type":"noul","instructions":"x","criteria":null})),
        question(json!({"type":"choice","instructions":"x","criteria":{}})),
        question(json!({"type":"choice","instructions":"x","criteria":{" ":"x"}})),
        question(json!({"type":"choice","instructions":"x","criteria":{"a":true}})),
        question(json!({"type":"score","instructions":"x","criteria":[]})),
        question(json!({"type":"score","instructions":"x","criteria":[1]})),
        question(json!({"type":"box","instructions":"x","criteria":{"top":0}})),
        question(json!({"type":"noul","instructions":"x","extra":true})),
        with("provider", json!("openai")),
        with("trace", json!([])),
        with("trace", Value::Null),
        with("session_id", json!("x".repeat(257))),
        with("user", json!("")),
        with("model", json!("vendor/a/b")),
        with("model", json!("vendor model/x")),
        with("model", json!(4)),
        with("extra", json!(true)),
    ];
    for case in cases {
        let response = fixture.send(&case)?;
        assert_eq!(response.status, 400, "{case}");
        assert_eq!(
            body(&response)?["error"]["code"],
            "decisions_request_invalid",
            "{case}"
        );
    }
    assert!(fixture.host.calls.borrow().is_empty());
    let unsupported = body(&fixture.send(&with("extra", json!(true)))?)?;
    assert_eq!(unsupported["error"]["message"], "Unsupported key: extra");
    assert!(unsupported["error"].get("param").is_none());
    let indexed = body(&fixture.send(&json!(["x"]))?)?;
    assert_eq!(indexed["error"]["message"], "Unsupported key: 0");
    let scalar = body(&fixture.send(&json!("x"))?)?;
    assert_eq!(scalar["error"]["message"], "Invalid JSON body");
    assert_eq!(scalar["error"]["code"], "invalid_request_error");
    let named = body(&fixture.send(&question(
        json!({"type":"score","instructions":"x","criteria":[]}),
    ))?)?;
    assert_eq!(
        named["error"]["message"],
        "questions.q.criteria must be a non-empty array"
    );
    assert_eq!(named["error"]["param"], "questions");
    assert_eq!(fixture.send(&with("provider", Value::Null))?.status, 200);
    assert_eq!(
        fixture
            .send(&question(
                json!({"type":"choice","instructions":["x"],"criteria":{"a":null}})
            ))?
            .status,
        200
    );
    Ok(())
}

#[test]
fn decisions_preserves_lone_surrogate_question_names_in_refusals() -> TestResult {
    let fixture = Fixture::new();
    let raw =
        r#"{"model":"baseten/elephant-1:free","state":"x","questions":{"\ud800":{"type":"box"}}}"#;
    let response = fixture.send_raw(SourceJson::parse(raw)?)?;
    assert_eq!(response.status, 400);
    assert!(response.body.as_str().contains(
        r#""message":"questions.\ud800.instructions must be a string, object, or array""#
    ));
    Ok(())
}

#[test]
fn decisions_maps_upstream_and_configuration_failures_without_dispatching_twice() -> TestResult {
    let mut fixture = Fixture::new();
    fixture.host.key = None;
    let missing = fixture.send(&minimal("baseten/elephant-1:free"))?;
    assert_eq!(missing.status, 503);
    assert_eq!(
        body(&missing)?["error"]["code"],
        "openrouter_api_key_missing"
    );
    assert!(fixture.host.calls.borrow().is_empty());

    let fixture = Fixture::new();
    for (upstream, expected, health) in [
        (400, 400, HealthKind::Reachable),
        (413, 413, HealthKind::Reachable),
        (429, 429, HealthKind::QuotaExhausted),
        (401, 502, HealthKind::AuthInvalid),
        (500, 502, HealthKind::UpstreamError),
        (503, 502, HealthKind::UpstreamError),
    ] {
        fixture
            .host
            .reply(upstream, &json!({"private":"never_echo"}))?;
        let response = fixture.send(&minimal("baseten/elephant-1:free"))?;
        assert_eq!(response.status, expected);
        assert_eq!(
            body(&response)?["error"]["code"],
            "openrouter_upstream_error"
        );
        assert!(!response.body.as_str().contains("never_echo"));
        assert_eq!(response.health[0].kind, health);
        assert_eq!(response.health[0].upstream_status, Some(upstream));
        assert_eq!(response.upstream_errors.len(), 1);
    }
    assert_eq!(fixture.host.calls.borrow().len(), 6);

    for payload in [json!({"answers": null}), json!([]), json!({"answers": []})] {
        fixture.host.reply(200, &payload)?;
        let invalid = fixture.send(&minimal("baseten/elephant-1:free"))?;
        assert_eq!(invalid.status, 502);
        assert_eq!(
            body(&invalid)?["error"]["code"],
            "openrouter_upstream_invalid_response"
        );
        assert_eq!(invalid.health[0].kind, HealthKind::UpstreamError);
    }
    Ok(())
}

#[test]
fn decisions_maps_transport_cancellation_and_gateway_refusals() -> TestResult {
    let fixture = Fixture::new();
    let transport = || DispatchFailure::transport(error("fixture_transport"));
    fixture
        .host
        .replies
        .borrow_mut()
        .push_back(Err(transport()));
    let unreachable = fixture.send(&minimal("baseten/elephant-1:free"))?;
    assert_eq!(unreachable.status, 502);
    assert_eq!(
        body(&unreachable)?["error"]["code"],
        "openrouter_upstream_unreachable"
    );
    assert_eq!(unreachable.health[0].kind, HealthKind::UpstreamError);

    let mut before = transport();
    before.before_transport = true;
    fixture.host.replies.borrow_mut().push_back(Err(before));
    assert!(
        fixture
            .send(&minimal("baseten/elephant-1:free"))?
            .health
            .is_empty()
    );

    fixture.host.cancelled.set(true);
    fixture
        .host
        .replies
        .borrow_mut()
        .push_back(Err(transport()));
    let cancelled = fixture.send(&minimal("baseten/elephant-1:free"))?;
    assert_eq!(cancelled.status, 499);
    assert_eq!(body(&cancelled)?["error"]["code"], "request_cancelled");
    assert_eq!(body(&cancelled)?["error"]["type"], "server_error");
    fixture.host.cancelled.set(false);

    let quota = GatewayError::new(
        429,
        "API key quota exhausted",
        Some("api_key_quota_exhausted"),
    );
    let mut refusal = DispatchFailure::gateway(quota, true);
    refusal.headers.insert("retry-after".into(), "7".into());
    fixture.host.replies.borrow_mut().push_back(Err(refusal));
    let refused = fixture.send(&minimal("baseten/elephant-1:free"))?;
    assert_eq!(refused.status, 429);
    assert_eq!(
        refused.headers.get("retry-after").map(String::as_str),
        Some("7")
    );
    assert!(refused.health.is_empty());
    Ok(())
}

#[test]
fn decisions_honours_the_authoritative_provider_selection() -> TestResult {
    let fixture = Fixture::new();
    let now = fixture.host.now_ms();
    let selection = json!(["uos_ai", "provider_selection", "v1"]);
    fixture.store.seed(
        selection.clone(),
        json!({"provider_ids":["codex"],"updated_at_ms":now}),
        None,
    );
    let disabled = fixture.send(&minimal("baseten/elephant-1:free"))?;
    assert_eq!(disabled.status, 503);
    assert_eq!(
        body(&disabled)?["error"]["code"],
        "decisions_provider_disabled"
    );
    assert!(fixture.host.calls.borrow().is_empty());
    let invalid = fixture.send(&json!({"model": 4}))?;
    assert_eq!(
        body(&invalid)?["error"]["code"],
        "decisions_request_invalid"
    );
    fixture.store.seed(
        selection,
        json!({"provider_ids":["openrouter"],"updated_at_ms":now}),
        None,
    );
    assert_eq!(
        fixture.send(&minimal("baseten/elephant-1:free"))?.status,
        200
    );
    Ok(())
}
