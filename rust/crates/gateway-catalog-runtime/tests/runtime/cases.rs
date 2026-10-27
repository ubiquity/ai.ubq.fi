use super::{Host, Outcome, Store, TestResult, body, catalog, ids, run};
use gateway_catalog_inputs::state;
use gateway_catalog_provider::host::{
    CatalogFetch, CatalogFetchResponse, CatalogMaintenance, CatalogObservation, CatalogProvider,
    CodexAccount,
};
use gateway_catalog_runtime::CatalogRuntime;
use gateway_catalog_versioned::VersionedCatalog;
use gateway_core::error::GatewayError;
use gateway_schema::{catalog_cache, catalog_store};
use serde_json::json;
use std::rc::Rc;
#[test]
fn exact_versions_do_not_share_catalog_bodies_or_validators() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.queue(Outcome::json(catalog("old")));
    host.queue(Outcome::json(catalog("new")));
    let old = run(runtime.versioned_models(&store, &host, "0.162.1", None)).unwrap();
    let new = run(runtime.versioned_models(&store, &host, "0.163.0", None)).unwrap();
    assert_eq!(ids(&old), ["old"]);
    assert_eq!(ids(&new), ["new"]);
    assert_ne!(old.headers["ETag"], new.headers["ETag"]);
    let conditional =
        run(runtime.versioned_models(&store, &host, "0.162.1", Some(&old.headers["ETag"])))
            .unwrap();
    assert_eq!(conditional.status, 304);
    assert!(conditional.body.is_none());
    assert_eq!(host.requests.borrow().len(), 2);
    assert_eq!(
        body(&old)["models"][0]["supported_reasoning_levels"][2]["effort"],
        "custom-tier"
    );
    let runtime_record = store.live(&catalog_store::runtime_config_key()).unwrap();
    assert_eq!(
        runtime_record.value["codex_models"]["client_version"],
        "0.163.0"
    );
}
#[test]
fn invalid_or_duplicate_version_never_dispatches() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    for version in ["", "0.163", "v0.163.0", "0.163.0&client_version=0.162.0"] {
        assert_eq!(
            run(runtime.versioned_models(&store, &host, version, None))
                .unwrap_err()
                .status,
            400
        );
    }
    assert!(host.requests.borrow().is_empty());
}
#[test]
fn single_account_304_revalidates_source_and_hashes_final_client_bytes() -> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    let old_time = store.now.get() - catalog_cache::FRESH_MS - 1;
    let source = catalog("one").to_string();
    store.seed("0.163.0", old_time, &source, Some("\"upstream\""))?;
    host.queue(Outcome::response(304, ""));
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", Some("\"upstream\"")))?;
    assert_eq!(response.status, 200);
    assert_eq!(response.headers["x-uos-cache"], "revalidated");
    assert_ne!(response.headers["ETag"], "\"upstream\"");
    assert!(
        matches!(&host.requests.borrow()[0],CatalogFetch::Codex{if_none_match:Some(etag),..} if etag=="\"upstream\"")
    );
    let loaded = run(catalog_store::load_catalog(
        &store,
        "0.163.0",
        "g",
        store.now.get(),
    ))?
    .ok_or("expected seeded catalog")?;
    assert!(loaded.metadata.fresh(store.now.get()));
    assert_eq!(loaded.source_body, Some(source));
    Ok(())
}
#[test]
fn pool_union_keeps_first_metadata_and_never_sends_one_accounts_etag() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.published
        .borrow_mut()
        .codex_accounts
        .push(CodexAccount {
            account_id: "account-b".into(),
            credential_fingerprint: "credential-b".into(),
        });
    host.queue(Outcome::json(json!({"top":"first","models":[{"slug":"shared","display_name":"first","supported_reasoning_levels":[{"effort":"none"}]},{"slug":"a"}]})));
    host.queue(Outcome::json(
        json!({"top":"second","models":[{"slug":"shared","display_name":"second"},{"slug":"b"}]}),
    ));
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None)).unwrap();
    assert_eq!(ids(&response), ["shared", "a", "b"]);
    assert_eq!(body(&response)["top"], "first");
    assert_eq!(body(&response)["models"][0]["display_name"], "first");
    assert!(host.requests.borrow().iter().all(|r| matches!(
        r,
        CatalogFetch::Codex {
            if_none_match: None,
            ..
        }
    )));
    let evidence = store.live(&state::account_models_key()).unwrap().value;
    assert_eq!(
        evidence["accounts"]["account-a"]["slugs"],
        json!(["shared", "a"])
    );
    assert_eq!(
        evidence["accounts"]["account-b"]["slugs"],
        json!(["shared", "b"])
    );
}
#[test]
fn union_survives_one_account_transport_failure_without_quota_writes() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.published
        .borrow_mut()
        .codex_accounts
        .push(CodexAccount {
            account_id: "account-b".into(),
            credential_fingerprint: "credential-b".into(),
        });
    host.queue(Outcome {
        response: Err(GatewayError::unavailable(
            "transport failed",
            "codex_upstream_unreachable",
        )),
        ticks: 0,
        callback: None,
    });
    host.queue(Outcome::json(catalog("b")));
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None)).unwrap();
    assert_eq!(ids(&response), ["b"]);
    assert!(host.observations.borrow().iter().any(|event|matches!(event,CatalogObservation::CodexTransportFailure{account,..} if account.account_id=="account-a")));
    assert_eq!(
        host.observations
            .borrow()
            .iter()
            .filter(|event| matches!(event, CatalogObservation::ProviderDegradation { .. }))
            .count(),
        1
    );
}
#[test]
fn failed_refresh_serves_retained_stale_body_and_releases_lease() -> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    store.seed(
        "0.163.0",
        store.now.get() - catalog_cache::FRESH_MS - 1,
        &catalog("stale").to_string(),
        None,
    )?;
    host.queue(Outcome::response(503, "{}"));
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None))?;
    assert_eq!(ids(&response), ["stale"]);
    assert_eq!(response.headers["x-uos-cache"], "stale");
    assert!(store.live(&catalog_store::lease_key("0.163.0")).is_none());
    Ok(())
}
#[test]
fn cold_wait_observes_existing_refresh_without_duplicate_fetch() -> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    store.put(catalog_store::auth_generation_key(), "g", json!("g"));
    assert!(run(catalog_store::acquire_refresh_lease(
        &store,
        "0.163.0",
        "other",
        store.now.get()
    ))?);
    let shared = store.clone();
    host.on_wait.replace(Some(Rc::new(move || {
        let seeded = shared.seed(
            "0.163.0",
            shared.now.get(),
            &catalog("shared").to_string(),
            None,
        );
        assert!(seeded.is_ok());
    })));
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None))?;
    assert_eq!(response.headers["x-uos-cache"], "wait");
    assert_eq!(ids(&response), ["shared"]);
    assert!(host.requests.borrow().is_empty());
    Ok(())
}
#[test]
fn cold_failure_uses_configured_static_fallback_without_inventing_codex_rows() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.published.borrow_mut().codex_accounts.clear();
    host.provider(CatalogProvider::Deepseek, "source-deepseek");
    store.put(
        state::selection_key(),
        "selection2",
        json!({"provider_ids":["deepseek"],"updated_at_ms":store.now.get()}),
    );
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None)).unwrap();
    assert!(ids(&response).contains(&"deepseek-flash".into()));
    assert_eq!(response.headers["x-uos-upstream"], "metered");
    assert!(host.requests.borrow().is_empty());
}
#[test]
fn expired_catalog_from_another_version_is_not_fallback() -> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    store.seed(
        "0.162.0",
        store.now.get(),
        &catalog("other-version").to_string(),
        None,
    )?;
    host.queue(Outcome::response(500, "{}"));
    assert!(run(runtime.versioned_models(&store, &host, "0.163.0", None)).is_err());
    Ok(())
}
#[test]
fn client_validator_changes_when_whitelist_changes_final_bytes() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.queue(Outcome::json(
        json!({"models":[{"slug":"one"},{"slug":"two"}]}),
    ));
    let initial = run(runtime.versioned_models(&store, &host, "0.163.0", None)).unwrap();
    store.put(
        state::whitelist_key(),
        "whitelist",
        json!({"model_ids":["one"],"updated_at_ms":store.now.get()}),
    );
    let narrowed =
        run(runtime.versioned_models(&store, &host, "0.163.0", Some(&initial.headers["ETag"])))
            .unwrap();
    assert_eq!(narrowed.status, 200);
    assert_eq!(ids(&narrowed), ["one"]);
    assert_ne!(narrowed.headers["ETag"], initial.headers["ETag"]);
    assert_eq!(host.requests.borrow().len(), 1);
}

