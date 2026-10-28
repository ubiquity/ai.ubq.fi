//! Fixed TEST producers only, from native-offline-fixture.rs:20-77 and
//! runtime.rs:137-248,571-695. No command accepts keys, rows, SQL or callbacks.
use gateway_admin_contracts::operations::{AdminHost, AdminInput};
use gateway_admin_contracts::routes::AdminOperation;
use gateway_auth_contracts::types::AuthHost;
use gateway_auth_primitives::store::key;
use gateway_authority::store::Authority;
use gateway_authority_api_quota::ApiQuotaOperations;
use gateway_authority_keys::{api_request, api_window};
use gateway_contracts::{identity::ApiKeyId, identity::RequestId};
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, RevisionCheck, StoredRecord,
};
use gateway_migration_fixture_port::{executor::support_run, port};
use gateway_migration_test_support::fixture::{RUNTIME_CASES, Receipt};
use gateway_source::error::GatewayError;
use gateway_source_graph::graph::GraphNode;
use serde_json::{Value, json};
use std::sync::atomic::{AtomicU64, Ordering};

struct Tokens(AtomicU64);
impl gateway_authority_contracts::TokenSource for Tokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        Ok(format!(
            "native-fixture-{}",
            self.0.fetch_add(1, Ordering::SeqCst)
        ))
    }
}
struct Clock(i64);
impl gateway_authority_contracts::AuthorityClock for Clock {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(self.0)
    }
}
struct Host(i64);
impl gateway_auth_contracts::webauthn::WebAuthnCrypto for Host {
    async fn validate_key(
        &self,
        _: &gateway_auth_contracts::webauthn::CosePublicKey,
    ) -> Result<(), GatewayError> {
        Err(port::error("not used in fixture"))
    }
    async fn verify_signature(
        &self,
        _: gateway_auth_contracts::webauthn::SignatureInput,
    ) -> Result<bool, GatewayError> {
        Err(port::error("not used in fixture"))
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        self.0
    }
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        Ok(vec![7; length])
    }
    async fn fetch_auth(
        &self,
        _: gateway_auth_contracts::upstream::AuthUpstreamRequest,
    ) -> Result<gateway_auth_contracts::upstream::AuthUpstreamResponse, GatewayError> {
        Err(port::error("no network in fixture"))
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Err(port::error("not used in fixture"))
    }
}
struct Capabilities;
impl gateway_admin_runtime_contracts::capabilities::RuntimeCapabilities for Capabilities {}
fn admin(body: Value, now_ms: i64) -> Result<AdminInput, GatewayError> {
    Ok(AdminInput {
        parameter: None,
        query: Default::default(),
        body: gateway_source::source_json::SourceJson::from_serializable(&body)?,
        actor: gateway_auth_contracts::types::AuthPrincipal {
            token: None,
            method: gateway_auth_contracts::types::AuthMethod::AdminAllowlist,
            is_admin: true,
            is_super_admin: false,
        },
        request_id: "isolated-admin".into(),
        now_ms,
    })
}
pub fn exercise(
    store: &port::Store,
    time: super::inputs::FixtureTime,
) -> Result<Receipt, GatewayError> {
    time.check()?;
    let now = time.runtime();
    let token = format!("u_{}", "1".repeat(64));
    let policy = support_run(gateway_auth_primitives::api_keys::authenticate(
        store, &token, now,
    ))?;
    if policy.key_id != "offline-key" {
        return Err(port::error("Original offline API-key fixture differs"));
    }
    let authority = Authority::with_clock(store, Tokens(AtomicU64::new(1)), Clock(now));
    let reservation = support_run(authority.reserve_api_key(
        &ApiKeyId("offline-key".into()),
        &RequestId("native-post-restart-admission".into()),
        "responses",
        now,
    ))?;
    if reservation.request_id.0 != "native-post-restart-admission" {
        return Err(port::error("Actual admission receipt differs"));
    }
    let key_id = &reservation.key_id.0;
    let version = &reservation.policy_version;
    let request_id = &reservation.request_id.0;
    let request_key = api_request(key_id, version, reservation.window_start_ms, request_id);
    let request = support_run(store.read(&request_key))?
        .ok_or_else(|| port::error("Actual admission row missing"))?;
    if request.json_value()?["state"] != "reserved" {
        return Err(port::error("Actual admission did not reserve"));
    }
    let host = Host(now);
    let runtime = gateway_admin_runtime::AdminRuntime {
        store,
        host: &host,
        capabilities: &Capabilities,
        config: &Default::default(),
    };
    let before = support_run(runtime.execute(AdminOperation::DebugGet, admin(Value::Null, now)?))?;
    if before
        .body
        .as_json()
        .is_none_or(|v| v["routing"]["scenario"] != "codex_429")
    {
        return Err(port::error("Original adopted debug fixture differs"));
    }
    support_run(runtime.execute(
        AdminOperation::DebugSet,
        admin(json!({"scenario":"normal","duration_ms":0}), now)?,
    ))?;
    let debug = key(json!(["uos_ai", "debug_routing", "v1"]));
    let updated = support_run(store.read(&debug))?
        .ok_or_else(|| port::error("Admin producer did not write"))?;
    if updated.json_value()?["scenario"] != "normal" {
        return Err(port::error("Admin producer value differs"));
    }
    if support_run(store.commit(AtomicTransition {
        checks: vec![RevisionCheck {
            key: debug.clone(),
            expected_revision: Some(updated.revision),
        }],
        mutations: vec![RecordMutation::Delete { key: debug.clone() }],
    }))? != CommitOutcome::Committed
    {
        return Err(port::error("Original latest deletion conflicted"));
    }
    if support_run(store.read(&debug))?.is_some() {
        return Err(port::error("Deleted adopted row reappeared"));
    }
    let graph_key = key(json!(["default", "native-graph", 0]));
    let original = support_run(store.read(&graph_key))?
        .ok_or_else(|| port::error("Original shared graph missing"))?;
    let mut graph = original.source_graph_value()?;
    let mut changed = false;
    for node in &mut graph.nodes {
        if let GraphNode::ArrayBuffer { bytes } = node {
            if bytes.len() <= 4 {
                return Err(port::error("Original shared buffer differs"));
            }
            bytes[4] = 99;
            changed = true;
        }
    }
    if !changed {
        return Err(port::error("Original shared buffer missing"));
    }
    let expected = StoredRecord::source_graph("latest-alias-write", graph, Some(time.deadline))?;
    if support_run(store.commit(AtomicTransition {
        checks: vec![RevisionCheck {
            key: graph_key.clone(),
            expected_revision: Some(original.revision),
        }],
        mutations: vec![RecordMutation::Put {
            key: graph_key.clone(),
            record: expected.clone(),
        }],
    }))? != CommitOutcome::Committed
    {
        return Err(port::error("Original graph producer conflicted"));
    }
    if support_run(store.read(&graph_key))? != Some(expected) {
        return Err(port::error("Actual graph write was not retained"));
    }
    // Read the actual durable product window, not a controller-injected quota row.
    let window = api_window(key_id, version, reservation.window_start_ms);
    if support_run(store.read(&window))?.is_none() {
        return Err(port::error("Actual quota window missing"));
    }
    time.check()?;
    Ok(Receipt {
        operation: "runtime_exercised",
        cases: Vec::from(RUNTIME_CASES),
        changed_keys: vec![request_key.0, window.0, graph_key.0],
        deleted_keys: vec![debug.0],
    })
}
