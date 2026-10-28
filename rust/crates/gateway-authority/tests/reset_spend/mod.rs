//! Source banked-reset claim/submission and native-codex-mutations probe boundaries.

use gateway_authority_keys as keys;
use gateway_authority_reset_claim::ResetClaimOperations;
use gateway_authority_reset_permit::{ResetPermitOperations, types::FullPoolPreparation};
use gateway_authority_reset_permit::{types::AutomaticResetPermit, types::FullPoolEvaluation};
use gateway_authority_reset_records::{AccountDayBudget, ResetClaim, full_pool::SourceResetCredit};
use gateway_authority_reset_records::{ResetConfig, ResetMode, full_pool::SourceResetInventory};
use gateway_authority_reset_records::{ResetRecord, ResetState, reset_invocation};
use gateway_authority_reset_records::{full_pool::SourceCreditCollation, utc_day};
use gateway_authority_reset_recovery::ResetRecoveryOperations;
use gateway_authority_reset_spend::ResetSpendOperations;
use gateway_authority_routing_selection::types::CohortProof;
use gateway_authority_routing_selection::{SelectionOperations, types::RouteSelection};
use gateway_authority_routing_signal::SignalOperations;
use gateway_authority_test_support::{TestResult, fixture_json, fixture_row};
use gateway_contracts::identity::PrincipalId;
use gateway_contracts::{identity::RequestContext, identity::RequestId, provider::CodexQuotaClass};
use gateway_kv_codec::key::RecordKey;
use gateway_routing_runtime_test_support::{authority::MemoryStore, authority::NOW};
use gateway_routing_runtime_test_support::{authority::TestAuthority, authority::admitted};
use gateway_routing_runtime_test_support::{authority::authority, authority::block};
use gateway_routing_runtime_test_support::{authority::pool, authority::run, authority::seed_pool};
use gateway_source::{error::GatewayError, js_string::JsString16};
use serde_json::json;
use std::cmp::Ordering;

struct NoCreditTie;
impl SourceCreditCollation for NoCreditTie {
    fn compare_credit_ids(&self, _: &JsString16, _: &JsString16) -> Result<Ordering, GatewayError> {
        panic!("one credit on one account has no collation tie")
    }
}
fn config() -> ResetConfig {
    ResetConfig {
        enabled: true,
        mode: ResetMode::Live,
        max_per_account_per_day: 1,
        max_per_account_per_window: 1,
        provider_approved: true,
        supports_idempotent_lookup: false,
        redeem_outcome_is_final: true,
    }
}
fn proof(authority: &TestAuthority, at: i64) -> CohortProof {
    let RouteSelection::QuotaBlocked { proof, .. } =
        run(authority.select_account(&"gpt-test".into(), CodexQuotaClass::Standard, at)).unwrap()
    else {
        panic!("requires actual exhausted source cohort")
    };
    assert!(proof.full_cohort_exhausted());
    proof
}
fn setup() -> (MemoryStore, TestAuthority) {
    let store = MemoryStore::default();
    seed_pool(&store, &pool());
    let authority = authority(&store);
    for _ in 0..2 {
        let account = admitted(&authority);
        run(authority.record_quota_block(&account, block(), NOW)).unwrap();
    }
    store.seed(keys::reset_config(), &config());
    store.seed(
        gateway_auth_primitives::store::key(json!([
            "uos_ai",
            "migrations",
            "codex_reset_usage_legacy_global_v1"
        ])),
        &json!({"completed_at_ms":NOW,"preserved_opt_out":false}),
    );
    (store, authority)
}
fn original() -> RequestContext {
    RequestContext {
        request_id: RequestId("actual-original-reset-request".into()),
        principal: PrincipalId::from_scalar("fixture-principal".into()),
        key_id: None,
        route: "/v1/responses".into(),
        started_at_ms: NOW,
    }
}
fn ready(authority: &TestAuthority, at: i64) -> (AutomaticResetPermit, String) {
    let proof = proof(authority, at);
    let original = original();
    let FullPoolPreparation::Observe(plan) =
        run(authority.prepare_full_pool_reset(&proof, &original)).unwrap()
    else {
        panic!("requires actual source observations")
    };
    let observations: Vec<_> = plan
        .bindings()
        .enumerate()
        .map(|(slot, binding)| {
            let credits: Vec<_> = if slot == 0 {
                vec![SourceResetCredit {
                    credit_id: JsString16::from_utf8("source-credit"),
                    status: JsString16::from_utf8("available"),
                    reset_type: JsString16::from_utf8("codex_rate_limits"),
                    expires_at_ms: None,
                }]
            } else {
                vec![]
            };
            SourceResetInventory {
                binding: binding.clone(),
                observed_at_ms: at,
                available_count: credits.len() as i64,
                credits,
            }
        })
        .collect();
    match run(authority.finish_full_pool_reset(*plan, &observations, &NoCreditTie)).unwrap() {
        FullPoolEvaluation::Ready(permit) => {
            let episode = permit.episode_hash().to_string();
            (*permit, episode)
        }
        other => panic!("expected one immediate live permit, got {other:?}"),
    }
}

