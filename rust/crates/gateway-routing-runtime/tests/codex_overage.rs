//! Source tests/codex-overage-fallback.test.ts and tests/codex-retry-after.test.ts at the
//! routing boundary: one overage serving attempt per request, never a shared probe lease.
use gateway_authority::{
    keys,
    routing_types::{QuotaBlock, QuotaBlockSource, RouteSelection, UnredeemedReset},
    store::Authority,
};
use gateway_core::provider::CodexQuotaClass;
use gateway_routing_runtime::{
    classify::{CODEX_CLIENT_RETRY_AFTER_MAX_SECONDS, codex_retry_after_seconds},
    session::RoutingSession,
    types::{CodexRecovery, CredentialFence, RoutingError},
};
use gateway_routing_runtime_test_support::{exec::*, host::*, input::*, seed::*, store::*};
use serde_json::json;

type Session = RoutingSession<Store, Store, Store, Host>;
type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
const WEEK_MS: i64 = 7 * 24 * 60 * 60_000;

fn blocked_cohort(store: &Store) -> TestResult {
    seed_key(store);
    seed_pool(store);
    let authority = Authority::with_clock(store.clone(), store.clone(), store.clone());
    for _ in 0..2 {
        let selection =
            run(authority.select_account(&"gpt-test".into(), CodexQuotaClass::Standard, NOW))?;
        let RouteSelection::Eligible(account) = selection else {
            return Err("expected an eligible subscription".into());
        };
        let block = QuotaBlock {
            blocked_until_ms: NOW + WEEK_MS,
            source: QuotaBlockSource::BodyResetsAt,
            observed_at_ms: Some(NOW),
            legacy_fallback: false,
            reset_at_ms: Some(NOW + WEEK_MS),
            reset_is_stable: true,
            reset_ambiguous: false,
            recovery_pending: false,
        };
        assert!(run(authority.record_quota_block(&account, block, NOW))?);
    }
    Ok(())
}
fn allow_overage(store: &Store, id: &str) {
    let hash = source_account(id).identity().administrative_account_hash;
    store.seed(keys::overage(&hash.0), json!({ "allow": true }));
}
fn session(store: &Store, id: &str, recovery: Option<CodexRecovery>) -> TestResult<Session> {
    let host = Host::new(store);
    *host.codex_recovery.lock().map_err(|_| "poisoned")? = recovery;
    let input = input(id, "gpt-test", &[]);
    Ok(run(Session::begin(
        input,
        store.clone(),
        host,
        store.clone(),
        store.clone(),
    ))?)
}
fn unredeemed(reason: &str) -> Option<CodexRecovery> {
    Some(CodexRecovery::Unredeemed(UnredeemedReset::declined(reason)))
}
fn assert_quota_refusal(error: &RoutingError) {
    assert_eq!(error.error.status, 429);
    assert_eq!(
        error.error.envelope.error.code.as_deref(),
        Some("codex_quota_blocked")
    );
    assert_eq!(error.error.envelope.error.error_type, "rate_limit_error");
    assert_eq!(
        error.headers.get("Retry-After").map(String::as_str),
        Some("60")
    );
    assert!(error.source_body.is_some());
}

#[test]
fn a_weekly_quota_deadline_is_capped_to_a_client_sized_retry() {
    let now = 1_000_000;
    assert_eq!(
        codex_retry_after_seconds(now + WEEK_MS, now),
        CODEX_CLIENT_RETRY_AFTER_MAX_SECONDS
    );
    assert_eq!(codex_retry_after_seconds(now + 45_000, now), 45);
    assert_eq!(codex_retry_after_seconds(now + 44_001, now), 45);
    assert_eq!(codex_retry_after_seconds(now - 10_000, now), 1);
    assert_eq!(codex_retry_after_seconds(now + 1, now), 1);
    assert_eq!(codex_retry_after_seconds(i64::MAX, i64::MIN), 60);
}

#[test]
fn the_default_off_setting_refuses_overage_with_a_bounded_quota_refusal() -> TestResult {
    let store = Store::new();
    blocked_cohort(&store)?;
    for (id, recovery) in [
        ("declined", None),
        ("no-credit", unredeemed("no_eligible_credit")),
        ("shadow", unredeemed("shadow_selected")),
    ] {
        let error = run(session(&store, id, recovery)?.next_attempt())
            .err()
            .ok_or("an overage-off cohort must not serve")?;
        assert_quota_refusal(&error);
    }
    Ok(())
}

#[test]
fn a_completed_or_ambiguous_reset_outcome_never_falls_back_to_overage() -> TestResult {
    let store = Store::new();
    blocked_cohort(&store)?;
    allow_overage(&store, "a");
    allow_overage(&store, "b");
    for reason in [
        "in_progress",
        "terminal_outcome_ambiguous",
        "routing_fence_stale",
    ] {
        let error = run(session(&store, reason, unredeemed(reason))?.next_attempt())
            .err()
            .ok_or("an unproven reason must not spend overage")?;
        assert_quota_refusal(&error);
    }
    Ok(())
}

#[test]
fn concurrent_enabled_overage_requests_do_not_contend_for_one_probe_lease() -> TestResult {
    let store = Store::new();
    blocked_cohort(&store)?;
    allow_overage(&store, "a");
    allow_overage(&store, "b");
    let first = session(&store, "overage-one", unredeemed("unsubmitted_transaction"))?;
    let second = session(
        &store,
        "overage-two",
        unredeemed("already_would_spend_once"),
    )?;
    let one = run(first.next_attempt())?.ok_or("first overage attempt")?;
    let two = run(second.next_attempt())?.ok_or("second overage attempt")?;
    let mut bindings = Vec::new();
    for attempt in [&one, &two] {
        let CredentialFence::Codex { admission, .. } = &attempt.credential else {
            return Err("overage serves through the blocked subscription".into());
        };
        assert!(!admission.probe_required && admission.probe.is_none());
        bindings.push(admission.binding.clone());
    }
    assert_eq!(bindings[0], bindings[1]);
    run(first.before_dispatch(&one))?;
    run(second.before_dispatch(&two))?;
    Ok(())
}
