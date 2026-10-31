use super::{
    Host, Outcome, Store, invoke, lift, ordinary_models, public_catalog, run, scalar_response,
};
use gateway_catalog_admin::admin::{AdminCatalogOperation, CatalogAdmin};
use gateway_catalog_inputs::state;
use gateway_catalog_provider::host::{CatalogFetch, CatalogProvider};
use gateway_catalog_runtime::CatalogRuntime;
use gateway_schema::catalog_store;
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::{
    cell::Cell,
    rc::Rc,
    task::{Context, Waker},
};
#[test]
fn models_get_retains_raw_extensions_and_contains_only_optional_read_failures() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    host.fail_publication.set(true);
    let raw = json!({"models":[{"slug":"x","untouched":true}],"source":" cli ","updated_at_ms":7,"client_version":23,"extension":{"exact":true}});
    store.put(catalog_store::codex_snapshot_key(), "full", raw.clone());
    assert_eq!(
        invoke(
            &runtime,
            &store,
            &host,
            AdminCatalogOperation::CodexModelsGet
        )
        .body,
        json!({"ok":true,"data":raw})
    );
    *store.fail_key.borrow_mut() = Some(catalog_store::codex_snapshot_key());
    assert_eq!(
        invoke(
            &runtime,
            &store,
            &host,
            AdminCatalogOperation::CodexModelsGet
        )
        .body,
        json!({"ok":true,"data":null})
    );
    *store.fail_key.borrow_mut() = Some(state::whitelist_key());
    assert_eq!(
        run(runtime.admin_catalog(&store, &host, AdminCatalogOperation::WhitelistGet))
            .unwrap_err()
            .body()
            .code(),
        Some("fixture_read")
    );
}
#[test]
fn picker_is_unfiltered_uses_actual_host_order_and_keeps_runtime_when_optional_snapshot_fails() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    host.queue(CatalogProvider::Surplus, json!({"data":[]}));
    invoke(
        &runtime,
        &store,
        &host,
        AdminCatalogOperation::CodexModelsSet {
            document: lift(
                &json!({"models":[{"slug":"z"},{"slug":"A"},{"slug":"a"},{"slug":"ä"}]}),
            ),
        },
    );
    store.put(
        state::whitelist_key(),
        "whitelist",
        json!({"model_ids":["z"],"updated_at_ms":1}),
    );
    store.put(
        state::selection_key(),
        "selection",
        json!({"provider_ids":["surplus"],"updated_at_ms":1}),
    );
    *store.fail_key.borrow_mut() = Some(catalog_store::codex_snapshot_key());
    let response = invoke(&runtime, &store, &host, AdminCatalogOperation::Catalog);
    assert_eq!(
        response.body["data"]["models"]
            .as_array()
            .unwrap()
            .iter()
            .map(|row| row["id"].as_str().unwrap())
            .collect::<Vec<_>>(),
        vec!["a", "A", "ä", "z"]
    );
    assert_eq!(response.body["data"]["filter_active"], true);
    assert_eq!(response.headers["cache-control"], "no-store");
    let input = run(runtime.load_state(&store, &host.publication.borrow(), host.now.get()))
        .unwrap()
        .input;
    assert!(
        ordinary_models(&input)["data"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    assert!(
        public_catalog(&input)["data"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    host.fail_compare.set(true);
    assert_eq!(
        run(runtime.admin_catalog(&store, &host, AdminCatalogOperation::Catalog))
            .unwrap_err()
            .body()
            .code(),
        Some("fixture_collation")
    );
}

#[test]
fn aggregate_refresh_forces_fixed_providers_preserves_last_good_and_marks_stale_claude() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    for provider in [
        CatalogProvider::Openlux,
        CatalogProvider::Surplus,
        CatalogProvider::Lithos,
        CatalogProvider::ClaudeCode,
    ] {
        host.configure(provider, provider.id());
    }
    host.queue(
        CatalogProvider::Openrouter,
        json!({"data":[{"id":"a","context_length":1000}]}),
    );
    host.queue(
        CatalogProvider::Openlux,
        json!({"data":[{"id":"a","supported_endpoint_types":["openai"]}]}),
    );
    host.queue(
        CatalogProvider::Surplus,
        json!({"data":[{"id":"b","supported_endpoint_types":["openai-response"]}]}),
    );
    host.queue(CatalogProvider::Lithos, json!({"data":[{"id":"c"}]}));
    host.queue(CatalogProvider::ClaudeCode,json!({"data":[{"id":"claude-observed","type":"model","display_name":"Observed","created_at":"2026-01-01T00:00:00Z"}],"has_more":false}));
    let first = invoke(&runtime, &store, &host, AdminCatalogOperation::Refresh);
    assert_eq!(host.requests.borrow().len(), 5);
    assert_eq!(first.body["data"]["openrouter"]["refreshed"], true);
    assert_eq!(first.body["data"]["claude_code"]["status"], "available");
    host.now.set(host.now.get() + 1);
    for provider in [
        CatalogProvider::Openrouter,
        CatalogProvider::Openlux,
        CatalogProvider::Surplus,
        CatalogProvider::Lithos,
        CatalogProvider::ClaudeCode,
    ] {
        host.responses
            .borrow_mut()
            .entry(provider)
            .or_default()
            .push_back(Outcome {
                body: Err(GatewayError::unavailable(
                    "offline fixture",
                    "fixture_offline",
                )),
                gate: None,
                hook: None,
            });
    }
    let failed = invoke(&runtime, &store, &host, AdminCatalogOperation::Refresh);
    assert_eq!(host.requests.borrow().len(), 10);
    assert_eq!(failed.body["data"]["openrouter"]["refreshed"], false);
    assert_eq!(failed.body["data"]["lithos"]["refreshed"], false);
    for provider in ["openrouter", "openlux", "surplus", "lithos"] {
        assert_eq!(
            failed.body["data"][provider],
            first.body["data"][provider]
                .clone()
                .as_object()
                .map(|row| {
                    let mut row = row.clone();
                    if row.contains_key("refreshed") {
                        row.insert("refreshed".into(), json!(false));
                    }
                    Value::Object(row)
                })
                .unwrap()
        );
    }
    assert_eq!(failed.body["data"]["claude_code"]["models"], 1);
    assert_eq!(failed.body["data"]["claude_code"]["stale"], true);
    assert_eq!(failed.body["data"]["claude_code"]["status"], "unavailable");
}

#[test]
fn cached_picker_read_uses_existing_catalog_without_discovery_while_refresh_stays_explicit() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    for provider in [
        CatalogProvider::Openlux,
        CatalogProvider::Surplus,
        CatalogProvider::Lithos,
        CatalogProvider::ClaudeCode,
    ] {
        host.configure(provider, provider.id());
    }
    store.put(
        state::selection_key(),
        "codex-only",
        json!({"provider_ids":["codex"],"updated_at_ms":1}),
    );

    let _cold = invoke(
        &runtime,
        &store,
        &host,
        AdminCatalogOperation::CatalogCached,
    );
    assert_eq!(host.warmed.get(), 0);
    assert_eq!(host.requests.borrow().len(), 0);

    host.queue(CatalogProvider::Openrouter, json!({"data":[]}));
    host.queue(
        CatalogProvider::Openlux,
        json!({"data":[{"id":"a","supported_endpoint_types":["openai"]}]}),
    );
    host.queue(CatalogProvider::Surplus, json!({"data":[]}));
    host.queue(CatalogProvider::Lithos, json!({"data":[]}));
    host.queue(
        CatalogProvider::ClaudeCode,
        json!({"data":[],"has_more":false}),
    );

    let refreshed = invoke(&runtime, &store, &host, AdminCatalogOperation::Refresh);
    assert_eq!(host.requests.borrow().len(), 5);
    assert_eq!(host.warmed.get(), 0);
    assert_eq!(refreshed.body["data"]["openrouter"]["refreshed"], true);

    let cached = invoke(
        &runtime,
        &store,
        &host,
        AdminCatalogOperation::CatalogCached,
    );
    assert_eq!(host.requests.borrow().len(), 5);
    assert_eq!(host.warmed.get(), 0);
    assert!(
        cached.body["data"]["models"]
            .as_array()
            .unwrap()
            .iter()
            .any(|row| row["id"] == "a")
    );
}

#[test]
fn replacement_during_pending_refresh_cannot_return_old_identity_snapshot() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    host.configure(CatalogProvider::Openlux, "first");
    let gate = Rc::new(Cell::new(false));
    host.queue(CatalogProvider::Openrouter, json!({"data":[]}));
    host.queue(CatalogProvider::Surplus, json!({"data":[]}));
    host.responses
        .borrow_mut()
        .entry(CatalogProvider::Openlux)
        .or_default()
        .push_back(Outcome {
            body: Ok(json!({"data":[{"id":"old","supported_endpoint_types":["openai"]}]})),
            gate: Some(gate.clone()),
            hook: None,
        });
    let mut pending =
        std::pin::pin!(runtime.admin_catalog(&store, &host, AdminCatalogOperation::Refresh));
    let waker = Waker::noop();
    let mut context = Context::from_waker(waker);
    assert!(pending.as_mut().poll(&mut context).is_pending());
    assert_eq!(host.requests.borrow().len(), 3);
    host.configure(CatalogProvider::Openlux, "replacement");
    gate.set(true);
    let result = scalar_response(run(pending).unwrap());
    assert_eq!(
        result.body["data"]["openlux"],
        json!({"models":0,"updated_at_ms":null})
    );
}

