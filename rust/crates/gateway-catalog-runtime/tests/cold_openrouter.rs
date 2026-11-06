use gateway_catalog_runtime::{
    CatalogRuntime,
    host::{
        CatalogFetch, CatalogFetchResponse, CatalogHost, CatalogProvider, CredentialPublication,
        ProviderBinding,
    },
};
use gateway_core::{
    error::GatewayError,
    model::ModelId,
    query::{QueryPage, QueryRequest, QueryStore},
    state::{
        AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, StoredRecord,
    },
};
use gateway_schema::{catalog::CatalogInput, catalog_store, selection::ProviderSelection};
use serde_json::json;
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, VecDeque},
    future::Future,
    rc::Rc,
    sync::Arc,
    task::{Context, Poll, Waker},
};
type TestResult = Result<(), Box<dyn std::error::Error>>;

fn run<T>(future: impl Future<Output = T>) -> T {
    let mut context = Context::from_waker(Waker::noop());
    let mut future = std::pin::pin!(future);
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
    }
}
#[derive(Clone, Default)]
struct Store(Rc<RefCell<BTreeMap<String, StoredRecord>>>);
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.0.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let mut rows = self.0.borrow_mut();
        if transition.checks.iter().any(|check| {
            rows.get(&check.key.0).map(|r| &r.revision) != check.expected_revision.as_ref()
        }) {
            return Ok(CommitOutcome::Conflict);
        }
        for mutation in transition.mutations {
            match mutation {
                RecordMutation::Put { key, record } => {
                    rows.insert(key.0, record);
                }
                RecordMutation::Delete { key } => {
                    rows.remove(&key.0);
                }
            }
        }
        Ok(CommitOutcome::Committed)
    }
}
impl QueryStore for Store {
    async fn query(&self, _: QueryRequest) -> Result<QueryPage, GatewayError> {
        Ok(QueryPage {
            entries: vec![],
            next_cursor: None,
        })
    }
}
struct Host {
    requests: Cell<usize>,
    blocked: Cell<bool>,
    publication: CredentialPublication,
    replies: RefCell<VecDeque<Result<CatalogFetchResponse, GatewayError>>>,
}
impl Host {
    async fn released(&self) {
        std::future::poll_fn(|_| {
            if self.blocked.get() {
                Poll::Pending
            } else {
                Poll::Ready(())
            }
        })
        .await;
    }
    fn new(reply: Result<CatalogFetchResponse, GatewayError>) -> Self {
        Self {
            requests: Cell::new(0),
            blocked: Cell::new(false),
            publication: CredentialPublication {
                providers: BTreeMap::from([(
                    CatalogProvider::Openrouter,
                    ProviderBinding {
                        identity: "fixture-openrouter-binding".into(),
                    },
                )]),
                ..Default::default()
            },
            replies: RefCell::new(VecDeque::from([reply])),
        }
    }
}
impl CatalogHost for Host {
    fn source_string_collator(&self) -> Arc<dyn gateway_schema::aliases::SourceStringCollator> {
        Arc::new(gateway_routing_runtime_test_support::source_catalog::Collator::captured())
    }
    fn now_ms(&self) -> i64 {
        1_800_000_000_000
    }
    fn unique_id(&self) -> String {
        format!("fixture-{}", self.requests.get())
    }
    async fn wait(&self, _: u32) -> Result<(), GatewayError> {
        self.released().await;
        Ok(())
    }
    async fn publication(&self) -> Result<CredentialPublication, GatewayError> {
        Ok(self.publication.clone())
    }
    async fn fetch(&self, request: CatalogFetch) -> Result<CatalogFetchResponse, GatewayError> {
        assert!(matches!(
            request,
            CatalogFetch::Provider {
                provider: CatalogProvider::Openrouter,
                ..
            }
        ));
        self.requests.set(self.requests.get() + 1);
        self.released().await;
        self.replies.borrow_mut().pop_front().ok_or_else(|| {
            GatewayError::unavailable(
                "Unexpected additional discovery",
                "fixture_unexpected_discovery",
            )
        })?
    }
}
fn reply() -> Result<CatalogFetchResponse, GatewayError> {
    Ok(CatalogFetchResponse::json(200,json!({"data":[{"id":"vendor/model","architecture":{"input_modalities":["text"],"output_modalities":["text"]}}]}).to_string()))
}
fn input(
    runtime: &CatalogRuntime,
    store: &Store,
    host: &Host,
) -> Result<CatalogInput, GatewayError> {
    run(runtime.load_state(store, &host.publication, host.now_ms())).map(|state| state.input)
}
#[test]
fn cold_empty_codex_request_waits_once_and_a_new_runtime_reloads_without_codex_generation()
-> TestResult {
    let (store, host, runtime) = (Store::default(), Host::new(reply()), CatalogRuntime::new());
    assert!(host.publication.codex_accounts.is_empty());
    assert!(run(store.read(&catalog_store::auth_generation_key()))?.is_none());
    let cold = input(&runtime, &store, &host)?;
    assert!(cold.rows("openrouter").is_empty());
    let loaded = run(runtime.resolve_request_model(
        &store,
        &host,
        &cold,
        &ModelId::from_scalar("vendor/model"),
        true,
    ))?
    .ok_or("Cold OpenRouter resolution did not reload the catalog")?;
    assert_eq!(host.requests.get(), 1);
    assert_eq!(loaded.input.rows("openrouter").len(), 1);
    assert!(run(store.read(&catalog_store::auth_generation_key()))?.is_none());
    let fresh = input(&CatalogRuntime::new(), &store, &host)?;
    assert_eq!(fresh.rows("openrouter"), loaded.input.rows("openrouter"));
    assert!(
        run(runtime.resolve_request_model(
            &store,
            &host,
            &loaded.input,
            &ModelId::from_scalar("other/model"),
            true
        ))?
        .is_none()
    );
    assert_eq!(host.requests.get(), 1);
    Ok(())
}
#[test]
fn disabled_unconfigured_earlier_direct_and_synthetic_requests_do_not_discover_openrouter()
-> TestResult {
    let (store, host, runtime) = (Store::default(), Host::new(reply()), CatalogRuntime::new());
    let cold = input(&runtime, &store, &host)?;
    let mut disabled = cold.clone();
    disabled.selection =
        ProviderSelection::normalize(&json!({"provider_ids":["deepseek"],"updated_at_ms":1}));
    let mut unconfigured = cold.clone();
    unconfigured
        .providers
        .get_mut("openrouter")
        .ok_or("Missing fixture provider")?
        .configured = false;
    for (input, model) in [
        (&disabled, "vendor/model"),
        (&unconfigured, "vendor/model"),
        (&cold, "ubiquity/deepseek-flash"),
        (&cold, gateway_providers::request::LITHOS_MODELS[0]),
        (&cold, "deepseek-flash"),
        (&cold, "vendor/invalid model"),
    ] {
        assert!(
            run(runtime.resolve_request_model(
                &store,
                &host,
                input,
                &ModelId::from_scalar(model),
                true
            ))?
            .is_none()
        );
    }
    assert_eq!(host.requests.get(), 0);
    Ok(())
}
#[test]
fn discovery_failure_and_cancellation_do_not_create_codex_generation_or_pool() -> TestResult {
    for status in [499, 502] {
        let (store, runtime) = (Store::default(), CatalogRuntime::new());
        let host = Host::new(Err(GatewayError::new(
            status,
            "fixture discovery failure",
            Some("fixture"),
        )));
        let cold = input(&runtime, &store, &host)?;
        let error = run(runtime.resolve_request_model(
            &store,
            &host,
            &cold,
            &ModelId::from_scalar("vendor/model"),
            true,
        ))
        .err()
        .ok_or("Discovery unexpectedly succeeded")?;
        assert_eq!(error.status, if status == 499 { 499 } else { 503 });
        assert_eq!(host.requests.get(), 1);
        assert!(run(store.read(&catalog_store::auth_generation_key()))?.is_none());
        assert!(run(store.read(&gateway_authority::keys::pool()))?.is_none());
    }
    Ok(())
}

