use gateway_admin_runtime_contracts::capabilities::Context;
use gateway_auth_contracts::{
    types::AuthHost, upstream::AuthUpstreamRequest, upstream::AuthUpstreamResponse,
    webauthn::CosePublicKey, webauthn::SignatureInput, webauthn::WebAuthnCrypto,
};
use gateway_claude_roster::{claude, claude::AccountBinding};
use gateway_claude_runtime_test_support::{fixture::NOW, fixture::account};
use gateway_claude_usage_history::{History, HistoryHost};
use gateway_claude_usage_history_records::{
    types::ClaudeAcceptedObservation, types::CredentialKind,
};
use gateway_claude_usage_runtime::{collect, fetch::Response, fetch::UsageHost};
use gateway_core::{state::AuthorityStore, state::RevisionCheck, state::StoredRecord};
use gateway_observability::ObservabilityRuntime;
use gateway_observability_contracts::{
    host_types::BuildIdentity, host_types::CapacityAccountRef, host_types::ClaudeAccountReadback,
    host_types::ClaudeRosterReadback, host_types::CodexAuthMetadata,
    host_types::CredentialMetadata, host_types::CredentialSource, host_types::ProviderInventory,
    types::HealthProvider, types::HealthTarget, types::ObservabilityHost,
};
use gateway_observability_test_support::{runtime::Host, runtime::Memory, runtime::run};
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use serde_json::json;
use std::{cell::Cell, cell::RefCell, rc::Rc, sync::Arc};
type TestResult = Result<(), Box<dyn std::error::Error>>;
#[derive(Clone)]
struct Fixture {
    base: Host,
    store: Memory,
    calls: Rc<Cell<usize>>,
    payload: Rc<RefCell<String>>,
}
impl Fixture {
    fn new() -> Self {
        let base = Host::default();
        base.at.set(NOW);
        Self{base,store:Memory::default(),calls:Rc::new(Cell::new(0)),payload:Rc::new(RefCell::new(r#"{"limits":[{"kind":"future","group":"weekly","percent":121.5,"resets_at":null,"severity":"raw","is_active":true,"scope":{"model":{"display_name":"model\ud800"}}},{"kind":"future","group":"weekly","percent":3,"resets_at":null,"severity":"raw","is_active":false,"scope":{"model":{"display_name":"model\ud800"}}}],"unknown":{"kept":"\udfff"}}"#.into()))}
    }
    fn seed(&self, accounts: Vec<AccountBinding>) -> Result<(), GatewayError> {
        let roster = claude::AccountStore {
            v: 1,
            active_account_id: accounts.first().map(|a| a.account_id.clone()),
            accounts,
            active_reason: "fixture".into(),
            updated_at_ms: NOW,
        };
        let key = claude::account_store_key();
        self.store.rows.borrow_mut().insert(
            key.0,
            claude::encode_roster_record("roster", &roster, None)?,
        );
        Ok(())
    }
}
fn missing() -> GatewayError {
    GatewayError::unavailable("fixture forbids auth/inference", "fixture_forbidden")
}
impl WebAuthnCrypto for Fixture {
    async fn validate_key(&self, _: &CosePublicKey) -> Result<(), GatewayError> {
        Err(missing())
    }
    async fn verify_signature(&self, _: SignatureInput) -> Result<bool, GatewayError> {
        Err(missing())
    }
}
impl AuthHost for Fixture {
    fn now_ms(&self) -> i64 {
        self.base.at.get()
    }
    fn random_bytes(&self, n: usize) -> Result<Vec<u8>, GatewayError> {
        self.base.random_bytes(n)
    }
    async fn fetch_auth(
        &self,
        _: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        Err(missing())
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Err(missing())
    }
}
impl HistoryHost for Fixture {
    fn now_ms(&self) -> i64 {
        self.base.at.get()
    }
    fn random_bytes(&self, n: usize) -> Result<Vec<u8>, GatewayError> {
        self.base.random_bytes(n)
    }
    fn parse_date(&self, value: &JsString16) -> Option<i64> {
        (value == &JsString16::from_utf8("1970-01-01T03:00:00Z")).then_some(10_800_000)
    }
}
impl UsageHost for Fixture {
    fn parse_usage_date(&self, value: &JsString16) -> Option<i64> {
        self.parse_date(value)
    }
    async fn usage_get(
        &self,
        _: &AccountBinding,
    ) -> Result<Response, gateway_claude_usage_runtime::fetch::GetFailure> {
        self.calls.set(self.calls.get() + 1);
        Ok(Response {
            status: 200,
            body: Ok(JsString16::from_utf8(&self.payload.borrow())),
            retry_after: None,
            observed_at_ms: self.base.at.get(),
        })
    }
    async fn accepted_usage(
        &self,
        event: &gateway_claude_usage_quota::observation::AcceptedObservation,
    ) -> Result<(), GatewayError> {
        let event = ClaudeAcceptedObservation::from_source(&event.source_value()?)?;
        History {
            store: &self.store,
            host: self,
        }
        .record_claude_capacity_observation(&event)
        .await?;
        Ok(())
    }
}
impl ObservabilityHost for Fixture {
    fn source_string_collator(
        &self,
    ) -> Arc<dyn gateway_model_selection::aliases::SourceStringCollator> {
        self.base.source_string_collator()
    }
    fn now_ms(&self) -> i64 {
        self.base.at.get()
    }
    fn random_bytes(&self, n: usize) -> Result<Vec<u8>, GatewayError> {
        self.base.random_bytes(n)
    }
    async fn wait_ms(&self, n: u32) -> Result<(), GatewayError> {
        self.base.wait_ms(n).await
    }
    fn build_identity(&self) -> Result<BuildIdentity, GatewayError> {
        Ok(BuildIdentity {
            git_sha: "b59243d674567d418d1410f8390fc7fe47a08c92".into(),
            deployment_id: "fixture".into(),
        })
    }
    async fn codex_capacity_accounts(&self) -> Result<Vec<CapacityAccountRef>, GatewayError> {
        Ok(vec![])
    }
    async fn metered_quota_snapshot(
        &self,
        _: bool,
        _: i64,
    ) -> Result<Option<serde_json::Value>, GatewayError> {
        Ok(None)
    }
    async fn collect_claude_usage(&self) -> Result<(), GatewayError> {
        let roster = claude::load_roster(&self.store, self, &claude::Bootstrap::default()).await?;
        for a in &roster.roster.accounts {
            collect::refresh(&self.store, self, a).await?;
        }
        Ok(())
    }
    async fn provider_inventory(&self, _: &Context) -> Result<ProviderInventory, GatewayError> {
        let roster = claude::load_roster(&self.store, self, &claude::Bootstrap::default()).await?;
        let mut accounts = Vec::new();
        for a in &roster.roster.accounts {
            let row = self
                .store
                .read(&gateway_claude_runtime::persistence::quota_key(a))
                .await?;
            let quota = row
                .as_ref()
                .map(|r| gateway_claude_usage_quota::parse_record(r, self.base.at.get()))
                .transpose()?
                .flatten()
                .map(|mut q| {
                    q.pool_proofs = None;
                    SourceJson::from_serializable(&q)
                })
                .transpose()?;
            accounts.push(ClaudeAccountReadback {
                target: HealthTarget {
                    provider: HealthProvider::ClaudeCode,
                    identity: format!(
                        "{}:{}:{}",
                        a.account_id, a.account_cohort_id, a.evidence_generation
                    ),
                    source_checks: vec![RevisionCheck {
                        key: claude::account_store_key(),
                        expected_revision: roster.revision.clone(),
                    }],
                },
                account_id: a.account_id.clone(),
                account_cohort_id: a.account_cohort_id.clone(),
                label: None,
                credential: CredentialMetadata {
                    kind: if a.credential.kind == claude::CredentialKind::Oauth {
                        CredentialKind::Oauth
                    } else {
                        CredentialKind::ApiKey
                    },
                    source: "kv".into(),
                    hint: "configured".into(),
                    label: None,
                    updated_at_ms: Some(NOW),
                },
                workspace: None,
                quota,
            });
        }
        Ok(ProviderInventory {
            codex: CodexAuthMetadata {
                source: CredentialSource::None,
                updated_at_ms: None,
                access_token_exp_ms: None,
                accounts: vec![],
            },
            configured: Default::default(),
            claude: ClaudeRosterReadback {
                accounts,
                active_account_id: roster.roster.active_account_id,
                active_reason: Some(roster.roster.active_reason),
                access: None,
                enabled: true,
            },
            metered_quota: None,
            quota_monitoring_configured: false,
            ubiquity_configured: false,
        })
    }
}
fn context() -> Context {
    Context {
        request_id: "usage-family-fixture".into(),
        now_ms: NOW,
    }
}
fn document(
    response: gateway_auth_contracts::types::AuthResponse,
) -> Result<SourceJson, GatewayError> {
    SourceJson::parse_bytes(&response.body.to_bytes()?)
}
fn member(v: &SourceJson, n: &str) -> Result<SourceJson, GatewayError> {
    source::member(v, n).ok_or_else(missing)
}
#[test]
fn automatic_capture_visits_unused_and_guarded_accounts_then_returns_updated_roster_history_health_and_rollups()
-> TestResult {
    let host = Fixture::new();
    let a = account(1);
    let b = account(2);
    host.seed(vec![a.clone(), b.clone()])?;
    let key = gateway_claude_runtime::persistence::quota_key(&b);
    host.store.rows.borrow_mut().insert(key.0,StoredRecord::json("guard",json!({"updated_at_ms":NOW,"windows":{},"standard":{},"last_error":{"kind":"workspace","status":403,"message":"existing guard","at_ms":NOW},"cooldown_until_ms":NOW+600_000}),None));
    let runtime = ObservabilityRuntime::new(host.store.clone(), host.clone());
    let body = document(run(runtime.capacity_response(false, context()))?)?;
    assert_eq!(host.calls.get(), 2);
    let rows = member(&body, "claude_accounts")?.array_items()?;
    assert_eq!(rows.len(), 2);
    for row in &rows {
        assert_eq!(member(&member(row, "usage")?, "state")?.as_str(), "\"ok\"");
        assert_eq!(
            member(&member(row, "usage_snapshot")?, "meters")?
                .array_items()?
                .len(),
            2
        );
    }
    assert_eq!(member(&rows[1], "state")?.as_str(), "\"unavailable\"");
    assert_eq!(
        member(&member(&body, "claude_history")?, "points")?
            .array_items()?
            .len(),
        2
    );
    let health = document(run(runtime
        .health()
        .passive_provider_response(true, context()))?)?;
    let health = member(&health, "claude_code")?;
    assert!(member(&member(&health, "quota")?, "usage_snapshot").is_ok());
    let rollup = document(run(runtime.capacity_rollups_response(90))?)?;
    assert_eq!(
        member(&member(&rollup, "claude_usage_rollups")?, "points")?
            .array_items()?
            .len(),
        4
    );
    assert!(member(&rollup, "claude_rollups")?.array_items()?.is_empty());
    let commits = host.store.commits.get();
    run(runtime.capacity_response(false, context()))?;
    assert_eq!(host.calls.get(), 2);
    assert_eq!(host.store.commits.get(), commits);
    host.base.at.set(NOW + 30_000);
    run(runtime.capacity_response(false, context()))?;
    assert_eq!(host.calls.get(), 4);
    let text = body.as_str();
    assert!(!text.contains("fixture-secret"));
    assert!(!text.contains("credential_fingerprint"));
    assert!(!text.contains("pool_proofs"));
    assert!(text.contains("\\ud800"));
    Ok(())
}
#[test]
fn snapshots_history_and_duplicate_meter_rollups_reopen_dedupe_and_preserve_transition_atomicity()
-> TestResult {
    let host = Fixture::new();
    let a = account(1);
    host.seed(vec![a.clone()])?;
    run(collect::refresh(&host.store, &host, &a))?;
    let history = History {
        store: &host.store,
        host: &host,
    };
    let before = run(history.claude_capacity_history(NOW));
    let points = member(&before, "points")?.array_items()?;
    assert_eq!(points.len(), 1);
    let rollups = run(history.usage_rollups(NOW - 3_600_000, NOW));
    let rows = member(&rollups, "points")?.array_items()?;
    assert_eq!(rows.len(), 2);
    assert_ne!(
        member(&member(&rows[0], "meter")?, "id")?,
        member(&member(&rows[1], "meter")?, "id")?
    );
    // Reopen from serialized durable records, without retaining runtime or lease state.
    let encoded = serde_json::to_vec(&*host.store.rows.borrow())?;
    let reopened = Memory::default();
    *reopened.rows.borrow_mut() = serde_json::from_slice(&encoded)?;
    let restarted = ObservabilityRuntime::new(reopened.clone(), host.base.clone());
    assert_eq!(run(restarted.claude_capacity_history(NOW)), before);
    assert_eq!(run(restarted.claude_usage_rollups(0, NOW)), rollups);
    // Two normal observations, then a changed meter anchor (including authoritative empty).
    host.base.at.set(NOW + 30_000);
    run(collect::refresh(&host.store, &host, &a))?;
    *host.payload.borrow_mut() = r#"{"limits":[]}"#.into();
    host.base.at.set(NOW + 60_000);
    run(collect::refresh(&host.store, &host, &a))?;
    let points = member(
        &run(history.claude_capacity_history(NOW + 60_000)),
        "points",
    )?
    .array_items()?;
    assert_eq!(points.len(), 3);
    assert!(
        member(&member(&points[2], "usage_snapshot")?, "meters")?
            .array_items()?
            .is_empty()
    );
    Ok(())
}

#[test]
fn stored_derived_meter_tampering_cannot_create_a_retained_usage_transition() -> TestResult {
    use gateway_claude_usage_history_records::{Point, object::SourceObject};
    use gateway_core::source_control::{
        decode_source_control_record, encode_source_control_record,
    };
    let host = Fixture::new();
    let account = account(1);
    host.seed(vec![account.clone()])?;
    run(collect::refresh(&host.store, &host, &account))?;
    host.base.at.set(NOW + 30_000);
    run(collect::refresh(&host.store, &host, &account))?;
    let history = History {
        store: &host.store,
        host: &host,
    };
    let view = run(history.claude_capacity_history(NOW + 30_000));
    let mut points = member(&view, "points")?.array_items()?;
    assert_eq!(points.len(), 2);
    let latest: Point = points[1].deserialize()?;
    let key = latest.history_key();
    let row = run(host.store.read(&key))?.ok_or("history row absent")?;
    let mut snapshot = SourceObject::parse(&member(&points[1], "usage_snapshot")?)?;
    snapshot.set("meters", &Vec::<SourceJson>::new())?;
    let mut point = SourceObject::parse(&points[1])?;
    point.set("usage_snapshot", &snapshot.source_json()?)?;
    points[1] = point.source_json()?;
    let mut bucket = SourceObject::parse(&decode_source_control_record(&row)?)?;
    bucket.set("points", &points)?;
    host.store.rows.borrow_mut().insert(
        key.0,
        encode_source_control_record(
            "derived-tampered",
            &bucket.source_json()?,
            row.expires_at_ms,
        )?,
    );
    host.base.at.set(NOW + 60_000);
    run(collect::refresh(&host.store, &host, &account))?;
    let points = member(
        &run(history.claude_capacity_history(NOW + 60_000)),
        "points",
    )?
    .array_items()?;
    assert_eq!(points.len(), 2);
    assert_eq!(
        member(&member(&points[1], "usage_snapshot")?, "meters")?
            .array_items()?
            .len(),
        2
    );
    let rollups = run(history.usage_rollups(0, NOW + 60_000));
    for point in member(&rollups, "points")?.array_items()? {
        assert_eq!(member(&point, "samples")?.as_str(), "2");
    }
    Ok(())
}
#[test]
fn corrupt_usage_rollup_refuses_entire_bucket_transaction_and_read_scan_reports_unavailable()
-> TestResult {
    let host = Fixture::new();
    let a = account(1);
    host.seed(vec![a.clone()])?;
    run(collect::refresh(&host.store, &host, &a))?;
    let key=host.store.rows.borrow().keys().find(|k|gateway_kv_codec::tuple_key::TupleKey::from_record_key(&gateway_kv_codec::key::RecordKey((*k).clone())).ok().and_then(|k|k.decoded_parts().ok()).is_some_and(|p|p.iter().any(|v|matches!(v,gateway_kv_codec::tuple_key::DecodedKeyPart::String(bytes) if bytes==b"usage_rollup")))).cloned().ok_or("usage key absent")?;
    host.store
        .rows
        .borrow_mut()
        .insert(key, StoredRecord::json("corrupt", json!({"v":999}), None));
    let before = host.store.rows.borrow().clone();
    host.base.at.set(NOW + 1);
    let snapshot = gateway_claude_usage_protocol::snapshot::parse(
        &SourceJson::parse(&host.payload.borrow())?,
        NOW + 1,
        &|_| None,
    )
    .ok_or("snapshot absent")?;
    let event = ClaudeAcceptedObservation {
        v: 1,
        observation_id: "new".into(),
        account_id: a.account_id,
        account_cohort_id: a.account_cohort_id,
        credential_kind: CredentialKind::Oauth,
        workspace_id: None,
        source_observed_at_ms: Some(NOW + 1),
        sampled_at_ms: NOW + 1,
        response_status: Some(200),
        state: gateway_claude_usage_history_records::types::ClaudeObservationState::Available,
        observation: source::object(vec![
            ("windows", SourceJson::parse("{}")?),
            ("standard", SourceJson::parse("{}")?),
            ("representative_claim", SourceJson::parse("null")?),
            ("observed_at_ms", SourceJson::from_serializable(&(NOW + 1))?),
            (
                "usage_snapshot",
                gateway_claude_usage_protocol::snapshot::serialize(&snapshot)?,
            ),
        ])?,
    };
    let history = History {
        store: &host.store,
        host: &host,
    };
    assert!(run(history.record_claude_capacity_observation(&event)).is_err());
    assert_eq!(*host.store.rows.borrow(), before);
    let read = run(history.usage_rollups(0, NOW + 1));
    assert_eq!(member(&read, "scan")?.as_str(), "\"unavailable\"");
    assert!(member(&read, "points")?.array_items()?.is_empty());
    Ok(())
}

#[test]
fn frozen_b592_usage_history_atomic_rows_retention_keys_and_rollup_readbacks_match() -> TestResult {
    #[derive(serde::Deserialize)]
    struct Row {
        key: serde_json::Value,
        value: SourceJson,
        expires_at_ms: Option<i64>,
    }
    #[derive(serde::Deserialize)]
    struct Case {
        event: SourceJson,
        rows: Vec<Row>,
        history: SourceJson,
        usage_rollups: SourceJson,
        rollups: SourceJson,
    }
    #[derive(serde::Deserialize)]
    struct Oracle {
        source_sha: String,
        cases: Vec<Case>,
    }
    let oracle: Oracle =
        serde_json::from_str(include_str!("fixtures/claude-usage-history-oracle.json"))?;
    assert_eq!(
        oracle.source_sha,
        "b59243d674567d418d1410f8390fc7fe47a08c92"
    );
    assert_eq!(oracle.cases.len(), 6);
    let host = Fixture::new();
    let history = History {
        store: &host.store,
        host: &host,
    };
    for case in oracle.cases {
        let event = ClaudeAcceptedObservation::from_source(&case.event)?;
        host.base.at.set(event.sampled_at_ms);
        run(history.record_claude_capacity_observation(&event))?;
        assert_eq!(host.store.rows.borrow().len(), case.rows.len());
        for row in case.rows {
            let key = gateway_kv_codec::tuple_key::TupleKey::from_json_parts(
                row.key.as_array().ok_or("tuple absent")?,
            )?
            .to_record_key();
            let actual = host.store.read(&key);
            let actual = run(actual)?.ok_or("source row missing")?;
            assert_eq!(actual.expires_at_ms, row.expires_at_ms);
            let actual = gateway_core::source_control::decode_source_control_record(&actual)?;
            assert_eq!(
                source::canonical(&actual)?,
                source::canonical(&row.value)?,
                "source stored row {:?}",
                row.key
            );
        }
        // Compare the complete source documents, including full raw payload and UTF16 labels.
        let actual = run(history.claude_capacity_history(event.sampled_at_ms));
        for name in [
            "v",
            "bucket_ms",
            "retention_ms",
            "stale_after_ms",
            "scan",
            "truncated",
            "points",
        ] {
            assert_eq!(
                source::canonical(&member(&actual, name)?)?,
                source::canonical(&member(&case.history, name)?)?,
                "history {name}"
            );
        }
        assert_eq!(
            run(history.usage_rollups(0, event.sampled_at_ms)),
            source::canonical(&case.usage_rollups)?
        );
        assert_eq!(
            run(history.claude_capacity_rollups(0, event.sampled_at_ms)),
            source::canonical(&case.rollups)?
        );
    }
    Ok(())
}

struct AdminComposition<'a>(&'a Fixture);
impl gateway_admin_runtime_contracts::capabilities::RuntimeCapabilities for AdminComposition<'_> {
    async fn claude(
        &self,
        request: gateway_admin_runtime_contracts::capabilities::ClaudeRequest,
        context: Context,
    ) -> Result<gateway_auth_contracts::types::AuthResponse, GatewayError> {
        let gateway_admin_runtime_contracts::capabilities::ClaudeRequest::RefreshUsage { document } =
            request
        else {
            return Err(missing());
        };
        let result = gateway_claude_usage_runtime::admin::refresh(
            &self.0.store,
            self.0,
            &claude::Bootstrap::default(),
            &document,
        )
        .await?;
        let runtime = ObservabilityRuntime::new(self.0.store.clone(), self.0.clone());
        let block = runtime
            .health()
            .passive_provider_response(true, context)
            .await?;
        let data = member(
            &SourceJson::parse_bytes(&block.body.to_bytes()?)?,
            "claude_code",
        )?;
        gateway_claude_usage_runtime::admin::response(result, data)
    }
}
#[test]
fn authenticated_admin_service_dispatch_executes_real_account_fenced_collector_and_updated_block()
-> TestResult {
    use gateway_auth_contracts::{types::AuthConfig, types::AuthRequest};
    use gateway_auth_primitives::crypto::Secret;
    let host = Fixture::new();
    let a = account(1);
    host.seed(vec![a.clone()])?;
    let capabilities = AdminComposition(&host);
    let runtime = gateway_admin_runtime::AdminRuntime {
        store: &host.store,
        host: &host,
        capabilities: &capabilities,
        config: &gateway_admin_runtime_contracts::RuntimeConfig::default(),
    };
    let config = AuthConfig {
        admin_tokens: vec![Secret::new("public-fixture-admin".into())],
        ..Default::default()
    };
    let service = gateway_admin::service::AdminService {
        store: &host.store,
        host: &host,
        capabilities: &runtime,
        config: &config,
        release: "public-source-fixture",
    };
    let mut request = AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/admin/providers/claude-code/quota/refresh".into(),
        body: b"bad json".to_vec(),
        ..Default::default()
    };
    let error = run(service.handle_route(&request, "denied"))
        .err()
        .ok_or("must authenticate before body")?;
    assert_eq!(error.status, 401);
    assert_eq!(host.calls.get(), 0);
    assert_eq!(host.store.commits.get(), 0);
    request
        .headers
        .insert("authorization".into(), "Bearer public-fixture-admin".into());
    assert_eq!(
        run(service.handle_route(&request, "body"))
            .err()
            .ok_or("invalid body accepted")?
            .status,
        400
    );
    for (body, status) in [
        (json!({}), 400),
        (json!({"account_id":"missing"}), 404),
        (
            json!({"account_id":a.account_id,"account_cohort_id":"old"}),
            409,
        ),
    ] {
        request.body = serde_json::to_vec(&body)?;
        assert_eq!(
            run(service.handle_route(&request, "invalid"))
                .err()
                .ok_or("invalid target accepted")?
                .status,
            status
        );
    }
    assert_eq!(host.calls.get(), 0);
    request.body = serde_json::to_vec(
        &json!({"account_id":a.account_id,"account_cohort_id":a.account_cohort_id}),
    )?;
    let response = run(service.handle_route(&request, "accepted"))?.ok_or("route absent")?;
    assert_eq!(response.status, 200);
    let response = document(response)?;
    assert_eq!(member(&response, "refreshed")?.as_str(), "true");
    let quota = member(&member(&response, "data")?, "quota")?;
    assert_eq!(
        member(&member(&quota, "usage")?, "state")?.as_str(),
        "\"ok\""
    );
    assert_eq!(
        member(&member(&quota, "usage_snapshot")?, "meters")?
            .array_items()?
            .len(),
        2
    );
    let fresh = document(run(service.handle_route(&request, "fresh"))?.ok_or("route absent")?)?;
    assert_eq!(member(&fresh, "reason")?.as_str(), "\"fresh\"");
    assert_eq!(host.calls.get(), 1);
    Ok(())
}
