use gateway_auth::crypto::sha256_hex;
use gateway_authority_provisioning::{model::*, policy};
use gateway_core::error::GatewayError;

fn binding() -> NativeBinding {
    NativeBinding {
        operation_id: "operation".into(),
        action_ref: "action".into(),
        owner_id: "owner".into(),
        owner_epoch: "epoch".into(),
        key_fingerprint: "a".repeat(64),
    }
}

#[test]
fn native_selector_needs_a_registered_identity_and_at_least_one_reference() {
    let expected = binding();
    let mut selector = NativeSelector {
        operation_id: Some("operation".into()),
        action_ref: None,
        owner_id: "owner".into(),
        owner_epoch: "epoch".into(),
        key_fingerprint: "a".repeat(64),
    };
    assert!(selector.matches(&expected));
    selector.action_ref = Some("other".into());
    assert!(!selector.matches(&expected));
    selector.action_ref = Some("action".into());
    selector.owner_id = "foreign".into();
    assert!(!selector.matches(&expected));
    selector.owner_id = "owner".into();
    selector.operation_id = None;
    selector.action_ref = None;
    assert!(!selector.matches(&expected));
}

#[test]
fn old_admitted_request_can_register_renewal_after_drain_but_new_root_cannot()
-> Result<(), GatewayError> {
    let mut gate = policy::new_gate(
        Identity {
            object_id: "a".repeat(64),
            source_sha: "b".repeat(40),
            source_tree: "c".repeat(40),
        },
        false,
    )?;
    gate.phase = Phase::Draining;
    let permit = Permit {
        id: "d".repeat(64),
        serving_epoch: 0,
        capability: "e".repeat(64),
    };
    let root = Lease {
        v: 1,
        id: permit.id.clone(),
        serving_epoch: 0,
        capability_sha256: sha256_hex(permit.capability.as_bytes()),
        parent_id: None,
        kind: LeaseKind::Root {
            request_id: "request".into(),
        },
        blocker: None,
        terminal: false,
    };
    let child = Acquire {
        id: "f".repeat(64),
        capability_sha256: "0".repeat(64),
        kind: LeaseKind::NativeOperation(binding()),
        parent: Some(permit.clone()),
    };
    assert!(policy::acquire(&gate, &child, Some(&root), false).is_ok());
    let fresh = Acquire {
        id: "1".repeat(64),
        capability_sha256: "2".repeat(64),
        kind: LeaseKind::Root {
            request_id: "another".into(),
        },
        parent: None,
    };
    assert!(policy::acquire(&gate, &fresh, None, true).is_err());
    let mut substituted = permit;
    substituted.capability = "3".repeat(64);
    let forged = Acquire {
        parent: Some(substituted),
        ..child
    };
    assert!(policy::acquire(&gate, &forged, Some(&root), true).is_err());
    Ok(())
}
