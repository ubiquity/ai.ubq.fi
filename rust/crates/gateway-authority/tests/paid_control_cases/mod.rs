use gateway_authority::paid_control::*;
use gateway_authority::paid_types::PaidAuthorization;
use gateway_authority_contracts::DAY_MS;
use gateway_authority_contracts::PAID_RETENTION_MS;
use gateway_authority_keys as keys;
use gateway_authority_paid_control_records::{PaidControlConfig, PaidSyntheticHop};
use gateway_authority_paid_records::{PaidAdmission, PaidRequest};
use gateway_authority_test_support::TestResult;
use gateway_authority_test_support::control::context;
use gateway_authority_test_support::control::no_attempt;
use gateway_authority_test_support::control::remove;
use gateway_authority_test_support::control::republish;
use gateway_authority_test_support::control::seed_controls;
use gateway_authority_test_support::control::seed_source;
use gateway_authority_test_support::control::set_body;
use gateway_authority_test_support::paid_request;
use gateway_authority_test_support::paid_setup;
use gateway_core::identity::ApiKeyId;
use gateway_core::identity::PaidAttemptId;
use gateway_core::identity::RequestId;
use gateway_core::provider::PaidProvider;
use gateway_core::state::{AtomicTransition, RecordKey, RecordMutation, RevisionCheck};
use gateway_providers::types::ClientSurface;
use gateway_routing_runtime_test_support::authority::MemoryStore;
use gateway_routing_runtime_test_support::authority::NOW;
use gateway_routing_runtime_test_support::authority::TestAuthority;
use gateway_routing_runtime_test_support::authority::Tokens;
use gateway_routing_runtime_test_support::authority::authority;
use gateway_routing_runtime_test_support::authority::policy;
use gateway_routing_runtime_test_support::authority::run;
use serde::de::DeserializeOwned;
use serde_json::{Value, json};
use std::sync::atomic::Ordering;

#[test]
fn paid_control_missing_custody_and_legacy_unfenced_policy_refuse() {
    let store = MemoryStore::default();
    paid_setup(&store);
    let authority = authority(&store);
    let input = paid_request("missing-custody", PaidProvider::Surplus);
    remove(&store, keys::paid_control_owner());
    assert!(run(authority.authorize_paid_route(&input, &context(&input), NOW)).is_err());
    seed_controls(&store);
    let mut raw = store.row(&keys::paid_routing()).unwrap().value;
    raw["v"] = json!(1);
    raw.as_object_mut().unwrap().remove("provenance");
    store.seed(keys::paid_routing(), &raw);
    assert!(run(authority.authorize_paid_route(&input, &context(&input), NOW)).is_err());
    no_attempt(&store, &input);
}

#[test]
fn every_source_and_control_change_invalidates_existing_and_new_paid_receipts() {
    let mut cases = paid_projection_source_keys();
    cases.extend([
        keys::paid_control_owner(),
        keys::paid_control_config(),
        keys::paid_control_observation(PaidProvider::Surplus),
        keys::paid_control_observation(PaidProvider::Metered),
    ]);
    for key in cases {
        for existing in [false, true] {
            let store = MemoryStore::default();
            paid_setup(&store);
            let authority = authority(&store);
            let input = paid_request("stale-control", PaidProvider::Surplus);
            let receipt = run(gateway_authority_test_support::original::authorize(
                &authority,
                &input,
                &context(&input),
                NOW,
            ))
            .unwrap();
            if existing {
                run(authority.reserve_paid(
                    &input,
                    &PaidAuthorization::DirectPaidModel,
                    &receipt,
                    NOW,
                ))
                .unwrap();
            }
            let mut raw = store
                .row(&key)
                .map(|row| row.value)
                .unwrap_or_else(|| json!({}));
            raw["source_changed"] = json!(true);
            store.seed(key.clone(), &raw);
            assert!(
                run(authority.reserve_paid(
                    &input,
                    &PaidAuthorization::DirectPaidModel,
                    &receipt,
                    NOW
                ))
                .is_err(),
                "{}",
                key.0
            );
            if !existing {
                no_attempt(&store, &input);
            } else {
                let window = store
                    .row(&keys::paid_window("key-a", policy().usage_reset_at_ms))
                    .unwrap();
                assert_eq!(window.value["pending_count"], 1);
            }
        }
    }
}

