use gateway_admin_contracts::{audit, routes::AdminOperation};
use gateway_admin_keys::keys::{ApiKeyRecord, KeyService, authority_policy_key, decode_record};
use gateway_admin_test_support::{
    contract_host::Host,
    key_fixtures::{Capabilities, Pricing, actor, source, wire_json},
    runtime_host::run,
    source_store::Store,
};
use gateway_core::state::AuthorityStore;
use serde_json::{Value, json};
use std::cell::RefCell;

#[test]
fn paid_limit_updates_and_disabling_preserve_source_pricing_without_provider_reinitialization() {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let caps = Pricing {
            inputs: RefCell::new(Vec::new()),
        };
        let service = KeyService {
            store: &store,
            host: &host,
            capabilities: &caps,
            release: "fixture",
        };
        let created = service
            .create(
                &source(&json!({"name":"Pricing","paid_fallback_limit_credits":1})),
                &actor(),
                "create",
            )
            .await
            .unwrap();
        let created_body = wire_json(&created.body);
        let id = created_body["id"].as_str().unwrap();
        assert!(caps.inputs.borrow().is_empty());
        service
            .mutate(
                AdminOperation::ApiKeysUpdate,
                &source(&json!({"id":id,"paid_fallback_enabled":true})),
                &actor(),
                "enable",
            )
            .await
            .unwrap();
        assert_eq!(caps.inputs.borrow().len(), 1);
        assert_eq!(caps.inputs.borrow()[0]["paid_fallback_limit_credits"], 1.0);
        for patch in [
            json!({"id":id,"paid_fallback_limit_credits":2}),
            json!({"id":id,"paid_fallback_enabled":true}),
            json!({"id":id,"paid_fallback_enabled":false}),
            json!({"id":id,"paid_fallback_limit_credits":3}),
        ] {
            service
                .mutate(
                    AdminOperation::ApiKeysUpdate,
                    &source(&patch),
                    &actor(),
                    "patch",
                )
                .await
                .unwrap();
            assert_eq!(caps.inputs.borrow().len(), 1);
            let row = store
                .read(&gateway_auth::api_keys::id_key(id))
                .await
                .unwrap()
                .unwrap();
            let record: ApiKeyRecord = decode_record(&row).unwrap();
            assert_eq!(
                record.paid_fallback_model_ids,
                [gateway_core::model::ModelId::from_scalar("paid-model")]
            );
            assert_eq!(record.paid_fallback_quota_per_credit, 1_000_000);
            assert_eq!(record.paid_fallback_pricing_checked_at_ms, Some(1_000_000));
            assert_eq!(
                record.paid_fallback_max_exposure_microcredits
                    [&gateway_core::model::ModelId::from_scalar("paid-model")],
                1000
            );
        }
        service
            .mutate(
                AdminOperation::ApiKeysUpdate,
                &source(&json!({"id":id,"paid_fallback_enabled":true})),
                &actor(),
                "reenable",
            )
            .await
            .unwrap();
        assert_eq!(caps.inputs.borrow().len(), 2);
        assert_eq!(caps.inputs.borrow()[1]["paid_fallback_limit_credits"], 3.0);
    });
}

#[test]
fn audit_never_persists_credential_material() {
    let raw = json!({"name":"key","usage_limit_requests":50,"token":"private-token","hash":"private-hash","authorization":"Bearer private","prompt":"private"});
    let sanitized = audit::safe_changed(&raw)
        .unwrap()
        .deserialize::<Value>()
        .unwrap();
    assert_eq!(sanitized, json!({"name":"key","usage_limit_requests":50}));
    let shared = audit::actor(&actor());
    assert_eq!(shared.kind, "shared_admin_credential");
    assert_eq!(shared.principal_id, None);
}