#[test]
fn cold_picker_starts_three_normal_discoveries_concurrently_and_only_schedules_enrichment() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    let gate = Rc::new(Cell::new(false));
    store.put(
        state::selection_key(),
        "disabled",
        json!({"provider_ids":["codex"],"updated_at_ms":1}),
    );
    for (provider, body) in [
        (
            CatalogProvider::Openlux,
            json!({"data":[{"id":"z","supported_endpoint_types":["openai"]}]}),
        ),
        (CatalogProvider::Surplus, json!({"data":[{"id":"a"}]})),
        (
            CatalogProvider::ClaudeCode,
            json!({"data":[{"id":"claude-a","type":"model","created_at":"2026-01-01T00:00:00Z"}],"has_more":false}),
        ),
    ] {
        host.configure(provider, provider.id());
        host.responses
            .borrow_mut()
            .entry(provider)
            .or_default()
            .push_back(Outcome {
                body: Ok(body),
                gate: Some(gate.clone()),
                hook: None,
            });
    }
    let mut pending =
        std::pin::pin!(runtime.admin_catalog(&store, &host, AdminCatalogOperation::Catalog));
    let waker = Waker::noop();
    let mut context = Context::from_waker(waker);
    assert!(pending.as_mut().poll(&mut context).is_pending());
    assert_eq!(host.warmed.get(), 1);
    assert_eq!(host.requests.borrow().len(), 3);
    assert!(host.requests.borrow().iter().all(|request| !matches!(
        request,
        CatalogFetch::Provider {
            provider: CatalogProvider::Openrouter,
            ..
        }
    )));
    gate.set(true);
    let result = scalar_response(run(pending).unwrap());
    let ids = result.body["data"]["models"]
        .as_array()
        .unwrap()
        .iter()
        .map(|row| row["id"].as_str().unwrap())
        .collect::<Vec<_>>();
    assert_eq!(ids, vec!["a", "claude-a", "ubiquity/deepseek-flash", "z"]);
    invoke(&runtime, &store, &host, AdminCatalogOperation::Catalog);
    assert_eq!(host.warmed.get(), 2);
    assert_eq!(host.requests.borrow().len(), 3);
}

