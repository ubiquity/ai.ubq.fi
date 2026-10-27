//! Source selection DTOs and the opaque cohort capability minted only by checked selection.
use gateway_authority_routing_records::types::ProbeLease;
use gateway_core::identity::AccountBinding;
use gateway_core::provider::CodexQuotaClass;
use gateway_core::state::RevisionCheck;
use gateway_core::{js_string::JsString16, model::ModelId};
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct AdmittedAccount {
    pub binding: AccountBinding,
    pub active_generation: u64,
    pub requested_model: ModelId,
    pub quota_class: CodexQuotaClass,
    pub probe_required: bool,
    pub probe: Option<ProbeLease>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct ScopedOverageProbe {
    pub binding: AccountBinding,
    pub requested_model: ModelId,
    pub quota_class: CodexQuotaClass,
    pub probe: ProbeLease,
}

/// Constructed only by a successful strong selection. Never deserialize an external proof.
/// ```compile_fail
/// use gateway_authority_routing_selection::types::CohortProof;
/// let _: CohortProof = serde_json::from_str("{}").unwrap();
/// ```
/// ```compile_fail
/// use gateway_authority_routing_selection::types::CohortProof;
/// let _ = CohortProof { checks: vec![], model: "x".into(), quota_class: gateway_core::provider::CodexQuotaClass::Standard, exhausted: true, unavailable: true, bindings: vec![] };
/// ```
#[derive(Clone, Debug)]
pub struct CohortProof {
    pub(crate) checks: Vec<RevisionCheck>,
    pub(crate) model: ModelId,
    pub(crate) quota_class: CodexQuotaClass,
    pub(crate) exhausted: bool,
    pub(crate) unavailable: bool,
    pub(crate) bindings: Vec<AccountBinding>,
}

impl CohortProof {
    /// Data for a consumer's existing effect CAS, never a standalone authorization.
    pub fn revision_fences(&self) -> &[RevisionCheck] {
        &self.checks
    }
    /// Pure comparison over the actual reader result. The caller must hydrate it through its private SAME effect read cache and keep all fences in the original final CAS.
    pub fn matches_current_evidence(
        &self,
        evidence: &gateway_authority_routing_evidence::reader::RoutingEvidence,
        quota_only: bool,
    ) -> bool {
        evidence.mode()
            == gateway_authority_routing_evidence::reader::RoutingReadMode::GlobalSelection
            && self.checks.iter().all(|check| {
                evidence
                    .revision_checks()
                    .iter()
                    .any(|current| current == check)
            })
            && gateway_authority_routing_policy::cohort_matches(
                &gateway_authority_routing_policy::RoutingPolicyData::from_evidence(evidence),
                &self.bindings,
                &self.model,
                self.quota_class,
                quota_only,
                evidence.observed_at_ms(),
            )
    }
    pub fn full_cohort_exhausted(&self) -> bool {
        self.exhausted
    }
    pub fn full_cohort_unavailable(&self) -> bool {
        self.unavailable
    }
    pub fn selected_count(&self) -> usize {
        self.bindings.len()
    }
    pub fn model(&self) -> &ModelId {
        &self.model
    }
    pub fn account_bindings(&self) -> &[AccountBinding] {
        &self.bindings
    }
    pub fn quota_class(&self) -> CodexQuotaClass {
        self.quota_class
    }
}

#[derive(Clone, Debug)]
pub enum RouteSelection {
    Eligible(AdmittedAccount),
    QuotaBlocked {
        retry_at_ms: Option<i64>,
        proof: CohortProof,
    },
    CredentialsInvalid {
        proof: CohortProof,
    },
    ModelUnavailable {
        model: ModelId,
        detail: Option<JsString16>,
    },
    RoutingUnavailable,
}

/// Non-inference account identity. The source model remains optional UTF16 and is never replaced.
#[derive(Clone, Debug, PartialEq)]
pub struct LiveAccountSelection {
    pub binding: AccountBinding,
    pub active_generation: u64,
    pub quota_class: CodexQuotaClass,
    pub probe_required: bool,
    pub model: Option<gateway_core::js_string::JsString16>,
}
#[derive(Clone, Debug, PartialEq)]
pub enum LiveRouteSelection {
    Eligible(LiveAccountSelection),
    QuotaBlocked { retry_at_ms: Option<i64> },
    CredentialsInvalid,
    ModelUnavailable { detail: Option<JsString16> },
    RoutingUnavailable,
}
