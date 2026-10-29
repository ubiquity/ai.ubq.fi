//! Complete stored routing journal DATA. Parsed records confer no live authority.
use crate::unavailable;
use gateway_authority_api_protocol::ApiReservation;
use gateway_authority_kernel_protocol::{KernelReservation, KernelTerminal};
use gateway_authority_paid_records::{PaidCorrelation, PaidReservation, PaidUsage};
use gateway_authority_routing_selection::types::AdmittedAccount;
use gateway_contracts::{
    identity::{PaidAttemptId, RequestContext},
    lifecycle::TransportObservation,
    model::ModelId,
    provider::UpstreamProvider,
};
use gateway_core::state::RevisionCheck;
use gateway_kv_codec::key::RecordKey;
use gateway_local_journal::{
    context::{decode_journal_context, same_source_value},
    local_work::{LocalWorkRecord, journal_local_work},
};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::{Deserialize, Serialize};

/// No credentials or request bodies are serialized in the recoverable journal.
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub enum CredentialFence {
    Direct {
        provider: UpstreamProvider,
    },
    Paid {
        provider: UpstreamProvider,
        owner_id: String,
        owner_epoch: String,
        configuration_generation: String,
        credential_binding: String,
    },
    Codex {
        admission: AdmittedAccount,
        #[serde(with = "record_key")]
        source_key: RecordKey,
        source_revision: String,
        native_generation: String,
    },
    Claude {
        account_id: String,
        cohort_id: String,
        credential_fingerprint: String,
        evidence_generation: String,
        workspace_id: Option<JsString16>,
        checks: Vec<RevisionCheckWire>,
    },
}
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct RevisionCheckWire {
    #[serde(with = "record_key")]
    pub key: RecordKey,
    pub expected_revision: Option<String>,
}
impl From<&RevisionCheck> for RevisionCheckWire {
    fn from(check: &RevisionCheck) -> Self {
        Self {
            key: check.key.clone(),
            expected_revision: check.expected_revision.clone(),
        }
    }
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct ChildRecord {
    pub nonce: String,
    pub request_id: String,
    pub phase: Phase,
    pub transport_started: bool,
    pub known_not_started: bool,
    pub settled: bool,
    pub model_sibling_retries: u8,
}
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct ExtraDispatch {
    pub nonce: String,
    pub provider: UpstreamProvider,
    pub transport_started: bool,
}
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct KernelIntent {
    pub terminal: KernelTerminal,
    pub reason: String,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub enum Phase {
    Prepared,
    Intent,
    Started,
    Terminal,
    Refused,
}
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct BillingUsage {
    pub quota: f64,
    pub input: i64,
    pub cached: Option<i64>,
    pub output: i64,
    pub provider_request_id: Option<String>,
    pub surplus: Option<(PaidAttemptId, Option<i64>)>,
}
impl BillingUsage {
    pub fn from_usage(usage: &PaidUsage) -> Self {
        let (provider_request_id, surplus) = match &usage.correlation {
            PaidCorrelation::ProviderRequestId(id) => (Some(id.clone()), None),
            PaidCorrelation::SurplusReservation {
                attempt_id,
                buyer_cost_micro,
            } => (None, Some((attempt_id.clone(), *buyer_cost_micro))),
        };
        Self {
            quota: usage.provider_quota,
            input: usage.input_tokens,
            cached: usage.cached_input_tokens,
            output: usage.output_tokens,
            provider_request_id,
            surplus,
        }
    }
    pub fn to_usage(&self) -> Result<PaidUsage, GatewayError> {
        let correlation = match (&self.provider_request_id, &self.surplus) {
            (Some(id), None) => PaidCorrelation::ProviderRequestId(id.clone()),
            (None, Some((attempt_id, buyer_cost_micro))) => PaidCorrelation::SurplusReservation {
                attempt_id: attempt_id.clone(),
                buyer_cost_micro: *buyer_cost_micro,
            },
            _ => return Err(unavailable("Invalid billing correlation.")),
        };
        Ok(PaidUsage {
            provider_quota: self.quota,
            input_tokens: self.input,
            cached_input_tokens: self.cached,
            output_tokens: self.output,
            correlation,
        })
    }
}
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct AttemptRecord {
    pub id: String,
    pub provider: UpstreamProvider,
    pub model: ModelId,
    pub cursor: usize,
    pub fence: CredentialFence,
    pub paid: Option<PaidReservation>,
    pub phase: Phase,
    pub transport_started: bool,
    #[serde(default)]
    pub response_nonce: Option<String>,
    #[serde(default)]
    pub response_count: u8,
    #[serde(default)]
    pub workspace_nonce: Option<String>,
    #[serde(default)]
    pub workspace_continuations: u8,
    pub known_not_started: bool,
    pub observation: Option<TransportObservation>,
    pub provider_request_id: Option<String>,
    pub billing: Option<BillingUsage>,
}
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct Journal {
    pub v: u32,
    #[serde(with = "gateway_local_journal::context::source_field")]
    pub context: RequestContext,
    pub input_digest: String,
    pub api: Option<ApiReservation>,
    pub kernel: Option<KernelReservation>,
    pub cursor: usize,
    pub attempts: Vec<AttemptRecord>,
    pub active: Option<String>,
    pub model_sibling_retries: u8,
    pub refreshed_versions: Vec<String>,
    pub recovery_attempted: bool,
    pub claude_unavailable: bool,
    pub semantic_committed: bool,
    pub terminal_intent: Option<TransportObservation>,
    pub finished: bool,
    pub recovering: bool,
    pub lease_expires_at_ms: i64,
    pub updated_at_ms: i64,
    pub extras_family: Option<gateway_extras_contract::family::ExtrasFamily>,
    pub extras_child_count: Option<u32>,
    pub extras_dispatch: Option<ExtraDispatch>,
    pub children: Vec<ChildRecord>,
    pub kernel_override: Option<KernelIntent>,
    /// Legacy v1 omits this field exactly; a v2 local row must carry it completely.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub local_work: Option<LocalWorkRecord>,
}
impl Journal {
    pub fn decode(value: &SourceJson) -> Result<Self, GatewayError> {
        let local = journal_local_work(value)?;
        let context = decode_journal_context(value)
            .map_err(|_| unavailable("Routing session is corrupt."))?;
        let row: Self = value
            .deserialize()
            .map_err(|_| unavailable("Routing session is corrupt."))?;
        let encoded = SourceJson::from_serializable(&row)
            .map_err(|_| unavailable("Routing session encoding failed."))?;
        if row.context != context || row.local_work != local || !same_source_value(&encoded, value)
        {
            return Err(unavailable(
                "Routing session has an incomplete or unknown shape.",
            ));
        }
        Ok(row)
    }
}

mod record_key {
    use gateway_kv_codec::{key::RecordKey, tuple_key::TupleKey};
    use serde::{Deserialize, Deserializer, Serializer, de::Error as _, ser::Error as _};
    pub fn serialize<S: Serializer>(key: &RecordKey, serializer: S) -> Result<S::Ok, S::Error> {
        TupleKey::from_record_key(key)
            .and_then(|key| key.validate_stored_key())
            .map_err(|_| S::Error::custom("Invalid source tuple key"))?;
        serializer.serialize_str(&key.0)
    }
    pub fn deserialize<'de, D: Deserializer<'de>>(decoder: D) -> Result<RecordKey, D::Error> {
        let key = RecordKey(String::deserialize(decoder)?);
        TupleKey::from_record_key(&key)
            .and_then(|key| key.validate_stored_key())
            .map_err(|_| D::Error::custom("Invalid source tuple key"))?;
        Ok(key)
    }
}
