//! Source tests/codex-overage-fallback.test.ts: enabled overage is a serving path, never a lease.
use gateway_authority::{
    capacity::{CodexCapacityAccountRef, CodexCapacitySample},
    full_pool_reset::types::FullPoolShadowDecision,
    keys,
    paid_control::PaidControlOwner,
    routing_codec::parse_routing_state,
    routing_types::{
        CapacityState, CapacityWindow, CapacityWindows, CohortProof, RouteSelection,
        UnredeemedReset,
    },
};
use gateway_core::provider::CodexQuotaClass;
use gateway_routing_runtime_test_support::authority::{
    MemoryStore, NOW, TestAuthority, admin_hash, admitted, authority, block, pool, routing_hash,
    run, seed_pool,
};
use serde_json::json;

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

fn allow(store: &MemoryStore, id: &str, allow: bool) {
    store.seed(keys::overage(&admin_hash(id).0), &json!({ "allow": allow }));
}
fn blocked_cohort(store: &MemoryStore) -> TestResult<TestAuthority> {
    seed_pool(store, &pool());
    let authority = authority(store);
    for _ in 0..2 {
        let account = admitted(&authority);
        assert!(run(authority.record_quota_block(&account, block(), NOW))?);
    }
    Ok(authority)
}
fn proof(authority: &TestAuthority, now: i64) -> TestResult<CohortProof> {
    match run(authority.select_account(&"gpt-test".into(), CodexQuotaClass::Standard, now))? {
        RouteSelection::QuotaBlocked { proof, .. } => Ok(proof),
        _ => Err("expected a quota-blocked cohort".into()),
    }
}
fn selected(reason: &str, id: &str) -> UnredeemedReset {
    UnredeemedReset {
        reason: reason.into(),
        selected_account: Some(admin_hash(id).0),
    }
}
fn admit(
    authority: &TestAuthority,
    unredeemed: &UnredeemedReset,
) -> TestResult<Option<gateway_authority::routing_types::AdmittedAccount>> {
    let proof = proof(authority, NOW)?;
    Ok(run(
        authority.admit_overage_fallback(&proof, unredeemed, NOW)
    )?)
}

#[test]
fn overage_off_is_a_hard_gate_and_enabled_overage_admits_concurrently() -> TestResult {
    let store = MemoryStore::default();
    let authority = blocked_cohort(&store)?;
    let empty = selected("no_eligible_credit", "a");
    assert!(admit(&authority, &empty)?.is_none());
    allow(&store, "a", true);
    let active_before = store.row(&keys::active()).ok_or("active")?.value;
    let routing_before = store.row(&keys::routing()).ok_or("routing")?.value;
    // Source 0231b78: two concurrent requests both serve; neither waits on a probe lease.
    let first = admit(&authority, &empty)?.ok_or("first overage admission")?;
    let second = admit(&authority, &empty)?.ok_or("second overage admission")?;
    assert_eq!(first, second);
    assert!(!first.probe_required && first.probe.is_none());
    assert_eq!(first.binding.account_id_hash, routing_hash("a"));
    let active: gateway_core::identity::ActiveSelection =
        serde_json::from_value(active_before.clone())?;
    assert_ne!(
        active.binding, first.binding,
        "overage names a blocked non-active account"
    );
    assert_eq!(
        store.row(&keys::active()).ok_or("active")?.value,
        active_before
    );
    assert_eq!(
        store.row(&keys::routing()).ok_or("routing")?.value,
        routing_before
    );
    run(authority.ensure_admission_current(&first, NOW))?;
    run(authority.ensure_admission_current(&second, NOW))?;
    // For a non-active account the per-account setting is also the dispatch fence.
    allow(&store, "a", false);
    assert!(run(authority.ensure_admission_current(&first, NOW)).is_err());
    assert!(admit(&authority, &empty)?.is_none());
    Ok(())
}

#[test]
fn only_reasons_proving_no_redemption_allow_the_overage_attempt() -> TestResult {
    let store = MemoryStore::default();
    let authority = blocked_cohort(&store)?;
    allow(&store, "a", true);
    for reason in [
        "no_eligible_credit",
        "inventory_empty",
        "inventory_no_eligible_codex_credit",
        "inventory_response_invalid_or_expired",
        "inventory_unavailable",
        "account_day_limit_reached",
        "shadow_selected",
        "already_would_spend_once",
        "unsubmitted_transaction",
    ] {
        assert!(
            admit(&authority, &selected(reason, "a"))?.is_some(),
            "{reason}"
        );
    }
    for reason in [
        "in_progress",
        "terminal_outcome_ambiguous",
        "previously_rejected",
        "no_credit",
        "configuration_unavailable",
        "routing_fence_stale",
        "shadow_decision_unavailable",
        "selected",
        "",
    ] {
        assert!(
            admit(&authority, &selected(reason, "a"))?.is_none(),
            "{reason}"
        );
    }
    Ok(())
}

