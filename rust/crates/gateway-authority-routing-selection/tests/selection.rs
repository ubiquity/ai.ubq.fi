pub mod support;
use gateway_authority_contracts::{DAY_MS, invalid};
use gateway_authority_keys as keys;
use gateway_authority_routing_evidence::{
    models::model_quota_class,
    reader::{RoutingEvidenceReader, RoutingReadMode},
};
use gateway_authority_routing_selection::{
    SelectionOperations,
    types::{LiveRouteSelection, RouteSelection},
};
use gateway_core::{
    identity::ActiveTransitionReason, identity::CredentialVersion, model::ModelId,
    source_control::encode_source_control_record, state::RecordMutation,
};
use gateway_source::{js_string::JsString16, source_json::SourceJson};
use serde_json::json;
use std::sync::atomic::Ordering;
use support::{CLASS, Fixture, NOW, TestResult, block, lease, ready};

#[test]
fn hot_selection_uses_check_only_identity_and_never_balances_to_sibling() -> TestResult {
    let fixture = Fixture::new()?;
    let first = fixture.admitted()?;
    let routing = fixture.row(&keys::routing())?;
    let active = fixture.row(&keys::active())?;
    fixture.clear_observations();
    let second = fixture.admitted()?;
    assert_eq!(first, second);
    assert_eq!(fixture.row(&keys::routing())?, routing);
    assert_eq!(fixture.row(&keys::active())?, active);
    assert!(fixture.last_transition()?.mutations.is_empty());
    assert_eq!(fixture.store.transitions.borrow().len(), 1);
    Ok(())
}

#[test]
fn headroom_reorder_and_same_identity_refresh_keep_one_active_generation() -> TestResult {
    let fixture = Fixture::new()?;
    let first = fixture.admitted()?;
    fixture.capacity([99.0, 0.0], Some(NOW + 10_000), NOW - 1)?;
    let sampled = fixture.admitted()?;
    assert_eq!(
        sampled.binding.account_id_hash,
        first.binding.account_id_hash
    );
    assert_eq!(sampled.active_generation, first.active_generation);
    let mut raw = fixture.raw_source()?;
    raw["accounts"]
        .as_array_mut()
        .ok_or_else(|| invalid("fixture accounts"))?
        .swap(0, 1);
    fixture.install_source(raw.clone(), "source-reordered")?;
    let reordered = fixture.admitted()?;
    assert_eq!(
        reordered.binding.account_id_hash,
        first.binding.account_id_hash
    );
    assert_eq!(reordered.binding.slot, 1);
    assert_eq!(reordered.active_generation, first.active_generation);
    raw["accounts"][1]["access_token"] = json!("synthetic-refreshed-a");
    fixture.install_source(raw, "source-refreshed")?;
    let refreshed = fixture.admitted()?;
    assert_eq!(
        refreshed.binding.account_id_hash,
        first.binding.account_id_hash
    );
    assert_ne!(
        refreshed.binding.credential_version,
        first.binding.credential_version
    );
    assert_eq!(refreshed.active_generation, first.active_generation);
    assert_eq!(fixture.active()?.transition_reason, None);
    Ok(())
}

#[test]
fn applicable_quota_advances_once_and_other_classes_keep_global_stickiness() -> TestResult {
    let fixture = Fixture::new()?;
    let first = fixture.admitted()?;
    let mut state = fixture.state()?;
    state.slots[0].insert_quota_block(CLASS, block(NOW - 10, NOW + 500));
    fixture.write_state(&state)?;
    let second = fixture.admitted()?;
    assert_ne!(
        first.binding.account_id_hash,
        second.binding.account_id_hash
    );
    assert_eq!(second.active_generation, first.active_generation + 1);
    assert_eq!(
        fixture.active()?.transition_reason,
        Some(ActiveTransitionReason::QuotaExhausted)
    );
    let RouteSelection::Eligible(reserve) = ready(fixture.select_account(
        &ModelId::from("gpt-reserve"),
        gateway_core::provider::CodexQuotaClass::Reserve,
        NOW,
    ))?
    else {
        return Err(invalid("reserve should remain eligible"));
    };
    assert_eq!(
        reserve.binding.account_id_hash,
        second.binding.account_id_hash
    );
    assert_eq!(reserve.active_generation, second.active_generation);
    Ok(())
}

