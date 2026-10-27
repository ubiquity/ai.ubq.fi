use super::FailIds;
use super::{Host, Outcome, SelectionReads, Store, catalog, ids, run};
use gateway_catalog_inputs::state::{self, source_key};
use gateway_catalog_provider::host::CatalogProvider;
use gateway_catalog_provider::host::ProviderBinding;
use gateway_catalog_provider::host::{
    CatalogFetch, CatalogFetchResponse, CodexAccount, CredentialPublication,
};
use gateway_catalog_runtime::CatalogRuntime;
use gateway_catalog_versioned::VersionedCatalog;
use gateway_core::source_json::SourceJson;
use gateway_schema::catalog_store;
use serde_json::json;
use std::{
    cell::Cell,
    rc::Rc,
    task::{Context, Waker},
};
#[test]
fn lost_lease_cannot_publish_or_delete_successors_lease() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    let shared = store.clone();
    host.queue(Outcome {
        response: Ok(CatalogFetchResponse::json(
            200,
            catalog("loser").to_string(),
        )),
        ticks: 0,
        callback: Some(Rc::new(move || {
            shared.put(
                catalog_store::lease_key("0.163.0"),
                "successor",
                json!({"owner":"successor","lease_until_ms":shared.now.get()+15000}),
            )
        })),
    });
    assert_eq!(
        run(runtime.versioned_models(&store, &host, "0.163.0", None))
            .unwrap_err()
            .envelope
            .error
            .code
            .as_deref(),
        Some("codex_catalog_unavailable")
    );
    assert!(
        store
            .live(&catalog_store::metadata_key("0.163.0"))
            .is_none()
    );
    assert_eq!(
        store
            .live(&catalog_store::lease_key("0.163.0"))
            .unwrap()
            .value["owner"],
        "successor"
    );
}
#[test]
fn auth_generation_change_returns_current_catalog_and_never_publishes_old_fetch() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    let shared = store.clone();
    host.queue(Outcome {
        response: Ok(CatalogFetchResponse::json(200, catalog("old").to_string())),
        ticks: 0,
        callback: Some(Rc::new(move || {
            shared.put(catalog_store::auth_generation_key(), "new", json!("new"))
        })),
    });
    assert!(run(runtime.versioned_models(&store, &host, "0.163.0", None)).is_err());
    assert!(
        store
            .live(&catalog_store::metadata_key("0.163.0"))
            .is_none()
    );
}
#[test]
fn credential_revision_change_between_chunks_and_metadata_cannot_publish() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    let shared = store.clone();
    store.after_chunks.replace(Some(Rc::new(move || {
        shared.put(
            source_key(&["ubq_ai", "codex_auth"]),
            "rotated",
            json!({"test":"rotated"}),
        )
    })));
    host.queue(Outcome::json(catalog("old")));
    assert!(run(runtime.versioned_models(&store, &host, "0.163.0", None)).is_err());
    assert!(
        store
            .live(&catalog_store::metadata_key("0.163.0"))
            .is_none()
    );
    assert_eq!(store.rows.borrow().len(), 3); // source pool, selection and auth generation; all private chunks were removed
}
#[test]
fn in_flight_heartbeat_renews_only_the_request_lease() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.queue(Outcome {
        response: Ok(CatalogFetchResponse::json(200, catalog("slow").to_string())),
        ticks: 4,
        callback: None,
    });
    let before = store.now.get();
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None)).unwrap();
    assert_eq!(ids(&response), ["slow"]);
    assert_eq!(store.now.get() - before, 20000);
    assert!(store.live(&catalog_store::lease_key("0.163.0")).is_none());
}
#[test]
fn owned_401_refresh_requires_durable_exact_publication() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    host.queue(Outcome {
        response: Ok(CatalogFetchResponse::json(401, "{}")),
        ticks: 0,
        callback: None,
    });
    host.queue(Outcome::json(catalog("renewed")));
    host.renewal.replace(Some(CodexAccount {
        account_id: "account-a".into(),
        credential_fingerprint: "new-credential".into(),
    }));
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None)).unwrap();
    assert_eq!(ids(&response), ["renewed"]);
    assert!(
        matches!(&host.requests.borrow()[1],CatalogFetch::Codex{account,..} if account.credential_fingerprint=="new-credential")
    );
}
#[test]
fn logical_lease_expiry_between_chunk_and_metadata_publication_fails_closed() {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    let shared = store.clone();
    store.after_chunks.replace(Some(Rc::new(move || {
        shared.now.set(shared.now.get() + 30001)
    })));
    host.queue(Outcome::json(catalog("late")));
    assert!(run(runtime.versioned_models(&store, &host, "0.163.0", None)).is_err());
    assert!(
        store
            .live(&catalog_store::metadata_key("0.163.0"))
            .is_none()
    );
}
// Source ba5ef05e provider-selection.test.ts allows the old in-flight caller's
// captured answer, but forbids it from replacing the saved cache/current view.
#[test]
fn pre_save_selection_read_cannot_replace_newer_pending_or_current_catalog_views() {
    let store = Store::new();
    let publication = CredentialPublication::default();
    store.put(
        state::selection_key(),
        "before",
        json!({"provider_ids":["openlux"],"updated_at_ms":1_700_000_000_000i64}),
    );
    let reads = SelectionReads {
        store: store.clone(),
        count: Cell::new(0),
        release_first: Cell::new(false),
        release_second: Cell::new(false),
    };
    let runtime = CatalogRuntime::new();
    let mut old = std::pin::pin!(runtime.load_state(&reads, &publication, store.now.get()));
    let mut cx = Context::from_waker(Waker::noop());
    assert!(old.as_mut().poll(&mut cx).is_pending());
    store.put(
        state::selection_key(),
        "saved",
        json!({"provider_ids":["cerebras"],"updated_at_ms":store.now.get()}),
    );
    let mut replacement = std::pin::pin!(runtime.load_state(&reads, &publication, store.now.get()));
    assert!(replacement.as_mut().poll(&mut cx).is_pending());
    reads.release_first.set(true);
    assert_eq!(
        run(old.as_mut())
            .unwrap()
            .input
            .selection
            .unwrap()
            .provider_ids,
        ["openlux"]
    );
    let current = run(runtime.load_state(&reads, &publication, store.now.get())).unwrap();
    assert_eq!(current.input.selection.unwrap().provider_ids, ["cerebras"]);
    assert!(replacement.as_mut().poll(&mut cx).is_pending());
    reads.release_second.set(true);
    assert_eq!(
        run(replacement.as_mut())
            .unwrap()
            .input
            .selection
            .unwrap()
            .provider_ids,
        ["cerebras"]
    );
    assert_eq!(
        run(runtime.load_state(&reads, &publication, store.now.get()))
            .unwrap()
            .input
            .selection
            .unwrap()
            .provider_ids,
        ["cerebras"]
    );
    assert_eq!(reads.count.get(), 4);
}
#[test]
fn versioned_union_and_persisted_account_evidence_keep_source_model_and_effort_units() {
    use gateway_core::{
        source_control::decode_source_control_record, source_serialization as source,
        value::ValueEncoding,
    };
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
    for raw in [
        r#"{"top":"first\udc00","models":[{"slug":"\ud800","display_name":"first","supported_reasoning_levels":[{"effort":"\udc00"}]}]}"#,
        r#"{"top":"second","models":[{"slug":"\ud800","display_name":"second"},{"slug":"\udc00"}]}"#,
    ] {
        host.queue(Outcome {
            response: Ok(CatalogFetchResponse::json(200, raw)),
            ticks: 0,
            callback: None,
        });
    }
    let response = run(runtime.versioned_models(&store, &host, "0.163.0", None)).unwrap();
    let document = SourceJson::parse(response.body.as_deref().unwrap()).unwrap();
    assert_eq!(
        source::member(&document, "top"),
        Some(SourceJson::parse(r#""first\udc00""#).unwrap())
    );
    let models = source::array(&source::member(&document, "models").unwrap()).unwrap();
    assert_eq!(models.len(), 2);
    assert_eq!(
        source::member(&models[0], "slug"),
        Some(SourceJson::parse(r#""\ud800""#).unwrap())
    );
    assert_eq!(
        source::member(&models[0], "display_name"),
        Some(SourceJson::parse(r#""first""#).unwrap())
    );
    let evidence = store.live(&state::account_models_key()).unwrap();
    assert_eq!(evidence.value_encoding, ValueEncoding::ProfileV2);
    let evidence_document = decode_source_control_record(&evidence).unwrap();
    let accounts = source::member(&evidence_document, "accounts").unwrap();
    let first = source::member(&accounts, "account-a").unwrap();
    let second = source::member(&accounts, "account-b").unwrap();
    assert_eq!(
        source::member(&first, "slugs"),
        Some(SourceJson::parse(r#"["\ud800"]"#).unwrap())
    );
    assert_eq!(
        source::member(&second, "slugs"),
        Some(SourceJson::parse(r#"["\ud800","\udc00"]"#).unwrap())
    );
    assert!(host.requests.borrow().iter().all(|request| matches!(
        request,
        CatalogFetch::Codex {
            if_none_match: None,
            ..
        }
    )));
    println!(
        "M42_VERSIONED_EVIDENCE_WRITER_FIXTURE={}",
        serde_json::to_string(&(state::account_models_key().0, evidence)).unwrap()
    );
}

#[test]
fn id_error_before_generation_or_lease_never_falls_back_or_acquires()
-> Result<(), Box<dyn std::error::Error>> {
    for ordinal in [1, 2] {
        let store = Store::new();
        let host = Host::new(&store);
        let failure = FailIds::at(&host, ordinal);
        let error =
            match run(CatalogRuntime::new().versioned_models(&store, &failure, "0.163.0", None)) {
                Ok(_) => return Err("failed ID was accepted".into()),
                Err(error) => error,
            };
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("secure_random_unavailable")
        );
        assert_eq!(failure.calls.get(), ordinal);
        assert!(host.requests.borrow().is_empty());
        assert!(store.live(&catalog_store::lease_key("0.163.0")).is_none());
        assert!(
            store
                .live(&catalog_store::metadata_key("0.163.0"))
                .is_none()
        );
        assert_eq!(
            store.live(&catalog_store::auth_generation_key()).is_some(),
            ordinal == 2
        );
    }
    Ok(())
}
#[test]
fn publication_id_error_retains_prior_evidence_and_releases_original_lease()
-> Result<(), Box<dyn std::error::Error>> {
    for ordinal in [3, 4] {
        let store = Store::new();
        let host = Host::new(&store);
        let mut response = CatalogFetchResponse::json(200, catalog("rng-source").to_string());
        response.single_account_observation_complete = true;
        host.queue(Outcome {
            response: Ok(response),
            ticks: 0,
            callback: None,
        });
        let failure = FailIds::at(&host, ordinal);
        let error =
            match run(CatalogRuntime::new().versioned_models(&store, &failure, "0.163.0", None)) {
                Ok(_) => return Err("failed publication ID was accepted".into()),
                Err(error) => error,
            };
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("secure_random_unavailable")
        );
        assert_eq!(failure.calls.get(), ordinal);
        assert_eq!(host.requests.borrow().len(), 1);
        assert!(store.live(&catalog_store::lease_key("0.163.0")).is_none());
        assert!(
            store
                .live(&catalog_store::metadata_key("0.163.0"))
                .is_none()
        );
        assert_eq!(
            store.live(&state::account_models_key()).is_some(),
            ordinal == 4
        );
        assert!(store.live(&catalog_store::auth_generation_key()).is_some());
    }
    Ok(())
}
#[test]
fn discovery_id_error_preserves_current_memo_without_durable_consuming_write()
-> Result<(), Box<dyn std::error::Error>> {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    store.put(
        catalog_store::auth_generation_key(),
        "generation-row",
        json!("generation"),
    );
    let binding = ProviderBinding {
        identity: "router-key".into(),
    };
    host.published
        .borrow_mut()
        .providers
        .insert(CatalogProvider::Openrouter, binding.clone());
    host.queue(Outcome::json(json!({"data":[{"id":"vendor/text"}]})));
    let failure = FailIds::at(&host, 1);
    let error =
        match run(runtime.refresh_provider(&store, &failure, CatalogProvider::Openrouter, false)) {
            Ok(_) => return Err("failed observation ID was accepted".into()),
            Err(error) => error,
        };
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("secure_random_unavailable")
    );
    assert!(
        runtime
            .cached_provider(&host.published.borrow(), CatalogProvider::Openrouter)
            .is_some()
    );
    assert!(
        store
            .live(&state::provider_observation_key(
                "generation",
                CatalogProvider::Openrouter,
                Some(&binding)
            ))
            .is_none()
    );
    assert_eq!(host.requests.borrow().len(), 1);
    Ok(())
}
#[test]
fn validation_id_error_preserves_accepted_memo_without_observation_write()
-> Result<(), Box<dyn std::error::Error>> {
    let store = Store::new();
    let host = Host::new(&store);
    let runtime = CatalogRuntime::new();
    store.put(
        catalog_store::auth_generation_key(),
        "generation-row",
        json!("generation"),
    );
    let binding = ProviderBinding {
        identity: "claude-owner".into(),
    };
    host.published
        .borrow_mut()
        .providers
        .insert(CatalogProvider::ClaudeCode, binding.clone());
    let failure = FailIds::at(&host, 1);
    let models = vec![SourceJson::from_serializable(
        &json!({"id":"claude-valid"}),
    )?];
    let error = match run(runtime.cache_validated_claude(
        &store,
        &failure,
        &binding,
        models,
        store.now.get(),
    )) {
        Ok(_) => return Err("failed validation ID was accepted".into()),
        Err(error) => error,
    };
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("secure_random_unavailable")
    );
    assert!(
        runtime
            .cached_provider(&host.published.borrow(), CatalogProvider::ClaudeCode)
            .is_some()
    );
    assert!(
        store
            .live(&state::provider_observation_key(
                "generation",
                CatalogProvider::ClaudeCode,
                Some(&binding)
            ))
            .is_none()
    );
    assert!(host.requests.borrow().is_empty());
    Ok(())
}
