//! Provider-outcome reconciliation and verified-reset recovery, with no provider I/O.
use crate::{PROBE_LEASE_MS, add, invalid, next_generation, routing::read_view, store::Authority};
use crate::{reset::ResetAuthorization, reset::authorization_fences, reset::check_record};
use crate::{text, unavailable};
use gateway_authority_contracts::TokenSource;
use gateway_authority_keys as keys;
use gateway_authority_reset_evidence::{types::FullPoolResetReconciliation, types::ResetCandidate};
use gateway_authority_reset_permit::ResetPermitOperations;
use gateway_authority_reset_records::{ResetRecord, ResetState, reset_invocation};
use gateway_authority_routing_records::types::ProbeLease;
use gateway_authority_routing_selection::types::AdmittedAccount;
use gateway_core::{error::GatewayError, identity::ActiveSelection};
use gateway_core::{identity::ActiveTransitionReason, state::AuthorityStore};

fn existing_reset_block_reason(row: &ResetRecord, now: i64) -> Result<Option<&str>, GatewayError> {
    let error = row.last_error_code.as_deref();
    Ok(Some(match row.state {
        ResetState::Verified if row.verified_at_ms.is_some() => return Ok(None),
        ResetState::Claimed if row.lease_expires_at_ms <= now => "unsubmitted_transaction",
        ResetState::Claimed => "in_progress",
        ResetState::Submitted | ResetState::Unknown => "terminal_outcome_ambiguous",
        ResetState::Rejected => error.unwrap_or("previously_rejected"),
        ResetState::Verified => return Err(invalid("Verified reset timestamp is missing.")),
    }))
}

impl<S: AuthorityStore, I: TokenSource> Authority<S, I> {
    /// Source preexistingBankedReset: never request inventory or create a redemption/budget.
    pub async fn reconcile_full_pool_reset(
        &self,
        proof: &gateway_authority_routing_selection::types::CohortProof,
        original: &gateway_core::identity::RequestContext,
    ) -> Result<FullPoolResetReconciliation, GatewayError> {
        if !self.migrate_full_pool_reset_settings().await? {
            return Ok(FullPoolResetReconciliation::Blocked {
                reason: "configuration_unavailable".into(),
            });
        }
        for _ in 0..4 {
            let now = self.fresh_time(original.started_at_ms)?;
            let mut tx = self.transaction(now)?;
            let candidates = match crate::full_pool_reset::capture_reconciliation_candidates(
                &mut tx, proof, original,
            )
            .await
            {
                Ok(candidates) => candidates,
                Err(error) => {
                    return Ok(FullPoolResetReconciliation::Blocked {
                        reason: error
                            .envelope
                            .error
                            .code
                            .unwrap_or_else(|| "configuration_unavailable".into()),
                    });
                }
            };
            let view = read_view(&mut tx).await?;
            let mut existing = None;
            for candidate in candidates {
                let invocation = reset_invocation(
                    &view.pool.accounts[candidate.binding.slot as usize]
                        .admin_account_hash
                        .0,
                    &candidate.binding.credential_version.0,
                    candidate.quota_reset_at_ms,
                )?;
                let key = keys::reset_redemption(
                    &invocation.identity.account_id_hash,
                    &invocation.identity.quota_generation,
                );
                let Some(row) = tx.read::<ResetRecord>(&key).await? else {
                    continue;
                };
                if row.v != 1 || row.identity != invocation.identity {
                    return Err(invalid("Existing reset record context does not match."));
                }
                if let Some(reason) = existing_reset_block_reason(&row, now)? {
                    return Ok(FullPoolResetReconciliation::Blocked {
                        reason: reason.into(),
                    });
                }
                existing = Some((candidate, row));
                break;
            }
            let before_commit = self.fresh_time(now)?;
            if !tx.commit().await? {
                continue;
            }
            let Some((candidate, record)) = existing else {
                return Ok(FullPoolResetReconciliation::NoExisting);
            };
            let stale = record.routing_generation != candidate.binding.routing_generation;
            return Ok(
                match self
                    .elect_reset_recovery(
                        ResetAuthorization::ExistingLedger(&candidate, original),
                        &record,
                        before_commit,
                        stale,
                    )
                    .await?
                {
                    Some(account) => FullPoolResetReconciliation::Recovered(account),
                    None => FullPoolResetReconciliation::Blocked {
                        reason: "routing_fence_stale".into(),
                    },
                },
            );
        }
        Err(unavailable(
            "routing_fence_stale",
            "Existing reset reconciliation changed concurrently.",
        ))
    }

