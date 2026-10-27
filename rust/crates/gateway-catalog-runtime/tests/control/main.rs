use gateway_authority::{keys, paid_control::*};
use gateway_catalog_provider::host::{
    CatalogFetch, CatalogFetchResponse, CatalogHost, CatalogProvider, CredentialPublication,
    ProviderBinding,
};
use gateway_routing_runtime_test_support::source_catalog;

use gateway_core::error::GatewayError;

use gateway_core::query::{QueryPage, QueryRequest, QueryStore};
use gateway_core::source_control::decode_source_control_record;
use gateway_core::source_json::SourceJson;
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, StoredRecord,
};
use gateway_schema::{
    catalog_store, runtime_config::build_runtime_config, snapshot::ModelSnapshot,
};
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
        self.put(
            catalog_store::runtime_config_key(),
            "runtime-rev",
            build_runtime_config(&snapshot, None, None, self.now.get())
                .unwrap()
                .deserialize::<Value>()
                .unwrap(),
        );
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
        decode_source_control_record(&self.get(&keys::paid_routing()).unwrap())
            .unwrap()
            .deserialize()
            .unwrap()
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
    fn source_string_collator(&self) -> Arc<dyn gateway_schema::aliases::SourceStringCollator> {
        Arc::new(source_catalog::Collator::captured())
    }
    fn now_ms(&self) -> i64 {
        self.store.now.get()
    }
    fn unique_id(&self) -> String {
        let sequence = self.sequence.get() + 1;
        self.sequence.set(sequence);
        format!("observation-{sequence}")
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
        let response = self
            .responses
            .borrow_mut()
            .pop_front()
            .expect("unexpected provider discovery");
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

mod cases;
