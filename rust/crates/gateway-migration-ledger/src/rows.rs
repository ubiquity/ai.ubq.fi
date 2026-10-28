//! Observed data and finite source predicates; no live authority or custody.
use gateway_core::state::StoredRecord;
use gateway_kv_codec::{key::RecordKey, tuple_key::TupleKey};
use gateway_source::error::GatewayError;
use serde_json::Value;

#[derive(Clone, Debug)]
pub struct ObservedRow {
    pub key: RecordKey,
    pub parts: Vec<Value>,
    pub record: StoredRecord,
}
impl ObservedRow {
    pub fn json(parts: Vec<Value>, record: StoredRecord) -> Result<Self, GatewayError> {
        let tuple = TupleKey::from_json_parts(&parts)?;
        tuple.validate_stored_key()?;
        Ok(Self {
            key: tuple.to_record_key(),
            parts,
            record,
        })
    }
    pub fn value(&self) -> Result<&Value, GatewayError> {
        self.record.json_value()
    }
}

pub fn starts(parts: &[Value], prefix: &[&str]) -> bool {
    parts.len() >= prefix.len()
        && parts
            .iter()
            .zip(prefix)
            .all(|(a, b)| a.as_str() == Some(*b))
}

pub fn api_id(value: Option<&Value>) -> bool {
    value
        .and_then(Value::as_str)
        .is_some_and(|s| !s.is_empty() && s.len() <= 200 && s == s.trim())
}

pub fn safe(value: Option<&Value>) -> bool {
    value.and_then(Value::as_f64).is_some_and(|v| {
        v.is_finite() && v.fract() == 0.0 && (0.0..=9_007_199_254_740_991.0).contains(&v)
    })
}

pub const DAY: i64 = 86_400_000;
