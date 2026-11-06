//! Portable transition-policy fixtures, not durable Worker/Frozen acceptance.
use gateway_auth::crypto::sha256_hex;
use gateway_authority_lifecycle::{maintenance, model::*, policy};
use gateway_authority_provisioning::intent::{Intent, Scope, authorization_digest, token};
use gateway_core::error::GatewayError;

fn binding(gate: &Gate) -> Binding {
    Binding {
        identity: gate.identity.clone(),
        serving_epoch: gate.serving_epoch,
    }
}
fn counts(roots: u64, blocked: u64) -> Counts {
    Counts {
        roots,
        blocked,
        native_operations: 0,
        native_joins: 0,
        import_frames: 0,
    }
}
fn running() -> Result<Gate, GatewayError> {
    let mut gate = policy::new_gate(
        Identity {
            object_id: "a".repeat(64),
            source_sha: "b".repeat(40),
            source_tree: "c".repeat(40),
        },
        false,
    )?;
    let import = ImportState {
        capture_id: "d".repeat(64),
        intent_digest: "e".repeat(64),
        header_digest: "f".repeat(64),
        records: 0,
        chain_digest: "1".repeat(64),
    };
    // Unit input for the pure finalization reducer; no SQL authority is created.
    gate.phase = Phase::Importing;
    gate.import = Some(import.clone());
    let plan = FinalizePlan {
        binding: binding(&gate),
        capture_id: import.capture_id.clone(),
        staged_records: 0,
        final_chain_digest: import.chain_digest,
        manifest_revision: "capture:fixture".into(),
        expected_owner: OwnerExpectation::Absent,
    };
    let facts = FinalizeFacts {
        progress_present: false,
        manifest_capture_id: import.capture_id,
        manifest_revision: "capture:fixture".into(),
        canonical_records: 1,
        owner: None,
    };
    let reference = policy::finalize(&mut gate, &plan, &facts, &"2".repeat(64), &counts(0, 0))?;
    policy::activate(&mut gate, &reference, &"3".repeat(64), true)?;
    Ok(gate)
}
fn drain(gate: &Gate) -> DrainPlan {
    DrainPlan {
        binding: binding(gate),
        drain_revision: gate.drain_revision,
    }
}
fn cancel(plan: &DrainPlan, digest: &str) -> CancelDrainPlan {
    CancelDrainPlan {
        binding: plan.binding.clone(),
        drain_revision: plan.drain_revision,
        drain_digest: digest.into(),
    }
}

#[test]
fn cancellation_preserves_epoch_history_and_retires_the_old_drain() -> Result<(), GatewayError> {
    let mut gate = running()?;
    let plan = drain(&gate);
    let digest = "4".repeat(64);
    maintenance::drain(&mut gate, &plan, &digest, &counts(1, 1), true)?;
    let historical = gate.frozen.clone();
    let cancellation = cancel(&plan, &digest);
    let (receipt, replayed) = maintenance::cancel(&mut gate, &cancellation, &"5".repeat(64), true)?;
    assert!(!replayed);
    assert_eq!(gate.phase, Phase::Running);
    assert_eq!(gate.serving_epoch, plan.binding.serving_epoch);
    assert_eq!(gate.frozen, historical);
    assert_eq!(gate.drain_revision, 1);
    assert_eq!(gate.drain_digest, None);
    assert_eq!(
        maintenance::cancelled(&gate, &plan, &digest),
        Some(receipt.clone())
    );
    assert!(maintenance::drain(&mut gate, &plan, &digest, &counts(1, 1), true).is_err());
    let before = serde_json::to_vec(&gate).expect("fixture JSON");
    let (replayed_receipt, replayed) =
        maintenance::cancel(&mut gate, &cancellation, &"5".repeat(64), true)?;
    assert!(replayed);
    assert_eq!(receipt, replayed_receipt);
    assert_eq!(serde_json::to_vec(&gate).expect("fixture JSON"), before);
    let next = drain(&gate);
    maintenance::drain(&mut gate, &next, &"6".repeat(64), &counts(1, 1), true)?;
    assert!(maintenance::cancel(&mut gate, &cancellation, &"5".repeat(64), true).is_err());
    assert_eq!(gate.phase, Phase::Draining);
    assert_eq!(gate.drain_digest.as_deref(), Some("6".repeat(64).as_str()));
    Ok(())
}