#[test]
fn runtime_catalog_body_is_not_capped_by_the_single_account_observation_limit() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.queue(Outcome::json(
        json!({"models":[{"slug":"large","opaque_source_metadata":"x".repeat(4 * 1024 * 1024)}]}),
    ));
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None)).unwrap();
    assert_eq!(ids(&response), ["large"]);
    assert!(response.body.as_ref().unwrap().len() > 4 * 1024 * 1024);
    assert!(store.live(&state::account_models_key()).is_none());
}

#[test]
fn complete_empty_account_observation_survives_invalid_rich_catalog() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    let mut observed = CatalogFetchResponse::json(200, "{\"models\":[]}");
    observed.single_account_observation_complete = true;
    host.queue(Outcome {
        response: Ok(observed),
        ticks: 0,
        callback: None,
    });
    assert!(run(runtime.versioned_models(&store, &host, "0.163.0", None)).is_err());
    assert_eq!(
        store.live(&state::account_models_key()).unwrap().value["accounts"]["account-a"]["slugs"],
        json!([])
    );
}
#[test]
fn wrong_version_metadata_never_serves_another_versions_rows() -> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    store.seed(
        "0.162.0",
        store.now.get(),
        &catalog("other").to_string(),
        None,
    )?;
    let other = store
        .live(&catalog_store::metadata_key("0.162.0"))
        .ok_or("expected seeded metadata")?;
    store.put(
        catalog_store::metadata_key("0.163.0"),
        "corrupt",
        other.value,
    );
    assert!(run(runtime.versioned_models(&store, &host, "0.163.0", None)).is_err());
    assert!(host.requests.borrow().is_empty());
    Ok(())
}
#[test]
fn version_pruning_failure_is_observed_without_discarding_the_catalog_answer() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    store.fail_query.set(true);
    host.queue(Outcome::json(catalog("served")));
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None)).unwrap();
    assert_eq!(ids(&response), ["served"]);
    assert!(host.observations.borrow().iter().any(|event| matches!(
        event,
        CatalogObservation::MaintenanceFailure {
            operation: CatalogMaintenance::VersionPruning,
            ..
        }
    )));
}
#[test]
fn optional_health_persistence_failure_does_not_fail_a_valid_catalog() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.fail_observation.set(true);
    let mut response = CatalogFetchResponse::json(200, catalog("served").to_string());
    response.provider_request_id = Some(" request-correlation ".into());
    response.access_token_expired = Some(false);
    response.observed_at_ms = Some(store.now.get());
    host.queue(Outcome {
        response: Ok(response),
        ticks: 0,
        callback: None,
    });
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None)).unwrap();
    assert_eq!(ids(&response), ["served"]);
    assert!(host.observations.borrow().iter().any(|event|matches!(event,CatalogObservation::CodexResponse{provider_request_id:Some(id),access_token_expired:Some(false),observed_at_ms:Some(_),..} if id=="request-correlation")));
}
// Source ba5ef05e: catalog/models.ts withDeepSeekOfficialModels replaces a matching
// trimmed slug (or id when the slug is blank), without moving the original slot.
#[test]
fn exact_version_response_replaces_stale_same_id_official_deepseek_rows() -> TestResult {
    let store = Store::new();
    let host = Host::new(&store);
    host.provider(CatalogProvider::Deepseek, "configured-deepseek");
    store.put(
        state::selection_key(),
        "all",
        json!({"provider_ids":[],"updated_at_ms":store.now.get()}),
    );
    store.seed("0.163.0", store.now.get(), &json!({"models":[
        {"slug":"unrelated-before","future":"retained"},
        {"slug":"  deepseek-v4-flash  ","owned_by":"stale-discovery","supported_reasoning_levels":["third-party"],"default_reasoning_level":"third-party"},
        {"id":" \u{feff}deepseek-flash ","owned_by":"stale-discovery","future":"replaced"}
    ],"future_envelope":"retained"}).to_string(), None)?;
    let runtime = CatalogRuntime::new();
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None))?;
    let result = body(&response);
    assert_eq!(result["future_envelope"], "retained");
    assert_eq!(result["models"][0]["future"], "retained");
    for (index, id) in [(1, "deepseek-v4-flash"), (2, "deepseek-flash")] {
        let row = &result["models"][index];
        assert_eq!(row["slug"], id);
        assert_eq!(row["owned_by"], "deepseek");
        let efforts = row["supported_reasoning_levels"]
            .as_array()
            .ok_or("missing levels")?
            .iter()
            .map(|level| level["effort"].as_str())
            .collect::<Vec<_>>();
        assert_eq!(
            efforts,
            [Some("none"), Some("low"), Some("high"), Some("max")]
        );
        assert_eq!(row["default_reasoning_level"], "high");
        assert!(row.get("future").is_none());
    }
    assert!(host.requests.borrow().is_empty());
    Ok(())
}
