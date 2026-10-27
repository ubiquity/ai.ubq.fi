//! Actual proof-bearing authorization remains with checked reservation owners.
use crate::routing_types::CohortProof;
use gateway_authority_claude_proof::ClaudeCohortProof;

#[derive(Clone, Debug)]
pub enum PaidAuthorization {
    SubscriptionUnavailable(CohortProof),
    ClaudeSubscriptionUnavailable(ClaudeCohortProof),
    DirectPaidModel,
}
