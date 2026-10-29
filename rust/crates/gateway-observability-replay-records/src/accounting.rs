//! Source capture-owned budget/accounting rows. This is unrelated to paid accounting.
use gateway_core::{state::AuthorityStore, state::StoredRecord};
use gateway_kv_codec::key::RecordKey;
use gateway_observability_contracts::{data, source_object::SourceObject};
use gateway_observability_replay_model::{
    records::read_json_record, replay_crypto, replay_model as model,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use serde_json::json;
pub const OVERHEAD: f64 = 8192.0;
pub const MAX_RECORDS: f64 = 50_000.0;
pub const RESERVATION_MS: i64 = 120_000;
pub const ATTEMPTS: usize = 5;
#[derive(Clone, Copy)]
pub struct RetentionLimits {
    budget: f64,
}
impl Default for RetentionLimits {
    fn default() -> Self {
        Self {
            budget: 1_073_741_824.0,
        }
    }
}
impl RetentionLimits {
    pub fn budget(self) -> f64 {
        self.budget
    }
    /// Source's injectable test seam; the public runtime always uses Default.
    pub fn for_fixture(bytes: u64) -> Self {
        Self {
            budget: bytes.clamp(65_536, gateway_contracts::identity::MAX_SAFE_INTEGER as u64)
                as f64,
        }
    }
    pub fn reserve(self) -> f64 {
        (self.budget / 16.0).floor().min(67_108_864.0)
    }
    pub fn payload(self) -> f64 {
        self.budget - self.reserve()
    }
}
pub fn accounting_key(at: f64, id: &str) -> RecordKey {
    data::key(json!([
        "uos_ai",
        "sentinel_replay",
        "v1",
        "accounting",
        at,
        id
    ]))
}
pub fn dedupe_key(fingerprint: &str) -> RecordKey {
    data::key(json!([
        "uos_ai",
        "sentinel_replay",
        "v1",
        "dedupe",
        fingerprint
    ]))
}
pub fn charge(ciphertext: usize, metadata: usize) -> f64 {
    (ciphertext.div_ceil(3) * 4 + metadata) as f64 + OVERHEAD
}
pub fn reservation_charge(plaintext: usize, metadata: usize) -> f64 {
    charge(
        plaintext
            .saturating_add(65_536 + 16)
            .min(replay_crypto::MAX_CIPHERTEXT_BYTES),
        metadata,
    )
}
pub fn metadata_charge(document: &SourceJson) -> f64 {
    document.as_bytes().len() as f64 + OVERHEAD
}
pub fn row_metadata_charge(row: Option<&StoredRecord>) -> Result<f64, GatewayError> {
    row.map(|r| read_json_record(r).map(|s| metadata_charge(&s)))
        .transpose()
        .map(|v| v.unwrap_or(0.0))
}
#[derive(Clone, Deserialize, Serialize)]
pub struct LedgerData {
    pub version: f64,
    pub budget_bytes: f64,
    pub stored_bytes: f64,
    pub reserved_bytes: f64,
    pub records: f64,
    pub status_records: f64,
    pub metadata_bytes: f64,
    pub evicted_bytes: f64,
    pub evicted_records: f64,
    pub expired_bytes: f64,
    pub expired_records: f64,
    pub status_pruned_records: f64,
    #[serde(deserialize_with = "model::nullable")]
    pub last_eviction_at_ms: Option<f64>,
    #[serde(deserialize_with = "model::nullable")]
    pub last_warning_at_ms: Option<f64>,
    #[serde(deserialize_with = "model::nullable")]
    pub last_full_at_ms: Option<f64>,
    #[serde(deserialize_with = "model::nullable")]
    pub last_skip_reason: Option<String>,
    pub bootstrap_complete: bool,
    #[serde(deserialize_with = "model::nullable")]
    pub bootstrap_cursor: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bootstrap_sweep: Option<String>,
    #[serde(deserialize_with = "model::nullable")]
    pub accounting_error: Option<String>,
    pub over_budget: bool,
}
const LEDGER_FIELDS: &[&str] = &[
    "version",
    "budget_bytes",
    "stored_bytes",
    "reserved_bytes",
    "records",
    "status_records",
    "metadata_bytes",
    "evicted_bytes",
    "evicted_records",
    "expired_bytes",
    "expired_records",
    "status_pruned_records",
    "last_eviction_at_ms",
    "last_warning_at_ms",
    "last_full_at_ms",
    "last_skip_reason",
    "bootstrap_complete",
    "bootstrap_cursor",
    "bootstrap_sweep",
    "accounting_error",
    "over_budget",
];
impl LedgerData {
    pub fn empty(limits: RetentionLimits) -> Self {
        Self {
            version: 1.0,
            budget_bytes: limits.budget,
            stored_bytes: 0.0,
            reserved_bytes: 0.0,
            records: 0.0,
            status_records: 0.0,
            metadata_bytes: 0.0,
            evicted_bytes: 0.0,
            evicted_records: 0.0,
            expired_bytes: 0.0,
            expired_records: 0.0,
            status_pruned_records: 0.0,
            last_eviction_at_ms: None,
            last_warning_at_ms: None,
            last_full_at_ms: None,
            last_skip_reason: None,
            bootstrap_complete: false,
            bootstrap_cursor: None,
            bootstrap_sweep: None,
            accounting_error: None,
            over_budget: false,
        }
    }
    pub fn valid(&self) -> bool {
        self.version == 1.0
            && [
                self.budget_bytes,
                self.stored_bytes,
                self.reserved_bytes,
                self.records,
                self.status_records,
                self.metadata_bytes,
                self.evicted_bytes,
                self.evicted_records,
                self.expired_bytes,
                self.expired_records,
                self.status_pruned_records,
            ]
            .into_iter()
            .all(model::counter)
            && [
                self.last_eviction_at_ms,
                self.last_warning_at_ms,
                self.last_full_at_ms,
            ]
            .into_iter()
            .all(|v| v.is_none_or(model::counter))
    }
    pub fn ready(&self) -> bool {
        self.bootstrap_complete && self.accounting_error.is_none()
    }
}
#[derive(Clone, Copy, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum AccountingState {
    Reserved,
    Stored,
    Evicting,
    Revoked,
}
#[derive(Clone, Copy, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ClaimKind {
    Evicted,
    Expired,
}
#[derive(Clone, Deserialize, Serialize)]
pub struct AccountingData {
    pub version: f64,
    pub capture_id: String,
    pub request_id: String,
    pub fingerprint: String,
    pub bytes: f64,
    pub state: AccountingState,
    pub fence: f64,
    pub created_at_ms: f64,
    pub expires_at_ms: f64,
    pub status_expires_at_ms: f64,
    pub stage: f64,
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        deserialize_with = "model::optional_nonnull"
    )]
    pub claim_kind: Option<ClaimKind>,
}
const ACCOUNT_FIELDS: &[&str] = &[
    "version",
    "capture_id",
    "request_id",
    "fingerprint",
    "bytes",
    "state",
    "fence",
    "created_at_ms",
    "expires_at_ms",
    "status_expires_at_ms",
    "stage",
    "claim_kind",
];
impl AccountingData {
    pub fn valid(&self) -> bool {
        self.version == 1.0
            && !self.capture_id.is_empty()
            && self.capture_id.encode_utf16().count() <= 128
            && model::digest(&self.fingerprint)
            && self.bytes > 0.0
            && self.fence >= 1.0
            && [
                self.bytes,
                self.fence,
                self.created_at_ms,
                self.expires_at_ms,
                self.status_expires_at_ms,
                self.stage,
            ]
            .into_iter()
            .all(model::counter)
    }
    pub fn matches(&self, key: &RecordKey) -> bool {
        accounting_key(self.created_at_ms, &self.capture_id) == *key
    }
}
pub fn decode<T: DeserializeOwned>(row: &StoredRecord, names: &[&str]) -> Result<T, GatewayError> {
    SourceObject::parse(&read_json_record(row)?)?
        .select(names)?
        .deserialize()
}
pub fn accounting(row: &StoredRecord, key: &RecordKey) -> Result<AccountingData, GatewayError> {
    let value: AccountingData = decode(row, ACCOUNT_FIELDS)?;
    if !value.valid() || !value.matches(key) {
        return Err(data::error("Sentinel replay accounting row is invalid"));
    }
    Ok(value)
}
pub async fn read_ledger<S: AuthorityStore>(
    store: &S,
    limits: RetentionLimits,
) -> Result<(LedgerData, Option<StoredRecord>), GatewayError> {
    let row = store.read(&model::key("budget")).await?;
    let value = if let Some(row) = row.as_ref() {
        decode(row, LEDGER_FIELDS)?
    } else {
        LedgerData::empty(limits)
    };
    if !value.valid() {
        return Err(data::error("Sentinel replay budget ledger is invalid"));
    }
    Ok((
        LedgerData {
            budget_bytes: limits.budget,
            ..value
        },
        row,
    ))
}
pub fn merged_document<T: Serialize>(
    previous: Option<&StoredRecord>,
    value: &T,
) -> Result<SourceJson, GatewayError> {
    let fresh = SourceJson::from_serializable(value)?;
    if let Some(row) = previous {
        let mut document = SourceObject::parse(&read_json_record(row)?)?;
        for (name, value) in fresh.object_entries()? {
            document.set(&name.try_utf8()?, &value)?;
        }
        document.source_json()
    } else {
        Ok(fresh)
    }
}