#[test]
fn observations_keep_binding_and_non_openrouter_generation_fences() -> TestResult {
    use gateway_provider_catalog_cache::observation;
    use gateway_schema::catalog::ProviderCatalog;
    let store = Store::default();
    let now = 1_800_000_000_000;
    let snapshot = ProviderCatalog {
        models: vec![gateway_core::source_json::SourceJson::parse(
            r#"{"id":"vendor/model"}"#,
        )?],
        updated_at_ms: Some(now),
        available: true,
        ..Default::default()
    };
    let public = observation::scope("openrouter", None, Some("binding-a"))
        .ok_or("Missing fixture observation scope")?;
    run(observation::persist(&store, &public, &snapshot, "r1", now))?;
    let after_codex_change = observation::scope("openrouter", Some("new-codex"), Some("binding-a"))
        .ok_or("Missing fixture observation scope")?;
    assert_eq!(
        run(observation::load(&store, &after_codex_change, now))?,
        Some(snapshot.clone())
    );
    let rotated = observation::scope("openrouter", None, Some("binding-b"))
        .ok_or("Missing fixture observation scope")?;
    assert!(run(observation::load(&store, &rotated, now))?.is_none());
    assert!(observation::scope("lithos", None, Some("binding-a")).is_none());
    let credential = observation::scope("lithos", Some("generation-a"), Some("binding-a"))
        .ok_or("Missing fixture observation scope")?;
    run(observation::persist(
        &store,
        &credential,
        &snapshot,
        "r2",
        now,
    ))?;
    let changed = observation::scope("lithos", Some("generation-b"), Some("binding-a"))
        .ok_or("Missing fixture observation scope")?;
    assert!(run(observation::load(&store, &changed, now))?.is_none());
    Ok(())
}

