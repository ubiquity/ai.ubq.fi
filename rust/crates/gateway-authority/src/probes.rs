//! Bounded routing probe transitions and the overage serving admission.
use crate::{
    PROBE_LEASE_MS, add, invalid, keys,
    native_control::actor::{self, NativeActorScope, NativeRenewalActor},
    next_generation,
    routing::{class_name, read_view, validate_cohort},
    routing_types::{AdmittedAccount, CohortProof, ProbeLease, UnredeemedReset},
    store::{Authority, TokenSource},
    text, unavailable,
};
use gateway_core::{error::GatewayError, state::AuthorityStore};

impl<S: AuthorityStore, I: TokenSource> Authority<S, I> {
    /// Administrative R-only half-open transition, not provider reset proof or an election.
    pub async fn recheck_codex_slot(
        &self,
        actor: &NativeRenewalActor,
        config: &gateway_auth::types::AuthConfig,
        slot_number: u8,
    ) -> Result<bool, GatewayError> {
        if !(1..=2).contains(&slot_number) {
            return Ok(false);
        }
        for _ in 0..3 {
            let now = self.fresh_time(0)?;
            let mut tx = self.transaction(now)?;
            if actor::read_current(&mut tx, actor.stored(), config)
                .await?
                .scope
                != NativeActorScope::Admin
            {
                return Err(unavailable(
                    "admin_auth_required",
                    "Administrative authentication is required.",
                ));
            }
            let Some(raw) = tx.read::<serde_json::Value>(&keys::routing()).await? else {
                return Ok(false);
            };
            let mut state = crate::routing_codec::parse_routing_state(&raw)?;
            let Some(slot) = state.slots.get_mut(usize::from(slot_number - 1)) else {
                return Ok(false);
            };
            if slot.quota_blocked_until_ms.is_some_and(|v| v != 0) {
                for block in slot.quota_blocks.values_mut() {
                    block.blocked_until_ms = now;
                    block.observed_at_ms = Some(now);
                    block.reset_ambiguous = true;
                }
                let first = slot
                    .quota_blocked_classes
                    .iter()
                    .find_map(|class| slot.quota_blocks.get(class))
                    .or_else(|| slot.quota_blocks.values().next())
                    .cloned();
                slot.quota_blocked_until_ms = Some(now);
                if let Some(block) = first {
                    slot.quota_block_source = Some(block.source);
                    slot.retained_reset_at_ms = block.reset_at_ms.or(slot.retained_reset_at_ms);
                    slot.reset_identity_stable = block.reset_is_stable;
                }
                slot.reset_identity_ambiguous = true;
                slot.quota_signal_observed_at_ms = Some(now);
                slot.generation = next_generation(slot.generation)?;
                slot.probe_lease = None;
                state.updated_at_ms = now;
                tx.put(&keys::routing(), &state, None)?;
            }
            actor::validate_current_time(actor.stored(), self.fresh_time(now)?)?;
            if tx.commit().await? {
                return Ok(true);
            }
        }
        Ok(false)
    }

    /// Called only by the bounded probe route; permission is strongly checked here.
    pub async fn claim_probe(
        &self,
        admission: &AdmittedAccount,
        overage_reason: Option<&str>,
        now: i64,
    ) -> Result<Option<AdmittedAccount>, GatewayError> {
        // Source 0231b78: overage is a serving admission (admit_overage_fallback), never a lease.
        if overage_reason.is_some() {
            return Ok(None);
        }
        for _ in 0..3 {
            let mut tx = self.transaction(now)?;
            let mut view = read_view(&mut tx).await?;
            if !view.matches(admission) {
                return Ok(None);
            }
            let allowed = view
                .overage
                .get(&admission.binding.account_id_hash.0)
                .copied()
                .unwrap_or(false);
            let slot = &mut view.state.slots[admission.binding.slot as usize];
            if slot
                .probe_lease
                .as_ref()
                .is_some_and(|lease| lease.expires_at_ms > now)
            {
                return Ok(None);
            }
            if slot
                .quota_blocks
                .get(class_name(admission.quota_class))
                .is_some_and(|block| block.blocked_until_ms > now)
                && !allowed
            {
                return Ok(None);
            }
            let lease = ProbeLease {
                token: self.tokens.next_token()?,
                generation: slot.generation as i64,
                circuit: crate::routing_types::ProbeCircuit::Quota,
                quota_class: Some(admission.quota_class),
                expires_at_ms: add(now, PROBE_LEASE_MS)?,
            };
            text(&lease.token)?;
            slot.probe_lease = Some(lease.clone());
            let mut admitted = admission.clone();
            admitted.probe = Some(lease);
            admitted.probe_required = false;
            let mut active = view
                .active
                .ok_or_else(|| invalid("Probe is missing its active selection."))?;
            active.binding = admitted.binding.clone();
            active.updated_at_ms = now;
            tx.put(&keys::routing(), &view.state, None)?;
            tx.put(&keys::active(), &active, None)?;
            if tx.commit().await? {
                return Ok(Some(admitted));
            }
        }
        Err(unavailable(
            "codex_routing_unavailable",
            "Probe claim changed concurrently.",
        ))
    }

