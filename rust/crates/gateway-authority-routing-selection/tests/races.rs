pub mod support;
use gateway_auth_credentials::provider_credentials::codex_source_pool_key;
use gateway_authority_contracts::invalid;
use gateway_authority_keys as keys;
use gateway_authority_routing_selection::{SelectionOperations, types::RouteSelection};
use gateway_contracts::{model::ModelId, provider::CodexQuotaClass};
use gateway_core::{
    state::AuthorityStore, state::CommitOutcome, state::RecordMutation, state::RevisionCheck,
    state::StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use serde_json::json;
use std::collections::{BTreeMap, BTreeSet};
use support::{CLASS, Fixture, NOW, TestResult, block, lease, ready};

fn fixed_keys(fixture: &Fixture) -> Result<BTreeSet<String>, gateway_source::error::GatewayError> {
    let mut keys: BTreeSet<_> = [
        keys::pool(),
        keys::routing(),
        keys::active(),
        keys::capacity(),
        codex_source_pool_key(),
        keys::models(),
        keys::provider_selection(),
    ]
    .into_iter()
    .map(|key| key.0)
    .collect();
    keys.extend(
        fixture
            .pool()?
            .accounts
            .iter()
            .map(|account| keys::overage(&account.admin_account_hash.0).0),
    );
    Ok(keys)
}

#[test]
fn every_physical_source_read_joins_the_original_selection_cas_once() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.admitted()?;
    fixture.clear_observations();
    fixture.admitted()?;
    let expected = fixed_keys(&fixture)?;
    let transition = fixture.last_transition()?;
    assert_eq!(
        transition
            .checks
            .iter()
            .map(|check| check.key.0.clone())
            .collect::<BTreeSet<_>>(),
        expected
    );
    let mut counts = BTreeMap::new();
    for key in fixture.store.reads.borrow().iter() {
        *counts.entry(key.0.clone()).or_insert(0) += 1;
    }
    assert_eq!(counts.keys().cloned().collect::<BTreeSet<_>>(), expected);
    // One physical read in the ordered capacity reconciliation and one in selection.
    // The selection's second routing read must use its original memoized record.
    assert!(counts.values().all(|count| *count == 2));
    assert!(transition.mutations.is_empty());
    Ok(())
}

#[test]
fn expired_capacity_payload_is_ignored_but_its_raw_revision_fences_the_cas() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.store.rows.borrow_mut().insert(
        keys::capacity().0,
        StoredRecord::json("expired-capacity", json!(["invalid-body"]), Some(NOW)),
    );
    fixture.block_all()?;
    let proof = fixture.proof()?;
    let fence = proof
        .revision_fences()
        .iter()
        .find(|check| check.key == keys::capacity())
        .ok_or_else(|| invalid("missing expired fence"))?;
    assert_eq!(fence.expected_revision.as_deref(), Some("expired-capacity"));
    assert!(proof.full_cohort_exhausted());
    let mut replacement = fixture.row(&keys::capacity())?;
    replacement.revision = "expired-capacity-raced".into();
    fixture
        .store
        .rows
        .borrow_mut()
        .insert(keys::capacity().0, replacement);
    assert!(!proof.matches_current_evidence(&fixture.evidence(NOW)?, true));
    Ok(())
}

#[test]
fn pool_revision_collision_retries_and_mints_only_the_successful_fences() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.block_all()?;
    let mut replacement = fixture.row(&keys::pool())?;
    replacement.revision = "pool-raced".into();
    *fixture.store.race.borrow_mut() = Some((keys::pool(), replacement));
    let proof = fixture.proof()?;
    assert_eq!(fixture.store.transitions.borrow().len(), 2);
    let fence = proof
        .revision_fences()
        .iter()
        .find(|check| check.key == keys::pool())
        .ok_or_else(|| invalid("missing pool fence"))?;
    assert_eq!(fence.expected_revision.as_deref(), Some("pool-raced"));
    assert!(proof.matches_current_evidence(&fixture.evidence(NOW)?, true));
    Ok(())
}

