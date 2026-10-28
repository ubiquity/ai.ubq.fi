//! Finite staged wire DATA and canonical frame hashing; no accepted plan or effect.
use crate::{PRIVATE_WIRE_MAX_BYTES, invalid, manifest::Manifest, record_key_wire};
use gateway_core::state::StoredRecord;
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};
pub const STAGED_BATCH_MAX_BYTES: usize = PRIVATE_WIRE_MAX_BYTES;

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct CanonicalRecord {
    #[serde(with = "record_key_wire")]
    pub key: RecordKey,
    pub record: StoredRecord,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct StageHeader {
    pub manifest: Manifest,
    pub records: usize,
    pub digest: String,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct StageBatch {
    pub capture_id: String,
    pub sequence: u64,
    pub previous_digest: String,
    pub records: Vec<CanonicalRecord>,
    pub digest: String,
}
#[derive(Deserialize, Serialize)]
#[serde(
    tag = "stage",
    content = "data",
    rename_all = "snake_case",
    deny_unknown_fields
)]
pub enum StageRequest {
    Begin(StageHeader),
    Append(StageBatch),
    Seal {
        capture_id: String,
    },
    /// Private post-seal control-owner binding. It proves the sealed target's
    /// exact manifest/source/projection facts and the absent-owner fence before
    /// one CAS. It never marks the target active or rewrites sealed data.
    BindOwner(StageOwnerBinding),
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct StageOwnerAccount {
    pub account_id_hash: String,
    pub access_generation: String,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct StageOwnerBinding {
    pub capture_id: String,
    pub source_git_sha: String,
    pub source_captured_at_ms: i64,
    pub source_native_version: String,
    pub source_native_sequence: String,
    pub source_pool_revision: String,
    pub normalized_pool_revision: String,
    pub accounts: Vec<StageOwnerAccount>,
    pub custody_observed_at_ms: i64,
    pub valid_through_ms: i64,
    pub bound_at_ms: i64,
    pub owner_id: String,
    pub owner_epoch: String,
    pub key_fingerprint: String,
    pub owner_revision: String,
}
#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct StageReceipt {
    pub capture_id: String,
    pub source_rows: usize,
    pub derived_rows: usize,
    pub accepted_records: usize,
    pub atomic_batches: u64,
    pub sealed: bool,
    pub owner_bound: bool,
    /// Data readiness is not a grant to activate a service or restore an owner.
    pub activation_authorized: bool,
}
pub fn initial(id: &str) -> String {
    gateway_auth_primitives::crypto::sha256_hex(format!("private-staged-v1:{id}").as_bytes())
}
pub fn extend(previous: &str, item: &CanonicalRecord) -> Result<String, GatewayError> {
    Ok(gateway_auth_primitives::crypto::sha256_hex(
        &serde_json::to_vec(&(previous, item))
            .map_err(|_| invalid("Staged row cannot be encoded"))?,
    ))
}
pub fn bounded<T: Serialize>(value: &T) -> Result<usize, GatewayError> {
    let bytes =
        serde_json::to_vec(value).map_err(|_| invalid("Private staged frame cannot be encoded"))?;
    if bytes.len() > STAGED_BATCH_MAX_BYTES {
        return Err(invalid("Private staged frame exceeds its bound"));
    }
    Ok(bytes.len())
}
