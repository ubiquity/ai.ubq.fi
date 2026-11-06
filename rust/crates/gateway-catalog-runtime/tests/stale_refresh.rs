//! Stale-while-revalidate: a leased stale catalog answers before its scheduled refresh runs.
use gateway_catalog_runtime::CatalogRuntime;
use gateway_catalog_runtime::host::{
    CatalogFetch, CatalogFetchResponse, CatalogHost, CodexAccount, CredentialPublication,
    LeasedRefresh,
};
use gateway_catalog_runtime::state::{self, source_key};
use gateway_core::error::GatewayError;
use gateway_core::query::{QueryEntry, QueryPage, QueryRequest, QueryStore};
use gateway_core::source_control::decode_source_control_record;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as source;
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, RevisionCheck,
    StoredRecord,
};
use gateway_core::tuple_key::TupleKey;
use gateway_routing_runtime_test_support::source_catalog::Collator;
use gateway_schema::catalog_cache::{self, CatalogResponse};
use gateway_schema::catalog_store;
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, VecDeque},
    future::Future,
    rc::Rc,
    sync::Arc,
    task::{Context, Poll, Waker},
};
type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
/// Converts a missing value or a failed result into the test's error instead of a panic.
trait Must<T> {
    fn must(self) -> TestResult<T>;
}
impl<T> Must<T> for Option<T> {
    fn must(self) -> TestResult<T> {
        self.ok_or_else(|| "missing value".into())
    }
}
impl<T, E: std::fmt::Debug> Must<T> for Result<T, E> {
    fn must(self) -> TestResult<T> {
        self.map_err(|error| format!("{error:?}").into())
    }
}

const VERSION: &str = "0.170.0";

fn run<T>(future: impl Future<Output = T>) -> T {
    let mut cx = Context::from_waker(Waker::noop());
    let mut future = std::pin::pin!(future);
    loop {
        if let Poll::Ready(result) = future.as_mut().poll(&mut cx) {
            return result;
        }
    }
}

#[derive(Clone)]
struct Store {
    rows: Rc<RefCell<BTreeMap<String, StoredRecord>>>,
    now: Rc<Cell<i64>>,
}
impl Store {
    fn new() -> Self {
        Self {
            rows: Rc::default(),
            now: Rc::new(Cell::new(1_800_000_000_000)),
        }
    }
    fn put(&self, key: RecordKey, revision: &str, value: Value) {
        let record = StoredRecord::json(revision, value, None);
        self.rows.borrow_mut().insert(key.0, record);
    }
    fn live(&self, key: &RecordKey) -> Option<StoredRecord> {
        let now = self.now.get();
        let rows = self.rows.borrow();
        rows.get(&key.0)
            .filter(|r| r.expires_at_ms.is_none_or(|at| at > now))
            .cloned()
    }
    fn seed_stale(&self, id: &str, etag: Option<&str>) -> TestResult {
        self.put(catalog_store::auth_generation_key(), "g", "g".into());
        let fetched = self.now.get() - catalog_cache::FRESH_MS - 1;
        let identity = catalog_cache::CatalogIdentity {
            client_version: VERSION,
            auth_generation: "g",
            body_generation: "seed",
        };
        let body = catalog(id).to_string();
        let prepared =
            catalog_cache::prepare_catalog(identity, &body, etag, None, fetched, &BTreeMap::new())?;
        assert!(run(catalog_store::publish_catalog(self, &prepared))?);
        Ok(())
    }
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
        let mut rows = self.rows.borrow_mut();
        for mutation in transition.mutations {
            match mutation {
                RecordMutation::Put { key, record } => rows.insert(key.0, record),
                RecordMutation::Delete { key } => rows.remove(&key.0),
            };
        }
        Ok(CommitOutcome::Committed)
    }
}
impl QueryStore for Store {
    async fn query(&self, request: QueryRequest) -> Result<QueryPage, GatewayError> {
        let validated = request.validate()?;
        let mut entries = Vec::new();
        for (key, record) in self.rows.borrow().iter() {
            let key = RecordKey(key.clone());
            if validated.accepts_record(&key, record)? {
                let record = record.clone();
                entries.push(QueryEntry { key, record });
            }
        }
        let mut keyed = (entries.into_iter())
            .map(|row| Ok((TupleKey::from_record_key(&row.key)?, row)))
            .collect::<Result<Vec<_>, GatewayError>>()?;
        keyed.sort_by(|left, right| left.0.cmp(&right.0));
        let mut entries: Vec<_> = keyed.into_iter().map(|(_, row)| row).collect();
        if request.reverse {
            entries.reverse();
        }
        let more = entries.len() > request.limit as usize;
        entries.truncate(request.limit as usize);
        let next_cursor = match entries.last() {
            Some(row) if more => Some(validated.cursor_after(&row.key)?),
            _ => None,
        };
        Ok(QueryPage {
            entries,
            next_cursor,
        })
    }
}

