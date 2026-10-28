//! Source model/metadata views over the one core JSON and UTF16 implementation.
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
use serde::Serialize;

pub(crate) fn json<T: Serialize + ?Sized>(value: &T) -> SourceJson {
    SourceJson::from_serializable(value).expect("typed model control is valid source JSON")
}
pub(crate) fn object(fields: Vec<(&str, SourceJson)>) -> SourceJson {
    source::object(fields).expect("typed model control object")
}
pub(crate) fn set(value: &mut SourceJson, key: &str, next: SourceJson) {
    let key = JsString16::from_utf8(key);
    let mut entries = source::entries(value).expect("model control object");
    if let Some((_, value)) = entries.iter_mut().find(|(name, _)| name == &key) {
        *value = next;
    } else {
        entries.push((key, next));
    }
    *value = source::object_keys(entries).expect("model control members");
}
pub(crate) fn is_object(value: &SourceJson) -> bool {
    value.as_str().starts_with('{')
}
pub(crate) fn is_record(value: &SourceJson) -> bool {
    is_object(value) || value.as_str().starts_with('[')
}
pub(crate) fn scalar(value: Option<&SourceJson>) -> Option<String> {
    // Only callers with independently finite fields (provider IDs/client versions) use this view.
    value
        .and_then(source::string)
        .and_then(|s| s.try_utf8().ok())
}
