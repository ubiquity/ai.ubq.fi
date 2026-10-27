//! Opaque selection and one-use response capabilities. Minting and final quota CAS stay in this owner.
use gateway_claude_policy::{host::ResponseObservation, response::Classification};
use gateway_claude_roster::{claude::AccountBinding, claude_generation::StateSnapshot};
use gateway_claude_usage_quota::{QuotaObservation, QuotaState};
use gateway_core::{model::ModelId, state::RevisionCheck};
use gateway_providers::types::{Headers, PrepareOptions};
use gateway_source::{js_string::JsString16, source_json::SourceJson};
#[derive(Clone)]
pub struct SelectedAccount {
    pub(crate) binding: AccountBinding,
    pub(crate) checks: Vec<RevisionCheck>,
    pub(crate) catalog_identity: String,
    pub(crate) generation: StateSnapshot,
    pub(crate) options: PrepareOptions,
    pub(crate) replay_scope: String,
    pub(crate) key_id: String,
    pub(crate) model: ModelId,
    pub(crate) raw: SourceJson,
    pub(crate) effective_effort: Option<JsString16>,
    pub(crate) disabled: bool,
    pub(crate) native_checks: Vec<RevisionCheck>,
    pub(crate) workspace_continuation: bool,
}
impl std::fmt::Debug for SelectedAccount {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SelectedAccount")
            .field("account_id", &self.binding.account_id)
            .field("cohort_id", &self.binding.account_cohort_id)
            .field("catalog_identity", &self.catalog_identity)
            .finish_non_exhaustive()
    }
}
impl SelectedAccount {
    pub fn effective_effort(&self) -> Option<&JsString16> {
        self.effective_effort.as_ref()
    }
    pub fn binding(&self) -> &AccountBinding {
        &self.binding
    }
    pub fn checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
    pub fn catalog_identity(&self) -> &str {
        &self.catalog_identity
    }
    pub fn replay_scope(&self) -> &str {
        &self.replay_scope
    }
    /// Copy proven capabilities only. Actual auth must be derived by the host
    /// from this same immutable binding at its preparation/header boundary.
    /// This method grants no credential validity or transport authorization.
    /// Original request inputs and canonical model are needed by the external
    /// replay seam; they grant no credential or transport authority.
    pub fn raw_request(&self) -> &SourceJson {
        &self.raw
    }
    pub fn requested_model(&self) -> &ModelId {
        &self.model
    }
    pub fn prepare_options(&self, mut base: PrepareOptions) -> PrepareOptions {
        base.supports_native_effort = self.options.supports_native_effort;
        base.supports_disabled_thinking = self.options.supports_disabled_thinking;
        base.native_reasoning_efforts = self.options.native_reasoning_efforts.clone();
        base
    }
}

#[derive(Clone, Debug)]
pub enum PoolSelection {
    NotApplicable,
    Eligible(Box<SelectedAccount>),
    Unavailable,
    /// Source HTTP constructor rejection: fixed 500 text/plain UTF8 response,
    /// exact body "Internal Server Error". This carries no quota or paid meaning.
    InternalServerError,
    Refused {
        status: u16,
        headers: Headers,
        body: SourceJson,
    },
}

/// An actual response head, not a client-constructible proof or a second refusal writer.
/// No Clone/Deserialize: finalization consumes it exactly once.
pub struct AcceptedHead {
    pub(crate) selected: SelectedAccount,
    pub(crate) model: ModelId,
    pub(crate) response: ResponseObservation,
    pub(crate) classification: Classification,
    pub(crate) observation: Option<QuotaObservation>,
    pub(crate) success_recorded: bool,
}

#[derive(Debug)]
pub enum OutcomeDecision {
    Preserve,
    Retry,
    Paid,
    /// Same original m14 admission/transport claim, with an owner-CAS-verified new workspace.
    WorkspaceContinuation(Box<SelectedAccount>),
}

pub(crate) struct CommittedOutcome {
    pub state: QuotaState,
}
