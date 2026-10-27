use super::FailIds;
use super::{Fetch, Host, Store, openlux, run, surplus};
use gateway_authority::{
    keys,
    paid_control::{PaidControlConfig, PaidProviderObservation, PaidSourceCheck},
};
use gateway_catalog_control::control::publish_paid_observation;
use gateway_catalog_control::control::publish_paid_routing;
use gateway_catalog_inputs::state;
use gateway_catalog_provider::host::{CatalogProvider, ProviderBinding};
use gateway_catalog_runtime::CatalogRuntime;
use gateway_core::{
    provider::PaidProvider, source_control::decode_source_control_record, source_json::SourceJson,
    state::StoredRecord,
};
use gateway_routing_runtime_test_support::source_catalog;
use gateway_schema::catalog_store;
use serde_json::{Value, json};
use std::{
    rc::Rc,
    task::{Context, Waker},
};
#[test]
fn missing_authenticated_control_owner_refuses_without_fetch_or_policy_write() {
    let store = Store::new();
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    assert_eq!(
        run(publish_paid_routing(&runtime, &store, &host))
            .unwrap_err()
            .envelope
            .error
            .code
            .as_deref(),
        Some("paid_control_unavailable")
    );
    assert!(host.requests.borrow().is_empty());
    assert!(store.get(&keys::paid_routing()).is_none());
}
#[test]
fn openlux_spend_identity_cannot_substitute_for_discovery_identity() {
    let store = Store::new();
    store.install(false, true);
    let host = Host::new(&store, false, true);
    host.publication
        .borrow_mut()
        .providers
        .get_mut(&CatalogProvider::Openlux)
        .unwrap()
        .identity = "openlux-credential".into();
    assert!(run(publish_paid_routing(&CatalogRuntime::new(), &store, &host)).is_err());
    assert!(host.requests.borrow().is_empty());
    assert!(store.get(&keys::paid_routing()).is_none());
}
#[test]
fn one_fact_legacy_configuration_is_rejected_before_discovery() {
    let store = Store::new();
    store.install(false, true);
    let mut record = store.get(&keys::paid_control_config()).unwrap();
    record.value["v"] = json!(1);
    record
        .value
        .as_object_mut()
        .unwrap()
        .remove("openlux_discovery_binding");
    store.put(keys::paid_control_config(), "legacy-config", record.value);
    let host = Host::new(&store, false, true);
    assert!(run(publish_paid_routing(&CatalogRuntime::new(), &store, &host)).is_err());
    assert!(host.requests.borrow().is_empty());
}
#[test]
fn real_fixed_publisher_captures_provider_facts_and_all_six_source_fences() {
    let store = Store::new();
    store.install(true, true);
    let host = Host::new(&store, true, true);
    let runtime = CatalogRuntime::new();
    host.queue(surplus("gpt-known"));
    host.queue(openlux("gpt-known"));
    let receipt = run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    let policy = store.projection();
    assert_eq!(policy.policy.v, 2);
    assert_eq!(
        policy.policy.models[&"gpt-known".into()],
        vec![PaidProvider::Surplus, PaidProvider::Metered]
    );
    assert_eq!(
        receipt.revision(),
        store.get(&keys::paid_routing()).unwrap().revision
    );
    assert_eq!(policy.provenance.source_checks.len(), 6);
    assert!(policy.provenance.source_valid_until_ms.is_none());
    assert!(
        store
            .get(&keys::paid_routing())
            .unwrap()
            .expires_at_ms
            .is_none()
    );
    let observation: PaidProviderObservation = decode_source_control_record(
        &store
            .get(&keys::paid_control_observation(PaidProvider::Surplus))
            .unwrap(),
    )
    .unwrap()
    .deserialize()
    .unwrap();
    assert_eq!(
        observation.models[0].deserialize::<Value>().unwrap()["input_price_per_token"],
        0.001
    );
    assert_eq!(
        observation.models[0].deserialize::<Value>().unwrap()["supports_tools"],
        true
    );
    assert!(observation.source_checks.is_empty());
    let openlux: PaidProviderObservation = decode_source_control_record(
        &store
            .get(&keys::paid_control_observation(PaidProvider::Metered))
            .unwrap(),
    )
    .unwrap()
    .deserialize()
    .unwrap();
    assert_eq!(openlux.credential_binding, "openlux-discovery-credential");
}
#[test]
fn retained_stale_catalog_is_usable_without_invented_refresh_expiry() {
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    host.queue(surplus("paid-only"));
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    store.now.set(store.now.get() + 10 * 86_400_000);
    let count = host.requests.borrow().len();
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    assert_eq!(host.requests.borrow().len(), count);
    assert_eq!(
        store.projection().policy.models[&"paid-only".into()],
        vec![PaidProvider::Surplus]
    );
    assert!(
        store
            .projection()
            .provenance
            .source_valid_until_ms
            .is_none()
    );
}
#[test]
fn failed_openlux_discovery_preserves_explicit_known_codex_missing_catalog_fallback() {
    let store = Store::new();
    store.install(false, true);
    let host = Host::new(&store, false, true);
    let runtime = CatalogRuntime::new();
    host.responses.borrow_mut().push_back(Fetch {
        body: source_catalog::json(&json!({})),
        status: 503,
        ticks: 0,
        after: None,
    });
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    let projection = store.projection();
    assert_eq!(
        projection.policy.models[&"gpt-known".into()],
        vec![PaidProvider::Metered]
    );
    assert!(projection.provenance.openlux_observation_revision.is_none());
    assert!(
        store
            .get(&keys::paid_control_observation(PaidProvider::Metered))
            .is_none()
    );
}
#[test]
fn failed_surplus_discovery_cannot_manufacture_a_priced_route() {
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    host.responses.borrow_mut().push_back(Fetch {
        body: source_catalog::json(&json!({})),
        status: 500,
        ticks: 0,
        after: None,
    });
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    assert!(store.projection().policy.models.is_empty());
}
#[test]
fn empty_openlux_catalog_is_distinct_from_missing_and_disables_fallback() {
    let store = Store::new();
    store.install(false, true);
    let host = Host::new(&store, false, true);
    let runtime = CatalogRuntime::new();
    host.queue(json!({"data":[]}));
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    assert!(store.projection().policy.models.is_empty());
    assert!(
        store
            .projection()
            .provenance
            .openlux_observation_revision
            .is_some()
    );
}
#[test]
fn superseded_configuration_during_fetch_cannot_publish_an_observation() {
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    let other = store.clone();
    host.responses.borrow_mut().push_back(Fetch {
        body: source_catalog::json(&surplus("old")),
        status: 200,
        ticks: 0,
        after: Some(Rc::new(move || {
            other.config("generation-2", "new-config-rev", true, false)
        })),
    });
    assert!(
        run(publish_paid_observation(
            &runtime,
            &store,
            &host,
            PaidProvider::Surplus,
            true
        ))
        .is_err()
    );
    assert!(
        store
            .get(&keys::paid_control_observation(PaidProvider::Surplus))
            .is_none()
    );
}
#[test]
fn source_selection_change_across_await_fails_the_original_projection_cas() {
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    let other = store.clone();
    host.responses.borrow_mut().push_back(Fetch {
        body: source_catalog::json(&surplus("old")),
        status: 200,
        ticks: 0,
        after: Some(Rc::new(move || {
            other.put(
                state::selection_key(),
                "selection-changed",
                json!({"provider_ids":["codex"],"updated_at_ms":other.now.get()}),
            )
        })),
    });
    assert!(run(publish_paid_routing(&runtime, &store, &host)).is_err());
    assert!(store.get(&keys::paid_routing()).is_none());
    assert!(
        store
            .get(&keys::paid_control_observation(PaidProvider::Surplus))
            .is_none()
    );
}
#[test]
fn control_change_immediately_before_policy_commit_refuses_atomically() {
    let store = Store::new();
    store.install(false, false);
    let host = Host::new(&store, false, false);
    let runtime = CatalogRuntime::new();
    let other = store.clone();
    store.before_policy.replace(Some(Rc::new(move || {
        other.put(
            catalog_store::auth_generation_key(),
            "new-generation",
            json!("new-generation"),
        )
    })));
    assert!(run(publish_paid_routing(&runtime, &store, &host)).is_err());
    assert!(store.get(&keys::paid_routing()).is_none());
}
#[test]
fn newer_observation_wins_and_older_awaited_producer_cannot_roll_it_back() {
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    host.responses.borrow_mut().push_back(Fetch {
        body: source_catalog::json(&surplus("older")),
        status: 200,
        ticks: 1,
        after: None,
    });
    host.queue(surplus("newer"));
    let mut first = std::pin::pin!(publish_paid_observation(
        &runtime,
        &store,
        &host,
        PaidProvider::Surplus,
        true
    ));
    let mut cx = Context::from_waker(Waker::noop());
    assert!(first.as_mut().poll(&mut cx).is_pending());
    let second = run(publish_paid_observation(
        &runtime,
        &store,
        &host,
        PaidProvider::Surplus,
        true,
    ))
    .unwrap();
    assert!(run(first).is_err());
    let row = store
        .get(&keys::paid_control_observation(PaidProvider::Surplus))
        .unwrap();
    assert_eq!(Some(row.revision.as_str()), second.revision());
    assert_eq!(row.value["models"][0]["id"], "newer");
}
#[test]
fn publication_replaces_full_model_map_and_never_retains_removed_permissions() {
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    host.queue(surplus("old-paid-model"));
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    host.queue(surplus("new-paid-model"));
    run(publish_paid_observation(
        &runtime,
        &store,
        &host,
        PaidProvider::Surplus,
        true,
    ))
    .unwrap();
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    assert!(
        !store
            .projection()
            .policy
            .models
            .contains_key(&"old-paid-model".into())
    );
    assert!(
        store
            .projection()
            .policy
            .models
            .contains_key(&"new-paid-model".into())
    );
}
#[test]
fn selection_update_does_not_expire_unrelated_provider_observation() {
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    host.queue(surplus("paid-only"));
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    store.put(
        state::selection_key(),
        "selection2",
        json!({"provider_ids":["codex"],"updated_at_ms":store.now.get()}),
    );
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    assert!(store.projection().policy.models.is_empty());
    assert_eq!(host.requests.borrow().len(), 1);
}
#[test]
fn current_binding_mismatch_refuses_instead_of_trusting_a_caller_fingerprint() {
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    host.publication.borrow_mut().providers.insert(
        CatalogProvider::Surplus,
        ProviderBinding {
            identity: "wrong-binding".into(),
        },
    );
    let runtime = CatalogRuntime::new();
    assert!(run(publish_paid_routing(&runtime, &store, &host)).is_err());
    assert!(host.requests.borrow().is_empty());
}
#[test]
fn advertised_whitelist_does_not_become_an_extra_paid_runtime_allowlist() {
    let store = Store::new();
    store.install(true, false);
    store.put(
        state::whitelist_key(),
        "whitelist",
        json!({"model_ids":["different-advertised-model"],"updated_at_ms":store.now.get()}),
    );
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    host.queue(surplus("paid-only"));
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    assert!(
        store
            .projection()
            .policy
            .models
            .contains_key(&"paid-only".into())
    );
}
#[test]
fn synthetic_possible_index_does_not_change_ordinary_provider_selection() {
    let store = Store::new();
    store.install(true, false);
    store.put(
        state::selection_key(),
        "selection",
        json!({"provider_ids":["ubiquity"],"updated_at_ms":store.now.get()}),
    );
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    host.queue(surplus("deepseek-v4.1-flash"));
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    assert_eq!(
        store.projection().policy.models[&"deepseek-v4.1-flash".into()],
        vec![PaidProvider::Surplus]
    );
    assert!(!store.projection().policy.codex_enabled);
    let selection = store.get(&state::selection_key()).unwrap();
    assert_eq!(selection.value["provider_ids"], json!(["ubiquity"]));
}

