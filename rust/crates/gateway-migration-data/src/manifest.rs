use crate::{RowData as Row, digest, invalid, json, key};
use gateway_core::state::StoredRecord;
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::collections::BTreeMap;

#[derive(Clone, Deserialize, Serialize)]
pub struct Manifest {
    pub v: u32,
    pub capture_id: String,
    pub source_git_sha: String,
    pub captured_at_ms: i64,
    pub source_rows: usize,
    pub mapping_checksum: String,
    pub derived: BTreeMap<String, String>,
    pub native_revision: Option<crate::native_capture::NativeRevision>,
}
pub fn manifest_key() -> RecordKey {
    key(json!(["uos_ai", "migration", "v3", "manifest"]))
}
pub fn mapping_key(id: &str, source: &str) -> RecordKey {
    key(json!([
        "uos_ai",
        "migration",
        "v3",
        "row",
        id,
        gateway_auth_primitives::crypto::sha256_hex(source.as_bytes())
    ]))
}

pub fn mapping_record(row: &Row, capture_id: &str) -> Result<StoredRecord, GatewayError> {
    Ok(StoredRecord::json(
        format!("capture:{capture_id}"),
        json(&row.mapping)?,
        None,
    ))
}
pub fn mapping_checksum(
    records: impl IntoIterator<Item = (String, StoredRecord)>,
) -> Result<String, GatewayError> {
    let mut entries = records
        .into_iter()
        .map(|(key, record)| Ok((key, digest(&record)?)))
        .collect::<Result<Vec<_>, GatewayError>>()?;
    entries.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(gateway_auth_primitives::crypto::sha256_hex(
        &serde_json::to_vec(&entries).map_err(|_| invalid("Mapping checksum cannot be encoded"))?,
    ))
}
