use super::{Host, Outcome, Store, TestResult, catalog, loader_fixture, run};
use gateway_catalog_provider::{
    host::{CatalogFetch, CatalogFetchResponse, CatalogProvider, ProviderBinding},
    providers,
};
use gateway_catalog_runtime::CatalogRuntime;
use gateway_routing_runtime_test_support::source_catalog;
use gateway_schema::catalog_store;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::{Value, json};
use std::{
    rc::Rc,
    task::{Context, Waker},
};
#[test]
fn claude_failure_retains_ids_and_withdraws_reasoning_permissions() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.provider(CatalogProvider::ClaudeCode, "claude-owner-1");
    host.queue(Outcome::json(json!({"data":[{"id":"claude-sonnet-5","created_at":"2026-10-01T00:00:00Z","capabilities":{"thinking":{"types":{"disabled":{"supported":true},"adaptive":{"supported":true}}},"effort":{"supported":true,"high":{"supported":true},"arbitrary":{"supported":true}}}}],"has_more":false})));
    let first = run(runtime.refresh_provider(&store, &host, CatalogProvider::ClaudeCode, false))
        .unwrap()
        .unwrap();
    assert_eq!(
        first.models[0].deserialize::<Value>().unwrap()["supports_disabled_thinking"],
        true
    );
    assert_eq!(
        first.models[0].deserialize::<Value>().unwrap()["supported_reasoning_levels"],
        json!(["high", "arbitrary"])
    );
    store.now.set(store.now.get() + 300001);
    host.queue(Outcome {
        response: Ok(CatalogFetchResponse::json(503, "{}")),
        ticks: 0,
        callback: None,
    });
    let stale = run(runtime.refresh_provider(&store, &host, CatalogProvider::ClaudeCode, false))
        .unwrap()
        .unwrap();
    assert!(stale.stale);
    assert_eq!(
        stale.models[0].deserialize::<Value>().unwrap()["id"],
        "claude-sonnet-5"
    );
    assert!(
        stale.models[0]
            .deserialize::<Value>()
            .unwrap()
            .get("supports_disabled_thinking")
            .is_none()
    );
    assert!(
        stale.models[0]
            .deserialize::<Value>()
            .unwrap()
            .get("supported_reasoning_levels")
            .is_none()
    );
    host.provider(CatalogProvider::ClaudeCode, "claude-owner-2");
    let published = host.published.borrow().clone();
    assert!(
        runtime
            .cached_provider(&published, CatalogProvider::ClaudeCode)
            .is_none()
    );
}
#[test]
fn provider_pages_are_bounded_and_repeated_cursors_fail_closed() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.provider(CatalogProvider::ClaudeCode, "claude-owner");
    for _ in 0..2 {
        host.queue(Outcome::json(
            json!({"data":[{"id":"claude-observed"}],"has_more":true,"last_id":"repeat"}),
        ));
    }
    assert!(
        run(runtime.refresh_provider(&store, &host, CatalogProvider::ClaudeCode, false))
            .unwrap()
            .is_none()
    );
    assert_eq!(host.requests.borrow().len(), 2);
}
#[test]
fn anonymous_surplus_is_public_data_without_an_inference_configuration() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.queue(Outcome::json(json!({"data":[{"id":"public-model"}]})));
    let snapshot = run(runtime.refresh_anonymous_surplus(&store, &host, false))
        .unwrap()
        .unwrap();
    assert!(!snapshot.configured);
    assert!(matches!(
        host.requests.borrow()[0],
        CatalogFetch::AnonymousSurplus
    ));
    let publication = host.published.borrow().clone();
    let public = run(runtime.load_public_state(&store, &publication, store.now.get())).unwrap();
    assert_eq!(
        public.input.rows("surplus")[0]
            .deserialize::<Value>()
            .unwrap()["id"],
        "public-model"
    );
    assert!(!public.input.configured("surplus"));
    let ordinary = run(runtime.load_state(&store, &publication, store.now.get())).unwrap();
    assert!(ordinary.input.rows("surplus").is_empty());
    assert!(!ordinary.refresh_due.contains(&CatalogProvider::Surplus));
    assert!(
        run(runtime.refresh_provider(&store, &host, CatalogProvider::Surplus, false))
            .unwrap()
            .is_none()
    );
    assert_eq!(host.requests.borrow().len(), 1);
}
#[test]
fn durable_provider_observations_hydrate_a_fresh_runtime_and_binding() {
    let store = Store::new();
    store.put(catalog_store::auth_generation_key(), "g", "g".into());
    let host = Host::new(&store);
    host.provider(CatalogProvider::Openrouter, "credential-a");
    host.queue(Outcome::json(json!({"data":[{"id":"openrouter-model"}]})));
    let first = CatalogRuntime::new();
    let snapshot = run(first.refresh_provider(&store, &host, CatalogProvider::Openrouter, false))
        .unwrap()
        .unwrap();
    assert_eq!(
        snapshot.models[0].deserialize::<Value>().unwrap()["id"],
        "openrouter-model"
    );
    assert_eq!(host.requests.borrow().len(), 1);

    // A fresh process has an empty memo and a host that would panic if asked
    // for another discovery; durable observations must supply the rows.
    let fresh = CatalogRuntime::new();
    let publication = host.published.borrow().clone();
    let state = run(fresh.load_state(&store, &publication, store.now.get())).unwrap();
    assert_eq!(
        state.input.rows("openrouter")[0]
            .deserialize::<Value>()
            .unwrap()["id"],
        "openrouter-model"
    );
    assert_eq!(host.requests.borrow().len(), 1);

    // A rotated credential binding must not reuse the old observation.
    host.provider(CatalogProvider::Openrouter, "credential-b");
    let rotated = CatalogRuntime::new();
    let publication = host.published.borrow().clone();
    let state = run(rotated.load_state(&store, &publication, store.now.get())).unwrap();
    assert!(state.input.rows("openrouter").is_empty());

    // A new catalog auth generation must not reuse the old observation.
    host.provider(CatalogProvider::Openrouter, "credential-a");
    store.put(catalog_store::auth_generation_key(), "g2", "g2".into());
    let rotated_generation = CatalogRuntime::new();
    let publication = host.published.borrow().clone();
    let state = run(rotated_generation.load_state(&store, &publication, store.now.get())).unwrap();
    assert!(state.input.rows("openrouter").is_empty());
}

