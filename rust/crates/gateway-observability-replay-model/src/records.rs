//! Named replay JSON/ProfileV2 DATA reader; it admits no other record family.
use gateway_core::state::StoredRecord;
use gateway_kv_codec::value::ValueEncoding;
use gateway_observability_contracts::data;
use gateway_source::{error::GatewayError, source_json::SourceJson};
pub fn read_json_record(row: &StoredRecord) -> Result<SourceJson, GatewayError> {
    match row.value_encoding {
        ValueEncoding::Json => SourceJson::from_serializable(row.json_value()?),
        ValueEncoding::ProfileV2 => row.profile_v2_source_json(),
        ValueEncoding::ProfileV1 | ValueEncoding::SourceGraphV1 => Err(data::error(
            "Replay metadata requires explicit Json or ProfileV2 encoding",
        )),
    }
}
