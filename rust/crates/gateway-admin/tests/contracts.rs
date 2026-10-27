use gateway_admin::service::AdminService;
use gateway_admin_contracts::{
    operations::{AdminHost, AdminInput, PaidKeyFields, missing_capability},
    routes::{AdminOperation, match_route},
};
use gateway_admin_test_support::{contract_host::Host, runtime_host::run, source_store::Store};
use gateway_auth::{
    crypto::Secret,
    types::{AuthConfig, AuthRequest, AuthResponse},
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use gateway_core::state::RevisionCheck;
use serde_json::{Value, json};
use std::{cell::RefCell, collections::BTreeMap};

struct CaptureBody {
    seen: RefCell<Vec<(AdminOperation, Value)>>,
}
impl AdminHost for CaptureBody {
    async fn execute(
        &self,
        operation: AdminOperation,
        input: AdminInput,
    ) -> Result<AuthResponse, GatewayError> {
        assert!(input.actor.is_admin);
        self.seen
            .borrow_mut()
            .push((operation, input.body.deserialize()?));
        Ok(AuthResponse::json(200, json!({"received":true})))
    }
    async fn prepare_key_deletion(&self, _: &str) -> Result<Vec<RevisionCheck>, GatewayError> {
        Err(missing_capability(AdminOperation::ApiKeysDelete))
    }
    async fn paid_key_policy(&self, _: &SourceJson) -> Result<PaidKeyFields, GatewayError> {
        Err(missing_capability(AdminOperation::ApiKeysUpdate))
    }
}

#[test]
fn actual_admin_dispatch_preserves_ndjson_empty_experiment_and_source_text_decoding() {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let caps = CaptureBody {
            seen: RefCell::new(Vec::new()),
        };
        let config = AuthConfig {
            admin_tokens: vec![Secret::new("fixture-admin".into())],
            ..Default::default()
        };
        let service = AdminService {
            store: &store,
            host: &host,
            capabilities: &caps,
            config: &config,
            release: "fixture",
        };
        let request = |path: &str, body: Vec<u8>| AuthRequest {
            method: "POST".into(),
            url: format!("https://ai.ubq.fi{path}"),
            headers: BTreeMap::from([("authorization".into(), "Bearer fixture-admin".into())]),
            body,
            native_peer: None,
        };
        let ndjson = "{\"key\":[\"one\"],\"value\":1}\n{\"key\":[\"two\"],\"value\":2}\n";
        let mut raw = vec![0xef, 0xbb, 0xbf];
        raw.extend_from_slice(ndjson.as_bytes());
        assert_eq!(
            service
                .handle_route(
                    &request("/admin/kv-migration/import?profile=prod", raw),
                    "import"
                )
                .await
                .unwrap()
                .unwrap()
                .status,
            200
        );
        assert_eq!(
            caps.seen.borrow()[0],
            (
                AdminOperation::KvMigrationImport,
                Value::String(ndjson.into())
            )
        );
        for body in [
            Vec::new(),
            b" \n\t".to_vec(),
            "\u{feff}\u{2003}\n".as_bytes().to_vec(),
        ] {
            assert_eq!(
                service
                    .handle_route(
                        &request("/admin/providers/codex/cache-scope-experiment", body),
                        "experiment"
                    )
                    .await
                    .unwrap()
                    .unwrap()
                    .status,
                200
            );
        }
        let calls = caps.seen.borrow().len();
        for body in ["{}", "null", "{\"model\":\"caller-chosen\"}", "\u{0085}"] {
            assert_eq!(
                service
                    .handle_route(
                        &request(
                            "/admin/providers/codex/cache-scope-experiment",
                            body.as_bytes().to_vec()
                        ),
                        "experiment"
                    )
                    .await
                    .unwrap_err()
                    .status,
                400
            );
        }
        let mut announced = request("/admin/kv-migration/import", Vec::new());
        announced
            .headers
            .insert("content-length".into(), (5 * 1024 * 1024 + 1).to_string());
        assert_eq!(
            service
                .handle_route(&announced, "large")
                .await
                .unwrap_err()
                .status,
            413
        );
        assert_eq!(
            service
                .handle_route(
                    &request(
                        "/admin/kv-migration/import",
                        vec![b' '; 5 * 1024 * 1024 + 1]
                    ),
                    "large"
                )
                .await
                .unwrap_err()
                .status,
            413
        );
        let mut anonymous = announced;
        anonymous.headers.remove("authorization");
        assert_eq!(
            service
                .handle_route(&anonymous, "anonymous")
                .await
                .unwrap_err()
                .status,
            401
        );
        assert_eq!(caps.seen.borrow().len(), calls);
    });
}

#[test]
fn every_source_admin_endpoint_is_accounted_for() {
    let source = include_str!("../../../../src/handler/index.ts");
    let table = source
        .split("const ADMIN_ROUTES:")
        .nth(1)
        .unwrap()
        .split("\n];")
        .next()
        .unwrap();
    let mut seen = 0;
    for tail in table.split("path: \"").skip(1) {
        let path = tail.split('"').next().unwrap();
        assert!(
            ["GET", "POST", "PATCH", "DELETE"]
                .iter()
                .any(|method| match_route(method, path).is_some()),
            "missing {path}"
        );
        seen += 1;
    }
    assert!(seen >= 45);
    assert_eq!(
        match_route("GET", "/admin/api-keys/key/paid-fallbacks")
            .unwrap()
            .operation,
        AdminOperation::ApiKeyPaidFallbacks
    );
    assert_eq!(
        match_route("POST", "/admin/providers/codex/2/recheck")
            .unwrap()
            .parameter
            .as_deref(),
        Some("2")
    );
    assert!(match_route("POST", "/admin/providers/codex/../../recheck").is_none());
}