#[test]
fn invalidity_requires_the_current_credential_fingerprint() -> TestResult {
    let fixture = Fixture::new()?;
    let mut state = fixture.state()?;
    state.slots[0].invalid_credential_version = Some(CredentialVersion("stale-credential".into()));
    fixture.write_state(&state)?;
    let first = fixture.admitted()?;
    state = fixture.state()?;
    state.slots[0].invalid_credential_version = Some(first.binding.credential_version.clone());
    fixture.write_state(&state)?;
    let second = fixture.admitted()?;
    assert_ne!(
        first.binding.account_id_hash,
        second.binding.account_id_hash
    );
    assert_eq!(
        fixture.active()?.transition_reason,
        Some(ActiveTransitionReason::CredentialInvalid)
    );
    Ok(())
}

#[test]
fn quota_and_invalidity_prove_unavailability_but_only_quota_proves_exhaustion() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.block_all()?;
    let quota = fixture.proof()?;
    assert!(quota.full_cohort_exhausted());
    assert!(quota.full_cohort_unavailable());
    assert_eq!(quota.selected_count(), 2);
    assert!(quota.matches_current_evidence(&fixture.evidence(NOW)?, true));
    let mut state = fixture.state()?;
    state.slots[1].invalid_credential_version = Some(state.slots[1].credential_version.clone());
    fixture.write_state(&state)?;
    let mixed = fixture.proof()?;
    assert!(!mixed.full_cohort_exhausted());
    assert!(mixed.full_cohort_unavailable());
    assert!(mixed.matches_current_evidence(&fixture.evidence(NOW)?, false));
    assert!(!mixed.matches_current_evidence(&fixture.evidence(NOW)?, true));
    assert!(!quota.matches_current_evidence(&fixture.evidence(NOW)?, false));
    state.slots[0].invalid_credential_version = Some(state.slots[0].credential_version.clone());
    fixture.write_state(&state)?;
    let RouteSelection::CredentialsInvalid { proof } = fixture.select()? else {
        return Err(invalid("invalid credentials expected"));
    };
    assert!(proof.full_cohort_unavailable());
    assert!(!proof.full_cohort_exhausted());
    Ok(())
}

#[test]
fn no_active_proof_records_post_normalization_revision_without_electing() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.block_all()?;
    let mut state = fixture.state()?;
    state.slots.swap(0, 1);
    fixture.write_state(&state)?;
    let prior_revision = fixture.row(&keys::routing())?.revision;
    let proof = fixture.proof()?;
    let transition = fixture.last_transition()?;
    let before = transition
        .checks
        .iter()
        .find(|check| check.key == keys::routing())
        .ok_or_else(|| invalid("missing routing fence"))?;
    assert_eq!(
        before.expected_revision.as_deref(),
        Some(prior_revision.as_str())
    );
    let after = proof
        .revision_fences()
        .iter()
        .find(|check| check.key == keys::routing())
        .ok_or_else(|| invalid("missing post fence"))?;
    assert_eq!(
        after.expected_revision.as_deref(),
        Some(fixture.row(&keys::routing())?.revision.as_str())
    );
    assert_ne!(before.expected_revision, after.expected_revision);
    assert!(fixture.store.rows.borrow().get(&keys::active().0).is_none());
    assert!(proof.matches_current_evidence(&fixture.evidence(NOW)?, true));
    Ok(())
}

