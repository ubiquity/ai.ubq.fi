use gateway_authority_reset_evidence::{types::Facts, types::ResetCandidate};
use gateway_authority_reset_records::full_pool::FullPoolShadowDecision;
use gateway_authority_reset_records::full_pool::SourceResetCredit;
use gateway_contracts::{identity::AccountBinding, identity::RequestContext};
use gateway_source::error::GatewayError;
use gateway_source::js_string::JsString16;
use serde::Serialize;
pub enum FullPoolPreparation {
    Declined { reason: String },
    Cached { decision: FullPoolShadowDecision },
    Observe(Box<FullPoolResetPlan>),
}
pub enum FullPoolEvaluation {
    Declined {
        reason: String,
    },
    Shadow {
        decision: FullPoolShadowDecision,
        duplicate: bool,
    },
    Ready(Box<AutomaticResetPermit>),
}
pub struct FullPoolResetPlan {
    pub(super) facts: Facts,
    pub(super) audit_present: bool,
}
impl FullPoolResetPlan {
    pub fn bindings(&self) -> impl Iterator<Item = &AccountBinding> {
        self.facts
            .members()
            .iter()
            .filter(|member| member.enabled)
            .map(|member| &member.candidate.binding)
    }
    pub fn original(&self) -> &RequestContext {
        self.facts.original()
    }
}

/// Minted only by live evaluation in the current complete source transaction; never reconstructed from wire.
/// ```compile_fail
/// use gateway_authority_reset_permit::types::AutomaticResetPermit;
/// let _: AutomaticResetPermit = serde_json::from_str("{}").unwrap();
/// ```
pub struct AutomaticResetPermit {
    pub(super) facts: Facts,
    pub(super) candidate: ResetCandidate,
    pub(super) decision: FullPoolShadowDecision,
    pub(super) credit: SourceResetCredit,
    pub(super) observed_at_ms: i64,
}
impl AutomaticResetPermit {
    pub fn candidate(&self) -> &ResetCandidate {
        &self.candidate
    }
    pub fn binding(&self) -> &AccountBinding {
        &self.candidate.binding
    }
    pub fn credit_id(&self) -> &JsString16 {
        &self.credit.credit_id
    }
    pub fn episode_hash(&self) -> &str {
        &self.decision.episode_hash
    }
    pub fn original(&self) -> &RequestContext {
        self.facts.original()
    }
    pub fn invocation(
        &self,
    ) -> Result<gateway_authority_reset_records::ResetInvocation, GatewayError> {
        gateway_authority_reset_records::reset_invocation(
            self.decision
                .selected_account_id_hash
                .as_deref()
                .ok_or_else(super::invalid)?,
            &self.candidate.binding.credential_version.0,
            self.candidate.quota_reset_at_ms,
        )
    }
    pub fn check_deadline(&self, now: i64) -> Result<(), GatewayError> {
        if now < self.observed_at_ms
            || now - self.observed_at_ms > 30_000
            || self.decision.expires_at_ms <= now
            || self
                .credit
                .expires_at_ms
                .is_some_and(|expiry| expiry <= now)
        {
            return Err(super::declined("inventory_response_invalid_or_expired"));
        }
        Ok(())
    }
    pub fn consume_body(&self) -> Result<gateway_source::source_json::SourceJson, GatewayError> {
        #[derive(Serialize)]
        struct Body<'a> {
            redeem_request_id: &'a str,
            credit_id: &'a JsString16,
        }
        let invocation = self.invocation()?;
        gateway_source::source_json::SourceJson::from_serializable(&Body {
            redeem_request_id: &invocation.idempotency_key,
            credit_id: &self.credit.credit_id,
        })
    }
}

impl AutomaticResetPermit {
    /// Fixed current DATA only; every caller must keep these fences in its existing effect CAS.
    pub async fn current_facts<S: gateway_core::state::AuthorityStore>(
        &self,
        store: &S,
        now: i64,
    ) -> Result<gateway_authority_reset_evidence::ResetFactsEvidence, GatewayError> {
        self.check_deadline(now)?;
        let evidence = gateway_authority_reset_evidence::capture_spend_facts(
            store,
            self.facts.proof(),
            self.facts.original(),
            now,
        )
        .await?;
        if evidence.facts().config().mode != gateway_authority_reset_records::ResetMode::Live
            || !evidence.facts().same(&self.facts)
        {
            return Err(super::declined("routing_fence_stale"));
        }
        Ok(evidence)
    }
}
macro_rules! opaque_debug {
    ($($name:ty),+ $(,)?) => { $(impl std::fmt::Debug for $name {
        fn fmt(&self, formatter:&mut std::fmt::Formatter<'_>) -> std::fmt::Result { formatter.write_str(concat!(stringify!($name), "([opaque])")) }
    })+ };
}
opaque_debug!(
    FullPoolResetPlan,
    AutomaticResetPermit,
    FullPoolPreparation,
    FullPoolEvaluation
);
