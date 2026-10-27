//! Actual proof-bearing reset candidates and admitted existing-only outcomes.
use gateway_authority_reset_records::{ResetConfig, full_pool::FullPoolFence};
use gateway_authority_routing_selection::{types::AdmittedAccount, types::CohortProof};
use gateway_core::{identity::AccountBinding, identity::RequestContext};

#[derive(Clone, Debug)]
pub struct ResetCandidate {
    pub binding: AccountBinding,
    pub quota_reset_at_ms: i64,
    pub request_started_at_ms: i64,
    pub proof: CohortProof,
}

/// Existing-ledger-only preflight; none of these outcomes authorizes credit use.
#[derive(Debug)]
pub enum FullPoolResetReconciliation {
    NoExisting,
    Blocked { reason: String },
    Recovered(AdmittedAccount),
}

#[derive(Clone)]
pub struct Member {
    pub candidate: ResetCandidate,
    pub administrative_hash: String,
    pub enabled: bool,
}
#[derive(Clone)]
pub struct Facts {
    pub(crate) proof: CohortProof,
    pub(crate) original: RequestContext,
    pub(crate) config: ResetConfig,
    pub(crate) members: Vec<Member>,
    pub(crate) fences: Vec<FullPoolFence>,
    pub(crate) settings_version: String,
    pub(crate) episode_hash: String,
}

impl Facts {
    pub fn proof(&self) -> &CohortProof {
        &self.proof
    }
    pub fn original(&self) -> &RequestContext {
        &self.original
    }
    pub fn config(&self) -> &ResetConfig {
        &self.config
    }
    pub fn members(&self) -> &[Member] {
        &self.members
    }
    pub fn fences(&self) -> &[FullPoolFence] {
        &self.fences
    }
    pub fn episode_hash(&self) -> &String {
        &self.episode_hash
    }
    pub fn same(&self, right: &Self) -> bool {
        let left = self;

        left.episode_hash == right.episode_hash
            && left.settings_version == right.settings_version
            && left.fences == right.fences
            && left.config == right.config
            && left
                .members
                .iter()
                .map(|member| (&member.candidate.binding, member.enabled))
                .eq(right
                    .members
                    .iter()
                    .map(|member| (&member.candidate.binding, member.enabled)))
    }
}
