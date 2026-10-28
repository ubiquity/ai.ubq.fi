use gateway_admin_contracts::{operations::AdminHost, routes::AdminOperation};
use gateway_admin_runtime::AdminRuntime;
use gateway_admin_runtime_contracts::{
    RuntimeConfig,
    capabilities::{CaptureRequest, CodexRequest, Context, RuntimeCapabilities},
};
use gateway_admin_test_support::{
    runtime_fixtures::{WireBody, input, runtime, seed_catalog},
    runtime_host::{Host, run},
    runtime_store::Store,
};
use gateway_auth_contracts::types::AuthResponse;
use gateway_kv_codec::tuple_key::{ScalarKeyPart, TupleKey};
use gateway_schema::catalog_store::codex_snapshot_key;
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::cell::RefCell;
type TestResult = Result<(), Box<dyn std::error::Error>>;
mod manual_reset;

#[test]
fn authorization_precedes_state_reads_and_has_no_super_admin_tier() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = runtime(&store, &host, &config);
    let mut denied = input(json!({"scenario":"codex_429"}));
    denied.actor.is_admin = false;
    assert_eq!(
        run(service.execute(AdminOperation::DebugSet, denied))
            .unwrap_err()
            .status,
        401
    );
    assert_eq!(store.reads.get(), 0);
    assert_eq!(
        run(service.execute(
            AdminOperation::DebugSet,
            input(json!({"scenario":"codex_429"}))
        ))
        .unwrap()
        .status,
        200
    );
}
#[test]
fn debug_expiry_and_unknown_scenarios_follow_source() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = runtime(&store, &host, &config);
    let before = run(service.execute(
        AdminOperation::DebugSet,
        input(json!({"scenario":"codex_503","duration_ms":10})),
    ))
    .unwrap();
    assert_eq!(before.body.wire_json()["routing"]["expires_at_ms"], 1000010);
    host.now.set(1000020);
    let mut request = input(Value::Null);
    request.now_ms = 1000020;
    assert_eq!(
        run(service.execute(AdminOperation::DebugGet, request))
            .unwrap()
            .body
            .wire_json()["routing"]["scenario"],
        "normal"
    );
    assert_eq!(
        run(service.execute(
            AdminOperation::DebugSet,
            input(json!({"scenario":"invented"}))
        ))
        .unwrap_err()
        .status,
        400
    );
}
#[test]
fn provider_selection_is_ordered_and_codex_absorbs_subscriptions() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = runtime(&store, &host, &config);
    let id = format!("codex:{}", "a".repeat(64));
    let response = run(service.execute(
        AdminOperation::ProviderSelectionSet,
        input(json!({"provider_ids":["openlux",id,"codex","codex","surplus"]})),
    ))
    .unwrap();
    assert_eq!(
        response.body.wire_json()["provider_ids"],
        json!(["codex", "surplus", "openlux"])
    );
    assert_eq!(
        run(service.execute(
            AdminOperation::ProviderSelectionSet,
            input(json!({"provider_ids":["new-provider"]}))
        ))
        .unwrap_err()
        .status,
        400
    );
    assert_eq!(
        store
            .get(
                TupleKey::from_scalar_parts(&[
                    ScalarKeyPart::String("uos_ai"),
                    ScalarKeyPart::String("provider_selection"),
                    ScalarKeyPart::String("v1")
                ])
                .to_record_key()
            )
            .unwrap()["provider_ids"],
        json!(["codex", "surplus", "openlux"])
    );
}
#[test]
fn defaults_keep_advertised_custom_reasoning_and_atomic_late_validation() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    seed_catalog(&store, true);
    let service = runtime(&store, &host, &config);
    let response=run(service.execute(AdminOperation::DefaultsSet,input(json!({"model":"gpt-source","reasoning_effort":"future-tier","kernel_policy_limit_requests":25})))).unwrap();
    assert_eq!(
        response.body.wire_json()["defaults"]["reasoning_effort"],
        "future-tier"
    );
    assert_eq!(
        store
            .get(
                TupleKey::from_scalar_parts(&[
                    ScalarKeyPart::String("uos_ai"),
                    ScalarKeyPart::String("authority"),
                    ScalarKeyPart::String("v1"),
                    ScalarKeyPart::String("kernel_defaults")
                ])
                .to_record_key()
            )
            .unwrap()["usage_limit_requests"],
        25
    );
    let prior = store.rows.borrow().clone();
    assert_eq!(
        run(service.execute(
            AdminOperation::DefaultsSet,
            input(json!({"model":"gpt-source","kernel_policy_window_ms":0}))
        ))
        .unwrap_err()
        .status,
        400
    );
    assert_eq!(*store.rows.borrow(), prior);
    assert_eq!(
        run(service.execute(
            AdminOperation::DefaultsSet,
            input(json!({"model":"gpt-source"}))
        ))
        .unwrap()
        .body
        .wire_json()["defaults"]["reasoning_effort"],
        "none"
    );
}
fn kernel_window(store: &Store, expiry: i64, intent: Value) {
    store.put(TupleKey::from_scalar_parts(&[ScalarKeyPart::String("uos_ai"),ScalarKeyPart::String("kernel_quota"),ScalarKeyPart::String("v2"),ScalarKeyPart::String("org_window"),ScalarKeyPart::String("owner")]).to_record_key(),json!({"v":2,"owner":"owner","repo":null,"usage_requests":0,"reserved_requests":1,"created_at_ms":800000,"usage_reset_at_ms":2000000,"applied_window_ms":604800000,"updated_at_ms":900000,"reservations":{"request":expiry}}));
    store.put(TupleKey::from_scalar_parts(&[ScalarKeyPart::String("uos_ai"),ScalarKeyPart::String("kernel_quota"),ScalarKeyPart::String("v2"),ScalarKeyPart::String("org_reservation"),ScalarKeyPart::String("owner"),ScalarKeyPart::Number(800000),ScalarKeyPart::String("request")]).to_record_key(),json!({"v":2,"owner":"owner","repo":null,"request_id":"request","route":"/v1/responses","window_created_at_ms":800000,"window_reset_at_ms":2000000,"reserved_at_ms":850000,"lease_expires_at_ms":expiry,"terminal_intent":intent,"state":null,"terminal_at_ms":null,"release_reason":null}));
}
#[test]
fn default_window_cutover_blocks_active_reservations_and_releases_its_claim() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    kernel_window(&store, 1500000, Value::Null);
    let service = runtime(&store, &host, &config);
    assert_eq!(
        run(service.execute(
            AdminOperation::DefaultsSet,
            input(json!({"kernel_policy_window_ms":200000}))
        ))
        .unwrap_err()
        .status,
        409
    );
    assert!(
        store
            .get(
                TupleKey::from_scalar_parts(&[
                    ScalarKeyPart::String("uos_ai"),
                    ScalarKeyPart::String("kernel_quota"),
                    ScalarKeyPart::String("v2"),
                    ScalarKeyPart::String("default_window_cutover")
                ])
                .to_record_key()
            )
            .is_none()
    );
    assert!(
        store
            .get(
                TupleKey::from_scalar_parts(&[
                    ScalarKeyPart::String("default"),
                    ScalarKeyPart::String("kernel_policy_window_ms")
                ])
                .to_record_key()
            )
            .is_none()
    );
}
#[test]
fn expired_kernel_terminal_intent_is_committed_before_default_cutover() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    kernel_window(&store, 900000, json!("committed"));
    let service = runtime(&store, &host, &config);
    run(service.execute(
        AdminOperation::DefaultsSet,
        input(json!({"kernel_policy_window_ms":200000})),
    ))
    .unwrap();
    let window = store
        .get(
            TupleKey::from_scalar_parts(&[
                ScalarKeyPart::String("uos_ai"),
                ScalarKeyPart::String("kernel_quota"),
                ScalarKeyPart::String("v2"),
                ScalarKeyPart::String("org_window"),
                ScalarKeyPart::String("owner"),
            ])
            .to_record_key(),
        )
        .unwrap();
    assert_eq!(window["usage_requests"], 1);
    assert_eq!(window["reserved_requests"], 0);
}
#[test]
fn defaults_work_from_runtime_catalog_without_optional_full_snapshot() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = runtime(&store, &host, &config);
    seed_catalog(&store, true);
    store.rows.borrow_mut().remove(&codex_snapshot_key().0);
    let response = run(service.execute(
        AdminOperation::DefaultsSet,
        input(json!({"model":"gpt-source","reasoning_effort":"future-tier"})),
    ))
    .unwrap();
    assert_eq!(response.body.wire_json()["defaults"]["model"], "gpt-source");
    assert_eq!(
        run(service.paid_key_policy(
            &input(json!({"paid_fallback_enabled":true,"paid_fallback_limit_credits":1})).body
        ))
        .unwrap_err()
        .status,
        503
    );
}