#[test]
fn cancellation_requires_current_phase_identity_revision_digest_and_ingress()
-> Result<(), GatewayError> {
    let gate = running()?;
    let plan = drain(&gate);
    let digest = "4".repeat(64);
    let mut active = gate.clone();
    maintenance::drain(&mut active, &plan, &digest, &counts(1, 1), true)?;
    let good = cancel(&plan, &digest);
    let mut wrong_object = good.clone();
    wrong_object.binding.identity.object_id = "0".repeat(64);
    let mut wrong_epoch = good.clone();
    wrong_epoch.binding.serving_epoch += 1;
    let mut wrong_source = good.clone();
    wrong_source.binding.identity.source_sha = "0".repeat(40);
    let mut wrong_tree = good.clone();
    wrong_tree.binding.identity.source_tree = "0".repeat(40);
    let mut wrong_revision = good.clone();
    wrong_revision.drain_revision += 1;
    let mut wrong_digest = good.clone();
    wrong_digest.drain_digest = "0".repeat(64);
    for invalid in [
        wrong_object,
        wrong_source,
        wrong_tree,
        wrong_epoch,
        wrong_revision,
        wrong_digest,
    ] {
        let mut candidate = active.clone();
        let before = serde_json::to_vec(&candidate).expect("fixture JSON");
        assert!(maintenance::cancel(&mut candidate, &invalid, &"5".repeat(64), true).is_err());
        assert_eq!(
            serde_json::to_vec(&candidate).expect("fixture JSON"),
            before
        );
    }
    assert!(maintenance::cancel(&mut active, &good, &"5".repeat(64), false).is_err());
    assert!(maintenance::cancel(&mut gate.clone(), &good, &"5".repeat(64), true).is_err());
    let mut frozen = gate.clone();
    maintenance::drain(&mut frozen, &plan, &digest, &counts(0, 0), true)?;
    assert!(maintenance::cancel(&mut frozen, &good, &"5".repeat(64), true).is_err());
    for occupied in [false, true] {
        let mut closed = policy::new_gate(gate.identity.clone(), occupied)?;
        let closed_plan = cancel(&drain(&closed), &digest);
        assert!(maintenance::cancel(&mut closed, &closed_plan, &"5".repeat(64), true).is_err());
    }
    let mut importing = policy::new_gate(gate.identity.clone(), false)?;
    importing.phase = Phase::Importing;
    let importing_plan = cancel(&drain(&importing), &digest);
    assert!(maintenance::cancel(&mut importing, &importing_plan, &"5".repeat(64), true).is_err());
    Ok(())
}

#[test]
fn completed_maintenance_cycles_clear_old_digests_and_fence_activation_replay()
-> Result<(), GatewayError> {
    let mut gate = running()?;
    for digit in [4u32, 6, 8] {
        let plan = drain(&gate);
        let digest = char::from_digit(digit, 16)
            .expect("fixture digit")
            .to_string()
            .repeat(64);
        maintenance::drain(&mut gate, &plan, &digest, &counts(0, 0), true)?;
        let frozen = gate.frozen.clone().expect("actual reducer Frozen");
        let activation = char::from_digit(digit + 1, 16)
            .expect("fixture digit")
            .to_string()
            .repeat(64);
        assert!(!policy::activate(&mut gate, &frozen, &activation, true)?);
        assert_eq!(gate.drain_digest, None);
        assert_eq!(gate.drain_revision, 0);
        assert_eq!(gate.last_cancellation, None);
        assert!(policy::activate(&mut gate, &frozen, &activation, true)?);
        assert!(maintenance::drain(&mut gate, &plan, &digest, &counts(1, 0), true).is_err());
        let next = drain(&gate);
        maintenance::drain(&mut gate, &next, &"f".repeat(64), &counts(1, 0), true)?;
        assert!(policy::activate(&mut gate, &frozen, &activation, true).is_err());
        let cancellation = cancel(&next, &"f".repeat(64));
        maintenance::cancel(&mut gate, &cancellation, &"e".repeat(64), true)?;
    }
    Ok(())
}

#[test]
fn corrupt_or_overflowing_maintenance_metadata_fails_closed() -> Result<(), GatewayError> {
    let mut gate = running()?;
    let plan = drain(&gate);
    maintenance::drain(&mut gate, &plan, &"4".repeat(64), &counts(1, 1), true)?;
    let cancellation = cancel(&plan, &"4".repeat(64));
    maintenance::cancel(&mut gate, &cancellation, &"5".repeat(64), true)?;
    let mut corrupt = gate.clone();
    corrupt.drain_revision += 1;
    assert!(maintenance::validate(&corrupt).is_err());
    let mut corrupt = gate.clone();
    corrupt
        .last_cancellation
        .as_mut()
        .expect("receipt")
        .cancel_digest = "z".repeat(64);
    assert!(maintenance::validate(&corrupt).is_err());
    let mut corrupt = gate;
    corrupt.drain_revision = gateway_core::identity::MAX_SAFE_INTEGER as u64 + 1;
    assert!(maintenance::validate(&corrupt).is_err());
    Ok(())
}

