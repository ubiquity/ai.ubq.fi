use gateway_kv_codec::key::RecordKey;
use gateway_kv_codec::tuple_key::TupleKey;
use serde::Deserialize;
pub fn serialize<S: serde::Serializer>(key: &RecordKey, serializer: S) -> Result<S::Ok, S::Error> {
    serializer.serialize_str(&key.0)
}
pub fn deserialize<'de, D: serde::Deserializer<'de>>(
    deserializer: D,
) -> Result<RecordKey, D::Error> {
    let key = RecordKey(String::deserialize(deserializer)?);
    TupleKey::from_record_key(&key)
        .and_then(|key| key.validate_stored_key())
        .map_err(serde::de::Error::custom)?;
    Ok(key)
}