/// Records scheduled refreshes instead of running them, so a test settles each one explicitly.
struct Host {
    store: Store,
    published: CredentialPublication,
    responses: RefCell<VecDeque<CatalogFetchResponse>>,
    requests: RefCell<Vec<CatalogFetch>>,
    scheduled: RefCell<Vec<LeasedRefresh>>,
    sequence: Cell<u32>,
}
impl Host {
    fn new(store: &Store) -> Self {
        let pool = source_key(&["ubq_ai", "codex_auth"]);
        store.put(pool.clone(), "p1", json!({"test":"nonsecret-publication"}));
        let selection = json!({"provider_ids":["codex"],"updated_at_ms":store.now.get()});
        store.put(state::selection_key(), "selection", selection);
        let account = CodexAccount {
            account_id: "account-a".into(),
            credential_fingerprint: "credential-a".into(),
        };
        let check = RevisionCheck {
            key: pool,
            expected_revision: Some("p1".into()),
        };
        Self {
            store: store.clone(),
            published: CredentialPublication {
                codex_accounts: vec![account],
                source_checks: vec![check],
                providers: BTreeMap::new(),
            },
            responses: RefCell::default(),
            requests: RefCell::default(),
            scheduled: RefCell::default(),
            sequence: Cell::new(0),
        }
    }
    fn queue(&self, response: CatalogFetchResponse) {
        self.responses.borrow_mut().push_back(response);
    }
    fn take_job(&self) -> LeasedRefresh {
        let mut scheduled = self.scheduled.borrow_mut();
        assert_eq!(scheduled.len(), 1, "exactly one refresh must be scheduled");
        scheduled.remove(0)
    }
}
impl CatalogHost for Host {
    fn source_string_collator(&self) -> Arc<dyn gateway_schema::aliases::SourceStringCollator> {
        Arc::new(Collator::captured())
    }
    fn now_ms(&self) -> i64 {
        self.store.now.get()
    }
    fn unique_id(&self) -> String {
        self.sequence.set(self.sequence.get() + 1);
        format!("uuid-{}", self.sequence.get())
    }
    async fn wait(&self, milliseconds: u32) -> Result<(), GatewayError> {
        let now = self.store.now.get();
        self.store.now.set(now + i64::from(milliseconds));
        Ok(())
    }
    async fn publication(&self) -> Result<CredentialPublication, GatewayError> {
        Ok(self.published.clone())
    }
    async fn fetch(&self, request: CatalogFetch) -> Result<CatalogFetchResponse, GatewayError> {
        self.requests.borrow_mut().push(request);
        let next = self.responses.borrow_mut().pop_front();
        next.ok_or_else(|| GatewayError::new(500, "fixture response queue is empty", None))
    }
    fn schedule_refresh(&self, job: LeasedRefresh) -> Option<LeasedRefresh> {
        self.scheduled.borrow_mut().push(job);
        None
    }
}

fn catalog(id: &str) -> Value {
    json!({"models":[{"slug":id}]})
}
fn ids(response: &CatalogResponse) -> TestResult<Vec<String>> {
    let body: Value = serde_json::from_str(response.body.as_ref().must()?).must()?;
    let models = body["models"].as_array().must()?;
    Ok(models
        .iter()
        .filter_map(|r| r["slug"].as_str())
        .map(str::to_owned)
        .collect())
}
fn lease_held(store: &Store) -> bool {
    store.live(&catalog_store::lease_key(VERSION)).is_some()
}