#[test]
fn admin_inputs_use_source_number_coercion_and_ecmascript_whitespace() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = runtime(&store, &host, &config);
    let response = run(service.execute(
        AdminOperation::DebugSet,
        input(json!({"scenario":"\u{feff}codex_429\u{feff}","duration_ms":["16"]})),
    ))
    .unwrap();
    assert_eq!(
        response.body.wire_json()["routing"]["expires_at_ms"],
        1000016
    );
    let response = run(service.execute(
        AdminOperation::ClaudeAccessSet,
        input(json!({"allowed_key_ids":["\u{0085}key\u{0085}"]})),
    ))
    .unwrap();
    assert_eq!(
        response.body.wire_json()["data"]["allowed_key_ids"],
        json!(["\u{0085}key\u{0085}"])
    );
    let mut request = input(Value::Null);
    request
        .query
        .insert("after_ms".into(), vec![" \u{feff}0x10\u{feff} ".into()]);
    request
        .query
        .insert("before_ms".into(), vec!["0x20".into()]);
    request.query.insert("limit".into(), vec!["0b1".into()]);
    assert_eq!(
        gateway_admin_runtime_contracts::parse::capture(&request, false).unwrap(),
        CaptureRequest::Interval {
            after_ms: 16,
            before_ms: 32,
            cursor: None
        }
    );
    request
        .query
        .insert("after_ms".into(), vec!["\u{0085}16\u{0085}".into()]);
    assert_eq!(
        gateway_admin_runtime_contracts::parse::capture(&request, false)
            .unwrap_err()
            .status,
        400
    );
}