    /// Only the authenticated approved native provider boundary may supply observed outcomes.
    pub async fn record_reset_outcome(
        &self,
        expected: &ResetRecord,
        state: ResetState,
        receipt: Option<&str>,
        error: Option<&str>,
        now: i64,
    ) -> Result<ResetRecord, GatewayError> {
        if !matches!(
            state,
            ResetState::Unknown | ResetState::Verified | ResetState::Rejected
        ) {
            return Err(invalid("Invalid reset provider outcome."));
        }
        if let Some(receipt) = receipt {
            text(receipt)?;
            if receipt.len() > 512 {
                return Err(invalid("Reset receipt is too large."));
            }
        }
        for _ in 0..4 {
            let mut tx = self.transaction(now)?;
            let key = keys::reset_redemption(
                &expected.identity.account_id_hash,
                &expected.identity.quota_generation,
            );
            let mut row: ResetRecord = tx
                .read(&key)
                .await?
                .ok_or_else(|| unavailable("stale_owner", "Reset record is missing."))?;
            check_record(&row, expected)?;
            if matches!(row.state, ResetState::Verified | ResetState::Rejected) {
                if row.state == state {
                    return Ok(row);
                }
                return Err(invalid("Reset provider terminal is already immutable."));
            }
            if !matches!(row.state, ResetState::Submitted | ResetState::Unknown) {
                return Err(unavailable(
                    "stale_owner",
                    "Reset has not crossed submission.",
                ));
            }
            if row.lease_expires_at_ms <= now {
                return Err(unavailable("stale_owner", "Reset owner lease expired."));
            }
            row.state = state;
            row.updated_at_ms = now;
            if state == ResetState::Verified {
                row.verified_at_ms = Some(now);
            }
            if row.provider_receipt_id.is_none() {
                row.provider_receipt_id = receipt.map(str::to_owned);
            }
            row.last_error_code = error.map(|error| error.chars().take(120).collect());
            tx.put(&key, &row, None)?;
            if tx.commit().await? {
                return Ok(row);
            }
        }
        Err(unavailable(
            "stale_owner",
            "Reset outcome changed concurrently.",
        ))
    }

    pub async fn elect_verified_reset(
        &self,
        candidate: &ResetCandidate,
        expected: &ResetRecord,
        now: i64,
    ) -> Result<Option<AdmittedAccount>, GatewayError> {
        self.elect_reset_recovery(ResetAuthorization::Legacy(candidate), expected, now, false)
            .await
    }

    /// A documented reset terminal grants a fenced probe, never recovered capacity.
    pub async fn elect_full_pool_reset(
        &self,
        permit: &gateway_authority_reset_permit::types::AutomaticResetPermit,
        expected: &ResetRecord,
    ) -> Result<Option<AdmittedAccount>, GatewayError> {
        let now = self.fresh_time(0)?;
        self.elect_reset_recovery(
            ResetAuthorization::ExistingLedger(permit.candidate(), permit.original()),
            expected,
            now,
            expected.routing_generation != permit.candidate().binding.routing_generation,
        )
        .await
    }