#[test]
fn live_probe_lease_is_incomplete_and_cannot_authorize_a_cohort() -> TestResult {
    let fixture = Fixture::new()?;
    let first = fixture.admitted()?;
    let mut state = fixture.state()?;
    state.slots[0].probe_lease = Some(lease(CLASS));
    fixture.write_state(&state)?;
    assert!(matches!(
        fixture.select()?,
        RouteSelection::RoutingUnavailable
    ));
    assert_eq!(
        fixture.active()?.binding.account_id_hash,
        first.binding.account_id_hash
    );
    fixture.store.rows.borrow_mut().remove(&keys::active().0);
    state.slots[1].probe_lease = Some(lease(CLASS));
    fixture.write_state(&state)?;
    let proof = fixture.proof()?;
    assert!(!proof.full_cohort_unavailable());
    assert!(!proof.full_cohort_exhausted());
    assert!(!proof.matches_current_evidence(&fixture.evidence(NOW)?, false));
    state.slots[0].probe_lease = None;
    state.slots[0].insert_quota_block(CLASS, block(NOW - 1, NOW + 200));
    fixture.write_state(&state)?;
    let RouteSelection::QuotaBlocked { proof, .. } = fixture.select()? else {
        return Err(invalid("partial quota cohort expected"));
    };
    assert!(!proof.full_cohort_unavailable());
    assert!(!proof.full_cohort_exhausted());
    Ok(())
}

#[test]
fn mixed_model_unavailability_and_quota_remain_an_incomplete_cohort() -> TestResult {
    let fixture = Fixture::new()?;
    let mut state = fixture.state()?;
    state.slots[0].insert_quota_block(CLASS, block(NOW - 1, NOW + 200));
    fixture.write_state(&state)?;
    fixture.put(
        keys::models(),
        "unsupported-model",
        &json!({"accounts":{},"unsupported":{"b":{"gpt-test":NOW-1}}}),
    )?;
    let RouteSelection::QuotaBlocked { proof, .. } = fixture.select()? else {
        return Err(invalid("partial quota cohort expected"));
    };
    assert!(!proof.full_cohort_exhausted());
    assert!(!proof.full_cohort_unavailable());
    assert!(!proof.matches_current_evidence(&fixture.evidence(NOW)?, false));
    Ok(())
}

#[test]
fn an_unselected_pool_never_mints_full_pool_authority() -> TestResult {
    let fixture = Fixture::new()?;
    fixture.put(
        keys::provider_selection(),
        "selection-r1",
        &json!({"provider_ids":["surplus"],"updated_at_ms":1}),
    )?;
    let proof = fixture.proof()?;
    assert_eq!(proof.selected_count(), 0);
    assert!(!proof.full_cohort_unavailable());
    assert!(!proof.full_cohort_exhausted());
    assert!(!proof.matches_current_evidence(&fixture.evidence(NOW)?, false));
    Ok(())
}

#[test]
fn same_version_sibling_catalog_can_advance_but_unknown_or_other_version_cannot() -> TestResult {
    for same_version in [false, true] {
        let fixture = Fixture::new()?;
        let first = fixture.admitted()?;
        let version = if same_version { "1.2.3" } else { "other" };
        fixture.put(
            keys::models(),
            "models-r1",
            &json!({"accounts":{
            "a":{"client_version":"1.2.3","updated_at_ms":1,"slugs":["other-model"]},
            "b":{"client_version":version,"updated_at_ms":1,"slugs":["gpt-test"]}
        },"unsupported":{}}),
        )?;
        let chosen = fixture.admitted()?;
        assert_eq!(
            chosen.binding.account_id_hash == first.binding.account_id_hash,
            !same_version
        );
        assert_eq!(
            fixture.active()?.transition_reason,
            same_version.then_some(ActiveTransitionReason::ModelUnavailable)
        );
    }
    Ok(())
}

#[test]
fn learned_utf16_model_unavailability_is_separate_from_quota_and_expires() -> TestResult {
    let fixture = Fixture::new()?;
    let source = SourceJson::parse(
        r#"{"accounts":{},"unsupported":{"a":{"\ud800":999999},"b":{"\ud800":999999}}}"#,
    )?;
    fixture.store.rows.borrow_mut().insert(
        keys::models().0,
        encode_source_control_record("models-utf16", &source, None)?,
    );
    let model = ModelId::from_source(JsString16::from_units(vec![0xd800]));
    let RouteSelection::ModelUnavailable {
        model: actual,
        detail: Some(detail),
    } = ready(fixture.select_account(&model, CLASS, NOW))?
    else {
        return Err(invalid("expected learned unsupported"));
    };
    assert_eq!(actual, model);
    assert!(detail.as_units().contains(&0xd800));
    assert!(
        fixture
            .state()?
            .slots
            .iter()
            .all(|slot| slot.quota_blocks.is_empty() && slot.invalid_credential_version.is_none())
    );
    assert!(matches!(
        ready(fixture.select_account(&model, CLASS, NOW + 7 * DAY_MS))?,
        RouteSelection::Eligible(_)
    ));
    Ok(())
}

