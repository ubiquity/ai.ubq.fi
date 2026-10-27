//! Finite source key shapes only; these DATA predicates grant no collector authority.
use crate::starts;
use serde_json::Value;

fn identity(value: &Value) -> bool {
    value.as_str().is_some_and(|value| !value.is_empty())
}

fn bucket(value: &Value, interval: u32) -> bool {
    value.as_f64().is_some_and(|value| {
        value >= 0.0
            && value <= gateway_contracts::identity::MAX_SAFE_INTEGER as f64
            && value % f64::from(interval) == 0.0
    })
}

pub fn usage_lease_key(parts: &[Value]) -> bool {
    parts.len() == 6
        && starts(parts, &["uos_ai", "claude_code_usage", "v1", "lease"])
        && parts[4..].iter().all(identity)
}

pub fn historical_capacity_key(parts: &[Value]) -> bool {
    if !starts(parts, &["uos_ai", "provider_capacity", "claude", "v1"])
        || parts.len() < 8
        || !parts[6..8].iter().all(identity)
    {
        return false;
    }
    match parts[4].as_str() {
        Some("history") => parts.len() == 8 && bucket(&parts[5], 900_000),
        Some("rollup") => {
            parts.len() == 10
                && bucket(&parts[5], 3_600_000)
                && matches!(parts[8].as_str(), Some("five_hour" | "seven_day"))
                && (parts[9].as_str() == Some("unknown") || bucket(&parts[9], 1))
        }
        Some("usage_rollup") => {
            parts.len() == 9
                && bucket(&parts[5], 3_600_000)
                && parts[8].as_str().is_some_and(|digest| {
                    digest.len() == 64
                        && digest
                            .bytes()
                            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
                })
        }
        _ => false,
    }
}
