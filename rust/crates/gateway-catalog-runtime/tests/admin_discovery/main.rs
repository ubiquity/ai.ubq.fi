use gateway_admin_test_support::ranked_collator::FixtureCollator;
use gateway_catalog_admin::{
    admin::{AdminCatalogHost, AdminCatalogOperation, CatalogAdmin},
    response::AdminCatalogResponse,
};
use gateway_catalog_provider::host::{
    CatalogFetch, CatalogFetchResponse, CatalogHost, CatalogProvider, CredentialPublication,
    ProviderBinding,
};
use gateway_catalog_runtime::CatalogRuntime;
use gateway_core::{
    query::{QueryEntry, QueryPage, QueryRequest, QueryStore},
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord},
};
use gateway_kv_codec::{key::RecordKey, tuple_key::TupleKey};
use gateway_model_selection::aliases::SourceStringCollator;
use gateway_schema::catalog_views::{
    ordinary_models as ordinary_models_source, public_catalog as public_catalog_source,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::Value;
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, VecDeque},
    future::{Future, poll_fn},
    rc::Rc,
    sync::Arc,
    task::{Context, Poll, Waker},
};

// Explicit scalar observations of these unchanged frozen scalar fixtures, not a runtime fallback.
fn lift(value: &Value) -> SourceJson {
    match SourceJson::from_serializable(value) {
        Ok(document) => document,
        Err(error) => panic!("frozen scalar fixture did not serialize: {error:?}"),
    }
}
fn scalar(value: &SourceJson) -> Value {
    match value.deserialize() {
        Ok(value) => value,
        Err(error) => panic!("frozen fixture is not scalar JSON: {error:?}"),
    }
}
fn ordinary_models(input: &gateway_catalog_input::catalog::CatalogInput) -> Value {
    scalar(&ordinary_models_source(input))
}
fn public_catalog(input: &gateway_catalog_input::catalog::CatalogInput) -> Value {
    scalar(&public_catalog_source(input, Host::new().source_string_collator().as_ref()).unwrap())
}
struct ScalarResponse {
    body: Value,
    headers: BTreeMap<String, String>,
}
fn scalar_response(response: AdminCatalogResponse) -> ScalarResponse {
    ScalarResponse {
        body: scalar(&response.body),
        headers: response.headers,
    }
}
fn run<T>(future: impl Future<Output = T>) -> T {
    let waker = Waker::noop();
    let mut context = Context::from_waker(waker);
    let mut future = std::pin::pin!(future);
    for _ in 0..10_000 {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
    }
    panic!("test future did not settle");
}
type Hook = Rc<dyn Fn()>;
#[derive(Clone, Default)]
struct Store {
    rows: Rc<RefCell<BTreeMap<String, StoredRecord>>>,
    fail_key: Rc<RefCell<Option<RecordKey>>>,
    before_commit: Rc<RefCell<Option<Hook>>>,
    forced_conflicts: Rc<Cell<u32>>,
    attempts: Rc<Cell<u32>>,
}
impl Store {
    fn put(&self, key: RecordKey, revision: &str, value: Value) {
        self.rows
            .borrow_mut()
            .insert(key.0, StoredRecord::json(revision, value, None));
    }
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        if self.fail_key.borrow().as_ref() == Some(key) {
            return Err(GatewayError::unavailable(
                "Fixture read unavailable",
                "fixture_read",
            ));
        }
        Ok(self.rows.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        self.attempts.set(self.attempts.get() + 1);
        let hook = self.before_commit.borrow_mut().take();
        if let Some(hook) = hook {
            hook();
        }
        if self.forced_conflicts.get() > 0 {
            self.forced_conflicts.set(self.forced_conflicts.get() - 1);
            return Ok(CommitOutcome::Conflict);
        }
        if transition.checks.iter().any(|check| {
            self.rows.borrow().get(&check.key.0).map(|r| &r.revision)
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
        let validated = request.validate()?;
        let mut rows = Vec::new();
        for (key, record) in self.rows.borrow().iter() {
            let key = RecordKey(key.clone());
            if validated.accepts_record(&key, record)? {
                rows.push((
                    TupleKey::from_record_key(&key)?,
                    QueryEntry {
                        key,
                        record: record.clone(),
                    },
                ));
            }
        }
        rows.sort_by_key(|(key, _)| key.clone());
        let mut rows = rows.into_iter().map(|(_, row)| row).collect::<Vec<_>>();
        if request.reverse {
            rows.reverse();
        }
        let more = rows.len() > request.limit as usize;
        rows.truncate(request.limit as usize);
        let next_cursor = if more {
            rows.last()
                .map(|row| validated.cursor_after(&row.key))
                .transpose()?
        } else {
            None
        };
        Ok(QueryPage {
            entries: rows,
            next_cursor,
        })
    }
}
struct Outcome {
    body: Result<Value, GatewayError>,
    gate: Option<Rc<Cell<bool>>>,
    hook: Option<Hook>,
}
impl Outcome {
    fn json(body: Value) -> Self {
        Self {
            body: Ok(body),
            gate: None,
            hook: None,
        }
    }
}
struct Host {
    now: Cell<i64>,
    sequence: Cell<u32>,
    publication: RefCell<CredentialPublication>,
    responses: RefCell<BTreeMap<CatalogProvider, VecDeque<Outcome>>>,
    codex: RefCell<VecDeque<Outcome>>,
    requests: RefCell<Vec<CatalogFetch>>,
    order: Vec<String>,
    fail_compare: Cell<bool>,
    fail_publication: Cell<bool>,
    warmed: Cell<u32>,
}
impl Host {
    fn new() -> Self {
        let Ok(golden) =
            serde_json::from_str::<Value>(include_str!("../fixtures/admin-catalog-golden.json"))
        else {
            panic!("frozen admin catalog fixture is invalid JSON");
        };
        let Some(order) = golden["locale_order"].as_array().and_then(|values| {
            values
                .iter()
                .map(|value| value.as_str().map(str::to_owned))
                .collect::<Option<Vec<_>>>()
        }) else {
            panic!("frozen locale order is not an array of strings");
        };
        Self {
            now: Cell::new(1_800_000_000_000),
            sequence: Cell::new(0),
            publication: RefCell::default(),
            responses: RefCell::default(),
            codex: RefCell::default(),
            requests: RefCell::default(),
            order,
            fail_compare: Cell::new(false),
            fail_publication: Cell::new(false),
            warmed: Cell::new(0),
        }
    }
    fn configure(&self, provider: CatalogProvider, identity: &str) {
        self.publication.borrow_mut().providers.insert(
            provider,
            ProviderBinding {
                identity: identity.into(),
            },
        );
    }
    fn queue(&self, provider: CatalogProvider, body: Value) {
        self.responses
            .borrow_mut()
            .entry(provider)
            .or_default()
            .push_back(Outcome::json(body));
    }
}
impl CatalogHost for Host {
    fn source_string_collator(&self) -> Arc<dyn SourceStringCollator> {
        Arc::new(FixtureCollator {
            order: self.order.clone(),
            failed: self.fail_compare.get(),
        })
    }
    fn now_ms(&self) -> i64 {
        self.now.get()
    }
    fn unique_id(&self) -> Result<String, GatewayError> {
        let id = self.sequence.get() + 1;
        self.sequence.set(id);
        Ok(format!("m24-{id}"))
    }
    async fn wait(&self, milliseconds: u32) -> Result<(), GatewayError> {
        let mut yielded = false;
        poll_fn(|context| {
            if yielded {
                Poll::Ready(())
            } else {
                yielded = true;
                context.waker().wake_by_ref();
                Poll::Pending
            }
        })
        .await;
        self.now.set(self.now.get() + i64::from(milliseconds));
        Ok(())
    }
    async fn publication(&self) -> Result<CredentialPublication, GatewayError> {
        if self.fail_publication.get() {
            return Err(GatewayError::unavailable(
                "Publication unavailable",
                "fixture_publication",
            ));
        }
        Ok(self.publication.borrow().clone())
    }
    async fn fetch(&self, request: CatalogFetch) -> Result<CatalogFetchResponse, GatewayError> {
        let outcome = match &request {
            CatalogFetch::Provider { provider, .. } => self
                .responses
                .borrow_mut()
                .get_mut(provider)
                .and_then(VecDeque::pop_front),
            CatalogFetch::Codex { .. } => self.codex.borrow_mut().pop_front(),
            CatalogFetch::AnonymousSurplus => self
                .responses
                .borrow_mut()
                .get_mut(&CatalogProvider::Surplus)
                .and_then(VecDeque::pop_front),
        };
        let Some(outcome) = outcome else {
            panic!("unexpected metadata fetch");
        };
        self.requests.borrow_mut().push(request);
        if let Some(hook) = outcome.hook {
            hook();
        }
        if let Some(gate) = outcome.gate {
            poll_fn(|context| {
                if gate.get() {
                    Poll::Ready(())
                } else {
                    context.waker().wake_by_ref();
                    Poll::Pending
                }
            })
            .await;
        }
        Ok(CatalogFetchResponse::json(200, outcome.body?.to_string()))
    }
}
impl AdminCatalogHost for Host {
    fn warm_openrouter(&self) {
        self.warmed.set(self.warmed.get() + 1);
    }
}
fn invoke(
    runtime: &CatalogRuntime,
    store: &Store,
    host: &Host,
    operation: AdminCatalogOperation,
) -> ScalarResponse {
    let response = match run(runtime.admin_catalog(store, host, operation)) {
        Ok(response) => response,
        Err(error) => panic!("catalog admin fixture operation failed: {error:?}"),
    };
    scalar_response(response)
}

mod cases;
