use gateway_admin_test_support::ranked_collator::FixtureCollator;
use gateway_catalog_runtime::{
    CatalogRuntime,
    host::{
        CatalogFetch, CatalogFetchResponse, CatalogHost, CatalogProvider, CredentialPublication,
        ProviderBinding,
    },
    providers,
};
use gateway_core::{
    error::GatewayError,
    js_string::JsString16,
    query::{QueryPage, QueryRequest, QueryStore},
    source_json::SourceJson,
    source_serialization as source,
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, StoredRecord},
};
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, VecDeque},
    future::Future,
    sync::Arc,
    task::{Context, Poll, Waker},
    time::Instant,
};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
struct EmptyStore;
impl AuthorityStore for EmptyStore {
    async fn read(&self, _: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(None)
    }
    async fn commit(&self, _: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        Err(GatewayError::unavailable(
            "Unexpected fixture write",
            "fixture_write",
        ))
    }
}
impl QueryStore for EmptyStore {
    async fn query(&self, _: QueryRequest) -> Result<QueryPage, GatewayError> {
        Err(GatewayError::unavailable(
            "Unexpected fixture query",
            "fixture_query",
        ))
    }
}
struct Host {
    provider: CatalogProvider,
    pages: RefCell<VecDeque<String>>,
    fetches: Cell<usize>,
}
impl Host {
    fn new(provider: CatalogProvider, pages: Vec<String>) -> Self {
        Self {
            provider,
            pages: RefCell::new(pages.into()),
            fetches: Cell::new(0),
        }
    }
}
impl CatalogHost for Host {
    fn source_string_collator(&self) -> Arc<dyn gateway_schema::aliases::SourceStringCollator> {
        Arc::new(FixtureCollator {
            order: Vec::new(),
            failed: true,
        })
    }
    fn now_ms(&self) -> i64 {
        1_800_000_000_000
    }
    fn unique_id(&self) -> String {
        "catalog-merge-fixture".into()
    }
    async fn wait(&self, _: u32) -> Result<(), GatewayError> {
        Ok(())
    }
    async fn publication(&self) -> Result<CredentialPublication, GatewayError> {
        Ok(CredentialPublication {
            providers: BTreeMap::from([(
                self.provider,
                ProviderBinding {
                    identity: "fixture-only".into(),
                },
            )]),
            ..CredentialPublication::default()
        })
    }
    async fn fetch(&self, request: CatalogFetch) -> Result<CatalogFetchResponse, GatewayError> {
        assert!(
            matches!(request, CatalogFetch::Provider { provider, .. } if provider == self.provider)
        );
        self.fetches.set(self.fetches.get() + 1);
        self.pages
            .borrow_mut()
            .pop_front()
            .map(|body| CatalogFetchResponse::json(200, body))
            .ok_or_else(|| GatewayError::unavailable("Unexpected fixture fetch", "fixture_fetch"))
    }
}
fn run<T>(future: impl Future<Output = T>) -> T {
    let mut future = std::pin::pin!(future);
    let mut context = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
    }
}
fn refresh(provider: CatalogProvider, pages: Vec<String>) -> TestResult<Vec<SourceJson>> {
    let host = Host::new(provider, pages);
    let snapshot =
        run(CatalogRuntime::new().refresh_provider(&EmptyStore, &host, provider, false))?
            .ok_or("Fixture discovery produced no snapshot")?;
    assert!(host.pages.borrow().is_empty());
    Ok(snapshot.models)
}