#[test]
fn source_change_inside_the_actual_paid_cas_never_writes_exposure() {
    for existing in [false, true] {
        let store = MemoryStore::default();
        paid_setup(&store);
        let authority = authority(&store);
        let input = paid_request("control-race", PaidProvider::Surplus);
        let receipt = run(gateway_authority_test_support::original::authorize(
            &authority,
            &input,
            &context(&input),
            NOW,
        ))
        .unwrap();
        if existing {
            run(authority.reserve_paid(&input, &PaidAuthorization::DirectPaidModel, &receipt, NOW))
                .unwrap();
        }
        let key = keys::paid_control_config();
        let mut raw = store.row(&key).unwrap().value;
        raw["generation"] = json!("superseded-generation");
        store.race(key, &raw);
        assert!(
            run(authority.reserve_paid(&input, &PaidAuthorization::DirectPaidModel, &receipt, NOW))
                .is_err()
        );
        if !existing {
            no_attempt(&store, &input);
        } else {
            assert_eq!(
                store
                    .row(&keys::paid_window("key-a", policy().usage_reset_at_ms))
                    .unwrap()
                    .value["pending_count"],
                1
            );
        }
    }
}

#[test]
fn source_stale_routes_remain_usable_without_invented_refresh_ttl() {
    let store = MemoryStore::default();
    paid_setup(&store);
    let authority = authority(&store);
    let input = paid_request("retained-stale", PaidProvider::Surplus);
    let key = keys::paid_control_observation(PaidProvider::Surplus);
    let mut raw = store.row(&key).unwrap().value;
    raw["observed_at_ms"] = json!(NOW - DAY_MS);
    raw["stale"] = json!(true);
    store.seed(key, &raw);
    republish(&store);
    let receipt = run(gateway_authority_test_support::original::authorize(
        &authority,
        &input,
        &context(&input),
        NOW,
    ))
    .unwrap();
    assert_eq!(receipt.owner_id(), "fixture-owner");
    assert_eq!(receipt.owner_epoch(), "fixture-epoch");
    assert_eq!(
        receipt.configuration_generation(),
        "fixture-config-generation"
    );
    assert_eq!(receipt.credential_binding(), "fixture-surplus-binding");
    assert!(matches!(
        run(authority.reserve_paid(&input, &PaidAuthorization::DirectPaidModel, &receipt, NOW))
            .unwrap(),
        PaidAdmission::Reserved(_)
    ));
}

#[test]
fn missing_openlux_catalog_keeps_configured_known_codex_fallback() {
    let store = MemoryStore::default();
    paid_setup(&store);
    let authority = authority(&store);
    remove(
        &store,
        keys::paid_control_observation(PaidProvider::Metered),
    );
    let selection = paid_projection_source_keys()[2].clone();
    store.seed(
        selection,
        &json!({"provider_ids":["openlux"],"updated_at_ms":NOW}),
    );
    let mut projection = store.row(&keys::paid_routing()).unwrap().value;
    projection["codex_enabled"] = json!(false);
    gateway_authority_test_support::control::seed_source(&store, keys::paid_routing(), &projection);
    republish(&store);
    let mut input = paid_request("missing-catalog", PaidProvider::Metered);
    input.model = "gpt-test".into();
    let receipt = run(gateway_authority_test_support::original::authorize(
        &authority,
        &input,
        &context(&input),
        NOW,
    ))
    .unwrap();
    assert!(matches!(
        run(authority.reserve_paid(&input, &PaidAuthorization::DirectPaidModel, &receipt, NOW))
            .unwrap(),
        PaidAdmission::Reserved(_)
    ));
}

