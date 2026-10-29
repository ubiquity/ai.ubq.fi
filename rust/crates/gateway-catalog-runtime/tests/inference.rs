use gateway_catalog_input::catalog::CatalogInput;
use gateway_catalog_inputs::state;
use gateway_catalog_provider::host::{
    CatalogFetch, CatalogFetchResponse, CatalogHost, CatalogProvider, CredentialPublication,
    ProviderBinding,
};
use gateway_catalog_runtime::CatalogRuntime;
use gateway_contracts::model::ModelId;
use gateway_request_normalization::document::SourceRequestDocument;
use gateway_routing_runtime_test_support::{
    exec::run, input::NOW, source_catalog::Collator, store::Store,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::json;
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, VecDeque},
    future::{Future, poll_fn},
    sync::Arc,
    task::{Context, Poll, Waker},
};

type TestResult = Result<(), Box<dyn std::error::Error>>;
const ALIAS: &str = "~deepseek/deepseek-flash-latest";

struct Host {
    publication: RefCell<CredentialPublication>,
    replies: RefCell<VecDeque<Result<CatalogFetchResponse, GatewayError>>>,
    requests: RefCell<Vec<CatalogFetch>>,
    ready: Cell<bool>,
}
impl Host {
    fn new() -> Self {
        Self {
            publication: RefCell::new(CredentialPublication {
                providers: BTreeMap::from([(
                    CatalogProvider::Openrouter,
                    ProviderBinding {
                        identity: "fixture-key".into(),
                    },
                )]),
                ..Default::default()
            }),
            replies: RefCell::default(),
            requests: RefCell::default(),
            ready: Cell::new(true),
        }
    }
    fn models(&self, ids: &[&str]) {
        self.replies
            .borrow_mut()
            .push_back(Ok(CatalogFetchResponse::json(
                200,
                json!({"data":ids.iter().map(|id|json!({"id":id})).collect::<Vec<_>>()} )
                    .to_string(),
            )));
    }
}
impl CatalogHost for Host {
    fn source_string_collator(
        &self,
    ) -> Arc<dyn gateway_model_selection::aliases::SourceStringCollator> {
        Arc::new(Collator::captured())
    }
    fn now_ms(&self) -> i64 {
        NOW
    }
    fn unique_id(&self) -> Result<String, GatewayError> {
        Ok("fixture-cold-catalog".into())
    }
    async fn publication(&self) -> Result<CredentialPublication, GatewayError> {
        Ok(self.publication.borrow().clone())
    }
    async fn wait(&self, _milliseconds: u32) -> Result<(), GatewayError> {
        poll_fn(|cx| {
            if self.ready.get() {
                Poll::Ready(Ok(()))
            } else {
                cx.waker().wake_by_ref();
                Poll::Pending
            }
        })
        .await
    }
    async fn fetch(&self, request: CatalogFetch) -> Result<CatalogFetchResponse, GatewayError> {
        self.requests.borrow_mut().push(request);
        self.wait(0).await?;
        self.replies
            .borrow_mut()
            .pop_front()
            .ok_or_else(|| GatewayError::new(500, "Unexpected fixture catalog call", None))?
    }
}
fn store() -> Store {
    let store = Store::new();
    store.seed(
        state::selection_key(),
        json!({"provider_ids":["ubiquity","openrouter"],"updated_at_ms":NOW}),
    );
    store
}
fn document(model: &str) -> Result<SourceRequestDocument, GatewayError> {
    SourceRequestDocument::new(SourceJson::from_serializable(
        &json!({"model":model,"input":"fixture"}),
    )?)
    .map_err(|error| error.diagnostic().clone())
}
fn current(
    runtime: &CatalogRuntime,
    store: &Store,
    host: &Host,
) -> Result<CatalogInput, GatewayError> {
    Ok(run(runtime.load_state(store, &host.publication.borrow(), NOW))?.input)
}
fn allowed(input: &CatalogInput, model: &str) -> bool {
    input.rows("openrouter").iter().any(|row| {
        gateway_source::source_serialization::member(row, "id")
            .and_then(|id| gateway_source::source_serialization::string(&id))
            .is_some_and(|id| ModelId::from_source(id).matches_scalar(model))
    })
}

#[test]
fn cold_synthetic_request_retains_the_discovered_alias() -> TestResult {
    let (store, host, runtime) = (store(), Host::new(), CatalogRuntime::new());
    store.seed(
        state::selection_key(),
        json!({"provider_ids":["ubiquity"],"updated_at_ms":NOW}),
    );
    host.models(&[ALIAS]);
    let mut input = current(&runtime, &store, &host)?;
    assert!(!allowed(&input, ALIAS));
    run(runtime.resolve_inference_catalog(
        &store,
        &host,
        &mut input,
        &document("ubiquity/deepseek-flash")?,
    ))?;
    assert!(allowed(&input, ALIAS));
    assert_eq!(host.requests.borrow().len(), 1);
    Ok(())
}

#[test]
fn direct_lithos_keeps_its_priority_without_waiting_for_openrouter() -> TestResult {
    let (store, host, runtime) = (store(), Host::new(), CatalogRuntime::new());
    store.seed(
        state::selection_key(),
        json!({"provider_ids":["lithos","openrouter"],"updated_at_ms":NOW}),
    );
    let mut input = current(&runtime, &store, &host)?;
    run(runtime.resolve_inference_catalog(
        &store,
        &host,
        &mut input,
        &document("deepseek-ai/DeepSeek-V4.1-Flash")?,
    ))?;
    assert!(host.requests.borrow().is_empty());
    Ok(())
}

