//! Source string/model observations over validated DATA; no proof or JSON constructor.
use gateway_contracts::model::ModelId;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};

pub fn js_trim_source(value: &JsString16) -> JsString16 {
    let units = value.as_units();
    let white =
        |unit: &u16| char::from_u32(u32::from(*unit)).is_some_and(crate::scalar::js_whitespace);
    let start = units.iter().take_while(|unit| white(unit)).count();
    let end = units.len()
        - units[start..]
            .iter()
            .rev()
            .take_while(|unit| white(unit))
            .count();
    JsString16::from_units(units[start..end].to_vec())
}

pub fn matches(value: &JsString16, literal: &str) -> bool {
    value.as_units().iter().copied().eq(literal.encode_utf16())
}

pub fn push_unique<T: PartialEq>(values: &mut Vec<T>, value: T) {
    if !values.contains(&value) {
        values.push(value);
    }
}

pub fn normalized(value: Option<&SourceJson>) -> Option<JsString16> {
    value
        .and_then(source::string)
        .map(|s| js_trim_source(&s))
        .filter(|s| !s.as_units().is_empty())
}

pub fn identifier(value: &SourceJson) -> Option<JsString16> {
    ["slug", "id", "model", "name"]
        .iter()
        .find_map(|key| source::member(value, key).as_ref().and_then(source::string))
}

pub fn model(value: &SourceJson) -> Option<ModelId> {
    identifier(value).map(ModelId::from_source)
}

pub fn positive(value: Option<&SourceJson>) -> Option<i64> {
    value
        .and_then(source::number)
        .filter(|n| {
            n.is_finite() && n.fract() == 0.0 && *n > 0.0 && *n <= crate::scalar::MAX_SAFE_INTEGER
        })
        .map(|n| n as i64)
}

pub fn concat(parts: &[&JsString16]) -> JsString16 {
    JsString16::from_units(
        parts
            .iter()
            .flat_map(|s| s.as_units().iter().copied())
            .collect(),
    )
}
