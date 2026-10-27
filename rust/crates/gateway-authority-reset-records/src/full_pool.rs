//! Ordinary full-pool reset audit, inventory and credit selection data; no permit mint.
pub mod audit;
pub mod selection;
use crate::{ResetConfig, ResetMode};
use gateway_contracts::identity::AccountBinding;
use gateway_source::{error::GatewayError, js_string::JsString16};
use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct FullPoolFence {
    pub slot: u64,
    pub account_id_hash: String,
    pub quota_generation: String,
    pub routing_generation: u64,
    pub quota_reset_at_ms: i64,
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct FullPoolShadowDecision {
    pub v: u8,
    pub episode_hash: String,
    pub created_at_ms: i64,
    pub expires_at_ms: i64,
    pub decision_reason: String,
    pub selected_account_id_hash: Option<String>,
    pub selected_credit_id_hash: Option<String>,
    pub selected_credit_expires_at_ms: Option<i64>,
    pub fences: Vec<FullPoolFence>,
}

/// Provider observations are not grants. Raw opaque IDs never serialize into audit KV.
#[derive(Clone)]
pub struct SourceResetCredit {
    pub credit_id: JsString16,
    pub status: JsString16,
    pub reset_type: JsString16,
    pub expires_at_ms: Option<i64>,
}
#[derive(Clone)]
pub struct SourceResetInventory {
    pub binding: AccountBinding,
    pub observed_at_ms: i64,
    pub available_count: i64,
    pub credits: Vec<SourceResetCredit>,
}
pub trait SourceCreditCollation {
    fn compare_credit_ids(
        &self,
        left: &JsString16,
        right: &JsString16,
    ) -> Result<Ordering, GatewayError>;
}

/// Pure credit ordering input. This projection grants no reset or inventory authority.
pub struct CreditCandidateData {
    pub binding: AccountBinding,
    pub enabled: bool,
}
impl std::fmt::Debug for SourceResetCredit {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("SourceResetCredit([opaque])")
    }
}
impl std::fmt::Debug for SourceResetInventory {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("SourceResetInventory([opaque])")
    }
}
fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Full-pool reset shadow schema or custody is invalid.",
        "codex_reset_shadow_schema_invalid",
    )
}
fn declined(reason: &str) -> GatewayError {
    GatewayError::unavailable("Automatic banked reset is not authorized.", reason)
}
pub fn policy(config: &ResetConfig) -> Result<(), GatewayError> {
    let reason = if !config.enabled {
        Some("feature_disabled")
    } else if config.mode == ResetMode::Disabled {
        Some("mode_disabled")
    } else if config.max_per_account_per_day <= 0 {
        Some("per_account_day_limit_invalid")
    } else if config.max_per_account_per_window != 1 {
        Some("per_account_window_limit_invalid")
    } else if config.mode == ResetMode::Live && config.max_per_account_per_day != 1 {
        Some("terminal_outcome_account_day_limit_must_be_one")
    } else if config.mode == ResetMode::Live
        && (!config.provider_approved || !config.redeem_outcome_is_final)
    {
        Some("provider_contract_unproven")
    } else {
        None
    };
    reason.map_or(Ok(()), |reason| Err(declined(reason)))
}