#[test]
fn key_creation_commits_identity_policy_window_and_audit_together() {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let caps = Capabilities { pending: false };
        let service = KeyService {
            store: &store,
            host: &host,
            capabilities: &caps,
            release: "fixture-release",
        };
        let result = service
            .create(
                &source(&json!({"name":"Fixture key","usage_limit_requests":0})),
                &actor(),
                "request",
            )
            .await
            .unwrap();
        assert_eq!(result.status, 200);
        let result_body = wire_json(&result.body);
        let id = result_body["id"].as_str().unwrap();
        let token = result_body["token"].as_str().unwrap();
        assert!(gateway_auth::api_keys::api_key_shape(token));
        assert!(
            store
                .read(&gateway_auth::api_keys::id_key(id))
                .await
                .unwrap()
                .is_some()
        );
        assert!(
            store
                .read(&authority_policy_key(id))
                .await
                .unwrap()
                .is_some()
        );
        assert!(
            store
                .read(&gateway_authority::keys::api_window(
                    id,
                    &format!("v3:{}", gateway_auth::api_keys::WEEK_MS),
                    1_000_000
                ))
                .await
                .unwrap()
                .is_some()
        );
        let audit = store
            .rows
            .borrow()
            .values()
            .find_map(|row| audit::decode_event(row).ok())
            .unwrap();
        assert_eq!(audit.action, "create");
        assert!(!serde_json::to_string(&audit).unwrap().contains(token));
        assert!(result_body.get("hash").is_none());
    });
}

#[test]
fn conflicting_creation_writes_nothing() {
    run(async {
        let store = Store::default();
        store.conflict.set(true);
        let host = Host::default();
        let caps = Capabilities { pending: false };
        let service = KeyService {
            store: &store,
            host: &host,
            capabilities: &caps,
            release: "fixture",
        };
        assert_eq!(
            service
                .create(&source(&json!({"name":"Fixture"})), &actor(), "request")
                .await
                .unwrap_err()
                .status,
            409
        );
        assert!(store.rows.borrow().is_empty());
    });
}

#[test]
fn malformed_or_unpriced_paid_key_never_writes_state() {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let caps = Capabilities { pending: false };
        let service = KeyService {
            store: &store,
            host: &host,
            capabilities: &caps,
            release: "fixture",
        };
        for body in [
            json!({"name":"bad\nkey"}),
            json!({"name":"key","paid_fallback_enabled":true}),
            json!({"name":"key","token":"wrong"}),
            json!({"name":"key","window_ms":0}),
        ] {
            assert_eq!(
                service
                    .create(&source(&body), &actor(), "request")
                    .await
                    .unwrap_err()
                    .status,
                400
            );
            assert!(store.rows.borrow().is_empty());
        }
        assert_eq!(service.create(&source(&json!({"name":"key","paid_fallback_enabled":true,"paid_fallback_limit_credits":1})),&actor(),"request").await.unwrap_err().status,503);
        assert!(store.rows.borrow().is_empty());
    });
}

#[test]
fn revoked_deletion_never_discards_pending_billing() {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let caps = Capabilities { pending: true };
        let service = KeyService {
            store: &store,
            host: &host,
            capabilities: &caps,
            release: "fixture",
        };
        let created = service
            .create(&source(&json!({"name":"Fixture"})), &actor(), "create")
            .await
            .unwrap();
        let created_body = wire_json(&created.body);
        let id = created_body["id"].as_str().unwrap();
        assert_eq!(
            service
                .mutate(
                    AdminOperation::ApiKeysDelete,
                    &source(&json!({"id":id})),
                    &actor(),
                    "delete"
                )
                .await
                .unwrap_err()
                .status,
            400
        );
        service
            .mutate(
                AdminOperation::ApiKeysRevoke,
                &source(&json!({"id":id})),
                &actor(),
                "revoke",
            )
            .await
            .unwrap();
        let before = store.rows.borrow().clone();
        assert_eq!(
            service
                .mutate(
                    AdminOperation::ApiKeysDelete,
                    &source(&json!({"id":id})),
                    &actor(),
                    "delete"
                )
                .await
                .unwrap_err()
                .status,
            409
        );
        assert_eq!(*store.rows.borrow(), before);
        let policy = store
            .read(&authority_policy_key(id))
            .await
            .unwrap()
            .unwrap();
        assert_eq!(
            policy.json_value().unwrap()["expires_at_ms"],
            json!(1_000_000)
        );
    });
}