#[test]
fn only_actual_source_deadlines_constrain_a_projection() {
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    let source_key = state::source_key(&["uos_ai", "fixture_owned_paid_credential"]);
    let until = store.now.get() + 10000;
    store.rows.borrow_mut().insert(
        source_key.0.clone(),
        StoredRecord::json(
            "credential-rev",
            json!({"fixture_owner":"fixture-owner"}),
            Some(until),
        ),
    );
    let mut config: PaidControlConfig =
        serde_json::from_value(store.get(&keys::paid_control_config()).unwrap().value).unwrap();
    config.source_checks = vec![PaidSourceCheck {
        key: source_key.0,
        expected_revision: Some("credential-rev".into()),
    }];
    store.put(
        keys::paid_control_config(),
        "config-with-source-deadline",
        json!(config),
    );
    host.queue(surplus("paid-only"));
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    assert_eq!(
        store.projection().provenance.source_valid_until_ms,
        Some(until)
    );
    let observation: PaidProviderObservation = decode_source_control_record(
        &store
            .get(&keys::paid_control_observation(PaidProvider::Surplus))
            .unwrap(),
    )
    .unwrap()
    .deserialize()
    .unwrap();
    assert_eq!(observation.source_valid_until_ms, Some(until));
    store.now.set(until + 1);
    assert!(run(publish_paid_routing(&runtime, &store, &host)).is_err());
}