fn claim(authority: &TestAuthority, permit: &AutomaticResetPermit) -> ResetRecord {
    let ResetClaim::Submit { record, .. } = run(authority.claim_full_pool_reset(permit)).unwrap()
    else {
        panic!("expected one source claim")
    };
    record
}
fn budget_key(permit: &AutomaticResetPermit) -> gateway_kv_codec::key::RecordKey {
    keys::reset_day(
        &permit.invocation().unwrap().identity.account_id_hash,
        &utc_day(NOW).unwrap(),
    )
}

#[test]
fn preexisting_reset_scan_never_claims_an_absent_ledger_or_budget() -> TestResult {
    let (store, authority) = setup();
    let cohort = proof(&authority, NOW);
    assert!(matches!(
        run(authority.reconcile_full_pool_reset(&cohort, &original()))?,
        gateway_authority_reset_evidence::types::FullPoolResetReconciliation::NoExisting
    ));
    for account in pool().accounts {
        let invocation = gateway_authority_reset_records::reset_invocation(
            &account.admin_account_hash.0,
            &account.credential_version.0,
            NOW + 60_000,
        )?;
        assert!(
            store
                .row(&keys::reset_redemption(
                    &invocation.identity.account_id_hash,
                    &invocation.identity.quota_generation
                ))
                .is_none()
        );
        assert!(
            store
                .row(&keys::reset_day(
                    &invocation.identity.account_id_hash,
                    &utc_day(NOW)?
                ))
                .is_none()
        );
    }
    Ok(())
}

#[test]
fn full_pool_respects_the_existing_shared_account_day_budget() {
    let (store, authority) = setup();
    let (permit, _) = ready(&authority, NOW);
    let claimed = claim(&authority, &permit);
    store.seed(
        budget_key(&permit),
        &AccountDayBudget {
            v: 1,
            day: utc_day(NOW).unwrap(),
            submission_count: 1,
            updated_at_ms: NOW,
        },
    );
    assert!(run(authority.prepare_full_pool_reset_submission(&permit, &claimed)).is_err());
    let budget: AccountDayBudget =
        serde_json::from_value(store.row(&budget_key(&permit)).unwrap().value).unwrap();
    assert_eq!(budget.submission_count, 1);
}

#[test]
fn preexisting_verified_reset_needs_no_new_credit_inventory() -> TestResult {
    for stale in [false, true] {
        let (store, authority) = setup();
        let (permit, _) = ready(&authority, NOW);
        let claimed = claim(&authority, &permit);
        let submitted = run(authority.prepare_full_pool_reset_submission(&permit, &claimed))?;
        run(authority.record_reset_outcome(
            &submitted,
            ResetState::Verified,
            Some("documented_reset"),
            None,
            NOW,
        ))?;
        if stale {
            let mut row = fixture_row(&store, &keys::routing())?.value;
            let slot = &mut row["slots"][permit.binding().slot as usize];
            slot["generation"] = json!(permit.binding().routing_generation + 2);
            slot["banked_reset_generation_ambiguous"] = json!(true);
            slot["quota_blocks_by_class"]["standard"]["banked_reset_generation_ambiguous"] =
                json!(true);
            store.seed(keys::routing(), &row);
        }
        // Deliberately no prepare/finish/inventory call after the physical outcome.
        let cohort = proof(&authority, NOW);
        let gateway_authority_reset_evidence::types::FullPoolResetReconciliation::Recovered(
            account,
        ) = run(authority.reconcile_full_pool_reset(&cohort, &original()))?
        else {
            panic!("known verified ledger must grant only a recovery probe")
        };
        assert!(!account.probe_required);
        assert!(account.probe.is_some());
        run(authority.ensure_admission_current(&account, NOW))?;
    }
    Ok(())
}