#[test]
fn raw_source_race_never_releases_a_proof_for_a_stale_normalized_pool() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.block_all()?;
    let mut replacement = fixture.row(&codex_source_pool_key())?;
    replacement.revision = "raw-source-raced".into();
    *fixture.store.race.borrow_mut() = Some((codex_source_pool_key(), replacement));
    let error = fixture
        .select()
        .err()
        .ok_or_else(|| invalid("stale source released selection"))?;
    assert_eq!(
        error.envelope.error.message,
        "Normalized Codex pool is not bound to the current source revision."
    );
    assert_eq!(fixture.store.transitions.borrow().len(), 1);
    assert!(fixture.store.rows.borrow().get(&keys::active().0).is_none());
    Ok(())
}

#[test]
fn all_three_selection_conflicts_return_the_original_error_without_an_admission() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.store.reject.set(3);
    let error = fixture
        .select()
        .err()
        .ok_or_else(|| invalid("conflicting election released admission"))?;
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("codex_routing_unavailable")
    );
    assert_eq!(
        error.envelope.error.message,
        "Codex routing changed concurrently."
    );
    assert_eq!(fixture.store.transitions.borrow().len(), 3);
    assert!(fixture.store.rows.borrow().get(&keys::active().0).is_none());
    assert!(
        fixture
            .store
            .rows
            .borrow()
            .get(&keys::routing().0)
            .is_none()
    );
    Ok(())
}

#[test]
fn read_revision_and_duplicate_mutation_token_collisions_fail_before_the_cas() -> TestResult {
    for token in ["pool-r1", "fresh-duplicate-revision"] {
        let fixture = Fixture::new()?;
        *fixture.tokens.fixed.borrow_mut() = Some(token.into());
        let error = fixture
            .select()
            .err()
            .ok_or_else(|| invalid("revision collision released selection"))?;
        assert_eq!(
            error.envelope.error.message,
            if token == "pool-r1" {
                "Token source reused an existing revision."
            } else {
                "Token source duplicated a mutation revision."
            }
        );
        assert!(fixture.store.transitions.borrow().is_empty());
        assert!(
            fixture
                .store
                .rows
                .borrow()
                .get(&keys::routing().0)
                .is_none()
        );
    }
    Ok(())
}

#[test]
fn model_selection_and_overage_fences_cannot_be_dropped_from_proof_currentness() -> TestResult {
    for key_kind in ["models", "selection", "overage"] {
        let fixture = Fixture::new()?;
        fixture.block_all()?;
        let proof = fixture.proof()?;
        let key = match key_kind {
            "models" => keys::models(),
            "selection" => keys::provider_selection(),
            _ => keys::overage(&fixture.pool()?.accounts[0].admin_account_hash.0),
        };
        let value = match key_kind {
            "models" => json!({"accounts":{},"unsupported":{}}),
            "selection" => json!({"provider_ids":["codex"],"updated_at_ms":1}),
            _ => json!({"allow":true}),
        };
        fixture.put(key, "new-source-fence", &value)?;
        assert!(!proof.matches_current_evidence(&fixture.evidence(NOW)?, false));
    }
    Ok(())
}

#[test]
fn consumer_must_keep_proof_fences_in_its_existing_effect_cas() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.block_all()?;
    let proof = fixture.proof()?;
    assert!(proof.matches_current_evidence(&fixture.evidence(NOW)?, true));
    let mut effect = fixture.last_transition()?;
    effect.checks = proof.revision_fences().to_vec();
    let effect_key = RecordKey("routing-service-synthetic-effect".into());
    effect.checks.push(RevisionCheck {
        key: effect_key.clone(),
        expected_revision: None,
    });
    effect.mutations = vec![RecordMutation::Put {
        key: effect_key.clone(),
        record: StoredRecord::json("effect-r1", json!("synthetic effect"), None),
    }];
    // A race after pure validation is rejected by that SAME eventual effect CAS.
    let mut replacement = fixture.row(&keys::pool())?;
    replacement.revision = "pool-after-validation".into();
    *fixture.store.race.borrow_mut() = Some((keys::pool(), replacement));
    assert_eq!(
        ready(fixture.store.commit(effect))?,
        CommitOutcome::Conflict
    );
    assert!(fixture.store.rows.borrow().get(&effect_key.0).is_none());
    Ok(())
}

