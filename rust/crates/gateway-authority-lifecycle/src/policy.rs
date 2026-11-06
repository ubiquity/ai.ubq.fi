use crate::model::{
    Authorization, Binding, Counts, FinalizeFacts, FinalizePlan, FrozenRef, Gate, Identity, Lease,
    LeaseKind, OwnerExpectation, Phase,
};
use gateway_auth::crypto::sha256_hex;
use gateway_core::error::GatewayError;

pub const MAX_LEASES: usize = 4096;

pub fn refused(message: &str) -> GatewayError {
    GatewayError::new(409, message, Some("state_transfer_control_refused"))
}

pub fn closed() -> GatewayError {
    GatewayError::new(
        503,
        "The gateway authority is closed for provisioning or maintenance",
        Some("state_transfer_ingress_closed"),
    )
}

pub fn hex(value: &str, length: usize) -> bool {
    value.len() == length
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

pub fn identity(value: &Identity) -> Result<(), GatewayError> {
    if !hex(&value.object_id, 64) || !hex(&value.source_sha, 40) || !hex(&value.source_tree, 40) {
        return Err(refused("Control identity is invalid"));
    }
    Ok(())
}

pub fn new_gate(value: Identity, occupied: bool) -> Result<Gate, GatewayError> {
    identity(&value)?;
    Ok(Gate {
        v: 1,
        identity: value,
        serving_epoch: 0,
        phase: if occupied {
            Phase::LegacyRunning
        } else {
            Phase::Provisioning
        },
        import: None,
        frozen: None,
        activation_digest: None,
        drain_digest: None,
        drain_revision: 0,
        last_cancellation: None,
    })
}

pub fn binding(gate: &Gate, value: &Binding) -> Result<(), GatewayError> {
    if gate.v != 1
        || gate.identity != value.identity
        || gate.serving_epoch != value.serving_epoch
        || gate.phase == Phase::LegacyRunning
    {
        return Err(refused(
            "Control binding is stale or its history is untracked",
        ));
    }
    Ok(())
}

pub fn counts(leases: &[Lease]) -> Counts {
    let mut count = Counts {
        roots: 0,
        native_operations: 0,
        native_joins: 0,
        import_frames: 0,
        blocked: 0,
    };
    for lease in leases {
        match lease.kind {
            LeaseKind::Root { .. } => count.roots += 1,
            LeaseKind::NativeOperation(_) => count.native_operations += 1,
            LeaseKind::NativeJoin(_) => count.native_joins += 1,
            LeaseKind::ImportFrame(_) => count.import_frames += 1,
        }
        count.blocked += u64::from(lease.blocker.is_some());
    }
    count
}

pub fn freeze(
    gate: &mut Gate,
    digest: &str,
    capture_id: String,
    finalization_digest: String,
) -> Result<FrozenRef, GatewayError> {
    if !hex(digest, 64) || !hex(&capture_id, 64) {
        return Err(refused("Freeze identity is invalid"));
    }
    let revision = sha256_hex(
        &serde_json::to_vec(&(
            &gate.identity,
            gate.serving_epoch,
            digest,
            &capture_id,
            &finalization_digest,
        ))
        .map_err(|_| refused("Freeze cannot be encoded"))?,
    );
    let reference = FrozenRef {
        identity: gate.identity.clone(),
        serving_epoch: gate.serving_epoch,
        freeze_id: digest.into(),
        freeze_revision: revision,
        capture_id,
        finalization_digest,
        roots: 0,
        native_operations: 0,
        native_joins: 0,
        import_frames: 0,
        blocked: 0,
    };
    gate.phase = Phase::Frozen;
    gate.frozen = Some(reference.clone());
    Ok(reference)
}

pub fn finalize(
    gate: &mut Gate,
    plan: &FinalizePlan,
    facts: &FinalizeFacts,
    digest: &str,
    counts: &Counts,
) -> Result<FrozenRef, GatewayError> {
    if let Some(reference) = &gate.frozen {
        if gate.phase == Phase::Frozen
            && reference.finalization_digest == digest
            && reference.capture_id == plan.capture_id
            && reference.identity == plan.binding.identity
            && reference.serving_epoch == plan.binding.serving_epoch
        {
            return Ok(reference.clone());
        }
    }
    binding(gate, &plan.binding)?;
    let import = gate
        .import
        .as_ref()
        .ok_or_else(|| refused("No exclusive import exists"))?;
    let owner_matches = match &plan.expected_owner {
        OwnerExpectation::Absent => facts.owner.is_none(),
        OwnerExpectation::Bound(owner) => facts.owner.as_ref() == Some(owner),
    };
    let expected = plan
        .staged_records
        .checked_add(1)
        .and_then(|v| {
            v.checked_add(u64::from(matches!(
                plan.expected_owner,
                OwnerExpectation::Bound(_)
            )))
        })
        .ok_or_else(|| refused("Inventory overflow"))?;
    if gate.phase != Phase::Importing
        || !counts.empty()
        || facts.progress_present
        || !owner_matches
        || import.capture_id != plan.capture_id
        || import.records != plan.staged_records
        || import.chain_digest != plan.final_chain_digest
        || facts.manifest_capture_id != plan.capture_id
        || facts.manifest_revision != plan.manifest_revision
        || facts.canonical_records != expected
    {
        return Err(refused(
            "Import is incomplete, active, unknown or has a mismatched owner/inventory",
        ));
    }
    freeze(gate, digest, plan.capture_id.clone(), digest.into())
}

pub fn activate(
    gate: &mut Gate,
    reference: &FrozenRef,
    digest: &str,
    ingress: bool,
) -> Result<bool, GatewayError> {
    if !ingress {
        return Err(refused("Configured ordinary ingress credential is absent"));
    }
    if gate.activation_digest.as_deref() == Some(digest)
        && gate.phase == Phase::Running
        && reference.serving_epoch.checked_add(1) == Some(gate.serving_epoch)
        && gate.frozen.as_ref() == Some(reference)
    {
        return Ok(true);
    }
    if !ingress
        || gate.phase != Phase::Frozen
        || reference.serving_epoch != gate.serving_epoch
        || gate.frozen.as_ref() != Some(reference)
        || !hex(digest, 64)
    {
        return Err(refused(
            "Activation requires its exact frozen epoch and configured ordinary ingress credential",
        ));
    }
    gate.serving_epoch = gate
        .serving_epoch
        .checked_add(1)
        .filter(|v| *v <= 9_007_199_254_740_991)
        .ok_or_else(|| refused("Serving epoch overflow"))?;
    gate.phase = Phase::Running;
    gate.activation_digest = Some(digest.into());
    gate.drain_digest = None;
    gate.drain_revision = 0;
    gate.last_cancellation = None;
    Ok(false)
}

pub fn authorization_binding(value: &Authorization) -> Binding {
    match value {
        Authorization::FinalizeImport(plan) => plan.binding.clone(),
        Authorization::Drain(value) => value.binding.clone(),
        Authorization::CancelDrain(value) => value.binding.clone(),
        Authorization::Activate(value) => Binding {
            identity: value.identity.clone(),
            serving_epoch: value.serving_epoch,
        },
    }
}