#[test]
fn selected_then_active_then_first_blocked_account_and_selection_is_not_a_hint() -> TestResult {
    let store = MemoryStore::default();
    let authority = blocked_cohort(&store)?;
    allow(&store, "a", true);
    // An evaluator-selected account whose setting is off never falls through to a sibling.
    assert!(admit(&authority, &selected("no_eligible_credit", "b"))?.is_none());
    allow(&store, "b", true);
    let chosen = admit(&authority, &selected("no_eligible_credit", "b"))?.ok_or("selected")?;
    assert_eq!(chosen.binding.account_id_hash, routing_hash("b"));
    let active: gateway_core::identity::ActiveSelection =
        serde_json::from_value(store.row(&keys::active()).ok_or("active")?.value)?;
    let unselected = UnredeemedReset::declined("unsubmitted_transaction");
    let chosen = admit(&authority, &unselected)?.ok_or("active")?;
    assert_eq!(chosen.binding, active.binding);
    Ok(())
}

#[test]
fn a_stale_cohort_proof_admits_no_overage() -> TestResult {
    let store = MemoryStore::default();
    let authority = blocked_cohort(&store)?;
    allow(&store, "a", true);
    let proof = proof(&authority, NOW)?;
    let later = NOW + 60_001;
    store.clock(later);
    let unredeemed = selected("inventory_empty", "a");
    assert!(run(authority.admit_overage_fallback(&proof, &unredeemed, later))?.is_none());
    Ok(())
}

fn decision(reason: &str, selected: Option<&str>, credit: Option<&str>) -> FullPoolShadowDecision {
    FullPoolShadowDecision {
        v: 1,
        episode_hash: "episode".into(),
        created_at_ms: NOW,
        expires_at_ms: NOW + 60_000,
        decision_reason: reason.into(),
        selected_account_id_hash: selected.map(str::to_owned),
        selected_credit_id_hash: credit.map(str::to_owned),
        selected_credit_expires_at_ms: None,
        fences: Vec::new(),
    }
}

#[test]
fn shadow_outcomes_use_the_source_reason_and_selected_account() {
    let picked = decision("selected", Some("admin-a"), Some("credit-a"));
    let shadow = UnredeemedReset::shadow(&picked, false);
    assert_eq!(shadow.reason, "shadow_selected");
    assert_eq!(shadow.selected_account.as_deref(), Some("admin-a"));
    assert_eq!(
        UnredeemedReset::shadow(&picked, true).reason,
        "already_would_spend_once"
    );
    assert_eq!(
        UnredeemedReset::cached(&picked).reason,
        "already_would_spend_once"
    );
    let empty = decision("no_eligible_credit", None, None);
    assert_eq!(
        UnredeemedReset::shadow(&empty, false),
        UnredeemedReset::declined("no_eligible_credit")
    );
    assert_eq!(
        UnredeemedReset::cached(&empty),
        UnredeemedReset::declined("no_eligible_credit")
    );
    // A cached decision without a recorded credit is not a duplicate would-spend.
    let partial = decision("selected", Some("admin-a"), None);
    assert_eq!(
        UnredeemedReset::cached(&partial),
        UnredeemedReset::declined("selected")
    );
    assert!(!UnredeemedReset::cached(&partial).allows_overage());
}

fn observe_exhausted_capacity(store: &MemoryStore, authority: &TestAuthority) -> TestResult {
    store.seed(
        keys::paid_control_owner(),
        &PaidControlOwner {
            v: 1,
            owner_id: "owner-a".into(),
            owner_epoch: "epoch-a".into(),
            key_fingerprint: "opaque-owner-key".into(),
        },
    );
    let account = CodexCapacityAccountRef {
        source_slot: 1,
        account_id: "a".into(),
    };
    let fence = run(authority.begin_codex_capacity_observation(std::slice::from_ref(&account)))?;
    let sample = CodexCapacitySample {
        account,
        state: CapacityState::Available,
        source_observed_at_ms: Some(NOW),
        snapshot_at_ms: NOW,
        windows: CapacityWindows {
            primary: Some(CapacityWindow {
                limit_window_seconds: Some(300),
                used_percent: Some(100.0),
                reset_at_ms: Some(NOW + 7 * 24 * 60 * 60_000),
            }),
            secondary: None,
        },
        additional_rate_limits: vec![],
    };
    run(authority.record_codex_capacity_observations(&fence, &[sample]))?;
    Ok(())
}

#[test]
fn exhausted_capacity_with_overage_enabled_serves_without_claiming_a_probe() -> TestResult {
    for enabled in [false, true] {
        let store = MemoryStore::default();
        seed_pool(&store, &pool());
        let authority = authority(&store);
        allow(&store, "a", enabled);
        observe_exhausted_capacity(&store, &authority)?;
        let first = admitted(&authority);
        let second = admitted(&authority);
        let state = parse_routing_state(&store.row(&keys::routing()).ok_or("routing")?.value)?;
        assert!(state.slots.iter().all(|slot| slot.probe_lease.is_none()));
        assert_eq!(first, second);
        assert!(!first.probe_required && first.probe.is_none());
        // Overage off keeps the exhausted subscription out of service until its reset.
        let expected = if enabled { "a" } else { "b" };
        assert_eq!(first.binding.account_id_hash, routing_hash(expected));
    }
    Ok(())
}