#[test]
fn blank_models_fail_before_reads_and_selection_does_not_require_a_probe_clock() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.with_clock.set(false);
    let error = ready(fixture.select_account(&ModelId::from("\u{feff} "), CLASS, NOW))
        .err()
        .ok_or_else(|| invalid("blank model accepted"))?;
    assert_eq!(
        error.envelope.error.message,
        "Missing or oversized authority identity."
    );
    assert!(fixture.store.reads.borrow().is_empty());
    let admission = fixture.admitted()?;
    ready(fixture.ensure_admission_current(&admission, NOW))?;
    Ok(())
}

#[test]
fn capacity_reconciliation_precedes_election_and_retains_every_original_fence() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.block_all()?;
    fixture.capacity([25.0, 100.0], Some(NOW + 500), NOW)?;
    fixture.clear_observations();
    let admission = fixture.admitted()?;
    assert_eq!(
        admission.binding.account_id_hash,
        fixture.pool()?.accounts[0].account_id_hash
    );
    let transitions = fixture.store.transitions.borrow();
    assert_eq!(transitions.len(), 2);
    assert_eq!(
        transitions[0]
            .checks
            .iter()
            .map(|check| check.key.0.clone())
            .collect::<BTreeSet<_>>(),
        fixed_keys(&fixture)?
    );
    let reconciled = transitions[0]
        .mutations
        .iter()
        .find_map(|mutation| match mutation {
            RecordMutation::Put { key, record } if *key == keys::routing() => {
                Some(record.revision.clone())
            }
            _ => None,
        })
        .ok_or_else(|| invalid("capacity R commit missing"))?;
    assert!(
        transitions[1]
            .checks
            .iter()
            .any(|check| check.key == keys::routing()
                && check.expected_revision.as_ref() == Some(&reconciled))
    );
    assert!(fixture.state()?.slots[0].quota_block_for(CLASS).is_none());
    Ok(())
}

#[test]
fn capacity_cas_collision_reloads_the_sample_before_electing() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.block_all()?;
    fixture.capacity([25.0, 100.0], Some(NOW + 500), NOW)?;
    let mut replacement = fixture.row(&keys::capacity())?;
    replacement.revision = "capacity-raced".into();
    replacement.value["observations"][0]["windows"]["primary"]["used_percent"] = json!(100.0);
    *fixture.store.race.borrow_mut() = Some((keys::capacity(), replacement));
    let RouteSelection::QuotaBlocked { proof, .. } = fixture.select()? else {
        return Err(invalid("stale positive sample elected an account"));
    };
    assert!(proof.full_cohort_exhausted());
    assert_eq!(fixture.store.transitions.borrow().len(), 3);
    assert!(
        proof
            .revision_fences()
            .iter()
            .any(|check| check.key == keys::capacity()
                && check.expected_revision.as_deref() == Some("capacity-raced"))
    );
    assert!(fixture.store.rows.borrow().get(&keys::active().0).is_none());
    Ok(())
}

#[test]
fn deadline_less_capacity_exhaustion_is_a_probe_and_future_deadline_is_a_block() -> TestResult {
    for reset in [None, Some(NOW + 500)] {
        let fixture = Fixture::new()?;
        fixture.capacity([100.0, 100.0], reset, NOW)?;
        match fixture.select()? {
            RouteSelection::Eligible(admission) => {
                assert!(reset.is_none());
                assert!(admission.probe_required);
            }
            RouteSelection::QuotaBlocked { retry_at_ms, proof } => {
                assert_eq!(retry_at_ms, reset);
                assert!(proof.full_cohort_exhausted());
            }
            _ => return Err(invalid("capacity produced wrong distinction")),
        }
    }
    Ok(())
}

