use gateway_admin_kernel::kernel_auth;
use gateway_admin_test_support::{contract_host::Host, runtime_host::run, source_store::Store};
use gateway_core::state::{AtomicTransition, AuthorityStore, CommitOutcome, StoredRecord};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::cell::Cell;

fn kernel_source_policy(scope: &str, repo: Option<&str>) -> Value {
    let mut value = json!({"v":2,"scope":scope,"owner":"owner","usage_limit_requests":"25.9","window_ms":"60000.9","expires_at_ms":0,"created_at_ms":1,"updated_at_ms":2});
    if let Some(repo) = repo {
        value["repo"] = json!(repo);
    }
    value
}

#[test]
fn kernel_auth_validates_both_source_policies_before_repo_precedence() {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let service = kernel_auth::KernelAuthService {
            store: &store,
            host: &host,
        };
        let repo = gateway_authority_keys::kernel_policy("owner", Some("repo"));
        let org = gateway_authority_keys::kernel_policy("owner", None);
        store.rows.borrow_mut().insert(
            repo.0,
            StoredRecord::json("repo", kernel_source_policy("repo", Some("repo")), None),
        );
        let mut corrupt_org = kernel_source_policy("org", None);
        corrupt_org["created_at_ms"] = json!(0.5);
        store.rows.borrow_mut().insert(
            org.0.clone(),
            StoredRecord::json("org-invalid", corrupt_org, None),
        );
        assert_eq!(
            service
                .resolve("owner", "repo", "/v1/responses")
                .await
                .unwrap_err()
                .status,
            503
        );
        store.rows.borrow_mut().insert(
            org.0,
            StoredRecord::json("org-valid", kernel_source_policy("org", None), None),
        );
        let result = service
            .resolve("owner", "repo", "/v1/responses")
            .await
            .unwrap();
        assert_eq!(result.limit_scope, "repo");
        assert!(result.has_policy); // The policy's expires_at_ms=0 does not erase its scope.
        assert_eq!(result.queue_recorded, None);
        assert!(
            store
                .read(&gateway_auth_primitives::store::key(json!([
                    "uos_ai",
                    "kernel_policy_queue"
                ])))
                .await
                .unwrap()
                .is_none()
        );
        store.rows.borrow_mut().insert(
            gateway_authority_keys::kernel_policy("owner", Some("repo")).0,
            StoredRecord::json("stored-null", Value::Null, None),
        );
        assert_eq!(
            service
                .resolve("owner", "repo", "/v1/responses")
                .await
                .unwrap()
                .limit_scope,
            "org"
        );
    });
}

#[test]
fn kernel_missing_policy_queue_retries_cas_without_double_counting_and_keeps_200() {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let service = kernel_auth::KernelAuthService {
            store: &store,
            host: &host,
        };
        let key = gateway_auth_primitives::store::key(json!(["uos_ai", "kernel_policy_queue"]));
        let mut rows: Vec<Value> = (0..201).map(|index| json!({"owner":"other","repo":format!("repo-{index}"),"request_count":1,"first_seen_at_ms":index,"last_seen_at_ms":index,"last_route":"/old"})).collect();
        rows.push(json!({"owner":" owner ","repo":" repo ","request_count":5.9,"first_seen_at_ms":10,"last_seen_at_ms":20,"last_route":"/original"}));
        rows.push(json!({"owner":"","repo":"invalid"}));
        store.rows.borrow_mut().insert(
            key.0.clone(),
            StoredRecord::json("queue-before", json!(rows), None),
        );
        store.conflict.set(true);
        let result = service.resolve("owner", "repo", "  ").await.unwrap();
        assert_eq!(result.limit_scope, "org");
        assert!(!result.has_policy);
        assert_eq!(result.queue_recorded, Some(true));
        let row = store.read(&key).await.unwrap().unwrap();
        let value = row.json_value().unwrap().as_array().unwrap();
        assert_eq!(value.len(), 200);
        assert_eq!(
            value[0],
            json!({"owner":"owner","repo":"repo","request_count":6.0,"first_seen_at_ms":10.0,"last_seen_at_ms":1_000_000.0,"last_route":"/original"})
        );
    });
}

struct AlwaysConflict<'a> {
    store: &'a Store,
    attempts: Cell<usize>,
}
impl AuthorityStore for AlwaysConflict<'_> {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        self.store.read(key).await
    }
    async fn commit(&self, _: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        self.attempts.set(self.attempts.get() + 1);
        Ok(CommitOutcome::Conflict)
    }
}

#[test]
fn kernel_queue_failure_is_bounded_and_does_not_invent_a_policy() {
    run(async {
        let store = Store::default();
        let conflicted = AlwaysConflict {
            store: &store,
            attempts: Cell::new(0),
        };
        let host = Host::default();
        let service = kernel_auth::KernelAuthService {
            store: &conflicted,
            host: &host,
        };
        let result = service
            .resolve("owner", "repo", "/v1/responses")
            .await
            .unwrap();
        assert_eq!(result.limit_scope, "org");
        assert!(!result.has_policy);
        assert_eq!(result.queue_recorded, Some(false));
        assert_eq!(conflicted.attempts.get(), 3);
        assert!(store.rows.borrow().is_empty());
    });
}
