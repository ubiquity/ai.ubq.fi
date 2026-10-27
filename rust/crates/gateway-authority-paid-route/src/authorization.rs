//! Actual proof-bearing authorization remains with checked reservation owners.
use gateway_authority_claude_proof::ClaudeCohortProof;
use gateway_authority_routing_selection::types::CohortProof;

#[derive(Clone, Debug)]
pub enum PaidAuthorization {
    SubscriptionUnavailable(CohortProof),
    ClaudeSubscriptionUnavailable(ClaudeCohortProof),
    DirectPaidModel,
}
