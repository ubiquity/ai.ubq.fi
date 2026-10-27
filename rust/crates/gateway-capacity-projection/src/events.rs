//! Finite source capacity event and reset-document shapes.
use crate::scalars::safe_i64;
use serde_json::{Value, json};
use std::collections::BTreeSet;
fn text(v: &Value, max: usize) -> bool {
    v.as_str()
        .is_some_and(|s| !s.is_empty() && s.encode_utf16().count() <= max)
}
fn time(v: &Value) -> bool {
    safe_i64(v).is_some()
}
fn nullable(value: &Value, name: &str, valid: impl Fn(&Value) -> bool) -> bool {
    value.get(name).is_some_and(|v| v.is_null() || valid(v))
}
fn percent(v: &Value) -> bool {
    v.as_f64()
        .is_some_and(|n| n.is_finite() && (0.0..=100.0).contains(&n))
}
pub fn valid_event(kind: &str, v: &Value) -> bool {
    if v["v"] != 1 || !text(&v["event_id"], 512) || !time(&v["observed_at_ms"]) {
        return false;
    }
    if kind == "downtime_event" {
        return v["provider"] == "openai"
            && matches!(
                v["failure_kind"].as_str(),
                Some("upstream_error" | "unreachable")
            )
            && nullable(v, "status", |n| {
                safe_i64(n).is_some_and(|s| (500..=599).contains(&s))
            });
    }
    if !matches!(safe_i64(&v["slot"]), Some(1 | 2)) {
        return false;
    }
    if kind == "reset_event" {
        return true;
    }
    if kind != "rate_limit_reset_event"
        || v["provider"] != "openai"
        || !matches!(v["window"].as_str(), Some("primary" | "secondary"))
        || [
            "previous_sampled_at_ms",
            "previous_reset_at_ms",
            "reset_at_ms",
        ]
        .iter()
        .any(|f| !time(&v[f]))
        || [
            "previous_used_percent",
            "current_used_percent",
            "capacity_gain_percentage_points",
        ]
        .iter()
        .any(|f| !percent(&v[f]))
    {
        return false;
    }
    safe_i64(&v["previous_sampled_at_ms"]) < safe_i64(&v["observed_at_ms"])
        && safe_i64(&v["previous_reset_at_ms"]) < safe_i64(&v["reset_at_ms"])
        && (v["previous_used_percent"].as_f64().unwrap_or(0.0)
            - v["current_used_percent"].as_f64().unwrap_or(0.0)
            - v["capacity_gain_percentage_points"].as_f64().unwrap_or(0.0))
        .abs()
            <= 0.001
}
pub fn parse_shadow(value: &Value) -> Option<Value> {
    if value["v"] != 1
        || !text(&value["episode_hash"], 512)
        || !time(&value["created_at_ms"])
        || !time(&value["expires_at_ms"])
        || safe_i64(&value["expires_at_ms"]) < safe_i64(&value["created_at_ms"])
        || !text(&value["decision_reason"], 128)
        || !nullable(value, "selected_account_id_hash", |v| text(v, 512))
        || !nullable(value, "selected_credit_id_hash", |v| text(v, 512))
        || !nullable(value, "selected_credit_expires_at_ms", time)
        || value["selected_account_id_hash"].is_null() != value["selected_credit_id_hash"].is_null()
    {
        return None;
    }
    let fences = value["fences"].as_array().filter(|a| !a.is_empty())?;
    let mut slots = BTreeSet::new();
    let mut parsed = Vec::new();
    for fence in fences {
        if !time(&fence["slot"])
            || !text(&fence["account_id_hash"], 512)
            || !text(&fence["quota_generation"], 512)
            || !time(&fence["routing_generation"])
            || !time(&fence["quota_reset_at_ms"])
            || !slots.insert(safe_i64(&fence["slot"])?)
        {
            return None;
        }
        parsed.push(json!({"slot":fence["slot"],"account_id_hash":fence["account_id_hash"],"quota_generation":fence["quota_generation"],"routing_generation":fence["routing_generation"],"quota_reset_at_ms":fence["quota_reset_at_ms"]}));
    }
    Some(
        json!({"v":1,"episode_hash":value["episode_hash"],"created_at_ms":value["created_at_ms"],"expires_at_ms":value["expires_at_ms"],"decision_reason":value["decision_reason"],"selected_account_id_hash":value["selected_account_id_hash"],"selected_credit_id_hash":value["selected_credit_id_hash"],"selected_credit_expires_at_ms":value["selected_credit_expires_at_ms"],"fences":parsed}),
    )
}
pub fn valid_redemption(v: &Value) -> bool {
    if v["v"] != 1
        || [
            "account_id_hash",
            "credential_version",
            "quota_generation",
            "idempotency_key_hash",
            "owner_token",
        ]
        .iter()
        .any(|f| !text(&v[f], 512))
        || [
            "routing_generation",
            "fence",
            "lease_expires_at_ms",
            "created_at_ms",
            "updated_at_ms",
        ]
        .iter()
        .any(|f| !time(&v[f]))
        || !nullable(v, "provider_receipt_id", |v| text(v, 512))
        || !nullable(v, "submitted_at_ms", time)
        || !nullable(v, "verified_at_ms", time)
        || !nullable(v, "last_error_code", |v| text(v, 128))
    {
        return false;
    }
    let created = safe_i64(&v["created_at_ms"]).unwrap_or(0);
    let updated = safe_i64(&v["updated_at_ms"]).unwrap_or(0);
    let submitted = safe_i64(&v["submitted_at_ms"]);
    let verified = safe_i64(&v["verified_at_ms"]);
    let error = !v["last_error_code"].is_null();
    if updated < created
        || submitted.is_some_and(|t| t < created || updated < t)
        || verified.is_some_and(|t| submitted.is_none_or(|s| t < s) || updated < t)
    {
        return false;
    }
    match v["state"].as_str() {
        Some("claimed") => {
            v["provider_receipt_id"].is_null()
                && submitted.is_none()
                && verified.is_none()
                && !error
        }
        Some("submitted") => submitted.is_some() && verified.is_none() && !error,
        Some("unknown") => submitted.is_some() && verified.is_none() && error,
        Some("verified") => submitted.is_some() && verified.is_some() && !error,
        Some("rejected") => verified.is_none() && error,
        _ => false,
    }
}
