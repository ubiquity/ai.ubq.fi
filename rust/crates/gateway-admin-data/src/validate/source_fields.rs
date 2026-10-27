//! Finite observations used only by the named source control audits.
//! The complete SourceJson document remains authoritative; no model/map is
//! replaced by a scalar view and ObservedRow's general interpretation is unchanged.
use gateway_source::{source_json::SourceJson, source_serialization as source};
use serde_json::Value;

pub fn member(value: &SourceJson, field: &str) -> Option<SourceJson> {
    source::member(value, field)
}
pub fn finite(value: &SourceJson, field: &str) -> Option<Value> {
    member(value, field)?.deserialize().ok()
}
pub fn text(value: &SourceJson, field: &str) -> Option<String> {
    member(value, field)?.deserialize().ok()
}
pub fn number(value: &SourceJson, field: &str) -> Option<f64> {
    source::number(&member(value, field)?)
}
pub fn integer(value: &SourceJson, field: &str) -> Option<i64> {
    source::number(&member(value, field)?)
        .filter(|number| {
            number.is_finite()
                && number.fract() == 0.0
                && (0.0..=9_007_199_254_740_991.0).contains(number)
        })
        .map(|number| number as i64)
}
pub fn same_value(left: &SourceJson, right: &SourceJson) -> bool {
    // Source JSON numbers inhabit the same Number domain after either physical
    // codec. Comparing serde integer/float variants would make 3 and 3.0 differ.
    match (source::number(left), source::number(right)) {
        (Some(left), Some(right)) => left == right,
        _ => match (left.deserialize::<Value>(), right.deserialize::<Value>()) {
            (Ok(left), Ok(right)) => left == right,
            _ => source::string(left)
                .zip(source::string(right))
                .is_some_and(|(left, right)| left == right),
        },
    }
}
pub fn same(left: &SourceJson, right: &SourceJson, field: &str) -> bool {
    match (member(left, field), member(right, field)) {
        (None, None) => true,
        (Some(left), Some(right)) => same_value(&left, &right),
        _ => false,
    }
}
pub fn equals(value: &SourceJson, field: &str, expected: &Value) -> bool {
    member(value, field)
        .zip(SourceJson::from_serializable(expected).ok())
        .is_some_and(|(actual, expected)| same_value(&actual, &expected))
}
pub fn safe(value: &SourceJson, field: &str) -> bool {
    super::safe(finite(value, field).as_ref())
}
pub fn positive(value: &SourceJson, field: &str) -> bool {
    super::positive(finite(value, field).as_ref())
}
pub fn nullable_safe(value: &SourceJson, field: &str, positive_only: bool) -> bool {
    super::nullable_safe(finite(value, field).as_ref(), positive_only)
}
pub fn nullable_string(value: &SourceJson, field: &str, nonempty: bool) -> bool {
    member(value, field).is_some_and(|value| {
        value.as_str() == "null"
            || source::string(&value).is_some_and(|text| !nonempty || !text.as_units().is_empty())
    })
}
