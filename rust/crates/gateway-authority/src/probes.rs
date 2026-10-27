//! Actor-bound administrative half-open transition. Actor semantic migration remains held.
use crate::{native_control::actor, native_control::actor::NativeActorScope};
use crate::{native_control::actor::NativeRenewalActor, store::Authority};
use gateway_authority_contracts::{TokenSource, next_generation, unavailable};
use gateway_authority_keys as keys;
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
            let mut state = gateway_authority_routing_records::codec::parse_routing_state(&raw)?;
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
}
