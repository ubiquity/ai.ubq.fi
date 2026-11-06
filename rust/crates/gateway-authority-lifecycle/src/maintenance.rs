//! Purpose-bound maintenance revisions never stand in for writer completion.
use crate::{model::*, policy};
use gateway_auth::crypto::sha256_hex;
use gateway_core::error::GatewayError;
const MAX_REVISION: u64 = gateway_core::identity::MAX_SAFE_INTEGER as u64;

fn cancellation_revision(
    plan: &CancelDrainPlan,
    next: u64,
    digest: &str,
) -> Result<String, GatewayError> {
    Ok(sha256_hex(
        &serde_json::to_vec(&(
            "uos-drain-cancellation-v1",
            &plan.binding.identity.object_id,
            &plan.binding.identity.source_sha,
            &plan.binding.identity.source_tree,
            plan.binding.serving_epoch,
            plan.drain_revision,
            next,
            &plan.drain_digest,
            digest,
        ))
        .map_err(|_| policy::refused("Cancellation receipt cannot be encoded"))?,
    ))
}

pub fn validate(gate: &Gate) -> Result<(), GatewayError> {
    policy::identity(&gate.identity)?;
    if gate.v != 1
        || gate.serving_epoch > MAX_REVISION
        || gate.drain_revision > MAX_REVISION
        || gate
            .drain_digest
            .as_ref()
            .is_some_and(|value| !policy::hex(value, 64))
        || (gate.phase == Phase::Running && gate.drain_digest.is_some())
        || (gate.phase == Phase::Draining && gate.drain_digest.is_none())
    {
        return Err(policy::refused("Maintenance metadata is invalid"));
    }
    if let Some(value) = &gate.last_cancellation {
        if !matches!(gate.phase, Phase::Running | Phase::Draining | Phase::Frozen) {
            return Err(policy::refused(
                "Cancellation metadata has an invalid phase",
            ));
        }
        policy::binding(gate, &value.plan.binding)?;
        if value.plan.drain_revision.checked_add(1) != Some(value.next_drain_revision)
            || value.next_drain_revision != gate.drain_revision
            || !policy::hex(&value.plan.drain_digest, 64)
            || !policy::hex(&value.cancel_digest, 64)
            || value.cancellation_revision
                != cancellation_revision(
                    &value.plan,
                    value.next_drain_revision,
                    &value.cancel_digest,
                )?
        {
            return Err(policy::refused("Cancellation metadata is invalid"));
        }
    } else if gate.drain_revision != 0 {
        return Err(policy::refused(
            "Maintenance revision has no causal cancellation",
        ));
    }
    if let Some(reference) = &gate.frozen {
        let epoch_matches = if gate.phase == Phase::Frozen {
            reference.serving_epoch == gate.serving_epoch
        } else {
            matches!(gate.phase, Phase::Running | Phase::Draining)
                && reference.serving_epoch.checked_add(1) == Some(gate.serving_epoch)
        };
        if !epoch_matches
            || reference.identity != gate.identity
            || !policy::hex(&reference.freeze_id, 64)
            || !policy::hex(&reference.freeze_revision, 64)
            || !policy::hex(&reference.capture_id, 64)
            || !policy::hex(&reference.finalization_digest, 64)
            || [
                reference.roots,
                reference.native_operations,
                reference.native_joins,
                reference.import_frames,
                reference.blocked,
            ]
            .iter()
            .any(|value| *value != 0)
        {
            return Err(policy::refused("Historical Frozen metadata is invalid"));
        }
    } else if matches!(gate.phase, Phase::Running | Phase::Draining | Phase::Frozen) {
        return Err(policy::refused("Tracked serving has no finalized source"));
    }
    Ok(())
}

pub fn drain(
    gate: &mut Gate,
    plan: &DrainPlan,
    digest: &str,
    counts: &Counts,
    ingress: bool,
) -> Result<Option<FrozenRef>, GatewayError> {
    validate(gate)?;
    policy::binding(gate, &plan.binding)?;
    if plan.drain_revision != gate.drain_revision || !policy::hex(digest, 64) {
        return Err(policy::refused("Drain revision is stale"));
    }
    if gate.phase == Phase::Frozen
        && gate
            .frozen
            .as_ref()
            .is_some_and(|value| value.freeze_id == digest)
    {
        return Ok(gate.frozen.clone());
    }
    if !ingress
        || !matches!(gate.phase, Phase::Running | Phase::Draining)
        || gate.drain_digest.as_ref().is_some_and(|old| old != digest)
        || (gate.drain_revision == MAX_REVISION && !counts.empty())
    {
        return Err(policy::refused(
            "Drain cannot start from the current authority",
        ));
    }
    gate.phase = Phase::Draining;
    gate.drain_digest = Some(digest.into());
    if counts.empty() {
        let previous = gate
            .frozen
            .clone()
            .ok_or_else(|| policy::refused("Drain has no finalized source"))?;
        policy::freeze(
            gate,
            digest,
            previous.capture_id,
            previous.finalization_digest,
        )?;
    }
    Ok(None)
}

pub fn cancel(
    gate: &mut Gate,
    plan: &CancelDrainPlan,
    digest: &str,
    ingress: bool,
) -> Result<(DrainCancellation, bool), GatewayError> {
    validate(gate)?;
    policy::binding(gate, &plan.binding)?;
    if !ingress
        || plan.drain_revision > MAX_REVISION
        || !policy::hex(digest, 64)
        || !policy::hex(&plan.drain_digest, 64)
    {
        return Err(policy::refused("Cancellation authorization is invalid"));
    }
    if let Some(value) = &gate.last_cancellation {
        if gate.phase == Phase::Running
            && gate.drain_digest.is_none()
            && &value.plan == plan
            && value.cancel_digest == digest
            && value.next_drain_revision == gate.drain_revision
        {
            return Ok((value.clone(), true));
        }
    }
    if gate.phase != Phase::Draining
        || plan.drain_revision != gate.drain_revision
        || gate.drain_digest.as_deref() != Some(plan.drain_digest.as_str())
        || gate
            .frozen
            .as_ref()
            .is_some_and(|value| value.freeze_id == plan.drain_digest)
    {
        return Err(policy::refused(
            "Cancellation does not name the current unfinished drain",
        ));
    }
    let next = gate
        .drain_revision
        .checked_add(1)
        .filter(|value| *value <= MAX_REVISION)
        .ok_or_else(|| policy::refused("Maintenance revision overflow"))?;
    let value = DrainCancellation {
        plan: plan.clone(),
        next_drain_revision: next,
        cancel_digest: digest.into(),
        cancellation_revision: cancellation_revision(plan, next, digest)?,
    };
    gate.phase = Phase::Running;
    gate.drain_digest = None;
    gate.drain_revision = next;
    gate.last_cancellation = Some(value.clone());
    Ok((value, false))
}

pub fn cancelled(gate: &Gate, plan: &DrainPlan, digest: &str) -> Option<DrainCancellation> {
    gate.last_cancellation
        .as_ref()
        .filter(|value| {
            value.plan.binding == plan.binding
                && value.plan.drain_revision == plan.drain_revision
                && value.plan.drain_digest == digest
        })
        .cloned()
}