#[test]
fn concurrent_requests_share_one_provider_discovery() -> TestResult {
    let (store, host, runtime) = (Store::default(), Host::new(reply()), CatalogRuntime::new());
    host.blocked.set(true);
    let mut first =
        Box::pin(runtime.refresh_provider(&store, &host, CatalogProvider::Openrouter, false));
    let mut context = Context::from_waker(Waker::noop());
    assert!(first.as_mut().poll(&mut context).is_pending());
    let mut second =
        Box::pin(runtime.refresh_provider(&store, &host, CatalogProvider::Openrouter, false));
    assert!(second.as_mut().poll(&mut context).is_pending());
    assert_eq!(host.requests.get(), 1);
    host.blocked.set(false);
    let first = run(first)?.ok_or("First discovery did not complete")?;
    let second = run(second)?.ok_or("Shared discovery did not complete")?;
    assert_eq!(first.models, second.models);
    assert_eq!(host.requests.get(), 1);
    Ok(())
}

#[test]
fn cancelled_pending_discovery_releases_its_singleflight() -> TestResult {
    let (store, host, runtime) = (Store::default(), Host::new(reply()), CatalogRuntime::new());
    host.blocked.set(true);
    let mut pending =
        Box::pin(runtime.refresh_provider(&store, &host, CatalogProvider::Openrouter, false));
    let mut context = Context::from_waker(Waker::noop());
    assert!(pending.as_mut().poll(&mut context).is_pending());
    assert_eq!(host.requests.get(), 1);
    drop(pending);
    host.blocked.set(false);
    let next = run(runtime.refresh_provider(&store, &host, CatalogProvider::Openrouter, false))?
        .ok_or("Cancelled discovery retained ownership")?;
    assert_eq!(next.models.len(), 1);
    assert_eq!(host.requests.get(), 2);
    Ok(())
}