    async fn elect_reset_recovery(
        &self,
        authorization: ResetAuthorization<'_>,
        expected: &ResetRecord,
        now: i64,
        stale: bool,
    ) -> Result<Option<AdmittedAccount>, GatewayError> {
        let candidate = authorization.candidate();
        for _ in 0..4 {
            let mut tx = self.transaction(now)?;
            let invocation = authorization_fences(&mut tx, authorization).await?;
            let key = keys::reset_redemption(
                &invocation.identity.account_id_hash,
                &invocation.identity.quota_generation,
            );
            let row: ResetRecord = tx
                .read(&key)
                .await?
                .ok_or_else(|| invalid("Verified reset record is missing."))?;
            if stale {
                if row.identity != expected.identity {
                    return Ok(None);
                }
            } else {
                check_record(&row, expected)?;
            }
            if row.state != ResetState::Verified
                || row.verified_at_ms.is_none()
                || (!stale && row.routing_generation != candidate.binding.routing_generation)
            {
                return Ok(None);
            }
            let mut view = read_view(&mut tx).await?;
            let slot_index = candidate.binding.slot as usize;
            let slot = &mut view.state.slots[slot_index];
            if slot
                .probe_lease
                .as_ref()
                .is_some_and(|lease| lease.expires_at_ms > now)
            {
                return Ok(None);
            }
            let released = slot
                .quota_block_for(candidate.proof.quota_class())
                .ok_or_else(|| unavailable("routing_fence_stale", "Reset quota class changed."))?;
            slot.remove_quota_class(candidate.proof.quota_class());
            slot.retained_reset_at_ms = released.reset_at_ms;
            slot.reset_identity_stable = released.reset_is_stable;
            slot.reset_identity_ambiguous = true;
            slot.recovery_pending = true;
            slot.generation = next_generation(slot.generation)?;
            let lease = ProbeLease {
                token: self.tokens.next_token()?,
                generation: slot.generation as i64,
                circuit: gateway_authority_routing_records::types::ProbeCircuit::Quota,
                quota_class: Some(candidate.proof.quota_class()),
                expires_at_ms: add(now, PROBE_LEASE_MS)?,
            };
            text(&lease.token)?;
            slot.probe_lease = Some(lease.clone());
            let binding = view.policy().binding(slot_index);
            let (active_generation, reason) = view.active.as_ref().map_or((1, None), |active| {
                (active.generation, active.transition_reason)
            });
            let changed = view
                .active
                .as_ref()
                .is_some_and(|active| active.binding.account_id_hash != binding.account_id_hash);
            let active_generation = if changed {
                next_generation(active_generation)?
            } else {
                active_generation
            };
            let active = ActiveSelection {
                v: 1,
                binding: binding.clone(),
                generation: active_generation,
                transition_reason: if changed {
                    Some(ActiveTransitionReason::QuotaExhausted)
                } else {
                    reason
                },
                updated_at_ms: now,
            };
            view.state.updated_at_ms = now;
            tx.put(&keys::routing(), &view.state, None)?;
            tx.put(&keys::active(), &active, None)?;
            let admitted = AdmittedAccount {
                binding,
                active_generation,
                requested_model: candidate.proof.model().clone(),
                quota_class: candidate.proof.quota_class(),
                probe_required: false,
                probe: Some(lease),
            };
            let before_commit = self.fresh_time(now)?;
            self.check_reset_authorization_deadline(authorization, before_commit)?;
            if admitted
                .probe
                .as_ref()
                .is_some_and(|lease| lease.expires_at_ms <= before_commit)
            {
                return Err(unavailable(
                    "routing_fence_stale",
                    "Reset recovery lease elapsed.",
                ));
            }
            if tx.commit().await? {
                return Ok(Some(admitted));
            }
        }
        Err(unavailable(
            "routing_fence_stale",
            "Verified reset election changed concurrently.",
        ))
    }

    /// A stale verified ledger permits one current-generation recovery probe, never another spend.
    /// It retains the reset identity as ambiguous and releases only this class's deadline.
    pub async fn claim_verified_reset_recovery(
        &self,
        candidate: &ResetCandidate,
        expected: &ResetRecord,
        now: i64,
    ) -> Result<Option<AdmittedAccount>, GatewayError> {
        self.elect_reset_recovery(ResetAuthorization::Legacy(candidate), expected, now, true)
            .await
    }
}
