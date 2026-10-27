//! Ordinary paid rows, validation and source billing projection; never paid authorization or settlement.
pub mod backfill;
pub mod source_records;
pub mod source_usage;
use gateway_authority_contracts::{invalid, nonnegative, positive};
use gateway_core::{error::GatewayError, identity::ApiKeyId, identity::MicrocreditLimit};
use gateway_core::{identity::PaidAttemptId, identity::RequestId, js_string::JsString16};
use gateway_core::{lifecycle::BillingState, lifecycle::DispatchState};
use gateway_core::{lifecycle::PaidTerminalState, model::ModelId, provider::PaidProvider};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct PaidReservation {
    pub key_id: ApiKeyId,
    pub request_id: RequestId,
    pub attempt_id: PaidAttemptId,
    pub provider: PaidProvider,
    pub window_reset_at_ms: i64,
    pub created_at_ms: i64,
    pub reserved_microcredits: i64,
}

#[derive(Clone, Debug)]
pub enum PaidAdmission {
    Reserved(PaidReservation),
    Existing(PaidReservation),
}

#[derive(Clone, Debug, PartialEq)]
pub struct PaidRequest {
    pub key_id: ApiKeyId,
    pub request_id: RequestId,
    pub attempt_id: PaidAttemptId,
    pub provider: PaidProvider,
    pub model: ModelId,
    pub route: String,
    pub path: String,
    pub stream: bool,
    pub reasoning: Option<JsString16>,
    pub created_at_ms: i64,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct PaidAttempt {
    pub v: u32,
    pub reservation: PaidReservation,
    pub policy_version: String,
    pub model: ModelId,
    pub route: String,
    pub path: String,
    pub stream: bool,
    pub reasoning: Option<JsString16>,
    pub quota_per_credit: i64,
    pub provider_request_id: Option<String>,
    pub dispatch_state: DispatchState,
    pub terminal_state: PaidTerminalState,
    pub billing_state: BillingState,
    pub provider_quota: Option<f64>,
    pub input_tokens: Option<i64>,
    pub cached_input_tokens: Option<i64>,
    pub output_tokens: Option<i64>,
    pub spend_microcredits: Option<i64>,
    pub transport_started: bool,
    pub reconciliation_attempts: i64,
    pub last_reconciliation_at_ms: Option<i64>,
    pub dispatched_at_ms: Option<i64>,
    pub terminal_at_ms: Option<i64>,
    pub settled_at_ms: Option<i64>,
    pub updated_at_ms: i64,
    pub usage_rollup_at_ms: Option<i64>,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct PaidWindow {
    pub v: u32,
    pub key_id: ApiKeyId,
    pub policy_version: String,
    pub window_reset_at_ms: i64,
    pub limit_microcredits: i64,
    pub settled_microcredits: i64,
    pub reserved_microcredits: i64,
    pub pending_count: i64,
    pub updated_at_ms: i64,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct PendingPaid {
    pub key_id: ApiKeyId,
    pub attempt_id: PaidAttemptId,
    pub created_at_ms: i64,
    pub next_reconciliation_at_ms: i64,
}

#[derive(Clone, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub struct ReconciliationGate {
    pub next_due_at_ms: Option<i64>,
    /// Only outstanding work, never settled history. Event owners enumerate due keys.
    pub pending: BTreeMap<String, PendingPaid>,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ReconciliationLease {
    pub token: String,
    pub expires_at_ms: i64,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct PaidRollup {
    pub v: u32,
    pub bucket_start_at_ms: i64,
    pub model: ModelId,
    pub provider: PaidProvider,
    pub request_count: i64,
    #[serde(rename = "quota_sum")]
    pub provider_quota: f64,
    pub input_tokens: i64,
    pub cached_input_tokens: i64,
    pub output_tokens: i64,
    #[serde(
        serialize_with = "serialize_rollup_spend",
        deserialize_with = "deserialize_rollup_spend"
    )]
    pub spend_microcredits: f64,
    pub first_request_at_ms: i64,
    pub last_request_at_ms: i64,
    pub updated_at_ms: i64,
}

#[derive(Clone, Debug)]
pub struct PaidUsage {
    pub provider_quota: f64,
    pub input_tokens: i64,
    pub cached_input_tokens: Option<i64>,
    pub output_tokens: i64,
    pub correlation: PaidCorrelation,
}

#[derive(Clone, Debug)]
pub enum PaidCorrelation {
    ProviderRequestId(String),
    SurplusReservation {
        attempt_id: PaidAttemptId,
        buyer_cost_micro: Option<i64>,
    },
}

// Source rollup spend is finite/nonnegative, unlike exact integer request/window billing.
fn deserialize_rollup_spend<'de, D: serde::Deserializer<'de>>(
    deserializer: D,
) -> Result<f64, D::Error> {
    let value = f64::deserialize(deserializer)?;
    if value.is_finite() && value >= 0.0 {
        Ok(value)
    } else {
        Err(serde::de::Error::custom("Invalid source aggregate spend"))
    }
}
fn serialize_rollup_spend<S: serde::Serializer>(
    value: &f64,
    serializer: S,
) -> Result<S::Ok, S::Error> {
    if !value.is_finite() || *value < 0.0 {
        return Err(serde::ser::Error::custom("Invalid source aggregate spend"));
    }
    if value.fract() == 0.0 && *value <= gateway_core::identity::MAX_SAFE_INTEGER as f64 {
        serializer.serialize_i64(*value as i64)
    } else {
        serializer.serialize_f64(*value)
    }
}

pub fn validate_attempt(row: &PaidAttempt, expected: &PaidReservation) -> Result<(), GatewayError> {
    if row.v != 3 || row.reservation != *expected {
        return Err(invalid("Paid attempt identity conflicts."));
    }
    nonnegative(row.reservation.reserved_microcredits)?;
    positive(row.quota_per_credit)?;
    nonnegative(row.reconciliation_attempts)?;
    if let Some(spend) = row.spend_microcredits {
        nonnegative(spend)?;
    }
    Ok(())
}
pub fn validate_paid_window(
    row: &PaidWindow,
    key: &ApiKeyId,
    reset: i64,
) -> Result<(), GatewayError> {
    if row.v != 3 || &row.key_id != key || row.window_reset_at_ms != reset {
        return Err(invalid("Paid window identity conflicts."));
    }
    nonnegative(row.settled_microcredits)?;
    nonnegative(row.reserved_microcredits)?;
    nonnegative(row.pending_count)?;
    MicrocreditLimit::try_from(row.limit_microcredits)
        .map_err(|_| invalid("Invalid paid window limit."))?;
    Ok(())
}
pub fn provider_label(provider: PaidProvider) -> &'static str {
    match provider {
        PaidProvider::Surplus => "surplus",
        PaidProvider::Metered => "metered",
    }
}
pub fn rollup_shard(request: &str) -> u8 {
    let mut hash: u32 = 2_166_136_261;
    for code in request.encode_utf16() {
        hash = (hash ^ u32::from(code)).wrapping_mul(16_777_619);
    }
    (hash % 16) as u8
}

pub fn pending_id(key: &ApiKeyId, attempt: &PaidAttemptId) -> String {
    serde_json::json!([key.0, attempt.0]).to_string()
}

pub fn arm_gate(gate: &mut ReconciliationGate) {
    gate.next_due_at_ms = gate
        .pending
        .values()
        .map(|row| row.next_reconciliation_at_ms)
        .min();
}