#[test]
fn cancellation_intent_binds_purpose_and_complete_current_plan() -> Result<(), GatewayError> {
    let mut gate = running()?;
    let plan = drain(&gate);
    let old_request = serde_json::json!({
        "v": 2,
        "request": {"operation":"drain","binding":plan.binding}
    });
    assert!(serde_json::from_value::<ControlEnvelope>(old_request).is_err());
    maintenance::drain(&mut gate, &plan, &"4".repeat(64), &counts(1, 1), true)?;
    let authorization = Authorization::CancelDrain(cancel(&plan, &"4".repeat(64)));
    let digest = authorization_digest(&authorization)?;
    let presented = token(
        &Scope::Control {
            purpose: Purpose::CancelDrain,
            authorization_digest: digest.clone(),
        },
        &"a".repeat(64),
    )?;
    let configured = sha256_hex(presented.as_bytes());
    let intent = Intent::verify(Some(&presented), Some(&configured))?;
    assert!(intent.require_control(&authorization).is_ok());
    assert!(intent.require_control(&Authorization::Drain(plan)).is_err());
    let wrong = token(
        &Scope::Control {
            purpose: Purpose::Drain,
            authorization_digest: digest,
        },
        &"b".repeat(64),
    )?;
    let wrong_digest = sha256_hex(wrong.as_bytes());
    assert!(
        Intent::verify(Some(&wrong), Some(&wrong_digest))?
            .require_control(&authorization)
            .is_err()
    );
    Ok(())
}

#[test]
fn old_activation_cannot_thaw_a_new_frozen_or_running_epoch() -> Result<(), GatewayError> {
    let mut gate = running()?;
    let old = gate.frozen.clone().expect("fixture finalization");
    let plan = drain(&gate);
    maintenance::drain(&mut gate, &plan, &"4".repeat(64), &counts(0, 0), true)?;
    let current = gate.frozen.clone().expect("current reducer Frozen");
    assert!(policy::activate(&mut gate, &old, &"3".repeat(64), true).is_err());
    assert_eq!(gate.phase, Phase::Frozen);
    policy::activate(&mut gate, &current, &"5".repeat(64), true)?;
    assert!(policy::activate(&mut gate, &old, &"3".repeat(64), true).is_err());
    assert_eq!(gate.serving_epoch, current.serving_epoch + 1);
    Ok(())
}

#[test]
fn maximum_revision_cannot_trap_live_writers_or_wrap_on_cancellation() -> Result<(), GatewayError> {
    let mut gate = running()?;
    let maximum = gateway_core::identity::MAX_SAFE_INTEGER as u64;
    let plan = CancelDrainPlan {
        binding: binding(&gate),
        drain_revision: maximum - 1,
        drain_digest: "4".repeat(64),
    };
    let digest = "5".repeat(64);
    // Boundary input for the pure metadata validator, not a durable authority.
    let revision = sha256_hex(
        &serde_json::to_vec(&(
            "uos-drain-cancellation-v1",
            &plan.binding.identity.object_id,
            &plan.binding.identity.source_sha,
            &plan.binding.identity.source_tree,
            plan.binding.serving_epoch,
            plan.drain_revision,
            maximum,
            &plan.drain_digest,
            &digest,
        ))
        .expect("fixture receipt"),
    );
    gate.drain_revision = maximum;
    gate.last_cancellation = Some(DrainCancellation {
        plan,
        next_drain_revision: maximum,
        cancel_digest: digest,
        cancellation_revision: revision,
    });
    maintenance::validate(&gate)?;
    let next = drain(&gate);
    let before = serde_json::to_vec(&gate).expect("fixture JSON");
    assert!(maintenance::drain(&mut gate, &next, &"6".repeat(64), &counts(1, 1), true).is_err());
    assert_eq!(serde_json::to_vec(&gate).expect("fixture JSON"), before);
    let mut unfinished = gate.clone();
    unfinished.phase = Phase::Draining;
    unfinished.drain_digest = Some("6".repeat(64));
    let cancel = cancel(&next, &"6".repeat(64));
    let before = serde_json::to_vec(&unfinished).expect("fixture JSON");
    assert!(maintenance::cancel(&mut unfinished, &cancel, &"7".repeat(64), true).is_err());
    assert_eq!(
        serde_json::to_vec(&unfinished).expect("fixture JSON"),
        before
    );
    maintenance::drain(&mut gate, &next, &"8".repeat(64), &counts(0, 0), true)?;
    let frozen = gate.frozen.clone().expect("zero-count reducer Frozen");
    policy::activate(&mut gate, &frozen, &"9".repeat(64), true)?;
    assert_eq!(gate.drain_revision, 0);
    assert_eq!(gate.last_cancellation, None);
    Ok(())
}
