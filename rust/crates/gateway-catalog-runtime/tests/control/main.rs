type TestResult = Result<(), Box<dyn std::error::Error>>;
use gateway_authority_keys as keys;
use gateway_authority_paid_control_records::*;
use gateway_catalog_provider::host::{
    CatalogFetch, CatalogFetchResponse, CatalogHost, CatalogProvider, CredentialPublication,
    ProviderBinding,
};
use gateway_routing_runtime_test_support::source_catalog;

use gateway_source::error::GatewayError;

use gateway_core::{
    query::{QueryPage, QueryRequest, QueryStore},
    source_control::decode_source_control_record,
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord},
};
use gateway_kv_codec::key::RecordKey;
use gateway_schema::{
    catalog_store, runtime_config::build_runtime_config, snapshot::ModelSnapshot,
};
use gateway_source::source_json::SourceJson;
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, VecDeque},
    future::{Future, poll_fn},
    rc::Rc,
    sync::Arc,
    task::{Context, Poll, Waker},
};
fn run<T>(future: impl Future<Output = T>) -> T {
    let mut future = std::pin::pin!(future);
    let mut cx = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(result) = future.as_mut().poll(&mut cx) {
            return result;
        }
    }
}
type Callback = Rc<dyn Fn()>;
#[derive(Clone)]
struct Store {
    rows: Rc<RefCell<BTreeMap<String, StoredRecord>>>,
    now: Rc<Cell<i64>>,
    before_policy: Rc<RefCell<Option<Callback>>>,
}
impl Store {
    fn new() -> Self {
        Self {
            rows: Rc::default(),
            now: Rc::new(Cell::new(1_800_000_000_000)),
            before_policy: Rc::default(),
        }
    }
    fn put(&self, key: RecordKey, revision: &str, value: Value) {
        self.rows
            .borrow_mut()
            .insert(key.0, StoredRecord::json(revision, value, None));
    }
    fn get(&self, key: &RecordKey) -> Option<StoredRecord> {
        self.rows
            .borrow()
            .get(&key.0)
            .filter(|row| row.expires_at_ms.is_none_or(|until| until > self.now.get()))
            .cloned()
    }
    fn config(&self, generation: &str, revision: &str, surplus: bool, openlux: bool) {
        self.put(
            keys::paid_control_config(),
            revision,
            json!(PaidControlConfig {
                v: PAID_CONTROL_CONFIG_VERSION,
                owner_id: "fixture-owner".into(),
                owner_epoch: "epoch-1".into(),
                generation: generation.into(),
                configuration_fingerprint: format!("configuration-{generation}"),
                surplus_binding: surplus.then(|| "surplus-credential".into()),
                openlux_discovery_binding: openlux.then(|| "openlux-discovery-credential".into()),
                openlux_binding: openlux.then(|| "openlux-credential".into()),
                source_checks: Vec::new()
            }),
        );
    }
    fn install(&self, surplus: bool, openlux: bool) {
        self.put(
            keys::paid_control_owner(),
            "owner-rev",
            json!(PaidControlOwner {
                v: 1,
                owner_id: "fixture-owner".into(),
                owner_epoch: "epoch-1".into(),
                key_fingerprint: "ephemeral-fixture-carrier-fingerprint".into()
            }),
        );
        self.config("generation-1", "config-rev", surplus, openlux);
        let snapshot = ModelSnapshot {
            models: vec![source_catalog::json(
                &json!({"slug":"gpt-known","context_window":128000,"supported_reasoning_levels":[{"effort":null},{"effort":"custom-tier"}]}),
            )],
            source: "chatgpt_codex".into(),
            updated_at_ms: self.now.get(),
            client_version: Some("0.163.0".into()),
        };
        let config = match build_runtime_config(&snapshot, None, None, self.now.get()) {
            Ok(config) => config,
            Err(error) => panic!("fixed runtime fixture configuration failed: {error:?}"),
        };
        let config = match config.deserialize::<Value>() {
            Ok(config) => config,
            Err(error) => panic!("fixed runtime fixture is not scalar JSON: {error:?}"),
        };
        self.put(catalog_store::runtime_config_key(), "runtime-rev", config);
        self.put(
            catalog_store::codex_snapshot_key(),
            "full-rev",
            json!(snapshot),
        );
        self.put(
            catalog_store::auth_generation_key(),
            "catalog-generation-rev",
            json!("catalog-generation"),
        );
    }
    fn projection(&self) -> PaidRoutingProjection {
        let Some(row) = self.get(&keys::paid_routing()) else {
            panic!("expected paid routing fixture projection");
        };
        match decode_source_control_record(&row).and_then(|document| document.deserialize()) {
            Ok(projection) => projection,
            Err(error) => panic!("invalid paid routing fixture projection: {error:?}"),
        }
    }
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.get(key))
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        if transition.mutations.iter().any(
            |mutation| matches!(mutation,RecordMutation::Put{key,..}if *key==keys::paid_routing()),
        ) {
            let callback = self.before_policy.borrow_mut().take();
            if let Some(callback) = callback {
                callback();
            }
        }
        if transition.checks.iter().any(|check| {
            self.get(&check.key).as_ref().map(|row| &row.revision)
                != check.expected_revision.as_ref()
        }) {
            return Ok(CommitOutcome::Conflict);
        }
        for mutation in transition.mutations {
            match mutation {
                RecordMutation::Put { key, record } => {
                    self.rows.borrow_mut().insert(key.0, record);
                }
                RecordMutation::Delete { key } => {
                    self.rows.borrow_mut().remove(&key.0);
                }
            }
        }
        Ok(CommitOutcome::Committed)
    }
}
impl QueryStore for Store {
    async fn query(&self, request: QueryRequest) -> Result<QueryPage, GatewayError> {
        request.validate()?;
        Err(GatewayError::unavailable(
            "This fixed-key publisher must not scan state",
            "unexpected_query",
        ))
    }
}
struct Fetch {
    body: SourceJson,
    status: u16,
    ticks: u32,
    after: Option<Callback>,
}
struct Host {
    store: Store,
    publication: RefCell<CredentialPublication>,
    responses: RefCell<VecDeque<Fetch>>,
    requests: RefCell<Vec<CatalogFetch>>,
    sequence: Cell<u32>,
}
impl Host {
    fn new(store: &Store, surplus: bool, openlux: bool) -> Self {
        let mut providers = BTreeMap::new();
        if surplus {
            providers.insert(
                CatalogProvider::Surplus,
                ProviderBinding {
                    identity: "surplus-credential".into(),
                },
            );
        }
        if openlux {
            providers.insert(
                CatalogProvider::Openlux,
                ProviderBinding {
                    identity: "openlux-discovery-credential".into(),
                },
            );
        }
        Self {
            store: store.clone(),
            publication: RefCell::new(CredentialPublication {
                providers,
                ..CredentialPublication::default()
            }),
            responses: RefCell::default(),
            requests: RefCell::default(),
            sequence: Cell::new(0),
        }
    }
    fn queue(&self, body: Value) {
        self.queue_source(source_catalog::json(&body));
    }
    fn queue_source(&self, body: SourceJson) {
        self.responses.borrow_mut().push_back(Fetch {
            body,
            status: 200,
            ticks: 0,
            after: None,
        });
    }
}
impl CatalogHost for Host {
    fn source_string_collator(
        &self,
    ) -> Arc<dyn gateway_model_selection::aliases::SourceStringCollator> {
        Arc::new(source_catalog::Collator::captured())
    }
    fn now_ms(&self) -> i64 {
        self.store.now.get()
    }
    fn unique_id(&self) -> Result<String, GatewayError> {
        let sequence = self.sequence.get() + 1;
        self.sequence.set(sequence);
        Ok(format!("observation-{sequence}"))
    }
    async fn wait(&self, milliseconds: u32) -> Result<(), GatewayError> {
        self.store
            .now
            .set(self.store.now.get() + i64::from(milliseconds));
        Ok(())
    }
    async fn publication(&self) -> Result<CredentialPublication, GatewayError> {
        Ok(self.publication.borrow().clone())
    }
    async fn fetch(&self, request: CatalogFetch) -> Result<CatalogFetchResponse, GatewayError> {
        self.requests.borrow_mut().push(request);
        let Some(response) = self.responses.borrow_mut().pop_front() else {
            panic!("unexpected provider discovery");
        };
        let mut ticks = response.ticks;
        poll_fn(|cx| {
            if ticks == 0 {
                Poll::Ready(())
            } else {
                ticks -= 1;
                cx.waker().wake_by_ref();
                Poll::Pending
            }
        })
        .await;
        if let Some(callback) = response.after {
            callback();
        }
        Ok(CatalogFetchResponse::json(
            response.status,
            response.body.as_str().to_owned(),
        ))
    }
}
fn surplus(id: &str) -> Value {
    json!({"data":[{"id":id,"supported_parameters":["tools","tool_choice"],"supported_features":["tools"],"pricing":{"prompt":"0.001","completion":"0.002"}}]})
}
fn openlux(id: &str) -> Value {
    json!({"data":[{"id":id,"supported_endpoint_types":["openai-response","openai"]}]})
}

