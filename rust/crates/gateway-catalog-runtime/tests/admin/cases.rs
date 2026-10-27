use super::{
    Host, Outcome, Store, build_runtime_config, invoke, lift, normalize_snapshot, ordinary_models,
    run, scalar,
};
use gateway_catalog_admin::admin::AdminCatalogOperation;
use gateway_catalog_admin::admin::CatalogAdmin;
use gateway_catalog_inputs::state;
use gateway_catalog_provider::host::CodexAccount;
use gateway_catalog_runtime::CatalogRuntime;
use gateway_catalog_versioned::VersionedCatalog;
use gateway_core::source_json::SourceJson;
use gateway_schema::{catalog_store, snapshot::SnapshotOverrides};
use serde_json::{Value, json};
use std::{
    cell::Cell,
    rc::Rc,
    task::{Context, Waker},
};
#[test]
fn source_upload_and_whitelist_vectors_match_handler_responses_and_state() {
    let golden: Value =
        serde_json::from_str(include_str!("../fixtures/admin-catalog-golden.json")).unwrap();
    for case in golden["operations"].as_array().unwrap() {
        let runtime = CatalogRuntime::new();
        let store = Store::default();
        let host = Host::new();
        let operation = if case["operation"] == "models" {
            AdminCatalogOperation::CodexModelsSet {
                document: lift(&case["input"].clone()),
            }
        } else {
            AdminCatalogOperation::WhitelistSet {
                document: lift(&case["input"].clone()),
            }
        };
        let actual = match run(runtime.admin_catalog(&store, &host, operation)) {
            Ok(response) => json!({"status":response.status,"body":response.body}),
            Err(error) => json!({"status":error.status,"body":scalar(error.body())}),
        };
        assert_eq!(actual, case["expected"], "{}", case["name"]);
        if actual["status"].as_u64().unwrap() >= 400 {
            assert!(store.rows.borrow().is_empty(), "{}", case["name"]);
        }
        if let Some(snapshot) = case.get("snapshot") {
            assert_eq!(
                store.value(&catalog_store::codex_snapshot_key()),
                *snapshot,
                "{}",
                case["name"]
            );
        }
        if let Some(expected) = case.get("runtime") {
            assert_eq!(
                store.value(&catalog_store::runtime_config_key()),
                *expected,
                "{}",
                case["name"]
            );
        }
    }
}

#[test]
fn two_key_save_retries_real_cas_and_retains_concurrent_runtime_default_without_generation_rotation()
 {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    host.seed(&store, "a", "1.0.0");
    let race_store = store.clone();
    let now = host.now.get();
    *store.before_commit.borrow_mut() = Some(Rc::new(move || {
        let snapshot = normalize_snapshot(
            &json!({"models":[{"slug":"b"},{"slug":"a"}]}),
            &SnapshotOverrides::default(),
            now,
        )
        .unwrap();
        race_store.put(
            catalog_store::codex_snapshot_key(),
            "refresh",
            json!(snapshot),
        );
        race_store.put(
            catalog_store::runtime_config_key(),
            "refresh-runtime",
            build_runtime_config(&snapshot, Some("b"), Some("ultra"), now).unwrap(),
        );
    }));
    let result = invoke(
        &runtime,
        &store,
        &host,
        AdminCatalogOperation::CodexModelsSet {
            document: lift(&json!({"models":[{"slug":"a"},{"slug":"b"}],"client_version":"2.0.0"})),
        },
    );
    assert_eq!(result.body["count"], 2);
    assert_eq!(store.attempts.get(), 2);
    let config = store.value(&catalog_store::runtime_config_key());
    assert_eq!(config["default_model"], "b");
    assert_eq!(config["default_reasoning_effort"], "ultra");
    assert_eq!(
        store.value(&catalog_store::auth_generation_key()),
        "generation"
    );
    let rows = store.rows.borrow();
    assert_eq!(
        rows[&catalog_store::codex_snapshot_key().0].revision,
        rows[&catalog_store::runtime_config_key().0].revision
    );
}

