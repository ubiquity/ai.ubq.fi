//! Pure profile projection preserves the existing malformed-business-data fallback.
use gateway_core::state::StoredRecord;
use gateway_kv_codec::key::RecordKey;
use gateway_migration_data::{
    MappingData as Mapping, RowData as Row, Transform, capture::SourceEntry,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::Value;
pub fn project(
    key: &RecordKey,
    parts: &[Value],
    record: StoredRecord,
) -> Result<StoredRecord, GatewayError> {
    let original = record.clone();
    let mut row = Row {
        key: key.clone(),
        parts: parts.to_vec(),
        record,
        mapping: Mapping {
            source: SourceEntry {
                key: vec![],
                value: SourceJson::parse("null")?,
                versionstamp: original.revision.clone(),
                expires_at_ms: original.expires_at_ms,
            },
            transform: Transform::Identity,
            baseline_digest: String::new(),
            native_origin: None,
        },
    };
    // Source import accepts parsed malformed business data. Retain it explicitly;
    // consumers fail closed and the separate source validation reports its errors.
    if gateway_migration_projection::projection::forward(&mut row).is_err() {
        return Ok(original);
    }
    Ok(row.record)
}
