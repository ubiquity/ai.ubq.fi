//! Actual proof-bearing reset candidates and admitted existing-only outcomes.
use crate::routing_types::{AdmittedAccount, CohortProof};
use gateway_core::identity::AccountBinding;

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
