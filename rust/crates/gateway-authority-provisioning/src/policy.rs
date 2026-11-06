//! Current writer capability verification; lifecycle transitions are separate.
use gateway_auth::crypto::sha256_hex;
use gateway_authority_lifecycle::model::{Acquire, Gate, Lease, LeaseKind, Permit, Phase};
use gateway_authority_lifecycle::policy::{closed, hex, refused};
use gateway_core::error::GatewayError;

pub fn valid_permit(gate: &Gate, permit: &Permit, lease: &Lease) -> Result<(), GatewayError> {
    if !hex(&permit.id, 64)
        || !hex(&permit.capability, 64)
        || lease.v != 1
        || lease.id != permit.id
        || lease.serving_epoch != permit.serving_epoch
        || gate.serving_epoch != permit.serving_epoch
        || lease.capability_sha256 != sha256_hex(permit.capability.as_bytes())
        || lease.terminal
    {
        return Err(refused(
            "Writer permit is absent, closed or belongs to another epoch",
        ));
    }
    Ok(())
}

pub fn authorize(gate: &Gate, lease: &Lease) -> Result<(), GatewayError> {
    match (&gate.phase, &lease.kind) {
        (
            Phase::Running | Phase::Draining | Phase::LegacyRunning,
            LeaseKind::Root { .. } | LeaseKind::NativeOperation(_) | LeaseKind::NativeJoin(_),
        ) => Ok(()),
        (Phase::Importing, LeaseKind::ImportFrame(value))
            if gate.import.as_ref() == Some(value) =>
        {
            Ok(())
        }
        _ => Err(closed()),
    }
}

pub fn acquire(
    gate: &Gate,
    value: &Acquire,
    parent: Option<&Lease>,
    ingress: bool,
) -> Result<(), GatewayError> {
    if !hex(&value.id, 64) || !hex(&value.capability_sha256, 64) {
        return Err(refused("Writer identity is invalid"));
    }
    match &value.kind {
        LeaseKind::Root { request_id } => {
            if request_id.is_empty() || request_id.len() > 128 || value.parent.is_some() {
                return Err(refused("Root binding is invalid"));
            }
            if matches!(gate.phase, Phase::LegacyRunning | Phase::Running) && ingress {
                return Ok(());
            }
        }
        LeaseKind::ImportFrame(import) => {
            if value.parent.is_none()
                && gate.phase == Phase::Importing
                && gate.import.as_ref() == Some(import)
            {
                return Ok(());
            }
        }
        LeaseKind::NativeOperation(native) | LeaseKind::NativeJoin(native) => {
            if !hex(&native.key_fingerprint, 64)
                || native.operation_id.is_empty()
                || native.action_ref.is_empty()
            {
                return Err(refused("Native operation selector is invalid"));
            }
            if let (Some(permit), Some(parent)) = (&value.parent, parent) {
                valid_permit(gate, permit, parent)?;
                authorize(gate, parent)?;
                match (&value.kind, &parent.kind) {
                    (LeaseKind::NativeOperation(_), LeaseKind::Root { .. }) => return Ok(()),
                    (LeaseKind::NativeJoin(a), LeaseKind::NativeOperation(b)) if a == b => {
                        return Ok(());
                    }
                    _ => {}
                }
            }
        }
    }
    Err(closed())
}
