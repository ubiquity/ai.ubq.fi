use gateway_admin_contracts::operations::AdminHost;
use gateway_admin_runtime::RuntimeConfig;
use gateway_admin_test_support::runtime_fixtures::seed_catalog;
use gateway_admin_test_support::{
    runtime_fixtures::{WireBody, input, runtime},
    runtime_host::{Host, run},
    runtime_store::Store,
};
use gateway_auth::types::{AuthConfig, AuthRequest};
use gateway_contracts::model::ModelId;
use gateway_core::state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation};
use serde_json::json;
use std::collections::BTreeMap;
type TestResult = Result<(), Box<dyn std::error::Error>>;

#[test]
fn paid_policy_uses_full_context_exposure_and_never_compact_fallback() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = runtime(&store, &host, &config);
    seed_catalog(&store, true);
    let fields = run(service.paid_key_policy(
        &input(json!({"paid_fallback_enabled":true,"paid_fallback_limit_credits":2.25})).body,
    ))
    .unwrap();
    assert_eq!(fields.limit_microcredits, 2250000);
    assert_eq!(
        fields.max_exposure_microcredits[&ModelId::from_scalar("gpt-source")],
        15000000
    );
    seed_catalog(&store, false);
    assert_eq!(
        run(service.paid_key_policy(
            &input(json!({"paid_fallback_enabled":true,"paid_fallback_limit_credits":-1})).body
        ))
        .unwrap_err()
        .status,
        503
    );
}
#[test]
fn deletion_refuses_pending_markers_and_returns_actual_guard_fences() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = runtime(&store, &host, &config);
    store.put(
        json!(["ubq_ai", "api_keys", "id", "key"]),
        json!({"id":"key","name":"deletion fixture","prefix":"u_0000000000","hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","created_at_ms":800000,"expires_at_ms":-1,"revoked_at_ms":900000,"usage_limit_requests":-1,"usage_requests":0,"usage_reset_at_ms":2000000,"window_ms":604800000,"usage_quota_version":3,"paid_fallback_enabled":false,"paid_fallback_limit_microcredits":0,"paid_fallback_spent_microcredits":0,"paid_fallback_reserved_microcredits":0,"paid_fallback_reservation_request_id":null,"paid_fallback_model_ids":[],"paid_fallback_quota_per_credit":0,"paid_fallback_max_exposure_microcredits":{},"paid_fallback_pricing_checked_at_ms":null}),
    );
    store.put(
        json!(["uos_ai", "paid_fallback", "v3", "pending", "key", "attempt"]),
        json!({"created_at_ms":900000}),
    );
    assert_eq!(
        run(service.prepare_key_deletion("key")).unwrap_err().status,
        409
    );
    assert!(
        store
            .get(json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "deletion_guard",
                "key"
            ]))
            .is_some()
    );
    store.rows.borrow_mut().remove(
        &gateway_auth::store::key(json!([
            "uos_ai",
            "paid_fallback",
            "v3",
            "pending",
            "key",
            "attempt"
        ]))
        .0,
    );
    let fences = run(service.prepare_key_deletion("key")).unwrap();
    let guard = gateway_authority::keys::paid_deletion("key");
    assert!(
        fences
            .iter()
            .any(|f| f.key == guard && f.expected_revision.is_some())
    );
    let again = run(service.prepare_key_deletion("key")).unwrap();
    assert_eq!(again[0], fences[0]);
}
#[test]
fn deletion_cleans_settled_state_then_owned_history_and_retains_ordinary_guard() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = runtime(&store, &host, &config);
    store.put(
        json!(["ubq_ai", "api_keys", "id", "key"]),
        json!({"id":"key","name":"deletion fixture","prefix":"u_0000000000","hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","created_at_ms":800000,"expires_at_ms":-1,"revoked_at_ms":900000,"usage_limit_requests":-1,"usage_requests":0,"usage_reset_at_ms":2000000,"window_ms":604800000,"usage_quota_version":3,"paid_fallback_enabled":false,"paid_fallback_limit_microcredits":0,"paid_fallback_spent_microcredits":0,"paid_fallback_reserved_microcredits":0,"paid_fallback_reservation_request_id":null,"paid_fallback_model_ids":[],"paid_fallback_quota_per_credit":0,"paid_fallback_max_exposure_microcredits":{},"paid_fallback_pricing_checked_at_ms":null}),
    );
    store.put(json!(["uos_ai","paid_fallback","v3","request","key","attempt"]),json!({"v":3,"reservation":{"key_id":"key","request_id":"original","attempt_id":"attempt","provider":"metered","window_reset_at_ms":2000000,"created_at_ms":900000,"reserved_microcredits":20},"policy_version":"v3:week","model":"gpt-source","route":"responses","path":"/v1/responses","stream":true,"reasoning":null,"quota_per_credit":1000,"provider_request_id":"provider-request","dispatch_state":"dispatched","terminal_state":"completed","billing_state":"settled","provider_quota":1.0,"input_tokens":10,"cached_input_tokens":null,"output_tokens":1,"spend_microcredits":10,"transport_started":true,"reconciliation_attempts":0,"last_reconciliation_at_ms":null,"dispatched_at_ms":900000,"terminal_at_ms":900100,"settled_at_ms":900200,"updated_at_ms":900200,"usage_rollup_at_ms":900200}));
    store.put(json!(["uos_ai","paid_fallback","v3","window","key",2000000]),json!({"v":3,"key_id":"key","policy_version":"v3:week","window_reset_at_ms":2000000,"limit_microcredits":100,"settled_microcredits":10,"reserved_microcredits":0,"pending_count":0,"updated_at_ms":900200}));
    store.put(
        json!([
            "uos_ai",
            "paid_fallback",
            "v3",
            "reconciliation_lease",
            "key"
        ]),
        json!({"token":"lease","expires_at_ms":900001}),
    );
    for key in [
        json!(["ubq_ai", "api_keys", "usage", "key"]),
        json!(["ubq_ai", "api_keys", "usage_daily", "key"]),
        json!([
            "uos_ai",
            "paid_fallback",
            "ledger",
            "key",
            900000,
            "original"
        ]),
        json!(["ubq_ai", "api_keys", "request_log", "key", 900000, "legacy"]),
        json!(["uos_ai", "api_key_usage", "v2", "key", "bucket"]),
        json!([
            "uos_ai",
            "api_key_usage",
            "v3",
            "request",
            "key",
            "policy",
            1,
            "original"
        ]),
    ] {
        store.put(key, json!({"retained":true}));
    }
    store.put(
        json!([
            "uos_ai",
            "api_key_usage",
            "v3",
            "request",
            "foreign",
            "policy",
            1,
            "keep"
        ]),
        json!({"keep":true}),
    );
    let fences = run(service.prepare_key_deletion("key")).unwrap();
    assert!(
        store
            .get(json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "request",
                "key",
                "attempt"
            ]))
            .is_none()
    );
    assert!(
        store
            .get(json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "window",
                "key",
                2000000
            ]))
            .is_none()
    );
    let outcome = run(store.commit(AtomicTransition {
        checks: fences.clone(),
        mutations: vec![RecordMutation::Delete {
            key: gateway_auth::api_keys::id_key("key"),
        }],
    }))
    .unwrap();
    assert_eq!(outcome, CommitOutcome::Committed);
    run(service.finish_deletion("key", &fences)).unwrap();
    assert_eq!(
        store
            .get(json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "deletion_guard",
                "key"
            ]))
            .unwrap(),
        json!({"created_at_ms":1000000})
    );
    assert!(
        store
            .get(json!([
                "uos_ai",
                "api_key_usage",
                "v3",
                "request",
                "key",
                "policy",
                1,
                "original"
            ]))
            .is_none()
    );
    assert!(
        store
            .get(json!([
                "uos_ai",
                "api_key_usage",
                "v3",
                "request",
                "foreign",
                "policy",
                1,
                "keep"
            ]))
            .is_some()
    );
    assert_eq!(
        run(service.prepare_key_deletion("key")).unwrap_err().status,
        404
    );
}