pub struct VictimMetadata {
    pub status: model::Status,
    pub tombstone: serde_json::Value,
    pub has_owner: bool,
    pub count: f64,
    pub bytes: f64,
}
pub fn victim_metadata(
    row: &AccountingData,
    kind: ClaimKind,
    at: i64,
) -> Result<VictimMetadata, GatewayError> {
    let reason = match kind {
        ClaimKind::Evicted => "storage_budget",
        ClaimKind::Expired => "payload_expired",
    };
    let status = model::Status {
        version: 1.0,
        request_id: row.request_id.clone(),
        status: match kind {
            ClaimKind::Evicted => model::CaptureState::Evicted,
            ClaimKind::Expired => model::CaptureState::Expired,
        },
        reason: Some(reason.into()),
        captured_at_ms: row.created_at_ms,
        manifest_key: None,
        fingerprint: None,
        expires_at_ms: None,
    };
    let tombstone = json!({"version":1,"fingerprint":row.fingerprint,"request_id":row.request_id,"evicted_at_ms":at,"reason":reason});
    let has_owner = !row.request_id.is_empty();
    let count = if has_owner { 2.0 } else { 1.0 };
    let bytes = metadata_charge(&gateway_source::source_json::SourceJson::from_serializable(
        &tombstone,
    )?) + if has_owner {
        metadata_charge(&gateway_source::source_json::SourceJson::from_serializable(
            &status,
        )?)
    } else {
        0.0
    };
    Ok(VictimMetadata {
        status,
        tombstone,
        has_owner,
        count,
        bytes,
    })
}
