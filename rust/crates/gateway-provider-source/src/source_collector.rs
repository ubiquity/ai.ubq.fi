use crate::source as s;
use gateway_core::source_json::SourceJson;
use serde_json::Value;

pub fn usage(value: &SourceJson) -> Option<Value> {
    (!s::is_null(value))
        .then(|| value.deserialize().ok())
        .flatten()
}