#[test]
fn local_development_deletion_releases_only_owner_marker_and_completes_after_absent_id() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = runtime(&store, &host, &config);
    let id = "local-development";
    store.put(
        json!(["ubq_ai", "api_keys", "id", id]),
        json!({"id":id,"name":"deletion fixture","prefix":"u_0000000000","hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","created_at_ms":800000,"expires_at_ms":-1,"revoked_at_ms":900000,"usage_limit_requests":-1,"usage_requests":0,"usage_reset_at_ms":2000000,"window_ms":604800000,"usage_quota_version":3,"paid_fallback_enabled":false,"paid_fallback_limit_microcredits":0,"paid_fallback_spent_microcredits":0,"paid_fallback_reserved_microcredits":0,"paid_fallback_reservation_request_id":null,"paid_fallback_model_ids":[],"paid_fallback_quota_per_credit":0,"paid_fallback_max_exposure_microcredits":{},"paid_fallback_pricing_checked_at_ms":null}),
    );
    store.put(
        json!(["uos_ai", "paid_fallback", "v3", "pending", id, "attempt"]),
        json!({"created_at_ms":900000}),
    );
    assert_eq!(
        run(service.prepare_key_deletion(id)).unwrap_err().status,
        409
    );
    let guard_key = json!(["uos_ai", "paid_fallback", "v3", "deletion_guard", id]);
    let guard = store.get(guard_key.clone()).unwrap();
    assert!(guard["local_deletion"]["owner"].is_null());
    assert!(guard["local_deletion"]["completed_at_ms"].is_null());
    store.rows.borrow_mut().remove(
        &gateway_auth::store::key(json!([
            "uos_ai",
            "paid_fallback",
            "v3",
            "pending",
            id,
            "attempt"
        ]))
        .0,
    );
    let first = run(service.prepare_key_deletion(id)).unwrap();
    assert_eq!(
        run(service.prepare_key_deletion(id)).unwrap_err().status,
        409
    );
    run(service.abort_deletion(id, &first)).unwrap();
    assert!(store.get(guard_key.clone()).unwrap()["local_deletion"]["owner"].is_null());
    let next = run(service.prepare_key_deletion(id)).unwrap();
    assert_eq!(
        run(service.finish_deletion(id, &next)).unwrap_err().status,
        409
    );
    run(store.commit(AtomicTransition {
        checks: next.clone(),
        mutations: vec![RecordMutation::Delete {
            key: gateway_auth::api_keys::id_key(id),
        }],
    }))
    .unwrap();
    run(service.finish_deletion(id, &next)).unwrap();
    let complete = store.get(guard_key).unwrap();
    assert_eq!(complete["local_deletion"]["completed_at_ms"], 1000000);
    assert!(complete["local_deletion"]["owner"].is_string());
}