#[test]
fn anonymous_surplus_uses_the_same_cache_across_credential_changes() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.queue(Outcome::json(json!({"data":[{"id":"cached-model"}]})));
    run(runtime.refresh_anonymous_surplus(&store, &host, false)).unwrap();
    for credential in ["spend-one", "spend-two"] {
        host.provider(CatalogProvider::Surplus, credential);
        let snapshot =
            run(runtime.refresh_provider(&store, &host, CatalogProvider::Surplus, false))
                .unwrap()
                .unwrap();
        assert_eq!(
            snapshot.models[0].deserialize::<Value>().unwrap()["id"],
            "cached-model"
        );
        let publication = host.published.borrow().clone();
        assert!(
            run(runtime.load_state(&store, &publication, store.now.get()))
                .unwrap()
                .input
                .configured("surplus")
        );
    }
    assert_eq!(host.requests.borrow().len(), 1);
}
#[test]
fn explicit_anonymous_requests_do_not_coalesce_and_newer_results_win() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    let mut old = Outcome::json(json!({"data":[{"id":"old"}]}));
    old.ticks = 1;
    host.queue(old);
    host.queue(Outcome::json(json!({"data":[{"id":"new"}]})));
    let mut first = std::pin::pin!(runtime.refresh_anonymous_surplus(&store, &host, false));
    let mut context = Context::from_waker(Waker::noop());
    assert!(first.as_mut().poll(&mut context).is_pending());
    assert_eq!(
        run(runtime.refresh_anonymous_surplus(&store, &host, false))
            .unwrap()
            .unwrap()
            .models[0]
            .deserialize::<Value>()
            .unwrap()["id"],
        "new"
    );
    assert_eq!(
        run(first).unwrap().unwrap().models[0]
            .deserialize::<Value>()
            .unwrap()["id"],
        "new"
    );
    assert_eq!(host.requests.borrow().len(), 2);
}
#[test]
fn anonymous_surplus_failure_retains_cache_and_honors_retry_and_cancellation() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.queue(Outcome::json(json!({"data":[{"id":"retained"}]})));
    run(runtime.refresh_anonymous_surplus(&store, &host, false)).unwrap();
    store.now.set(store.now.get() + 300_001);
    host.queue(Outcome {
        response: Ok(CatalogFetchResponse::json(503, "")),
        ticks: 0,
        callback: None,
    });
    assert_eq!(
        run(runtime.refresh_anonymous_surplus(&store, &host, false))
            .unwrap()
            .unwrap()
            .models[0]
            .deserialize::<Value>()
            .unwrap()["id"],
        "retained"
    );
    run(runtime.refresh_anonymous_surplus(&store, &host, false)).unwrap();
    assert_eq!(host.requests.borrow().len(), 2);
    host.queue(Outcome {
        response: Err(GatewayError::new(499, "Cancelled", Some("cancelled"))),
        ticks: 0,
        callback: None,
    });
    assert_eq!(
        run(runtime.refresh_anonymous_surplus(&store, &host, true))
            .unwrap_err()
            .status,
        499
    );
}
#[test]
fn validated_claude_metadata_uses_current_source_fences_and_the_existing_memo() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.provider(CatalogProvider::ClaudeCode, "validated-owner");
    let binding = ProviderBinding {
        identity: "validated-owner".into(),
    };
    let rows = vec![
        SourceJson::from_serializable(
            &json!({"id":"claude-validated","supported_reasoning_levels":["high"]}),
        )
        .unwrap(),
    ];
    assert!(
        run(runtime.cache_validated_claude(&store, &host, &binding, rows, store.now.get()))
            .unwrap()
    );
    let cached = run(runtime.refresh_provider(&store, &host, CatalogProvider::ClaudeCode, false))
        .unwrap()
        .unwrap();
    assert_eq!(
        cached.models[0].deserialize::<Value>().unwrap()["id"],
        "claude-validated"
    );
    assert!(host.requests.borrow().is_empty());
    host.provider(CatalogProvider::ClaudeCode, "replacement-owner");
    assert!(
        !run(runtime.cache_validated_claude(&store, &host, &binding, vec![], store.now.get()))
            .unwrap()
    );
    assert!(
        runtime
            .cached_provider(&host.published.borrow(), CatalogProvider::ClaudeCode)
            .is_none()
    );
}
#[test]
fn validation_publication_cannot_be_overwritten_by_an_older_inflight_refresh() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.provider(CatalogProvider::ClaudeCode, "same-owner");
    let mut old = Outcome::json(json!({"data":[{"id":"claude-older"}],"has_more":false}));
    old.ticks = 1;
    host.queue(old);
    let mut pending =
        std::pin::pin!(runtime.refresh_provider(&store, &host, CatalogProvider::ClaudeCode, false));
    let mut context = Context::from_waker(Waker::noop());
    assert!(pending.as_mut().poll(&mut context).is_pending());
    let binding = ProviderBinding {
        identity: "same-owner".into(),
    };
    assert!(
        run(runtime.cache_validated_claude(
            &store,
            &host,
            &binding,
            vec![SourceJson::from_serializable(&json!({"id":"claude-validated"})).unwrap()],
            store.now.get()
        ))
        .unwrap()
    );
    assert_eq!(
        run(pending).unwrap().unwrap().models[0]
            .deserialize::<Value>()
            .unwrap()["id"],
        "claude-validated"
    );
}
#[test]
fn public_openrouter_enrichment_does_not_require_routing_credentials() -> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.queue(Outcome::json(json!({"data":[{"id":"vendor/model","context_length":123456,"top_provider":{"context_length":200000},"reasoning":{"supported_efforts":["custom","none"],"default_effort":"custom"}}]})));
    run(runtime.refresh_provider(&store, &host, CatalogProvider::Openrouter, false))?;
    store.seed(
        "0.163.0",
        store.now.get(),
        &catalog("vendor/model").to_string(),
        None,
    )?;
    let state = run(runtime.load_state(&store, &host.published.borrow(), store.now.get()))?;
    assert!(!state.input.configured("openrouter"));
    assert_eq!(
        state.input.enrichment[&"vendor/model".into()].max_context_window_tokens,
        Some(200000)
    );
    Ok(())
}
#[test]
fn openrouter_empty_failure_retains_cache_and_ambiguous_matches_are_unknown() {
    let rows = vec![
        source_catalog::json(&json!({"id":"a/model","context_window_tokens":1})),
        SourceJson::from_serializable(&json!({"id":"b/model","context_window_tokens":2})).unwrap(),
    ];
    assert!(providers::match_openrouter(&rows, &"model".into()).is_none());
    assert_eq!(
        providers::match_openrouter(&rows, &"a/model".into())
            .unwrap()
            .deserialize::<Value>()
            .unwrap()["context_window_tokens"],
        1
    );
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.queue(Outcome::json(json!({"data":[{"id":"a/model"}]})));
    run(runtime.refresh_provider(&store, &host, CatalogProvider::Openrouter, false)).unwrap();
    store.now.set(store.now.get() + 300001);
    host.queue(Outcome::json(json!({"data":[]})));
    let retained = run(runtime.refresh_provider(&store, &host, CatalogProvider::Openrouter, false))
        .unwrap()
        .unwrap();
    assert_eq!(
        retained.models[0].deserialize::<Value>().unwrap()["id"],
        "a/model"
    );
    run(runtime.refresh_provider(&store, &host, CatalogProvider::Openrouter, false)).unwrap();
    assert_eq!(host.requests.borrow().len(), 2);
}
#[test]
fn overlapping_forced_provider_refreshes_keep_the_newer_publication() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.provider(CatalogProvider::Openlux, "openlux-owner");
    let mut slow = Outcome::json(json!({"data":[{"id":"older"}]}));
    slow.ticks = 1;
    host.queue(slow);
    host.queue(Outcome::json(json!({"data":[{"id":"newer"}]})));
    let mut first =
        std::pin::pin!(runtime.refresh_provider(&store, &host, CatalogProvider::Openlux, true));
    let waker = Waker::noop();
    let mut cx = Context::from_waker(waker);
    assert!(first.as_mut().poll(&mut cx).is_pending());
    let second = run(runtime.refresh_provider(&store, &host, CatalogProvider::Openlux, true))
        .unwrap()
        .unwrap();
    let first = run(first).unwrap().unwrap();
    assert_eq!(
        second.models[0].deserialize::<Value>().unwrap()["id"],
        "newer"
    );
    assert_eq!(
        first.models[0].deserialize::<Value>().unwrap()["id"],
        "newer"
    );
}
#[test]
fn claude_pages_share_the_original_deadline_and_codex_has_no_invented_timeout() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.provider(CatalogProvider::ClaudeCode, "claude-owner");
    let start = store.now.get();
    let shared = store.clone();
    host.queue(Outcome {
        response: Ok(CatalogFetchResponse::json(
            200,
            json!({"data":[{"id":"claude-one"}],"has_more":true,"last_id":"one"}).to_string(),
        )),
        ticks: 0,
        callback: Some(Rc::new(move || shared.now.set(shared.now.get() + 7000))),
    });
    host.queue(Outcome::json(
        json!({"data":[{"id":"claude-two"}],"has_more":false}),
    ));
    let snapshot = run(runtime.refresh_provider(&store, &host, CatalogProvider::ClaudeCode, false))
        .unwrap()
        .unwrap();
    assert_eq!(snapshot.models.len(), 2);
    let requests = host.requests.borrow();
    for request in requests.iter() {
        assert!(
            matches!(request, CatalogFetch::Provider { deadline_ms: Some(deadline), .. } if *deadline == start + 8000)
        );
    }
    assert_eq!(
        requests[1].remaining_timeout_ms(store.now.get()).unwrap(),
        Some(1000)
    );
    assert!(requests[1].remaining_timeout_ms(start + 8000).is_err());
    let codex = CatalogFetch::Codex {
        account: host.published.borrow().codex_accounts[0].clone(),
        client_version: "0.163.0".into(),
        if_none_match: None,
    };
    assert_eq!(codex.remaining_timeout_ms(start).unwrap(), None);
    assert_eq!(codex.max_body_bytes(), None);
}
// Source ba5ef05e admin-models-catalog.test.ts: paid discovery is encountered
// before the official declaration; the latter describes this gateway route.
#[test]
fn later_official_deepseek_public_metadata_overwrites_paid_enrichment_only_for_its_route()
-> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    host.provider(CatalogProvider::Surplus, "configured-surplus");
    host.provider(CatalogProvider::Deepseek, "configured-deepseek");
    let runtime = CatalogRuntime::new();
    host.queue(Outcome::json(json!({"data":[
        {"id":"deepseek-v4-flash","created":1_800_000_000i64,"provider":"surplus"},
        {"id":"unrelated-discovery","provider":"surplus"}
    ]})));
    run(runtime.refresh_provider(&store, &host, CatalogProvider::Surplus, true))?;
    host.queue(Outcome::json(json!({"data":[
        {"id":"deepseek-v4-flash","context_length":1_048_576,"top_provider":{"context_length":1_048_576},"reasoning":{"supported_efforts":["third-party"],"default_effort":"third-party"}},
        {"id":"unrelated-discovery","context_length":123_456,"reasoning":{"supported_efforts":["retained-custom"],"default_effort":"retained-custom"}}
    ]})));
    run(runtime.refresh_provider(&store, &host, CatalogProvider::Openrouter, true))?;
    let state = run(runtime.load_state(&store, &host.published.borrow(), store.now.get()))?;
    let catalog =
        gateway_schema::catalog_views::build_catalog_snapshot(&state.input, &loader_fixture())?
            .deserialize::<Value>()?;
    let rows = catalog["models"].as_array().ok_or("missing rows")?;
    let official = rows
        .iter()
        .find(|row| row["id"] == "deepseek-v4-flash")
        .ok_or("missing model")?;
    assert_eq!(
        official["providers"]
            .as_array()
            .ok_or("missing providers")?
            .iter()
            .map(|provider| provider["id"].as_str())
            .collect::<Vec<_>>(),
        [Some("surplus"), Some("deepseek")]
    );
    assert_eq!(official["created"], 1_800_000_000i64);
    assert_eq!(official["context_window_tokens"], 1_000_000);
    assert_eq!(official["max_context_window_tokens"], 1_000_000);
    assert_eq!(
        official["supported_reasoning_levels"],
        json!(["none", "low", "high", "max"])
    );
    assert_eq!(official["default_reasoning_effort"], "high");
    assert_eq!(official["context_source"], "provider_discovery");
    assert_eq!(official["reasoning_source"], "provider_discovery");
    let unrelated = rows
        .iter()
        .find(|row| row["id"] == "unrelated-discovery")
        .ok_or("missing model")?;
    assert_eq!(unrelated["context_window_tokens"], 123_456);
    assert_eq!(
        unrelated["supported_reasoning_levels"],
        json!(["retained-custom"])
    );
    assert_eq!(unrelated["context_source"], "openrouter");
    Ok(())
}