#[test]
fn exhausted_conflicts_leave_both_original_rows_and_report_source_failure() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    host.seed(&store, "a", "1.0.0");
    let original = store.value(&catalog_store::codex_snapshot_key());
    let config = store.value(&catalog_store::runtime_config_key());
    store.forced_conflicts.set(3);
    let error = run(runtime.admin_catalog(
        &store,
        &host,
        AdminCatalogOperation::CodexModelsSet {
            document: lift(&json!({"models":[{"slug":"b"}]})),
        },
    ))
    .unwrap_err();
    assert_eq!(error.status, 500);
    assert_eq!(store.attempts.get(), 3);
    assert_eq!(store.value(&catalog_store::codex_snapshot_key()), original);
    assert_eq!(store.value(&catalog_store::runtime_config_key()), config);
}

#[test]
fn replacement_upload_preserves_verified_negative_prompt_cache_evidence() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    invoke(
        &runtime,
        &store,
        &host,
        AdminCatalogOperation::CodexModelsSet {
            document: lift(&json!({"models":[{"slug":"a","prompt_cache":false}]})),
        },
    );
    invoke(
        &runtime,
        &store,
        &host,
        AdminCatalogOperation::CodexModelsSet {
            document: lift(&json!({"models":[{"slug":"a"}]})),
        },
    );
    assert_eq!(
        store.value(&catalog_store::codex_snapshot_key())["models"][0]["prompt_cache"],
        false
    );
    assert_eq!(
        store.value(&catalog_store::runtime_config_key())["codex_models"]["models"][0]["prompt_cache"],
        false
    );
}

#[test]
fn whitelist_source_write_is_unconditional_retains_unknown_ids_and_empty_list_clears_filter() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    let raced = store.clone();
    *store.fail_key.borrow_mut() = Some(state::whitelist_key());
    *store.before_commit.borrow_mut() = Some(Rc::new(move || {
        raced.put(
            state::whitelist_key(),
            "other",
            json!({"model_ids":["other"],"updated_at_ms":1}),
        )
    }));
    let result = invoke(
        &runtime,
        &store,
        &host,
        AdminCatalogOperation::WhitelistSet {
            document: lift(&json!({"model_ids":[" missing ","missing",""]})),
        },
    );
    assert_eq!(result.body["model_ids"], json!(["missing"]));
    assert_eq!(store.attempts.get(), 1);
    *store.fail_key.borrow_mut() = None;
    assert_eq!(
        invoke(&runtime, &store, &host, AdminCatalogOperation::WhitelistGet).body["data"]["model_ids"],
        json!(["missing"])
    );
    invoke(
        &runtime,
        &store,
        &host,
        AdminCatalogOperation::WhitelistSet {
            document: lift(&json!({"model_ids":[]})),
        },
    );
    assert_eq!(
        invoke(&runtime, &store, &host, AdminCatalogOperation::WhitelistGet).body["data"]["model_ids"],
        json!([])
    );
}
#[test]
fn save_while_versioned_discovery_waits_cannot_be_replaced_by_older_version() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    host.seed(&store, "a", "1.0.0");
    host.publication
        .borrow_mut()
        .codex_accounts
        .push(CodexAccount {
            account_id: "fixture-account".into(),
            credential_fingerprint: "fixture-fingerprint".into(),
        });
    let gate = Rc::new(Cell::new(false));
    host.codex.borrow_mut().push_back(Outcome{body:Ok(json!({"models":[{"slug":"old-upstream","supported_reasoning_levels":[{"effort":"none"}]}]})),gate:Some(gate.clone()),hook:None});
    let mut refresh = std::pin::pin!(runtime.versioned_models(&store, &host, "1.1.0", None));
    let waker = Waker::noop();
    let mut context = Context::from_waker(waker);
    assert!(refresh.as_mut().poll(&mut context).is_pending());
    invoke(
        &runtime,
        &store,
        &host,
        AdminCatalogOperation::CodexModelsSet {
            document: lift(
                &json!({"models":[{"slug":"new-upload","supported_reasoning_levels":[{"effort":"none"},{"effort":"ultra"},{"effort":"future-tier"}]}],"client_version":"2.0.0"}),
            ),
        },
    );
    gate.set(true);
    let versioned = run(refresh).unwrap();
    let body: Value = serde_json::from_str(versioned.body.as_ref().unwrap()).unwrap();
    assert_eq!(body["models"][0]["slug"], "old-upstream");
    assert_eq!(
        store.value(&catalog_store::codex_snapshot_key())["models"][0]["slug"],
        "new-upload"
    );
    let input = run(runtime.load_state(&store, &host.publication.borrow(), host.now.get()))
        .unwrap()
        .input;
    assert_eq!(ordinary_models(&input)["data"][0]["id"], "new-upload");
    assert_eq!(
        store.value(&catalog_store::auth_generation_key()),
        "generation"
    );
}