#[test]
fn authenticated_admin_service_uses_runtime_deletion_completion_hooks() -> TestResult {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let runtime = runtime(&store, &host, &config);
    let auth = AuthConfig {
        admin_tokens: vec![gateway_auth::crypto::Secret::new("fixture-admin".into())],
        ..AuthConfig::default()
    };
    let admin = gateway_admin::service::AdminService {
        store: &store,
        host: &host,
        capabilities: &runtime,
        config: &auth,
        release: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    };
    let mut request = AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/admin/api-keys".into(),
        headers: BTreeMap::from([("authorization".into(), "Bearer fixture-admin".into())]),
        body: serde_json::to_vec(
            &json!({"name":"source lifecycle","expires_at_ms":-1,"usage_limit_requests":-1}),
        )?,
        ..AuthRequest::default()
    };
    let created =
        run(admin.handle_route(&request, "create-key"))?.ok_or("expected matched admin route")?;
    let id = created.body.wire_json()["id"]
        .as_str()
        .ok_or("expected created key ID")?
        .to_owned();
    assert_eq!(created.status, 200);
    request.url = "https://ai.ubq.fi/admin/api-keys/revoke".into();
    request.body = serde_json::to_vec(&json!({"id":id}))?;
    run(admin.handle_route(&request, "revoke-key"))?.ok_or("expected matched admin route")?;
    store.put(
        json!(["uos_ai", "paid_fallback", "ledger", id, 900000, "record"]),
        json!({"retained":true}),
    );
    request.method = "DELETE".into();
    request.url = "https://ai.ubq.fi/admin/api-keys".into();
    let deleted =
        run(admin.handle_route(&request, "delete-key"))?.ok_or("expected matched admin route")?;
    assert_eq!(deleted.status, 200);
    assert!(store.get(json!(["ubq_ai", "api_keys", "id", id])).is_none());
    assert!(
        store
            .get(json!([
                "uos_ai",
                "paid_fallback",
                "ledger",
                id,
                900000,
                "record"
            ]))
            .is_none()
    );
    assert!(
        store
            .get(json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "deletion_guard",
                id
            ]))
            .is_some()
    );
    let before = store.rows.borrow().clone();
    request.headers.clear();
    assert_eq!(
        run(admin.handle_route(&request, "unauthorized-delete"))
            .err()
            .ok_or("expected authentication failure")?
            .status,
        401
    );
    assert_eq!(*store.rows.borrow(), before);
    Ok(())
}