#[test]
fn broad_model_union_does_not_grant_endpoint_tool_or_price_capability() {
    let store = MemoryStore::default();
    paid_setup(&store);
    let authority = authority(&store);
    let key = keys::paid_control_observation(PaidProvider::Surplus);
    let mut observation = store.row(&key).unwrap().value;
    observation["models"][0]["supported_endpoint_types"] = json!(["openai"]);
    observation["models"][0]["supports_tools"] = json!(false);
    store.seed(key.clone(), &observation);
    republish(&store);
    let mut input = paid_request("endpoint", PaidProvider::Surplus);
    assert!(run(authority.authorize_paid_route(&input, &context(&input), NOW)).is_err());
    input.path = "/v1/chat/completions".into();
    input.route = "chat.completions".into();
    let mut ctx = context(&input);
    assert!(run(authority.authorize_paid_route(&input, &ctx, NOW)).is_ok());
    set_body(
        &mut ctx.body,
        "tools",
        json!([{"type":"function","name":"fixture"}]),
    );
    assert!(run(authority.authorize_paid_route(&input, &ctx, NOW)).is_err());
    observation["models"][0]["input_price_per_token"] = Value::Null;
    store.seed(key, &observation);
    republish(&store);
    assert!(run(authority.authorize_paid_route(&input, &context(&input), NOW)).is_err());
}

#[test]
fn synthetic_scope_is_server_owned_and_matches_current_source_alias() {
    let store = MemoryStore::default();
    paid_setup(&store);
    let authority = authority(&store);
    let selection = paid_projection_source_keys()[2].clone();
    store.seed(
        selection.clone(),
        &json!({"provider_ids":["ubiquity"],"updated_at_ms":NOW}),
    );
    let mut projection = store.row(&keys::paid_routing()).unwrap().value;
    projection["codex_enabled"] = json!(false);
    gateway_authority_test_support::control::seed_source(&store, keys::paid_routing(), &projection);
    republish(&store);
    let mut input = paid_request("synthetic", PaidProvider::Metered);
    input.model = "deepseek-v4.1-flash".into();
    let mut ctx = context(&input);
    ctx.original_model = gateway_schema::aliases::SYNTHETIC_MODEL.into();
    set_body(&mut ctx.body, "model", json!(ctx.original_model));
    set_body(&mut ctx.body, "synthetic_hop", json!("openlux"));
    assert!(run(authority.authorize_paid_route(&input, &ctx, NOW)).is_err());
    ctx.synthetic_hop = Some(PaidSyntheticHop::Openlux);
    assert!(run(authority.authorize_paid_route(&input, &ctx, NOW)).is_ok());
    input.model = "paid-model".into();
    assert!(run(authority.authorize_paid_route(&input, &ctx, NOW)).is_err());
    input.model = "deepseek-v4.1-flash".into();
    store.seed(
        selection,
        &json!({"provider_ids":["openlux"],"updated_at_ms":NOW}),
    );
    republish(&store);
    assert!(run(authority.authorize_paid_route(&input, &ctx, NOW)).is_err());
}

#[test]
fn receipt_binds_logical_model_not_physical_effort_alias_and_exact_attempt() {
    let store = MemoryStore::default();
    paid_setup(&store);
    let authority = authority(&store);
    let mut key_policy = policy();
    key_policy
        .paid_fallback_models
        .extend(["gpt-5.6-sol".into(), "gpt-5.6-sol-low".into()]);
    key_policy.paid_fallback_max_exposure_microcredits.extend([
        ("gpt-5.6-sol".into(), 1000),
        ("gpt-5.6-sol-low".into(), 1000),
    ]);
    store.seed(keys::api_policy("key-a"), &key_policy);
    let mut input = paid_request("logical-alias", PaidProvider::Metered);
    input.model = "gpt-5.6-sol".into();
    input.reasoning = Some("none".into());
    let receipt = run(gateway_authority_test_support::original::authorize(
        &authority,
        &input,
        &context(&input),
        NOW,
    ))
    .unwrap();
    input.model = "gpt-5.6-sol-low".into();
    assert!(
        run(authority.reserve_paid(&input, &PaidAuthorization::DirectPaidModel, &receipt, NOW))
            .is_err()
    );
    input.model = "gpt-5.6-sol".into();
    input.attempt_id = PaidAttemptId("different-attempt".into());
    assert!(
        run(authority.reserve_paid(&input, &PaidAuthorization::DirectPaidModel, &receipt, NOW))
            .is_err()
    );
    assert!(!format!("{receipt:?}").contains("fixture-authenticated-key"));
}