#[test]
fn optional_and_unpaired_source_models_remain_selection_only() -> TestResult {
    let fixture = Fixture::new()?;
    for model in [
        None,
        Some(JsString16::from_units(vec![])),
        Some(JsString16::from_units(vec![0xd800])),
    ] {
        let LiveRouteSelection::Eligible(selected) =
            ready(fixture.select_live_account(model.as_ref(), NOW))?
        else {
            return Err(invalid("expected live selection"));
        };
        assert_eq!(selected.model, model);
        let lexical = model
            .clone()
            .map(ModelId::from_source)
            .unwrap_or_else(|| ModelId::from(""));
        assert_eq!(selected.quota_class, model_quota_class(&lexical));
        ready(fixture.ensure_live_account_current(&selected, NOW))?;
    }
    Ok(())
}

#[test]
fn currentness_rejects_replaced_credentials_for_inference_and_live() -> TestResult {
    let fixture = Fixture::new()?;
    let admission = fixture.admitted()?;
    let LiveRouteSelection::Eligible(live) = ready(fixture.select_live_account(None, NOW))? else {
        return Err(invalid("expected live selection"));
    };
    let mut raw = fixture.raw_source()?;
    raw["accounts"][0]["access_token"] = json!("synthetic-replaced-access");
    fixture.install_source(raw, "source-replaced")?;
    assert!(ready(fixture.ensure_admission_current(&admission, NOW)).is_err());
    assert!(ready(fixture.ensure_live_account_current(&live, NOW)).is_err());
    Ok(())
}

#[test]
fn admission_probe_expiry_uses_the_same_trusted_clock_after_reads() -> TestResult {
    let fixture = Fixture::new()?;
    let mut admission = fixture.admitted()?;
    let mut state = fixture.state()?;
    let probe = lease(CLASS);
    state.slots[0].probe_lease = Some(probe.clone());
    fixture.write_state(&state)?;
    admission.probe = Some(probe);
    ready(fixture.ensure_admission_current(&admission, NOW))?;
    fixture.clock.0.store(NOW + 100, Ordering::SeqCst);
    let error = ready(fixture.ensure_admission_current(&admission, NOW))
        .err()
        .ok_or_else(|| invalid("expired probe accepted"))?;
    assert_eq!(error.envelope.error.message, "Codex probe lease is stale.");
    fixture.with_clock.set(false);
    let error = ready(fixture.ensure_admission_current(&admission, NOW))
        .err()
        .ok_or_else(|| invalid("clockless probe accepted"))?;
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("authority_clock_unavailable")
    );
    fixture.with_clock.set(true);
    fixture.clock.0.store(NOW - 1, Ordering::SeqCst);
    let error = ready(fixture.ensure_admission_current(&admission, NOW))
        .err()
        .ok_or_else(|| invalid("backward clock accepted"))?;
    assert_eq!(error.envelope.error.code.as_deref(), Some("invalid_clock"));
    Ok(())
}

#[test]
fn normalization_and_selection_commit_together_before_returning_admission() -> TestResult {
    let fixture = Fixture::new()?;
    let admission = fixture.admitted()?;
    let transition = fixture.last_transition()?;
    assert_eq!(transition.mutations.len(), 2);
    assert!(transition.mutations.iter().any(
        |mutation| matches!(mutation, RecordMutation::Put { key, .. } if *key == keys::routing())
    ));
    assert!(transition.mutations.iter().any(
        |mutation| matches!(mutation, RecordMutation::Put { key, .. } if *key == keys::active())
    ));
    assert_eq!(admission.binding, fixture.active()?.binding);
    assert_eq!(admission.active_generation, fixture.active()?.generation);
    let pinned = ready(
        RoutingEvidenceReader::new(&fixture.store, NOW)?.read(RoutingReadMode::PinnedObservation),
    )?;
    fixture.block_all()?;
    assert!(!fixture.proof()?.matches_current_evidence(&pinned, true));
    Ok(())
}
