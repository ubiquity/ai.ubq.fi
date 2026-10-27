//! Raw source metadata and effective normalized views are distinct contracts.
//! Owner parse-None/default behavior does not authorize rewriting captured data.
use gateway_core::error::GatewayError;
use gateway_core::{source_control::decode_source_control_record, value::ValueEncoding};
use gateway_migration_data::{RowData as Row, starts};
use serde_json::Value;

pub fn named_key(parts: &[Value]) -> bool {
    gateway_migration_data::claude::usage_lease_key(parts)
        || (parts.len() == 2
            && (starts(parts, &["ubq_ai", "codex_models"])
                || starts(parts, &["uos_ai", "codex_models_whitelist"])))
        || (parts.len() == 3
            && (starts(parts, &["uos_ai", "codex_account_models", "v1"])
                || starts(parts, &["uos_ai", "runtime_config", "v2"])))
}
pub(crate) fn retain_raw(row: &Row) -> Result<bool, GatewayError> {
    if !named_key(&row.parts)
        || !matches!(
            row.record.value_encoding,
            ValueEncoding::Json | ValueEncoding::ProfileV2
        )
    {
        return Ok(false);
    }
    // No owner normalizer is called here. Metadata views and collector lease
    // DATA retain their raw source values. Explicit model-snapshot migration
    // audit failures ran before this projection.
    decode_source_control_record(&row.record)?;
    Ok(true)
}