#[test]
fn cross_page_updates_keep_first_position_and_exact_utf16_rows() -> TestResult {
    let first = r#"{"data":[{"id":"claude-b","display_name":"first-b"},{"id":"claude-a","display_name":"first-a"},{"id":"claude-b","display_name":"ignored-within-page"},{"id":"claude-\ud800","display_name":"surrogate-first"},{"id":"claude-\\ud800","display_name":"literal-escape"},{"id":"claude-\ufffd","display_name":"replacement"}],"has_more":true,"last_id":"next-page"}"#;
    let second = r#"{"data":[{"id":"claude-a","display_name":"latest-a"},{"id":"claude-\ud800","display_name":"surrogate-latest"},{"id":"claude-c","display_name":"new-c"},{"id":"claude-a","display_name":"ignored-second-duplicate"}],"has_more":false}"#;
    let (a, _) = providers::parse_page(CatalogProvider::ClaudeCode, &SourceJson::parse(first)?)?;
    let (b, _) = providers::parse_page(CatalogProvider::ClaudeCode, &SourceJson::parse(second)?)?;
    let actual = refresh(
        CatalogProvider::ClaudeCode,
        vec![first.into(), second.into()],
    )?;
    assert_eq!(
        actual,
        vec![
            a[0].clone(),
            b[0].clone(),
            b[1].clone(),
            a[3].clone(),
            a[4].clone(),
            b[2].clone()
        ]
    );
    let ids = actual
        .iter()
        .map(|row| source::member(row, "id").as_ref().and_then(source::string))
        .collect::<Vec<_>>();
    let mut surrogate = "claude-".encode_utf16().collect::<Vec<_>>();
    surrogate.push(0xd800);
    assert_eq!(
        ids,
        vec![
            Some("claude-b".into()),
            Some("claude-a".into()),
            Some(JsString16::from_units(surrogate)),
            Some("claude-\\ud800".into()),
            Some("claude-\u{fffd}".into()),
            Some("claude-c".into())
        ]
    );
    Ok(())
}

#[test]
fn single_page_filters_missing_and_empty_ids_and_keeps_first_duplicate() -> TestResult {
    let page = r#"{"data":[{}, {"id":null}, {"id":7}, {"id":"vendor/a","context_length":100}, {"id":"vendor/\u0061","context_length":200}, {"id":"","context_length":300}, {"id":"vendor/b","context_length":400}]}"#;
    let actual = refresh(CatalogProvider::Openrouter, vec![page.into()])?;
    let (expected, _) =
        providers::parse_page(CatalogProvider::Openrouter, &SourceJson::parse(page)?)?;
    assert_eq!(actual, expected);
    assert_eq!(actual.len(), 2);
    assert_eq!(
        source::member(&actual[0], "context_window_tokens"),
        Some(SourceJson::parse("100")?)
    );
    assert_eq!(
        source::member(&actual[1], "id"),
        Some(SourceJson::parse(r#""vendor/b""#)?)
    );
    Ok(())
}

fn thread_cpu_ns() -> TestResult<Option<u128>> {
    #[cfg(target_os = "linux")]
    {
        let stat = std::fs::read_to_string("/proc/thread-self/schedstat")?;
        Ok(Some(
            stat.split_whitespace()
                .next()
                .ok_or("Missing thread CPU time")?
                .parse()?,
        ))
    }
    #[cfg(not(target_os = "linux"))]
    {
        Ok(None)
    }
}

#[test]
fn offline_discovery_workload_reports_cpu_and_wall_time() -> TestResult {
    for count in [32, 128, 512] {
        let rows = (0..count)
            .map(|n| format!(r#"{{"id":"vendor/model-{n}","context_length":131072}}"#))
            .collect::<Vec<_>>();
        let page = format!(r#"{{"data":[{}]}}"#, rows.join(","));
        let (mut cpu, mut wall) = (Some(0_u128), 0_u128);
        for _ in 0..3 {
            let host = Host::new(CatalogProvider::Openrouter, vec![page.clone()]);
            let runtime = CatalogRuntime::new();
            let before = thread_cpu_ns()?;
            let start = Instant::now();
            let snapshot = run(runtime.refresh_provider(
                &EmptyStore,
                &host,
                CatalogProvider::Openrouter,
                false,
            ))?
            .ok_or("Workload discovery produced no snapshot")?;
            wall += start.elapsed().as_nanos();
            cpu = cpu
                .zip(before.zip(thread_cpu_ns()?))
                .map(|(sum, (before, after))| sum + after - before);
            assert_eq!(snapshot.models.len(), count);
            assert_eq!(host.fetches.get(), 1);
        }
        println!("CATALOG_DISCOVERY rows={count} repetitions=3 cpu_ns={cpu:?} wall_ns={wall}");
    }
    Ok(())
}