#[test]
fn full_pool_spend_uses_one_existing_budget_and_unknown_never_resubmits() -> TestResult {
    let (store, authority) = setup();
    let (permit, _) = ready(&authority, NOW);
    let claimed = claim(&authority, &permit);
    let submitted = run(authority.prepare_full_pool_reset_submission(&permit, &claimed))?;
    run(authority.renew_full_pool_reset_before_redeem(&permit, &submitted))?;
    let unknown = run(authority.record_reset_outcome(
        &submitted,
        ResetState::Unknown,
        None,
        Some("transport_lost"),
        NOW,
    ))?;
    assert!(matches!(
        run(authority.reconcile_full_pool_reset(&proof(&authority, NOW), &original()))?,
        gateway_authority_reset_evidence::types::FullPoolResetReconciliation::Blocked { .. }
    ));
    assert!(run(authority.prepare_full_pool_reset_submission(&permit, &unknown)).is_err());
    assert!(run(authority.renew_full_pool_reset_before_redeem(&permit, &unknown)).is_err());
    store.clock(NOW + 30_001);
    let (current, _) = ready(&authority, NOW + 30_001);
    let ResetClaim::Reconcile { record, .. } = run(authority.claim_full_pool_reset(&current))?
    else {
        panic!("durable unknown permits reconciliation only")
    };
    assert_eq!(record.state, ResetState::Unknown);
    assert!(run(authority.renew_full_pool_reset_before_redeem(&current, &record)).is_err());
    let budget: AccountDayBudget = fixture_json(&store, &budget_key(&permit))?;
    assert_eq!(budget.submission_count, 1);
    Ok(())
}

#[test]
fn full_pool_settings_and_policy_races_cannot_spend_the_day_budget() {
    for changed in ["settings", "policy"] {
        let (store, authority) = setup();
        let (permit, _) = ready(&authority, NOW);
        let claimed = claim(&authority, &permit);
        match changed {
            "settings" => store.race(
                keys::reset_usage(&pool().accounts[1].admin_account_hash.0),
                &json!({"enabled":false}),
            ),
            _ => {
                let mut disabled = config();
                disabled.enabled = false;
                store.race(keys::reset_config(), &disabled);
            }
        }
        assert!(
            run(authority.prepare_full_pool_reset_submission(&permit, &claimed)).is_err(),
            "{changed}"
        );
        assert!(store.row(&budget_key(&permit)).is_none(), "{changed}");
    }
}

#[test]
fn interleaved_live_prepares_keep_first_permit_submit_capable_with_one_ledger() {
    let (store, authority) = setup();
    let (first, first_episode) = ready(&authority, NOW);
    let (second, _) = ready(&authority, NOW);
    let claimed = claim(&authority, &first);
    let ResetClaim::InProgress(interleaved) =
        run(authority.claim_full_pool_reset(&second)).unwrap()
    else {
        panic!("the existing claim must win without minting a second submission")
    };
    assert_eq!(interleaved.identity, claimed.identity);

    // A later prepare after the claim must not change any shadow revision or invalidate `first`.
    let _ = ready(&authority, NOW);
    let submitted = run(authority.prepare_full_pool_reset_submission(&first, &claimed)).unwrap();
    assert_eq!(submitted.state, ResetState::Submitted);
    let (after_submit_permit, _) = ready(&authority, NOW);
    let renewed = run(authority.renew_full_pool_reset_before_redeem(&first, &submitted)).unwrap();
    assert_eq!(renewed.state, ResetState::Submitted);
    assert!(store.row(&keys::reset_shadow(&first_episode)).is_none());
    let ResetClaim::InProgress(after_submit_record) =
        run(authority.claim_full_pool_reset(&after_submit_permit)).unwrap()
    else {
        panic!("the durable ledger must refuse a second live claim")
    };
    assert_eq!(after_submit_record.identity, claimed.identity);
    let budget: AccountDayBudget =
        serde_json::from_value(store.row(&budget_key(&first)).unwrap().value).unwrap();
    assert_eq!(budget.submission_count, 1);
}

