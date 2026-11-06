//! Source `readClaudeUsageRollup`: the separate direct-meter rollup namespace.
//! The header-meter writer adds no rows here; existing rows stay readable.
use crate::{
    json::{self, Kind, finite, get, record},
    snapshot::{MeterAnchor, read_meter},
};
use gateway_core::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};

pub const PREFIX: [&str; 5] = [
    "uos_ai",
    "provider_capacity",
    "claude",
    "v1",
    "usage_rollup",
];
pub const BUCKET_MS: f64 = 3_600_000.0;

/// Everything the reader needs to recompute the row's exact source key.
#[derive(Clone, Debug, PartialEq)]
pub struct UsageRollup {
    pub account_id: String,
    pub account_cohort_id: String,
    pub last_observed_at_ms: f64,
    pub meter: MeterAnchor,
}
impl UsageRollup {
    /// Source `claudeUsageRollupKey` bucket: the meter's own observation hour.
    pub fn key_bucket(&self) -> f64 {
        (self.meter.observed_at_ms / BUCKET_MS).floor() * BUCKET_MS
    }
}
fn identity_units(value: &SourceJson) -> Option<JsString16> {
    source::string(value).filter(|text| (1..=200).contains(&text.as_units().len()))
}
fn identity(value: Option<SourceJson>) -> Option<String> {
    identity_units(&value?)?.try_utf8().ok()
}
fn timestamp(value: Option<SourceJson>) -> Option<f64> {
    finite(value.as_ref()).filter(|n| json::safe_integer(*n) && *n >= 0.0)
}
fn percent(value: Option<SourceJson>) -> Option<Option<f64>> {
    match value {
        Some(v) if json::kind(&v) == Kind::Null => Some(None),
        other => finite(other.as_ref()).map(Some),
    }
}
fn bounded(value: &SourceJson, meter: &MeterAnchor, bucket: f64, first: f64, last: f64) -> bool {
    first <= last
        && bucket % BUCKET_MS == 0.0
        && meter.observed_at_ms == last
        && first >= bucket
        && last < bucket + BUCKET_MS
        && finite(get(value, "v").as_ref()) == Some(1.0)
}
pub fn read_usage_rollup(value: &SourceJson) -> Option<UsageRollup> {
    if !record(Some(value)) {
        return None;
    }
    let account_id = identity(get(value, "account_id"))?;
    let account_cohort_id = identity(get(value, "account_cohort_id"))?;
    let kind = get(value, "credential_kind").and_then(|v| source::string(&v))?;
    if !["oauth", "api_key"].contains(&kind.try_utf8().ok()?.as_str()) {
        return None;
    }
    let workspace = get(value, "workspace_id")?;
    if json::kind(&workspace) != Kind::Null && identity_units(&workspace).is_none() {
        return None;
    }
    let bucket = timestamp(get(value, "bucket_start_at_ms"))?;
    let first = timestamp(get(value, "first_observed_at_ms"))?;
    let last = timestamp(get(value, "last_observed_at_ms"))?;
    if timestamp(get(value, "samples")).is_none_or(|n| n < 1.0) {
        return None;
    }
    let meter = read_meter(&get(value, "meter")?)?;
    if !bounded(value, &meter, bucket, first, last) {
        return None;
    }
    let (min, max) = (
        percent(get(value, "min_used_percent"))?,
        percent(get(value, "max_used_percent"))?,
    );
    if min.is_some() != max.is_some()
        || min.zip(max).is_some_and(|(min, max)| {
            min > max
                || meter
                    .used_percent
                    .is_some_and(|used| used < min || used > max)
        })
    {
        return None;
    }
    Some(UsageRollup {
        account_id,
        account_cohort_id,
        last_observed_at_ms: last,
        meter,
    })
}