#[test]
fn anonymous_surplus_is_publicly_visible_without_enabling_ordinary_or_paid_routing() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    host.queue(
        CatalogProvider::Surplus,
        json!({"data":[{"id":"a"},{"id":"z"}]}),
    );
    let admin = invoke(&runtime, &store, &host, AdminCatalogOperation::Catalog);
    assert_eq!(admin.body["data"]["models"].as_array().unwrap().len(), 2);
    assert!(matches!(
        host.requests.borrow()[0],
        CatalogFetch::AnonymousSurplus
    ));
    let publication = host.publication.borrow();
    assert!(publication.providers.is_empty());
    let public_state =
        run(runtime.load_public_state(&store, &publication, host.now.get())).unwrap();
    assert!(!public_state.input.configured("surplus"));
    assert_eq!(public_state.input.rows("surplus").len(), 2);
    let ordinary = run(runtime.load_state(&store, &publication, host.now.get())).unwrap();
    assert!(ordinary.input.rows("surplus").is_empty());
    assert!(
        ordinary_models(&ordinary.input)["data"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    drop(publication);
    store.put(
        state::whitelist_key(),
        "only-z",
        json!({"model_ids":["z"],"updated_at_ms":1}),
    );
    let public = scalar_response(run(runtime.public_catalog(&store, &host)).unwrap());
    assert_eq!(public.body["object"], "uos.model_catalog");
    assert_eq!(public.body["data"].as_array().unwrap().len(), 1);
    assert_eq!(public.body["data"][0]["id"], "z");
    assert!(public.body.get("ok").is_none());
    assert!(public.body.get("whitelist").is_none());
    store.put(
        state::selection_key(),
        "disabled",
        json!({"provider_ids":["codex"],"updated_at_ms":1}),
    );
    let disabled = scalar_response(run(runtime.public_catalog(&store, &host)).unwrap());
    assert!(disabled.body["data"].as_array().unwrap().is_empty());
    assert_eq!(host.requests.borrow().len(), 1);
}
