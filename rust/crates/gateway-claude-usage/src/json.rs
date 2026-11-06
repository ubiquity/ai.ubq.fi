//! Source `typeof` checks over validated JSON text. String leaves stay UTF-16
//! units, so an unpaired surrogate in provider text is retained, not rejected.
use gateway_core::{source_json::SourceJson, source_serialization as source};

pub const MAX_SAFE_INTEGER: f64 = 9_007_199_254_740_991.0;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Kind {
    Object,
    Array,
    String,
    Null,
    Boolean,
    Number,
}
pub fn kind(value: &SourceJson) -> Kind {
    match value.as_str().trim_start().as_bytes().first() {
        Some(b'{') => Kind::Object,
        Some(b'[') => Kind::Array,
        Some(b'"') => Kind::String,
        Some(b'n') => Kind::Null,
        Some(b't' | b'f') => Kind::Boolean,
        _ => Kind::Number,
    }
}
/// Source `isRecord`: any non-null object, arrays included.
pub fn record(value: Option<&SourceJson>) -> bool {
    value.is_some_and(|v| matches!(kind(v), Kind::Object | Kind::Array))
}
pub fn is(value: Option<&SourceJson>, expected: Kind) -> bool {
    value.is_some_and(|v| kind(v) == expected)
}
pub fn finite(value: Option<&SourceJson>) -> Option<f64> {
    value
        .filter(|v| kind(v) == Kind::Number)
        .and_then(source::number)
        .filter(|n| n.is_finite())
}
pub fn safe_integer(n: f64) -> bool {
    n.is_finite() && n.fract() == 0.0 && n.abs() <= MAX_SAFE_INTEGER
}
pub fn nullable_number(value: Option<&SourceJson>) -> bool {
    is(value, Kind::Null) || finite(value).is_some()
}
pub fn nullable_string(value: Option<&SourceJson>) -> bool {
    is(value, Kind::Null) || is(value, Kind::String)
}
pub fn optional_string(value: Option<&SourceJson>) -> bool {
    value.is_none() || nullable_string(value)
}
/// Member access on a record; arrays and scalars have no named members here.
pub fn get(value: &SourceJson, name: &str) -> Option<SourceJson> {
    (kind(value) == Kind::Object)
        .then(|| source::member(value, name))
        .flatten()
}
pub fn null() -> SourceJson {
    SourceJson::null()
}
