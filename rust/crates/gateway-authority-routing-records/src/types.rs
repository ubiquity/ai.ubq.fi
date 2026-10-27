//! Source-shaped serial routing records; parsed rows are observations, never grants.
use gateway_contracts::provider::CodexQuotaClass;
use gateway_contracts::{
    identity::CredentialVersion, identity::PoolVersionstamp, identity::RoutingAccountHash,
    identity::SubscriptionSelectionHash,
};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct PoolAccount {
    pub account_id_hash: RoutingAccountHash,
    pub admin_account_hash: SubscriptionSelectionHash,
    pub credential_version: CredentialVersion,
    pub enabled: bool,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct SubscriptionPool {
    pub v: u32,
    pub pool_versionstamp: PoolVersionstamp,
    pub accounts: Vec<PoolAccount>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct QuotaBlock {
    pub blocked_until_ms: i64,
    pub source: QuotaBlockSource,
    pub legacy_fallback: bool,
    #[serde(rename = "quota_signal_observed_at_ms")]
    pub observed_at_ms: Option<i64>,
    #[serde(rename = "observed_reset_at_ms")]
    pub reset_at_ms: Option<i64>,
    #[serde(rename = "observed_reset_at_is_stable")]
    pub reset_is_stable: bool,
    #[serde(rename = "banked_reset_generation_ambiguous")]
    pub reset_ambiguous: bool,
    #[serde(rename = "banked_reset_recovery_probe_pending")]
    pub recovery_pending: bool,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum QuotaBlockSource {
    BodyResetsAt,
    HeaderRetryAfter,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ProbeCircuit {
    Quota,
    UpstreamTimeout,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ProbeLease {
    pub token: String,
    pub generation: i64,
    pub circuit: ProbeCircuit,
    pub quota_class: Option<CodexQuotaClass>,
    pub expires_at_ms: i64,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct RoutingSlot {
    pub account_id_hash: Option<RoutingAccountHash>,
    pub credential_version: CredentialVersion,
    pub quota_blocked_until_ms: Option<i64>,
    pub quota_block_source: Option<QuotaBlockSource>,
    pub quota_blocked_classes: Vec<String>,
    pub generation: u64,
    pub invalid_credential_version: Option<CredentialVersion>,
    #[serde(rename = "quota_blocks_by_class")]
    pub quota_blocks: BTreeMap<String, QuotaBlock>,
    pub probe_lease: Option<ProbeLease>,
    pub primary_used_percent: Option<f64>,
    pub secondary_used_percent: Option<f64>,
    pub quota_signal_observed_at_ms: Option<i64>,
    pub capacity_observed_at_ms: Option<i64>,
    pub upstream_timeout_blocked_until_ms: Option<i64>,
    #[serde(rename = "observed_reset_at_ms")]
    pub retained_reset_at_ms: Option<i64>,
    #[serde(rename = "observed_reset_at_is_stable")]
    pub reset_identity_stable: bool,
    #[serde(rename = "banked_reset_generation_ambiguous")]
    pub reset_identity_ambiguous: bool,
    #[serde(rename = "banked_reset_recovery_probe_pending")]
    pub recovery_pending: bool,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct RoutingState {
    pub v: u32,
    pub updated_at_ms: i64,
    #[serde(rename = "banked_reset_legacy_identity_unresolved")]
    pub legacy_identity_unresolved: bool,
    pub slots: Vec<RoutingSlot>,
}