    pub async fn finish_probe(
        &self,
        admission: &AdmittedAccount,
        recovered: bool,
        now: i64,
    ) -> Result<bool, GatewayError> {
        let Some(expected) = &admission.probe else {
            return Ok(false);
        };
        for _ in 0..3 {
            let mut tx = self.transaction(now)?;
            let mut view = read_view(&mut tx).await?;
            if !view.current_binding(&admission.binding) {
                return Ok(false);
            }
            let slot = &mut view.state.slots[admission.binding.slot as usize];
            if slot.probe_lease.as_ref() != Some(expected) {
                return Ok(false);
            }
            slot.probe_lease = None;
            if recovered {
                slot.remove_quota_class(admission.quota_class);
                if slot.quota_blocks.is_empty() {
                    slot.retained_reset_at_ms = None;
                    slot.reset_identity_ambiguous = false;
                    slot.reset_identity_stable = false;
                    slot.recovery_pending = false;
                }
            }
            tx.put(&keys::routing(), &view.state, None)?;
            if tx.commit().await? {
                return Ok(true);
            }
        }
        Err(unavailable(
            "codex_routing_unavailable",
            "Probe completion changed concurrently.",
        ))
    }

    /// Source planOverageFallback after 0231b78: enabled overage is a serving path, not a
    /// single-flight probe, so concurrent requests never contend for a lease. Only a reason that
    /// proves no reset was redeemed qualifies, and the per-account setting stays a hard gate.
    pub async fn admit_overage_fallback(
        &self,
        proof: &CohortProof,
        unredeemed: &UnredeemedReset,
        now: i64,
    ) -> Result<Option<AdmittedAccount>, GatewayError> {
        if !unredeemed.allows_overage() {
            return Ok(None);
        }
        for _ in 0..3 {
            let mut tx = self.transaction(now)?;
            if !validate_cohort(&mut tx, proof, true).await? {
                return Ok(None);
            }
            let view = read_view(&mut tx).await?;
            let accounts = &view.pool.accounts;
            let find = |wanted: &dyn Fn(usize) -> bool| {
                (0..accounts.len()).find(|slot| accounts[*slot].enabled && wanted(*slot))
            };
            let selected = unredeemed
                .selected_account
                .as_deref()
                .and_then(|hash| find(&|slot| accounts[slot].admin_account_hash.0 == hash))
                .or_else(|| {
                    let active = view.active.as_ref()?;
                    find(&|slot| {
                        active.binding.slot == slot as u64
                            && active.binding.account_id_hash == accounts[slot].account_id_hash
                    })
                })
                .or_else(|| find(&|_| true));
            let Some(slot) = selected else {
                return Ok(None);
            };
            let Some(active) = view.active.as_ref() else {
                return Ok(None);
            };
            let admitted = AdmittedAccount {
                binding: view.binding(slot),
                active_generation: active.generation,
                requested_model: proof.model.clone(),
                quota_class: proof.quota_class,
                probe_required: false,
                probe: None,
            };
            // The per-account setting is a hard gate even for the active account.
            let allowed = view.overage.get(&accounts[slot].account_id_hash.0).copied();
            if allowed != Some(true) || !view.matches(&admitted) {
                return Ok(None);
            }
            if tx.commit().await? {
                return Ok(Some(admitted));
            }
        }
        Err(unavailable(
            "codex_routing_unavailable",
            "Overage admission changed concurrently.",
        ))
    }
}
