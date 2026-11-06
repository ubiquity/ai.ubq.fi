use gateway_auth::crypto::sha256_hex;
use gateway_authority_provisioning::{
    intent::{Intent, Scope, authorization_digest, token},
    model::*,
    policy,
};
use gateway_core::error::GatewayError;

fn identity() -> Identity {
    Identity {
        object_id: "a".repeat(64),
        source_sha: "b".repeat(40),
        source_tree: "c".repeat(40),
    }
}
fn empty_counts() -> Counts {
    Counts {
        roots: 0,
        native_operations: 0,
        native_joins: 0,
        import_frames: 0,
        blocked: 0,
    }
}
fn imported() -> Result<(Gate, FinalizePlan, FinalizeFacts), GatewayError> {
    let mut gate = policy::new_gate(identity(), false)?;
    gate.phase = Phase::Importing;
    gate.import = Some(ImportState {
        capture_id: "d".repeat(64),
        intent_digest: "e".repeat(64),
        header_digest: "f".repeat(64),
        records: 12,
        chain_digest: "0".repeat(64),
    });
    let plan = FinalizePlan {
        binding: Binding {
            identity: identity(),
            serving_epoch: 0,
        },
        capture_id: "d".repeat(64),
        staged_records: 12,
        final_chain_digest: "0".repeat(64),
        manifest_revision: "capture:fixture".into(),
        expected_owner: OwnerExpectation::Absent,
    };
    let facts = FinalizeFacts {
        progress_present: false,
        manifest_capture_id: plan.capture_id.clone(),
        manifest_revision: plan.manifest_revision.clone(),
        canonical_records: 13,
        owner: None,
    };
    Ok((gate, plan, facts))
}

#[test]
fn configured_credential_never_opens_a_fresh_authority() -> Result<(), GatewayError> {
    let gate = policy::new_gate(identity(), false)?;
    let acquire = Acquire {
        id: "1".repeat(64),
        capability_sha256: "2".repeat(64),
        kind: LeaseKind::Root {
            request_id: "request".into(),
        },
        parent: None,
    };
    assert_eq!(gate.phase, Phase::Provisioning);
    assert!(policy::acquire(&gate, &acquire, None, true).is_err());
    assert!(policy::acquire(&gate, &acquire, None, false).is_err());
    Ok(())
}

#[test]
fn full_import_stays_frozen_without_the_configured_ordinary_credential() -> Result<(), GatewayError>
{
    let (mut gate, plan, facts) = imported()?;
    let reference = policy::finalize(&mut gate, &plan, &facts, &"1".repeat(64), &empty_counts())?;
    assert_eq!(gate.phase, Phase::Frozen);
    assert!(policy::activate(&mut gate, &reference, &"2".repeat(64), false).is_err());
    assert_eq!(gate.phase, Phase::Frozen);
    assert!(!policy::activate(
        &mut gate,
        &reference,
        &"2".repeat(64),
        true
    )?);
    assert_eq!(gate.phase, Phase::Running);
    assert_eq!(gate.serving_epoch, 1);
    gate.phase = Phase::Draining;
    assert!(policy::activate(
        &mut gate,
        &reference,
        &"2".repeat(64),
        true
    )?);
    assert_eq!(
        gate.phase,
        Phase::Draining,
        "Old activation cannot reopen a later drain"
    );
    assert!(policy::finalize(&mut gate, &plan, &facts, &"1".repeat(64), &empty_counts()).is_err());
    Ok(())
}

#[test]
fn active_unknown_or_wrong_owner_imports_cannot_finalize() -> Result<(), GatewayError> {
    let (gate, plan, mut facts) = imported()?;
    let mut counts = empty_counts();
    counts.import_frames = 1;
    assert!(policy::finalize(&mut gate.clone(), &plan, &facts, &"1".repeat(64), &counts).is_err());
    counts.import_frames = 0;
    counts.blocked = 1;
    assert!(policy::finalize(&mut gate.clone(), &plan, &facts, &"1".repeat(64), &counts).is_err());
    facts.owner = Some(OwnerBinding {
        owner_id: "owner".into(),
        owner_epoch: "epoch".into(),
        key_fingerprint: "9".repeat(64),
        owner_revision: "revision".into(),
    });
    assert!(
        policy::finalize(
            &mut gate.clone(),
            &plan,
            &facts,
            &"1".repeat(64),
            &empty_counts()
        )
        .is_err()
    );
    Ok(())
}

#[test]
fn legacy_history_never_supplies_a_drain_binding() -> Result<(), GatewayError> {
    let gate = policy::new_gate(identity(), true)?;
    assert_eq!(gate.phase, Phase::LegacyRunning);
    assert!(
        policy::binding(
            &gate,
            &Binding {
                identity: identity(),
                serving_epoch: 0
            }
        )
        .is_err()
    );
    Ok(())
}

#[test]
fn v2_purposes_bind_the_complete_authorization_and_v1_remains_read_only() -> Result<(), GatewayError>
{
    let authorization = Authorization::Drain(Binding {
        identity: identity(),
        serving_epoch: 0,
    });
    let digest = authorization_digest(&authorization)?;
    assert_eq!(
        digest,
        "a70516e05accc27487fad3f41a0ef4696e64e30bc1a5d0f77e6b057273754e82"
    );
    let presented = token(
        &Scope::Control {
            purpose: Purpose::Drain,
            authorization_digest: digest,
        },
        &"1".repeat(64),
    )?;
    let configured = sha256_hex(presented.as_bytes());
    let intent = Intent::verify(Some(&presented), Some(&configured))?;
    assert!(intent.require_control(&authorization).is_ok());
    let substituted = Authorization::Drain(Binding {
        identity: identity(),
        serving_epoch: 1,
    });
    assert!(intent.require_control(&substituted).is_err());
    assert!(intent.require_export().is_err());
    let export = token(&Scope::Export, &"2".repeat(64))?;
    let export_digest = sha256_hex(export.as_bytes());
    let export = Intent::verify(Some(&export), Some(&export_digest))?;
    assert!(export.require_export().is_ok());
    assert!(export.require_control(&authorization).is_err());
    Ok(())
}