/// Test-private failure injection through the real finite CatalogHost interface.
struct FailIds<'a> {
    host: &'a Host,
    ordinal: u32,
    calls: Cell<u32>,
}
impl<'a> FailIds<'a> {
    fn at(host: &'a Host, ordinal: u32) -> Self {
        Self {
            host,
            ordinal,
            calls: Cell::new(0),
        }
    }
}
impl CatalogHost for FailIds<'_> {
    fn source_string_collator(
        &self,
    ) -> std::sync::Arc<dyn gateway_model_selection::aliases::SourceStringCollator> {
        self.host.source_string_collator()
    }
    fn now_ms(&self) -> i64 {
        self.host.now_ms()
    }
    fn unique_id(&self) -> Result<String, GatewayError> {
        let next = self.calls.get() + 1;
        self.calls.set(next);
        if next == self.ordinal {
            return Err(GatewayError::unavailable(
                "Secure randomness is unavailable.",
                "secure_random_unavailable",
            ));
        }
        self.host.unique_id()
    }
    async fn wait(&self, milliseconds: u32) -> Result<(), GatewayError> {
        self.host.wait(milliseconds).await
    }
    async fn publication(&self) -> Result<CredentialPublication, GatewayError> {
        self.host.publication().await
    }
    async fn fetch(&self, request: CatalogFetch) -> Result<CatalogFetchResponse, GatewayError> {
        self.host.fetch(request).await
    }
    async fn observe(
        &self,
        observation: gateway_catalog_provider::host::CatalogObservation,
    ) -> Result<(), GatewayError> {
        self.host.observe(observation).await
    }
    async fn refresh_codex(
        &self,
        account: &gateway_catalog_provider::host::CodexAccount,
    ) -> Result<gateway_catalog_provider::host::CodexAccount, GatewayError> {
        self.host.refresh_codex(account).await
    }
}

mod cases;
