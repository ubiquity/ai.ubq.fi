use gateway_admin_runtime::claude::{self, AccountBinding};
use gateway_auth::{
    types::AuthHost,
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_claude_runtime::host::{AcceptedObservation, ClaudePoolHost};
use gateway_claude_runtime_test_support::fixture::{Host, NOW, Store, account, run, seed};
use gateway_claude_usage_protocol::state::UsageState;
use gateway_claude_usage_runtime::{
    admin,
    collect::{self, Refresh},
    fetch::{self, Response, ResultData, UsageHost},
};
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source, state::StoredRecord,
};
use serde::Deserialize;
use serde_json::json;
use std::{
    cell::{Cell, RefCell},
    future::Future,
    task::{Context, Poll, Waker},
};
type TestResult = Result<(), Box<dyn std::error::Error>>;
struct Fixture {
    base: Host,
    calls: Cell<usize>,
    held: Cell<bool>,
    response: RefCell<(u16, JsString16, Option<String>)>,
    dates: Vec<(JsString16, Option<i64>)>,
    unreachable: Cell<bool>,
}
impl Fixture {
    fn new(store: &Store) -> Result<Self, Box<dyn std::error::Error>> {
        let oracle = SourceJson::parse(include_str!("fixtures/source-oracle.json"))?;
        Ok(Self {
            base: Host::new(store),
            calls: Cell::new(0),
            held: Cell::new(false),
            response: RefCell::new((
                200,
                r#"{"five_hour":{"utilization":42,"resets_at":null},"limits":[]}"#.into(),
                None,
            )),
            dates: source::member(&oracle, "dates")
                .ok_or("dates absent")?
                .deserialize()?,
            unreachable: Cell::new(false),
        })
    }
}
impl WebAuthnCrypto for Fixture {
    async fn validate_key(&self, k: &CosePublicKey) -> Result<(), GatewayError> {
        self.base.validate_key(k).await
    }
    async fn verify_signature(&self, i: SignatureInput) -> Result<bool, GatewayError> {
        self.base.verify_signature(i).await
    }
}
impl AuthHost for Fixture {
    fn now_ms(&self) -> i64 {
        self.base.now_ms()
    }
    fn random_bytes(&self, n: usize) -> Result<Vec<u8>, GatewayError> {
        self.base.random_bytes(n)
    }
    async fn fetch_auth(
        &self,
        r: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        self.base.fetch_auth(r).await
    }
    async fn kernel_principal(
        &self,
        a: &str,
        b: &str,
        c: &str,
        d: &str,
    ) -> Result<String, GatewayError> {
        self.base.kernel_principal(a, b, c, d).await
    }
}
impl UsageHost for Fixture {
    fn parse_usage_date(&self, s: &JsString16) -> Option<i64> {
        self.dates
            .iter()
            .find(|(key, _)| key == s)
            .and_then(|(_, v)| *v)
    }
    async fn accepted_usage(&self, event: &AcceptedObservation) -> Result<(), GatewayError> {
        self.base.accepted_observation(event).await
    }
    async fn usage_get(&self, _: &AccountBinding) -> Result<Response, gateway_claude_usage_runtime::fetch::GetFailure> {
        self.calls.set(self.calls.get() + 1);
        std::future::poll_fn(|_| {
            if self.held.get() {
                Poll::Pending
            } else {
                Poll::Ready(())
            }
        })
        .await;
        if self.unreachable.get() {
            return Err(fetch::GetFailure::Unreachable(Some(" disconnected\n diagnostic ".into())));
        }
        let (status, body, retry_after) = self.response.borrow().clone();
        Ok(Response {
            status,
            body: Ok(body),
            retry_after,
            observed_at_ms: self.now_ms(),
        })
    }
}
fn body(
    response: gateway_auth::types::AuthResponse,
) -> Result<SourceJson, Box<dyn std::error::Error>> {
    Ok(SourceJson::parse_bytes(&response.body.to_bytes()?)?)
}
fn usage(at: i64, state: &str) -> UsageState {
    UsageState {
        source: "oauth_usage".into(),
        state: state.into(),
        status: Some(200),
        message: None,
        attempted_at_ms: at,
        observed_at_ms: Some(at),
    }
}
#[test]
fn durable_lease_coalesces_concurrent_callers_and_owned_cleanup_preserves_successor() -> TestResult
{
    let store = Store::new();
    let a = account(1);
    seed(&store, vec![a.clone()]);
    let host = Fixture::new(&store)?;
    host.held.set(true);
    let mut first = Box::pin(collect::refresh(&store, &host, &a));
    let mut cx = Context::from_waker(Waker::noop());
    assert!(matches!(first.as_mut().poll(&mut cx), Poll::Pending));
    assert_eq!(host.calls.get(), 1);
    let key = collect::lease_key(&a);
    let lease = store.get(&key).ok_or("lease absent")?;
    assert_eq!(lease.expires_at_ms, Some(NOW + 20_000));
    assert_eq!(lease.json_value()?["until_ms"], NOW + 10_000);
    assert!(matches!(
        run(collect::refresh(&store, &host, &a))?,
        Refresh::Busy
    ));
    assert_eq!(host.calls.get(), 1);
    // A new durable owner supersedes the expired lease while the old GET is held.
    store.put_record(
        &key,
        StoredRecord::json(
            "successor",
            json!({"owner":"successor","until_ms":NOW+30_000}),
            Some(NOW + 40_000),
        ),
    );
    host.held.set(false);
    assert!(matches!(run(first)?, Refresh::Completed { .. }));
    assert_eq!(
        store.get(&key).ok_or("successor removed")?.revision,
        "successor"
    );
    assert!(matches!(
        run(collect::refresh(&store, &host, &a))?,
        Refresh::Fresh(_)
    ));
    assert_eq!(host.calls.get(), 1);
    Ok(())
}
#[test]
fn inference_recency_is_independent_and_usage_failure_never_mints_pool_proof() -> TestResult {
    let store = Store::new();
    let a = account(1);
    seed(&store, vec![a.clone()]);
    let host = Fixture::new(&store)?;
    let q = fetch::observation(
        &SourceJson::parse(r#"{"five_hour":{"utilization":42,"resets_at":null}}"#)?,
        NOW,
        &|_| None,
    )
    .ok_or("observation absent")?;
    run(gateway_claude_runtime::quota::record_usage(
        &store,
        &host,
        &a,
        Some(&q),
        &usage(NOW - 30_000, "ok"),
    ))?;
    let mut state = store.quota(&a).ok_or("quota absent")?;
    state.updated_at_ms = NOW as f64;
    state
        .windows
        .get_mut("5h")
        .ok_or("window absent")?
        .observed_at_ms = NOW as f64;
    store.put_record(
        &gateway_claude_runtime::persistence::quota_key(&a),
        gateway_claude_usage_quota::stored_state(&state, "inference-new".into())?,
    );
    *host.response.borrow_mut() = (
        401,
        r#"{"error":{"message":"invalid usage token"}}"#.into(),
        None,
    );
    assert!(matches!(
        run(collect::refresh(&store, &host, &a))?,
        Refresh::Completed { .. }
    ));
    assert_eq!(host.calls.get(), 1);
    let q = store.quota(&a).ok_or("quota absent")?;
    assert_eq!(
        q.usage.as_ref().ok_or("usage absent")?.state,
        "auth_invalid"
    );
    assert!(q.pool_proofs.is_none());
    assert!(q.last_error.is_none());
    assert!(q.cooldown_until_ms.is_none());
    assert!(q.blocked_until_ms.is_none());
    assert_eq!(q.windows["5h"].utilization, Some(0.42));
    assert!(q.usage_snapshot.is_some());
    assert!(host.base.health.borrow().is_empty());
    assert!(store.get(&collect::lease_key(&a)).is_none());
    Ok(())
}
#[test]
fn held_get_cannot_write_old_cohort_and_claim_cas_checks_actual_current_roster() -> TestResult {
    let store = Store::new();
    let a = account(1);
    seed(&store, vec![a.clone()]);
    let host = Fixture::new(&store)?;
    host.held.set(true);
    let mut first = Box::pin(collect::refresh(&store, &host, &a));
    let mut cx = Context::from_waker(Waker::noop());
    assert!(matches!(first.as_mut().poll(&mut cx), Poll::Pending));
    let mut changed = a.clone();
    changed.account_cohort_id = account(2).account_cohort_id;
    seed(&store, vec![changed.clone()]);
    host.held.set(false);
    assert!(matches!(run(first)?, Refresh::Changed));
    assert!(store.quota(&a).is_none());
    assert!(store.quota(&changed).is_none());
    assert!(host.base.accepted.borrow().is_empty());
    assert!(store.get(&collect::lease_key(&a)).is_none());
    assert!(matches!(
        run(collect::refresh(&store, &host, &a))?,
        Refresh::Changed
    ));
    assert_eq!(host.calls.get(), 1);
    Ok(())
}
#[test]
fn all_frozen_source_status_errors_and_utf16_diagnostics_match() -> TestResult {
    #[derive(Deserialize)]
    struct Case {
        status: u16,
        body: JsString16,
        retry: Option<String>,
        result: SourceJson,
    }
    #[derive(Deserialize)]
    struct Oracle {
        errors: Vec<Case>,
    }
    let oracle: Oracle = serde_json::from_str(include_str!("fixtures/source-oracle.json"))?;
    assert_eq!(oracle.errors.len(), 13);
    let store = Store::new();
    let a = account(1);
    seed(&store, vec![a.clone()]);
    let host = Fixture::new(&store)?;
    for c in oracle.errors {
        *host.response.borrow_mut() = (c.status, c.body, c.retry);
        let ResultData::Failure(f) = run(fetch::fetch(&host, &a))? else {
            return Err("failure expected".into());
        };
        let actual = source::canonical(&SourceJson::from_serializable(
            &json!({"ok":false,"kind":f.kind,"status":f.status,"observed_at_ms":f.observed_at_ms,"retry_after_ms":f.retry_after_ms}),
        )?)?;
        for name in ["ok", "kind", "status", "observed_at_ms", "retry_after_ms"] {
            assert_eq!(
                source::member(&actual, name),
                source::member(&c.result, name),
                "{name} status {}",
                c.status
            );
        }
        assert_eq!(
            SourceJson::from_serializable(&f.message)?,
            source::member(&c.result, "message").ok_or("message absent")?
        );
    }
    host.unreachable.set(true);
    let ResultData::Failure(f) = run(fetch::fetch(&host, &a))? else {
        return Err("failure expected".into());
    };
    assert_eq!(f.kind, "unreachable");
    assert_eq!(f.message, Some("disconnected diagnostic".into()));
    Ok(())
}
#[test]
fn admin_validation_and_all_response_branches_use_post_collection_block() -> TestResult {
    let store = Store::new();
    let a = account(1);
    seed(&store, vec![a.clone()]);
    let host = Fixture::new(&store)?;
    for raw in [
        "null",
        "{}",
        r#"{"account_id":1}"#,
        r#"{"account_id":"a","account_cohort_id":null}"#,
    ] {
        assert_eq!(
            admin::parse(&SourceJson::parse(raw)?)
                .err()
                .ok_or("must fail")?
                .status,
            400
        );
    }
    let target = admin::parse(&SourceJson::from_serializable(
        &json!({"account_id":"missing"}),
    )?)?;
    assert_eq!(
        run(admin::resolve(
            &store,
            &host,
            &claude::Bootstrap::default(),
            &target
        ))
        .err()
        .ok_or("must fail")?
        .status,
        404
    );
    let target = admin::parse(&SourceJson::from_serializable(
        &json!({"account_id":a.account_id,"account_cohort_id":"old"}),
    )?)?;
    assert_eq!(
        run(admin::resolve(
            &store,
            &host,
            &claude::Bootstrap::default(),
            &target
        ))
        .err()
        .ok_or("must fail")?
        .status,
        409
    );
    let data = SourceJson::parse(r#"{"accepted":"fresh-evidence"}"#)?;
    assert_eq!(
        admin::response(Refresh::Changed, data.clone())
            .err()
            .ok_or("changed must fail")?
            .status,
        409
    );
    for result in [Refresh::Busy, Refresh::Fresh(usage(NOW, "ok"))] {
        let b = body(admin::response(result, data.clone())?)?;
        assert_eq!(source::member(&b, "data"), Some(data.clone()));
        assert_eq!(
            source::member(&b, "refreshed")
                .ok_or("missing refreshed")?
                .as_str(),
            "false"
        );
    }
    let refreshed = run(collect::refresh(&store, &host, &a))?;
    let b = body(admin::response(refreshed, data.clone())?)?;
    assert_eq!(
        source::member(&b, "refreshed")
            .ok_or("missing refreshed")?
            .as_str(),
        "true"
    );
    assert_eq!(host.base.accepted.borrow().len(), 1);
    for (status, expected) in [(403, 200), (429, 502), (500, 502)] {
        store.now.set(store.now.get() + 30_000);
        *host.response.borrow_mut() = (status, "scope unavailable".into(), Some("2.2".into()));
        let response = admin::response(run(collect::refresh(&store, &host, &a))?, data.clone())?;
        assert_eq!(response.status, expected);
        if status == 429 {
            assert_eq!(
                response.headers.get("Retry-After").map(String::as_str),
                Some("3")
            );
        }
    }
    let mut key = a.clone();
    key.credential.kind = claude::CredentialKind::ApiKey;
    key.credential_fingerprint = claude::fingerprint(&key.credential, None);
    seed(&store, vec![key.clone()]);
    store.now.set(store.now.get() + 30_000);
    let calls = host.calls.get();
    let response = admin::response(run(collect::refresh(&store, &host, &key))?, data)?;
    assert_eq!(response.status, 200);
    assert_eq!(calls, host.calls.get());
    assert_eq!(
        store
            .quota(&key)
            .ok_or("quota absent")?
            .usage
            .ok_or("usage absent")?
            .state,
        "unsupported_credential"
    );
    Ok(())
}
#[test]
fn older_attempts_and_snapshots_do_not_replace_newer_success_and_restart_retains_state()
-> TestResult {
    let path = std::env::temp_dir().join(format!(
        "claude-usage-restart-{}.sqlite",
        std::process::id()
    ));
    let a = account(1);
    {
        let store = Store::open(&path);
        seed(&store, vec![a.clone()]);
        let host = Fixture::new(&store)?;
        assert!(matches!(
            run(collect::refresh(&store, &host, &a))?,
            Refresh::Completed { .. }
        ));
        let q = fetch::observation(
            &SourceJson::parse(r#"{"seven_day":{"utilization":1,"resets_at":null}}"#)?,
            NOW - 1,
            &|_| None,
        )
        .ok_or("observation absent")?;
        run(gateway_claude_runtime::quota::record_usage(
            &store,
            &host,
            &a,
            Some(&q),
            &usage(NOW - 1, "scope_missing"),
        ))?;
    }
    let store = Store::open(&path);
    let q = store.quota(&a).ok_or("quota absent")?;
    assert_eq!(q.usage.ok_or("usage absent")?.state, "ok");
    let snapshot = q.usage_snapshot.ok_or("snapshot absent")?;
    let (payload, at) = gateway_claude_usage_protocol::snapshot::provenance(&snapshot)
        .ok_or("provenance absent")?;
    assert_eq!(at, NOW);
    assert!(source::member(&payload, "limits").is_some());
    assert!(source::member(&payload, "seven_day").is_none());
    drop(store);
    std::fs::remove_file(path)?;
    Ok(())
}

#[test]
fn a_refresh_committing_before_claim_is_found_by_the_second_freshness_check()->TestResult{
 let store=Store::new();let a=account(1);seed(&store,vec![a.clone()]);let host=Fixture::new(&store)?;
 let s=store.clone();let binding=a.clone();
 *store.after_read.borrow_mut()=Some((gateway_claude_runtime::persistence::quota_key(&a),Box::new(move||{
  let h=Host::new(&s);let result=run(gateway_claude_runtime::quota::record_usage(&s,&h,&binding,None,&usage(NOW,"ok")));
  assert!(result.is_ok());
 })));
 assert!(matches!(run(collect::refresh(&store,&host,&a))?,Refresh::Fresh(_)));
 assert_eq!(host.calls.get(),0);assert!(store.get(&collect::lease_key(&a)).is_none());Ok(())
}
#[test]
fn roster_change_between_strong_read_and_lease_claim_is_rejected_by_the_real_cas()->TestResult{
 let store=Store::new();let a=account(1);seed(&store,vec![a.clone()]);let host=Fixture::new(&store)?;
 let s=store.clone();let id=a.account_id.clone();
 *store.after_read.borrow_mut()=Some((gateway_claude_runtime::persistence::quota_key(&a),Box::new(move||{
  let h=Host::new(&s);assert!(run(claude::remove_account(&s,&h,&claude::Bootstrap::default(),Some(&id))).is_ok());
 })));
 assert!(matches!(run(collect::refresh(&store,&host,&a))?,Refresh::Busy));assert_eq!(host.calls.get(),0);assert!(store.get(&collect::lease_key(&a)).is_none());Ok(())
}

#[test]
fn pristine_bootstrap_returns_empty_before_legacy_workspace_and_tombstone_prevents_resurrection()->TestResult {
    let store=Store::new();let host=Host::new(&store);
    let read=std::rc::Rc::new(Cell::new(false));let mark=read.clone();
    *store.after_read.borrow_mut()=Some((gateway_auth::store::key(json!(["ubq_ai","claude_code_workspace"])),Box::new(move||mark.set(true))));
    let empty=run(claude::load_roster(&store,&host,&claude::Bootstrap::default()))?;
    assert!(empty.roster.accounts.is_empty());assert!(empty.revision.is_none());assert!(!read.get());assert_eq!(store.commits.get(),0);
    seed(&store,vec![]);
    let a=account(1);
    let seed=claude::Bootstrap{credential:Some(a.credential),workspace:None};
    let empty=run(claude::load_roster(&store,&host,&seed))?;
    assert!(empty.roster.accounts.is_empty());assert!(empty.revision.is_some());assert!(!read.get());Ok(())
}