#[test]
fn malformed_absolute_source_expiry_fails_closed() {
    let store = Store::new();
    store.install(false, false);
    let host = Host::new(&store, false, false);
    let runtime = CatalogRuntime::new();
    store
        .rows
        .borrow_mut()
        .get_mut(&keys::paid_control_owner().0)
        .unwrap()
        .expires_at_ms = Some(i64::MAX);
    assert!(run(publish_paid_routing(&runtime, &store, &host)).is_err());
    assert!(store.get(&keys::paid_routing()).is_none());
}

#[test]
fn custody_metadata_contains_only_the_observed_routing_fields() {
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    let mut response = surplus("paid-only");
    response["data"][0]["description"] = json!("unrelated provider prose");
    response["data"][0]["created"] = json!(12345);
    host.queue(response);
    run(publish_paid_observation(
        &runtime,
        &store,
        &host,
        PaidProvider::Surplus,
        true,
    ))
    .unwrap();
    let observation = store
        .get(&keys::paid_control_observation(PaidProvider::Surplus))
        .unwrap()
        .value;
    assert!(observation["models"][0].get("description").is_none());
    assert_eq!(observation["models"][0]["created"], 12345);
    assert_eq!(observation["models"][0]["input_price_per_token"], 0.001);
    let cached = runtime
        .cached_provider(&host.publication.borrow(), CatalogProvider::Surplus)
        .unwrap();
    assert_eq!(
        cached.models[0].deserialize::<Value>().unwrap()["description"],
        "unrelated provider prose"
    );
}
#[test]
fn source_paid_observation_and_projection_preserve_opaque_models_without_new_authority() {
    use gateway_core::{js_string::JsString16, model::ModelId, value::ValueEncoding};
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    let model = ModelId::from_source(JsString16::from_units(vec![0xd800]));
    host.queue_source(SourceJson::parse(r#"{"data":[{"id":"\ud800","supported_parameters":["tools","tool_choice"],"supported_features":["tools"],"pricing":{"prompt":"0.001","completion":"0.002"}}]}"#).unwrap());
    run(publish_paid_routing(&runtime, &store, &host)).unwrap();
    let observation = store
        .get(&keys::paid_control_observation(PaidProvider::Surplus))
        .unwrap();
    assert_eq!(observation.value_encoding, ValueEncoding::ProfileV2);
    assert_eq!(
        store.projection().policy.models.get(&model),
        Some(&vec![PaidProvider::Surplus])
    );
    assert!(
        !store
            .projection()
            .policy
            .models
            .contains_key(&ModelId::from_scalar(r"\ud800"))
    );
    let row = store.get(&keys::paid_routing()).unwrap();
    assert_eq!(row.value_encoding, ValueEncoding::ProfileV2);
    println!(
        "M42_PAID_PROJECTION_WRITER_FIXTURE={}",
        serde_json::to_string(&(keys::paid_routing().0, row)).unwrap()
    );
}

#[test]
fn paid_id_errors_preserve_prior_observation_and_policy_without_consuming_write()
-> Result<(), Box<dyn std::error::Error>> {
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    host.queue(surplus("paid-only"));
    run(publish_paid_routing(&runtime, &store, &host))?;
    let policy = store.get(&keys::paid_routing()).map(|row| row.revision);
    let observation = store
        .get(&keys::paid_control_observation(PaidProvider::Surplus))
        .map(|row| row.revision);
    let requests = host.requests.borrow().len();
    for observation_only in [true, false] {
        let failure = FailIds::at(&host, 1);
        let result = if observation_only {
            run(publish_paid_observation(
                &runtime,
                &store,
                &failure,
                PaidProvider::Surplus,
                false,
            ))
            .map(|_| ())
        } else {
            run(publish_paid_routing(&runtime, &store, &failure)).map(|_| ())
        };
        let error = match result {
            Ok(()) => return Err("failed paid publication ID was accepted".into()),
            Err(error) => error,
        };
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("secure_random_unavailable")
        );
        assert_eq!(
            store.get(&keys::paid_routing()).map(|row| row.revision),
            policy
        );
        assert_eq!(
            store
                .get(&keys::paid_control_observation(PaidProvider::Surplus))
                .map(|row| row.revision),
            observation
        );
        assert_eq!(host.requests.borrow().len(), requests);
    }
    Ok(())
}
