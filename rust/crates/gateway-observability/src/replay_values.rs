//! Logical JSON leaves for declared replay record schemas, with explicit ProfileV2 text.
use gateway_core::{
    state::RecordKey, state::RecordMutation, state::StoredRecord, value::ValueEncoding,
};
use gateway_observability_contracts::{data, source_object::portable_json};
use gateway_source::{error::GatewayError, source_json::SourceJson};

pub(crate) fn read_json_record(row: &StoredRecord) -> Result<SourceJson, GatewayError> {
    match row.value_encoding {
        ValueEncoding::Json => SourceJson::from_serializable(row.json_value()?),
        ValueEncoding::ProfileV2 => row.profile_v2_source_json(),
        ValueEncoding::ProfileV1 | ValueEncoding::SourceGraphV1 => Err(data::error(
            "Replay metadata requires explicit Json or ProfileV2 encoding",
        )),
    }
}
pub(crate) fn put_json<H: gateway_observability_contracts::types::ObservabilityHost>(
    host: &H,
    key: RecordKey,
    document: &SourceJson,
    expiry: Option<i64>,
) -> Result<RecordMutation, GatewayError> {
    let value = portable_json(document)?;
    let revision = data::uuid(host)?;
    let record = if let Some(json) = value.to_json_lossless() {
        StoredRecord::json(revision, json, expiry)
    } else {
        StoredRecord::profile_v2(revision, value, expiry)?
    };
    Ok(RecordMutation::Put { key, record })
}