#[test]
fn stale_cache_returns_before_its_background_refresh_settles() -> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    store.seed_stale("stale", Some("\"catalog-etag\""))?;
    let stale = run(runtime.versioned_models(&store, &host, VERSION, None))?;
    assert_eq!(stale.headers["x-uos-cache"], "stale");
    assert_eq!(
        stale.headers["Cache-Control"],
        "private, max-age=300, stale-while-revalidate=86400"
    );
    assert_eq!(ids(&stale)?, ["stale"]);
    // The answer precedes any upstream request; the scheduled refresh still owns the lease.
    assert!(host.requests.borrow().is_empty());
    assert!(lease_held(&store));
    let job = host.take_job();

    host.queue(CatalogFetchResponse::json(
        200,
        catalog("refreshed").to_string(),
    ));
    let settled = run(runtime.refresh_leased(&store, &host, &job, None))?;
    assert_eq!(ids(&settled)?, ["refreshed"]);
    assert!(!lease_held(&store));
    let requests = host.requests.borrow().clone();
    assert!(matches!(
        requests.as_slice(),
        [CatalogFetch::Codex { if_none_match: Some(tag), .. }] if tag == "\"catalog-etag\""
    ));

    let refreshed = run(runtime.versioned_models(&store, &host, VERSION, None))?;
    assert_eq!(refreshed.headers["x-uos-cache"], "hit");
    assert_eq!(ids(&refreshed)?, ["refreshed"]);
    assert!(host.scheduled.borrow().is_empty());
    Ok(())
}

#[test]
fn failed_background_refresh_retains_stale_body_and_releases_lease() -> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    store.seed_stale("stale", None)?;
    let response = run(runtime.versioned_models(&store, &host, VERSION, None))?;
    assert_eq!(ids(&response)?, ["stale"]);
    assert_eq!(response.headers["x-uos-cache"], "stale");
    host.queue(CatalogFetchResponse::json(503, "{}"));
    let recovered = run(runtime.refresh_leased(&store, &host, &host.take_job(), None))?;
    assert_eq!(ids(&recovered)?, ["stale"]);
    assert_eq!(recovered.headers["x-uos-cache"], "stale");
    assert!(!lease_held(&store));
    // The retained stale body still answers, and the next request schedules a new refresh.
    let again = run(runtime.versioned_models(&store, &host, VERSION, None))?;
    assert_eq!(ids(&again)?, ["stale"]);
    assert!(lease_held(&store));
    host.take_job();
    Ok(())
}

#[test]
fn cold_miss_still_waits_for_the_upstream_catalog() -> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.queue(CatalogFetchResponse::json(200, catalog("cold").to_string()));
    let response = run(runtime.versioned_models(&store, &host, VERSION, None))?;
    assert_eq!(response.headers["x-uos-cache"], "miss");
    assert_eq!(ids(&response)?, ["cold"]);
    assert!(host.scheduled.borrow().is_empty());
    assert!(!lease_held(&store));
    Ok(())
}

#[test]
fn single_account_evidence_retains_trimmed_units_and_distinguishes_escape_text() -> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    let raw = r#"{"models":[{"slug":" \ud800 "},{"slug":"\ud800"},{"slug":"\\ud800"},{"id":"\udc00"},{"slug":" "}]}"#;
    let mut response = CatalogFetchResponse::json(200, raw);
    response.single_account_observation_complete = true;
    host.queue(response);
    // A blank row rejects the catalog body, but the account evidence is published first.
    let _rejected = run(runtime.versioned_models(&store, &host, VERSION, None));
    let evidence = store.live(&state::account_models_key()).must()?;
    let document = decode_source_control_record(&evidence).must()?;
    let accounts = source::member(&document, "accounts").must()?;
    let account = source::member(&accounts, "account-a").must()?;
    assert_eq!(
        source::member(&account, "slugs"),
        Some(SourceJson::parse(r#"["\ud800","\\ud800","\udc00"]"#).must()?)
    );
    Ok(())
}
