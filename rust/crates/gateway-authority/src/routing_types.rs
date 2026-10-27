//! Source-shaped capacity records, selection results and private proof capabilities.
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