#[test]
fn actual_admin_source_documents_publish_opaque_models_source_labels_and_whitelist_units() {
    use gateway_core::{
        source_control::decode_source_control_record, source_serialization as source,
        value::ValueEncoding,
    };
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    let document=SourceJson::parse(r#"{"models":[{"slug":"\ud800","supported_reasoning_levels":["\udc00"],"default_reasoning_level":"\udc00"}],"source":"label\ud800"}"#).unwrap();
    let saved = run(runtime.admin_catalog(
        &store,
        &host,
        AdminCatalogOperation::CodexModelsSet {
            document: document.clone(),
        },
    ))
    .unwrap();
    assert_eq!(saved.status, 200);
    assert_eq!(
        source::member(&saved.body, "source"),
        Some(SourceJson::parse(r#""label\ud800""#).unwrap())
    );
    let snapshot = store
        .rows
        .borrow()
        .get(&catalog_store::codex_snapshot_key().0)
        .unwrap()
        .clone();
    let config = store
        .rows
        .borrow()
        .get(&catalog_store::runtime_config_key().0)
        .unwrap()
        .clone();
    assert_eq!(snapshot.value_encoding, ValueEncoding::ProfileV2);
    assert_eq!(config.value_encoding, ValueEncoding::ProfileV2);
    assert_eq!(snapshot.revision, config.revision);
    assert_eq!(snapshot.expires_at_ms, None);
    assert_eq!(config.expires_at_ms, None);
    let fetched =
        run(runtime.admin_catalog(&store, &host, AdminCatalogOperation::CodexModelsGet)).unwrap();
    assert_eq!(
        source::member(&fetched.body, "data"),
        Some(decode_source_control_record(&snapshot).unwrap())
    );
    let whitelist = SourceJson::parse(r#"{"model_ids":["\ud800","\udc00","\\ud800"]}"#).unwrap();
    let saved = run(runtime.admin_catalog(
        &store,
        &host,
        AdminCatalogOperation::WhitelistSet {
            document: whitelist.clone(),
        },
    ))
    .unwrap();
    assert_eq!(
        source::member(&saved.body, "model_ids"),
        source::member(&whitelist, "model_ids")
    );
    let fetched =
        run(runtime.admin_catalog(&store, &host, AdminCatalogOperation::WhitelistGet)).unwrap();
    let data = source::member(&fetched.body, "data").unwrap();
    assert_eq!(
        source::member(&data, "model_ids"),
        source::member(&whitelist, "model_ids")
    );
    assert_eq!(
        store
            .rows
            .borrow()
            .get(&state::whitelist_key().0)
            .unwrap()
            .value_encoding,
        ValueEncoding::ProfileV2
    );
    assert!(host.requests.borrow().is_empty());
    println!(
        "M42_ADMIN_SOURCE_WRITER_FIXTURE={}",
        serde_json::to_string(&(catalog_store::codex_snapshot_key().0, snapshot)).unwrap()
    );
}

#[test]
fn blank_source_label_is_rejected_before_admin_publication_mutates_rows() {
    let runtime = CatalogRuntime::new();
    let store = Store::default();
    let host = Host::new();
    let document = SourceJson::parse(r#"{"models":[{"slug":"a"}],"source":""}"#).unwrap();
    let error = run(runtime.admin_catalog(
        &store,
        &host,
        AdminCatalogOperation::CodexModelsSet { document },
    ))
    .unwrap_err();
    assert_eq!(error.status, 409);
    assert!(store.rows.borrow().is_empty());
    assert!(host.requests.borrow().is_empty());
}
