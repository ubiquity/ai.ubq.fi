//! Finite report predicates; they never construct authority or source custody.
pub mod source_fields;
use gateway_migration_ledger::rows::{safe, starts};
use gateway_profile_values::scalar::{json_number, text};
use serde_json::Value;

pub fn positive(value: Option<&Value>) -> bool {
    safe(value) && value.and_then(Value::as_f64).is_some_and(|v| v > 0.0)
}
pub fn nullable_safe(value: Option<&Value>, positive_only: bool) -> bool {
    value.is_some_and(|v| {
        v.is_null()
            || if positive_only {
                positive(Some(v))
            } else {
                safe(Some(v))
            }
    })
}
pub fn nullable_text(value: Option<&Value>, nonempty: bool) -> bool {
    value.is_some_and(|v| v.is_null() || v.as_str().is_some_and(|s| !nonempty || !s.is_empty()))
}
pub fn under(parts: &[Value], prefix: &[&str]) -> bool {
    parts.len() > prefix.len() && starts(parts, prefix)
}
pub fn reference(parts: &[Value]) -> String {
    let parts: Vec<Value> = parts
        .iter()
        .map(|v| {
            if let Some(n) = v.as_f64() {
                json_number(Some(n))
            } else {
                v.clone()
            }
        })
        .collect();
    serde_json::to_string(&parts).unwrap_or_default()
}
pub fn valid_api_request(v: &Value) -> bool {
    if v["v"] != 3
        || !["key_id", "request_id", "route"]
            .iter()
            .all(|f| text(v, f).is_some_and(|s| !s.is_empty()))
        || !["reserved_at_ms", "lease_expires_at_ms"]
            .iter()
            .all(|f| safe(v.get(f)))
        || !nullable_safe(v.get("dispatched_at_ms"), false)
        || !nullable_safe(v.get("released_at_ms"), false)
        || !nullable_text(v.get("release_reason"), false)
    {
        return false;
    }
    let provider = &v["provider"];
    if !v.get("provider").is_some_and(|p| {
        p.is_null()
            || p.as_str().is_some_and(|p| {
                [
                    "cerebras",
                    "chatgpt_codex",
                    "claude_code",
                    "deepseek",
                    "lithos",
                    "openrouter",
                    "removed_provider",
                    "metered",
                    "surplus",
                    "voyage",
                ]
                .contains(&p)
            })
    }) {
        return false;
    }
    match text(v, "state") {
        Some("reserved") => {
            provider.is_null()
                && v["dispatched_at_ms"].is_null()
                && v["released_at_ms"].is_null()
                && v["release_reason"].is_null()
        }
        Some("dispatched") => {
            !provider.is_null()
                && !v["dispatched_at_ms"].is_null()
                && v["released_at_ms"].is_null()
                && v["release_reason"].is_null()
        }
        Some("released") => {
            provider.is_null()
                && v["dispatched_at_ms"].is_null()
                && !v["released_at_ms"].is_null()
                && !v["release_reason"].is_null()
        }
        _ => false,
    }
}
pub fn valid_kernel_policy(v: &Value, scope: &str, owner: &str, repo: Option<&str>) -> bool {
    gateway_migration_ledger::kernel::normalize_policy(v, scope, owner, repo).is_some()
}