#[test]
fn source_defined_expiry_crossing_awaited_paid_reads_never_commits() {
    let store = MemoryStore::default();
    paid_setup(&store);
    let authority = authority(&store);
    let input = paid_request("control-expiry", PaidProvider::Surplus);
    let key = keys::paid_control_observation(PaidProvider::Surplus);
    let mut raw = store.row(&key).unwrap().value;
    raw["source_valid_until_ms"] = json!(NOW + 100);
    store.seed(key, &raw);
    republish(&store);
    let receipt = run(gateway_authority_test_support::original::authorize(
        &authority,
        &input,
        &context(&input),
        NOW,
    ))
    .unwrap();
    store.jump_on_read(keys::paid_deletion("key-a"), NOW + 100);
    assert!(
        run(authority.reserve_paid(&input, &PaidAuthorization::DirectPaidModel, &receipt, NOW))
            .is_err()
    );
    no_attempt(&store, &input);
}

#[test]
fn malformed_missing_and_derived_source_fences_never_authorize() {
    for mode in ["missing", "duplicate", "malformed", "derived"] {
        let store = MemoryStore::default();
        paid_setup(&store);
        let authority = authority(&store);
        let input = paid_request("bad-fence", PaidProvider::Surplus);
        let mut raw = store.row(&keys::paid_routing()).unwrap().value;
        let checks = raw["provenance"]["source_checks"].as_array_mut().unwrap();
        match mode {
            "missing"=>{checks.pop();},
            "duplicate"=>checks.push(checks[0].clone()),
            "malformed"=>checks[0]["key"]=json!("not-a-canonical-key"),
            "derived"=>checks.push(json!({"key":keys::paid_control_config().0,"expected_revision":revision(&store,&keys::paid_control_config())})),
            _=>unreachable!(),
        }
        store.seed(keys::paid_routing(), &raw);
        assert!(run(authority.authorize_paid_route(&input, &context(&input), NOW)).is_err());
    }
}

#[test]
fn openlux_discovery_and_spend_bindings_are_distinct_current_facts() {
    let store = MemoryStore::default();
    paid_setup(&store);
    let authority = authority(&store);
    let input = paid_request("two-openlux-roles", PaidProvider::Metered);
    let receipt = run(gateway_authority_test_support::original::authorize(
        &authority,
        &input,
        &context(&input),
        NOW,
    ))
    .unwrap();
    assert_eq!(receipt.credential_binding(), "fixture-openlux-binding");
    let observation_key = keys::paid_control_observation(PaidProvider::Metered);
    let mut observation = store.row(&observation_key).unwrap().value;
    observation["credential_binding"] = json!("fixture-openlux-binding");
    store.seed(observation_key, &observation);
    republish(&store);
    assert!(run(authority.authorize_paid_route(&input, &context(&input), NOW)).is_err());
    no_attempt(&store, &input);
}

#[test]
fn strict_v2_configuration_requires_explicit_nullable_roles() {
    let store = MemoryStore::default();
    paid_setup(&store);
    let raw = store.row(&keys::paid_control_config()).unwrap().value;
    for field in [
        "surplus_binding",
        "openlux_discovery_binding",
        "openlux_binding",
    ] {
        let mut missing = raw.clone();
        missing.as_object_mut().unwrap().remove(field);
        assert!(serde_json::from_value::<PaidControlConfig>(missing).is_err());
    }
    let mut unknown = raw.clone();
    unknown["unknown_control"] = json!(true);
    assert!(serde_json::from_value::<PaidControlConfig>(unknown).is_err());
    let mut explicit_null = raw.clone();
    explicit_null["openlux_discovery_binding"] = Value::Null;
    assert!(
        serde_json::from_value::<PaidControlConfig>(explicit_null)
            .unwrap()
            .openlux_discovery_binding
            .is_none()
    );
    let mut legacy = raw;
    legacy["v"] = json!(1);
    store.seed(keys::paid_control_config(), &legacy);
    republish(&store);
    let authority = authority(&store);
    let input = paid_request("legacy-config-version", PaidProvider::Metered);
    assert!(run(authority.authorize_paid_route(&input, &context(&input), NOW)).is_err());
}

mod source;