#[test]
fn completed_background_discovery_updates_the_earlier_request_snapshot() -> TestResult {
    let (store, host, runtime) = (store(), Host::new(), CatalogRuntime::new());
    let mut input = current(&runtime, &store, &host)?;
    host.models(&[ALIAS]);
    run(runtime.refresh_provider(&store, &host, CatalogProvider::Openrouter, false))?;
    assert!(!allowed(&input, ALIAS));
    run(runtime.resolve_inference_catalog(&store, &host, &mut input, &document(ALIAS)?))?;
    assert!(allowed(&input, ALIAS));
    assert_eq!(host.requests.borrow().len(), 1);
    Ok(())
}

#[test]
fn warm_empty_or_unknown_catalog_never_invents_a_model_or_forces_refresh() -> TestResult {
    for ids in [vec![], vec!["openai/known"]] {
        let (store, host, runtime) = (store(), Host::new(), CatalogRuntime::new());
        host.models(&ids);
        run(runtime.refresh_provider(&store, &host, CatalogProvider::Openrouter, false))?;
        let mut input = current(&runtime, &store, &host)?;
        run(runtime.resolve_inference_catalog(&store, &host, &mut input, &document(ALIAS)?))?;
        assert!(!allowed(&input, ALIAS));
        assert_eq!(host.requests.borrow().len(), 1);
    }
    Ok(())
}

#[test]
fn disabled_unconfigured_or_unrelated_requests_do_not_discover() -> TestResult {
    for mode in ["disabled", "unconfigured", "unrelated", "malformed"] {
        let (store, host, runtime) = (store(), Host::new(), CatalogRuntime::new());
        if mode == "disabled" {
            store.seed(
                state::selection_key(),
                json!({"provider_ids":["codex"],"updated_at_ms":NOW}),
            );
        }
        if mode == "unconfigured" {
            host.publication.borrow_mut().providers.clear();
        }
        let mut input = current(&runtime, &store, &host)?;
        let model = match mode {
            "unrelated" => "gpt-model",
            "malformed" => "openai/has space",
            _ => ALIAS,
        };
        run(runtime.resolve_inference_catalog(&store, &host, &mut input, &document(model)?))?;
        assert!(host.requests.borrow().is_empty());
        assert!(!allowed(&input, ALIAS));
    }
    Ok(())
}

#[test]
fn discovery_failure_keeps_the_alias_unavailable_and_cancellation_propagates() -> TestResult {
    for status in [503, 499] {
        let (store, host, runtime) = (store(), Host::new(), CatalogRuntime::new());
        host.replies.borrow_mut().push_back(Err(GatewayError::new(
            status,
            "fixture discovery failure",
            None,
        )));
        let mut input = current(&runtime, &store, &host)?;
        let result =
            run(runtime.resolve_inference_catalog(&store, &host, &mut input, &document(ALIAS)?));
        if status == 499 {
            assert_eq!(result.err().map(|e| e.status), Some(499));
        } else {
            result?;
        }
        assert!(!allowed(&input, ALIAS));
    }
    Ok(())
}

#[test]
fn simultaneous_cold_requests_share_one_actual_discovery() -> TestResult {
    let (store, host, runtime) = (store(), Host::new(), CatalogRuntime::new());
    let mut first_input = current(&runtime, &store, &host)?;
    let mut second_input = first_input.clone();
    host.models(&[ALIAS]);
    host.ready.set(false);
    let request = document(ALIAS)?;
    let mut first = std::pin::pin!(runtime.resolve_inference_catalog(
        &store,
        &host,
        &mut first_input,
        &request
    ));
    let mut second = std::pin::pin!(runtime.resolve_inference_catalog(
        &store,
        &host,
        &mut second_input,
        &request
    ));
    let mut cx = Context::from_waker(Waker::noop());
    assert!(first.as_mut().poll(&mut cx).is_pending());
    assert!(second.as_mut().poll(&mut cx).is_pending());
    assert_eq!(host.requests.borrow().len(), 1);
    host.ready.set(true);
    run(first)?;
    run(second)?;
    assert_eq!(host.requests.borrow().len(), 1);
    Ok(())
}

#[test]
fn credential_replacement_during_discovery_cannot_admit_the_old_observation() -> TestResult {
    let (store, host, runtime) = (store(), Host::new(), CatalogRuntime::new());
    let mut input = current(&runtime, &store, &host)?;
    host.models(&[ALIAS]);
    host.ready.set(false);
    let request = document(ALIAS)?;
    {
        let mut pending =
            std::pin::pin!(runtime.resolve_inference_catalog(&store, &host, &mut input, &request));
        assert!(
            pending
                .as_mut()
                .poll(&mut Context::from_waker(Waker::noop()))
                .is_pending()
        );
        host.publication.borrow_mut().providers.insert(
            CatalogProvider::Openrouter,
            ProviderBinding {
                identity: "replaced-fixture-key".into(),
            },
        );
        host.ready.set(true);
        run(pending)?;
    }
    assert!(!allowed(&input, ALIAS));
    assert_eq!(host.requests.borrow().len(), 1);
    Ok(())
}
