type TestResult = Result<(), Box<dyn std::error::Error>>;
use gateway_catalog_inputs::state::{self, source_key};
use gateway_catalog_provider::host::{
    CatalogFetch, CatalogFetchResponse, CatalogHost, CatalogObservation, CatalogProvider,
    CodexAccount, CredentialPublication, ProviderBinding,
};
use gateway_core::error::GatewayError;
use gateway_core::query::{QueryEntry, QueryPage, QueryRequest, QueryStore};

use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, RevisionCheck,
    StoredRecord,
};
use gateway_core::tuple_key::TupleKey;
use gateway_routing_runtime_test_support::source_catalog::{self, Collator};
use gateway_schema::catalog_cache;
use gateway_schema::catalog_store;
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, VecDeque},
    future::{Future, poll_fn},
    rc::Rc,
    sync::Arc,
    task::{Context, Poll, Waker},
};
fn loader_fixture() -> Collator {
    Collator::from_catalog_json(include_str!("../fixtures/catalog-loader-sort-v1.json"))
}
fn run<T>(future: impl Future<Output = T>) -> T {
    let waker = Waker::noop();
    let mut cx = Context::from_waker(waker);
    let mut future = std::pin::pin!(future);
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
    after_chunks: Rc<RefCell<Option<Callback>>>,
    fail_query: Rc<Cell<bool>>,
}
impl Store {
    fn new() -> Self {
        Self {
            rows: Rc::default(),
            now: Rc::new(Cell::new(1_800_000_000_000)),
            after_chunks: Rc::default(),
            fail_query: Rc::default(),
        }
    }
    fn put(&self, key: RecordKey, revision: &str, value: Value) {
        self.rows
            .borrow_mut()
            .insert(key.0, StoredRecord::json(revision, value, None));
    }
    fn live(&self, key: &RecordKey) -> Option<StoredRecord> {
        self.rows
            .borrow()
            .get(&key.0)
            .filter(|r| r.expires_at_ms.is_none_or(|at| at > self.now.get()))
            .cloned()
    }
    fn seed(&self, version: &str, fetched: i64, body: &str, etag: Option<&str>) -> TestResult {
        self.put(catalog_store::auth_generation_key(), "g", "g".into());
        let prepared = catalog_cache::prepare_catalog(
            catalog_cache::CatalogIdentity {
                client_version: version,
                auth_generation: "g",
                body_generation: &format!("seed-{version}-{fetched}"),
            },
            body,
            etag,
            None,
            fetched,
            &BTreeMap::new(),
        )?;
        assert!(run(catalog_store::publish_catalog(self, &prepared))?);
        Ok(())
    }
}
fn is_catalog_chunk(key: &RecordKey) -> Result<bool, GatewayError> {
    let tuple = TupleKey::from_record_key(key)?;
    for name in ["codex_catalog_chunk", "codex_catalog_source_chunk"] {
        let prefix = TupleKey::from_record_key(&source_key(&["ubq_ai", name]))?;
        if tuple.is_strict_descendant_of(&prefix) {
            return Ok(true);
        }
    }
    Ok(false)
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.live(key))
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        if transition.checks.iter().any(|c| {
            self.live(&c.key).as_ref().map(|r| &r.revision) != c.expected_revision.as_ref()
        }) {
            return Ok(CommitOutcome::Conflict);
        }
        let mut chunks = false;
        for mutation in &transition.mutations {
            if let RecordMutation::Put { key, .. } = mutation
                && is_catalog_chunk(key)?
            {
                chunks = true;
                break;
            }
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
        if chunks {
            let callback = self.after_chunks.borrow_mut().take();
            if let Some(callback) = callback {
                callback();
            }
        }
        Ok(CommitOutcome::Committed)
    }
}
impl QueryStore for Store {
    async fn query(&self, request: QueryRequest) -> Result<QueryPage, GatewayError> {
        if self.fail_query.get() {
            return Err(GatewayError::unavailable(
                "Query unavailable",
                "test_query_unavailable",
            ));
        }
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
    response: Result<CatalogFetchResponse, GatewayError>,
    ticks: u32,
    callback: Option<Callback>,
}
impl Outcome {
    fn json(body: Value) -> Self {
        Self::response(200, body.to_string())
    }
    fn response(status: u16, body: impl Into<String>) -> Self {
        Self {
            response: Ok(CatalogFetchResponse::json(status, body)),
            ticks: 0,
            callback: None,
        }
    }
}
struct Host {
    store: Store,
    published: RefCell<CredentialPublication>,
    responses: RefCell<VecDeque<Outcome>>,
    requests: RefCell<Vec<CatalogFetch>>,
    sequence: Cell<u32>,
    on_wait: RefCell<Option<Callback>>,
    renewal: RefCell<Option<CodexAccount>>,
    observations: RefCell<Vec<CatalogObservation>>,
    fail_observation: Cell<bool>,
}
impl Host {
    fn new(store: &Store) -> Self {
        let pool = source_key(&["ubq_ai", "codex_auth"]);
        store.put(pool.clone(), "p1", json!({"test":"nonsecret-publication"}));
        store.put(
            state::selection_key(),
            "selection",
            json!({"provider_ids":["codex"],"updated_at_ms":store.now.get()}),
        );
        Self {
            store: store.clone(),
            published: RefCell::new(CredentialPublication {
                codex_accounts: vec![CodexAccount {
                    account_id: "account-a".into(),
                    credential_fingerprint: "credential-a".into(),
                }],
                source_checks: vec![RevisionCheck {
                    key: pool,
                    expected_revision: Some("p1".into()),
                }],
                providers: BTreeMap::new(),
            }),
            responses: RefCell::default(),
            requests: RefCell::default(),
            sequence: Cell::new(0),
            on_wait: RefCell::default(),
            renewal: RefCell::default(),
            observations: RefCell::default(),
            fail_observation: Cell::new(false),
        }
    }
    fn queue(&self, response: Outcome) {
        self.responses.borrow_mut().push_back(response);
    }
    fn provider(&self, provider: CatalogProvider, identity: &str) {
        self.published.borrow_mut().providers.insert(
            provider,
            ProviderBinding {
                identity: identity.into(),
            },
        );
    }
}
impl CatalogHost for Host {
    fn source_string_collator(&self) -> Arc<dyn gateway_schema::aliases::SourceStringCollator> {
        Arc::new(source_catalog::Collator::captured())
    }
    fn now_ms(&self) -> i64 {
        self.store.now.get()
    }
    fn unique_id(&self) -> Result<String, GatewayError> {
        let n = self.sequence.get() + 1;
        self.sequence.set(n);
        Ok(format!("uuid-{n}"))
    }
    async fn wait(&self, milliseconds: u32) -> Result<(), GatewayError> {
        self.store
            .now
            .set(self.store.now.get() + i64::from(milliseconds));
        let callback = self.on_wait.borrow_mut().take();
        if let Some(callback) = callback {
            callback();
        }
        Ok(())
    }
    async fn publication(&self) -> Result<CredentialPublication, GatewayError> {
        Ok(self.published.borrow().clone())
    }
    async fn fetch(&self, request: CatalogFetch) -> Result<CatalogFetchResponse, GatewayError> {
        self.requests.borrow_mut().push(request);
        let Some(outcome) = self.responses.borrow_mut().pop_front() else {
            panic!("unexpected catalog fetch");
        };
        let mut ticks = outcome.ticks;
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
        if let Some(callback) = outcome.callback {
            callback();
        }
        outcome.response
    }
    async fn observe(&self, observation: CatalogObservation) -> Result<(), GatewayError> {
        self.observations.borrow_mut().push(observation);
        if self.fail_observation.get() {
            return Err(GatewayError::unavailable(
                "Optional sink unavailable",
                "catalog_observation_unavailable",
            ));
        }
        Ok(())
    }
    async fn refresh_codex(&self, expected: &CodexAccount) -> Result<CodexAccount, GatewayError> {
        let replacement = self.renewal.borrow_mut().take().ok_or_else(|| {
            GatewayError::unavailable(
                "Native credential owner unavailable",
                "catalog_credential_owner_unavailable",
            )
        })?;
        assert_eq!(expected.account_id, replacement.account_id);
        let mut published = self.published.borrow_mut();
        for account in &mut published.codex_accounts {
            if account.account_id == replacement.account_id {
                *account = replacement.clone();
            }
        }
        for fence in &mut published.source_checks {
            fence.expected_revision = Some("p2".into());
            self.store
                .put(fence.key.clone(), "p2", json!({"test":"renewed"}));
        }
        Ok(replacement)
    }
}
fn catalog(id: &str) -> Value {
    json!({"models":[{"slug":id,"supported_reasoning_levels":[{"effort":null},{"effort":"ultra"},{"effort":"custom-tier"}],"default_reasoning_level":null}]})
}

mod cases;