#[test]
fn capacity_materialization_reconciles_a_newer_sample_over_stale_ambiguous_reset() -> TestResult {
    let fixture = Fixture::new()?;
    let mut state = fixture.state()?;
    let mut prior = block(NOW - 10, NOW + 100);
    prior.reset_ambiguous = true;
    prior.recovery_pending = true;
    state.slots[0].insert_quota_block(CLASS, prior);
    fixture.write_state(&state)?;
    fixture.capacity([100.0, 0.0], Some(NOW + 1000), NOW)?;
    assert!(ready(fixture.materialize_capacity_quota_blocks(
        &ModelId::from("gpt-test"),
        CLASS,
        NOW
    ))?);
    let state = fixture.state()?;
    let actual = state.slots[0]
        .quota_block_for(CLASS)
        .ok_or_else(|| invalid("materialized block missing"))?;
    assert_eq!(actual.reset_at_ms, Some(NOW + 1000));
    assert_eq!(actual.observed_at_ms, Some(NOW));
    assert!(!actual.reset_ambiguous);
    assert!(actual.reset_is_stable);
    assert!(actual.recovery_pending);
    Ok(())
}

#[test]
fn materialization_keeps_newer_live_signal_and_foreign_class_probe_generation() -> TestResult {
    for class in [CLASS, CodexQuotaClass::Spark] {
        let fixture = Fixture::new()?;
        let mut state = fixture.state()?;
        state.slots[0].probe_lease = Some(lease(class));
        state.slots[0].generation = 7;
        fixture.write_state(&state)?;
        fixture.capacity([100.0, 0.0], Some(NOW + 1000), NOW)?;
        assert_eq!(
            ready(fixture.materialize_capacity_quota_blocks(
                &ModelId::from("gpt-test"),
                CLASS,
                NOW
            ))?,
            class != CLASS
        );
        assert_eq!(
            fixture.state()?.slots[0].probe_lease,
            state.slots[0].probe_lease
        );
        assert_eq!(fixture.state()?.slots[0].generation, 7);
    }
    let fixture = Fixture::new()?;
    let mut state = fixture.state()?;
    let prior = block(NOW + 1, NOW + 500);
    state.slots[0].insert_quota_block(CLASS, prior.clone());
    fixture.write_state(&state)?;
    fixture.capacity([100.0, 0.0], Some(NOW + 1000), NOW)?;
    assert!(!ready(fixture.materialize_capacity_quota_blocks(
        &ModelId::from("gpt-test"),
        CLASS,
        NOW + 2
    ))?);
    assert_eq!(
        fixture.state()?.slots[0].quota_block_for(CLASS),
        Some(prior)
    );
    Ok(())
}

#[test]
fn malformed_metadata_and_backend_errors_preserve_error_order_without_reseeding() -> TestResult {
    let fixture = Fixture::new()?;
    let mut corrupt = StoredRecord::json(" ", json!(false), Some(NOW));
    corrupt.schema_version = 99;
    fixture
        .store
        .rows
        .borrow_mut()
        .insert(keys::capacity().0, corrupt.clone());
    let error = fixture
        .select()
        .err()
        .ok_or_else(|| invalid("corrupt capacity accepted"))?;
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("authority_state_version_unsupported")
    );
    assert_eq!(fixture.row(&keys::capacity())?, corrupt);
    assert!(fixture.store.transitions.borrow().is_empty());
    *fixture.store.failure.borrow_mut() = Some(keys::routing());
    let error = fixture
        .select()
        .err()
        .ok_or_else(|| invalid("backend failure accepted"))?;
    assert_eq!(
        error.envelope.error.message,
        "synthetic backend read failed"
    );
    assert!(fixture.store.transitions.borrow().is_empty());
    Ok(())
}
