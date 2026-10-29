//! Complete persisted replay-reference comparison; conversion is private to this owner.
use gateway_core::state::StoredRecord;
use gateway_kv_codec::{
    key::RecordKey,
    tuple_key::{KeyPart, TupleKey},
};
use gateway_observability_contracts::source_object::SourceObject;
use gateway_observability_replay_model::records::read_json_record;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};

pub(crate) fn tuple_from_source(parts: &[SourceJson]) -> Result<RecordKey, GatewayError> {
    let mut key = Vec::new();
    for value in parts {
        let text = value.as_str();
        if text.starts_with('"') {
            key.push(KeyPart::source_string_utf16(
                value.deserialize::<JsString16>()?.as_units(),
            ))
        } else if let Ok(value) = value.deserialize::<bool>() {
            key.push(KeyPart::Boolean(value))
        } else {
            let value = value.deserialize::<f64>()?;
            key.push(KeyPart::Number(value))
        }
    }
    TupleKey::from_parts(&key).map(|k| k.to_record_key())
}
/// Compare the complete persisted dedupe reference at the caller's existing phase.
/// The result is ordinary comparison DATA, never a deletion or currentness permit.
pub fn dedupe_record_matches_claimed_manifest(
    record: &StoredRecord,
    expected: &RecordKey,
) -> Result<bool, GatewayError> {
    let value = SourceObject::parse(&read_json_record(record)?)?;
    let Some(parts) = value
        .get("manifest_key")
        .map(|v| v.array_items())
        .transpose()?
    else {
        return Ok(false);
    };
    Ok(parts.len() == 7 && tuple_from_source(&parts)? == *expected)
}
