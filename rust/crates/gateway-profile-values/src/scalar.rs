//! Pure JSON scalar and error helpers shared by admin data consumers.

use gateway_source::error::GatewayError;
use serde_json::{Value, json};

pub fn number(value: &Value, field: &str) -> Option<f64> {
    value.get(field).and_then(Value::as_f64)
}
pub fn integer(value: &Value, field: &str) -> Option<i64> {
    number(value, field)
        .filter(|v| {
            v.is_finite() && v.fract() == 0.0 && (0.0..=9_007_199_254_740_991.0).contains(v)
        })
        .map(|v| v as i64)
}
pub fn text<'a>(value: &'a Value, field: &str) -> Option<&'a str> {
    value.get(field).and_then(Value::as_str)
}
pub fn present(value: &Value, field: &str) -> Value {
    value.get(field).cloned().unwrap_or(Value::Null)
}
pub fn positive(value: Option<f64>) -> Option<f64> {
    value.filter(|v| v.is_finite() && *v > 0.0)
}
pub fn json_number(value: Option<f64>) -> Value {
    match value.filter(|v| v.is_finite()) {
        Some(v) if v.fract() == 0.0 && v.abs() <= 9_007_199_254_740_991.0 => json!(v as i64),
        Some(v) => json!(v),
        None => Value::Null,
    }
}
pub fn error(message: impl Into<String>) -> GatewayError {
    GatewayError::new(400, message, Some("invalid_migration_data"))
}
