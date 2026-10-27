//! Finite borrowed reset inputs and fixed current-source DATA; effects retain the original final CAS.
mod backend;
use backend::ReadTransaction;
use gateway_authority_contracts::{invalid, unavailable};
use gateway_authority_reset_evidence::types::ResetCandidate;
use gateway_authority_reset_permit::types::AutomaticResetPermit;
use gateway_authority_reset_records::utc_day;
use gateway_authority_reset_records::{ResetInvocation, ResetRecord, hash, reset_invocation};
use gateway_authority_routing_records::slot::class_name;
use gateway_core::state::AuthorityStore;
use gateway_core::{error::GatewayError, identity::RequestContext, state::RevisionCheck};
#[derive(Clone, Copy)]
pub enum ResetAuthorization<'a> {
    Legacy(&'a ResetCandidate),
    FullPool(&'a AutomaticResetPermit),
    ExistingLedger(&'a ResetCandidate, &'a RequestContext),
}
pub struct ResetChecks {
    checks: Vec<RevisionCheck>,
}
impl ResetChecks {
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
}
pub struct ResetFenceData {
    invocation: ResetInvocation,
    checks: Vec<RevisionCheck>,
}
impl ResetFenceData {
    pub fn invocation(&self) -> &ResetInvocation {
        &self.invocation
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
}
impl<'a> ResetAuthorization<'a> {
    pub fn candidate(self) -> &'a ResetCandidate {
        match self {
            Self::Legacy(candidate) | Self::ExistingLedger(candidate, _) => candidate,
            Self::FullPool(permit) => permit.candidate(),
        }
    }
    async fn validate_in_cache<S: AuthorityStore>(
        self,
        tx: &mut ReadTransaction<'_, S>,
    ) -> Result<(), GatewayError> {
        if let Self::FullPool(permit) = self {
            let now = tx.now;
            let evidence = {
                let reader = tx.routing_read_store();
                permit.current_facts(&reader, now).await?
            };
            for check in evidence.revision_checks() {
                if !tx.expect(&check.key, &check.expected_revision) {
                    return Err(invalid(
                        "Routing evidence conflicts with an existing transaction fence.",
                    ));
                }
            }
        }
        if let Self::ExistingLedger(candidate, original) = self {
            let now = tx.now;
            let evidence = {
                let reader = tx.routing_read_store();
                gateway_authority_reset_evidence::capture_existing_facts(
                    &reader,
                    &candidate.proof,
                    original,
                    now,
                )
                .await?
            };
            for check in evidence.revision_checks() {
                if !tx.expect(&check.key, &check.expected_revision) {
                    return Err(invalid(
                        "Routing evidence conflicts with an existing transaction fence.",
                    ));
                }
            }
            if !evidence
                .facts()
                .members()
                .iter()
                .filter(|member| member.enabled)
                .any(|member| {
                    member.candidate.binding == candidate.binding
                        && member.candidate.quota_reset_at_ms == candidate.quota_reset_at_ms
                })
            {
                return Err(unavailable(
                    "routing_fence_stale",
                    "Existing reset recovery context changed.",
                ));
            }
        }
        Ok(())
    }
    pub async fn current_data<S: AuthorityStore>(
        self,
        store: &S,
        now: i64,
    ) -> Result<ResetChecks, GatewayError> {
        let mut tx = ReadTransaction::new(store, now)?;
        self.validate_in_cache(&mut tx).await?;
        Ok(ResetChecks {
            checks: tx.checks(),
        })
    }
    pub async fn fence_data<S: AuthorityStore>(
        self,
        store: &S,
        now: i64,
    ) -> Result<ResetFenceData, GatewayError> {
        let mut tx = ReadTransaction::new(store, now)?;
        self.validate_in_cache(&mut tx).await?;
        let invocation = match self {
            Self::Legacy(candidate) => {
                let evidence = {
                    let reader = tx.routing_read_store();
                    gateway_authority_reset_evidence::capture_candidate_fences(
                        &reader, candidate, now,
                    )
                    .await?
                };
                for check in evidence.revision_checks() {
                    if !tx.expect(&check.key, &check.expected_revision) {
                        return Err(invalid(
                            "Routing evidence conflicts with an existing transaction fence.",
                        ));
                    }
                }
                evidence.invocation().clone()
            }
            Self::FullPool(permit) => permit.invocation()?,
            Self::ExistingLedger(candidate, _) => {
                let evidence = {
                    let reader = tx.routing_read_store();
                    gateway_authority_routing_evidence::reader::RoutingEvidenceReader::new(&reader,now)?.read(gateway_authority_routing_evidence::reader::RoutingReadMode::GlobalSelection).await?
                };
                for check in evidence.revision_checks() {
                    if !tx.expect(&check.key, &check.expected_revision) {
                        return Err(invalid(
                            "Routing evidence conflicts with an existing transaction fence.",
                        ));
                    }
                }
                reset_invocation(
                    &evidence.pool().accounts[candidate.binding.slot as usize]
                        .admin_account_hash
                        .0,
                    &candidate.binding.credential_version.0,
                    candidate.quota_reset_at_ms,
                )?
            }
        };
        Ok(ResetFenceData {
            invocation,
            checks: tx.checks(),
        })
    }
}
pub fn live_clock(
    record: &ResetRecord,
    candidate: &ResetCandidate,
    now: i64,
) -> Result<(), GatewayError> {
    if candidate.quota_reset_at_ms <= now {
        return Err(unavailable(
            "quota_window_expired",
            "The quota window has already recovered.",
        ));
    }
    if record.lease_expires_at_ms <= now {
        return Err(unavailable("stale_owner", "Reset ownership lease expired."));
    }
    if utc_day(record.created_at_ms)? != utc_day(now)? {
        return Err(unavailable(
            "claim_day_elapsed",
            "Reset claim crossed its UTC day.",
        ));
    }
    Ok(())
}
pub fn shadow_episode(candidate: &ResetCandidate) -> String {
    let value = serde_json::json!({"model":candidate.proof.model(),"class":class_name(candidate.proof.quota_class()),"bindings":candidate.proof.account_bindings(),"reset":candidate.quota_reset_at_ms});
    hash(&value.to_string())
}