#[test]
fn full_pool_claim_rechecks_inventory_deadline_after_awaited_reads() {
    let (store, authority) = setup();
    let (permit, _) = ready(&authority, NOW);
    let identity = permit.invocation().unwrap().identity;
    let key = keys::reset_redemption(&identity.account_id_hash, &identity.quota_generation);
    store.jump_on_read(key.clone(), NOW + 30_001);
    assert!(run(authority.claim_full_pool_reset(&permit)).is_err());
    assert!(store.row(&key).is_none());
    assert!(store.row(&budget_key(&permit)).is_none());
}

#[test]
fn full_pool_verified_reset_returns_exact_halfopen_lease_not_recovered_capacity() {
    let (store, authority) = setup();
    let (permit, _) = ready(&authority, NOW);
    store.clock(NOW + 25_000);
    let claimed = claim(&authority, &permit);
    let submitted = run(authority.prepare_full_pool_reset_submission(&permit, &claimed)).unwrap();
    store.clock(NOW + 30_001);
    let verified = run(authority.record_reset_outcome(
        &submitted,
        ResetState::Verified,
        Some("documented_reset"),
        None,
        NOW + 30_001,
    ))
    .unwrap();
    let account = run(authority.elect_full_pool_reset(&permit, &verified))
        .unwrap()
        .unwrap();
    assert!(!account.probe_required);
    let lease = account.probe.as_ref().unwrap();
    assert_eq!(
        account.binding.routing_generation,
        permit.binding().routing_generation + 1
    );
    assert_eq!(lease.generation as u64, account.binding.routing_generation);
    assert_eq!(lease.quota_class, Some(CodexQuotaClass::Standard));
    let row = store.row(&keys::routing()).unwrap();
    let slot = &row.value["slots"][account.binding.slot as usize];
    assert_eq!(slot["banked_reset_generation_ambiguous"], true);
    assert_eq!(slot["banked_reset_recovery_probe_pending"], true);
    assert_eq!(slot["observed_reset_at_ms"], NOW + 60_000);
    run(authority.ensure_admission_current(&account, NOW)).unwrap();
    let mut raced = row.value;
    raced["slots"][account.binding.slot as usize]["probe_lease"]["token"] = json!("foreign-lease");
    store.seed(keys::routing(), &raced);
    assert!(run(authority.ensure_admission_current(&account, NOW)).is_err());
    assert!(!run(authority.finish_probe(&account, true, NOW)).unwrap());
}

#[test]
fn expired_probe_claim_preserves_source_generation_and_checks_current_expiry() {
    let store = MemoryStore::default();
    let mut single = pool();
    single.accounts.truncate(1);
    seed_pool(&store, &single);
    let authority = authority(&store);
    let account = admitted(&authority);
    let mut expired = block();
    expired.blocked_until_ms = NOW + 1;
    expired.reset_at_ms = Some(NOW + 1);
    run(authority.record_quota_block(&account, expired, NOW)).unwrap();
    store.clock(NOW + 2);
    let RouteSelection::Eligible(selected) =
        run(authority.select_account(&"gpt-test".into(), CodexQuotaClass::Standard, NOW + 2))
            .unwrap()
    else {
        panic!("expired source quota circuit must be probe eligible")
    };
    let claimed = run(authority.claim_probe(&selected, None, NOW + 2))
        .unwrap()
        .unwrap();
    assert_eq!(claimed.binding, selected.binding);
    assert_eq!(
        claimed.probe.as_ref().unwrap().generation as u64,
        selected.binding.routing_generation
    );
    run(authority.ensure_admission_current(&claimed, NOW + 2)).unwrap();
    store.clock(NOW + 30_002);
    assert!(run(authority.ensure_admission_current(&claimed, NOW + 30_002)).is_err());
}

#[test]
fn pinned_source_outcome_does_not_read_or_rewrite_unrelated_active_election() {
    let store = MemoryStore::default();
    seed_pool(&store, &pool());
    let authority = authority(&store);
    let account = admitted(&authority);
    store.seed(
        keys::active(),
        &json!({"v":1,"generation":"unrelated_corruption"}),
    );
    assert!(run(authority.record_quota_block(&account, block(), NOW)).unwrap());
    assert_eq!(
        store.row(&keys::active()).unwrap().value["generation"],
        "unrelated_corruption"
    );
}
