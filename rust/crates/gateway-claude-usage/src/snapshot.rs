//! Source `claude-code-usage-schema.ts`: Claude Code 2.1.310 usage payloads.
//! A stored snapshot is revalidated and its meters are recomputed from the
//! payload provenance, never trusted from stored derived fields.
use crate::json::{self, Kind, finite, get, is, nullable_number, nullable_string, record};
use gateway_admin_data::profile_date::iso_millis;
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};

pub const LEGACY_WINDOWS: [&str; 5] = [
    "five_hour",
    "seven_day",
    "seven_day_oauth_apps",
    "seven_day_opus",
    "seven_day_sonnet",
];
const LEGACY_LABELS: [&str; 5] = [
    "5-hour window",
    "Weekly window",
    "Weekly OAuth apps",
    "Weekly Opus",
    "Weekly Sonnet",
];
const RECOGNIZED: [&str; 4] = ["limits", "extra_usage", "cinder_cove", "wattle_ember"];

fn valid_window(value: Option<&SourceJson>) -> bool {
    record(value)
        && value.is_some_and(|v| {
            nullable_number(get(v, "utilization").as_ref())
                && nullable_string(get(v, "resets_at").as_ref())
        })
}
fn named(scope: Option<SourceJson>) -> bool {
    match scope {
        None => true,
        Some(scope) => {
            is(Some(&scope), Kind::Null)
                || (record(Some(&scope)) && is(get(&scope, "display_name").as_ref(), Kind::String))
        }
    }
}
pub fn valid_scope(value: Option<&SourceJson>) -> bool {
    match value {
        None => true,
        Some(v) if json::kind(v) == Kind::Null => true,
        Some(v) => record(Some(v)) && named(get(v, "model")) && named(get(v, "surface")),
    }
}
fn valid_limit(value: &SourceJson) -> bool {
    record(Some(value))
        && ["kind", "group", "severity"]
            .iter()
            .all(|name| is(get(value, name).as_ref(), Kind::String))
        && finite(get(value, "percent").as_ref()).is_some()
        && nullable_string(get(value, "resets_at").as_ref())
        && is(get(value, "is_active").as_ref(), Kind::Boolean)
        && valid_scope(get(value, "scope").as_ref())
}
fn valid_extra_usage(value: &SourceJson) -> bool {
    record(Some(value))
        && is(get(value, "is_enabled").as_ref(), Kind::Boolean)
        && ["monthly_limit", "used_credits", "utilization"]
            .iter()
            .all(|name| nullable_number(get(value, name).as_ref()))
        && ["currency", "disabled_reason"]
            .iter()
            .all(|name| json::optional_string(get(value, name).as_ref()))
}
fn valid_period(value: &SourceJson) -> bool {
    record(Some(value))
        && is(get(value, "starts_at").as_ref(), Kind::String)
        && is(get(value, "ends_at").as_ref(), Kind::String)
}
fn valid_credit(value: &SourceJson) -> bool {
    if !valid_window(Some(value)) || !json::optional_string(get(value, "label").as_ref()) {
        return false;
    }
    if !["limit_dollars", "used_dollars", "remaining_dollars"]
        .iter()
        .all(|name| get(value, name).is_none_or(|v| nullable_number(Some(&v))))
    {
        return false;
    }
    match get(value, "periods") {
        None => true,
        Some(periods) if json::kind(&periods) == Kind::Null => true,
        Some(periods) => source::array(&periods).is_some_and(|rows| rows.iter().all(valid_period)),
    }
}
fn present(value: &SourceJson, name: &str) -> Option<SourceJson> {
    get(value, name).filter(|v| json::kind(v) != Kind::Null)
}
fn valid_payload(value: &SourceJson) -> bool {
    if json::kind(value) != Kind::Object
        || !LEGACY_WINDOWS
            .iter()
            .chain(&RECOGNIZED)
            .any(|name| get(value, name).is_some())
    {
        return false;
    }
    if LEGACY_WINDOWS
        .iter()
        .any(|name| present(value, name).is_some_and(|w| !valid_window(Some(&w))))
    {
        return false;
    }
    if present(value, "limits").is_some_and(|limits| {
        source::array(&limits).is_none_or(|rows| !rows.iter().all(valid_limit))
    }) {
        return false;
    }
    present(value, "extra_usage").is_none_or(|v| valid_extra_usage(&v))
        && present(value, "cinder_cove").is_none_or(|v| valid_window(Some(&v)))
        && present(value, "wattle_ember").is_none_or(|v| valid_credit(&v))
}
/// Source `Date.parse` for the provider's ISO reset text; other text is no instant.
fn reset_time(value: &SourceJson) -> Option<i64> {
    source::string(value)?
        .try_utf8()
        .ok()
        .and_then(|text| iso_millis(&text))
        .filter(|at| *at > 0 && json::safe_integer(*at as f64))
}
fn meter(
    mut fields: Vec<(&str, SourceJson)>,
    resets_at: SourceJson,
    at: i64,
) -> Result<SourceJson, GatewayError> {
    let reset_at_ms = SourceJson::from_serializable(&reset_time(&resets_at))?;
    fields.extend([
        ("resets_at", resets_at),
        ("reset_at_ms", reset_at_ms),
        ("observed_at_ms", SourceJson::from_serializable(&at)?),
    ]);
    source::object(fields)
}
fn display_name(scope: &SourceJson, name: &str) -> Option<JsString16> {
    get(scope, name)
        .and_then(|value| get(&value, "display_name"))
        .and_then(|value| source::string(&value))
}
fn limit_meters(rows: Vec<SourceJson>, at: i64) -> Result<Vec<SourceJson>, GatewayError> {
    let mut occurrences: Vec<(String, usize)> = Vec::new();
    let mut meters = Vec::new();
    for row in rows {
        let leaf = |name| get(&row, name).unwrap_or_else(json::null);
        let scope = get(&row, "scope").unwrap_or_else(json::null);
        let (kind, group) = (leaf("kind"), leaf("group"));
        let tuple = source::canonical(&source::json_array(&[
            kind.clone(),
            group.clone(),
            scope.clone(),
        ])?)?;
        let tuple = tuple.as_str().to_owned();
        let occurrence = match occurrences.iter_mut().find(|(seen, _)| *seen == tuple) {
            Some((_, count)) => std::mem::replace(count, *count + 1),
            None => {
                occurrences.push((tuple.clone(), 1));
                0
            }
        };
        let separator = JsString16::from_utf8(" · ");
        let mut label = Vec::new();
        let parts = [source::string(&group), source::string(&kind)];
        let names = [
            display_name(&scope, "model"),
            display_name(&scope, "surface"),
        ];
        for (index, part) in parts.into_iter().chain(names).flatten().enumerate() {
            if index > 0 {
                label.extend_from_slice(separator.as_units());
            }
            label.extend_from_slice(part.as_units());
        }
        let id = format!("limits:{tuple}:{occurrence}");
        meters.push(meter(
            vec![
                ("id", source::quoted(&JsString16::from_utf8(&id))?),
                ("label", source::quoted(&JsString16::from_units(label))?),
                ("kind", kind),
                ("group", group),
                ("scope", scope),
                ("severity", leaf("severity")),
                ("is_active", leaf("is_active")),
                ("used_percent", leaf("percent")),
            ],
            leaf("resets_at"),
            at,
        )?);
    }
    Ok(meters)
}
fn legacy_meters(payload: &SourceJson, at: i64) -> Result<Vec<SourceJson>, GatewayError> {
    let mut meters = Vec::new();
    for (key, label) in LEGACY_WINDOWS.into_iter().zip(LEGACY_LABELS) {
        let Some(row) = present(payload, key) else {
            continue;
        };
        let leaf = |name| get(&row, name).unwrap_or_else(json::null);
        let group = if key == "five_hour" {
            "session"
        } else {
            "weekly"
        };
        meters.push(meter(
            vec![
                (
                    "id",
                    SourceJson::from_serializable(&format!("legacy:{key}"))?,
                ),
                ("label", SourceJson::from_serializable(label)?),
                ("kind", SourceJson::from_serializable(key)?),
                ("group", SourceJson::from_serializable(group)?),
                ("scope", json::null()),
                ("severity", json::null()),
                ("is_active", SourceJson::from_serializable(&false)?),
                ("used_percent", leaf("utilization")),
            ],
            leaf("resets_at"),
            at,
        )?);
    }
    Ok(meters)
}
/// Successful snapshots replace the complete prior shape; missing is not zero or last-known data.
pub fn parse_snapshot(payload: &SourceJson, observed_at_ms: f64) -> Option<SourceJson> {
    if !json::safe_integer(observed_at_ms) || observed_at_ms <= 0.0 {
        return None;
    }
    let payload = source::canonical(payload).ok()?;
    if !valid_payload(&payload) {
        return None;
    }
    let at = observed_at_ms as i64;
    let meters = match present(&payload, "limits").and_then(|l| source::array(&l)) {
        Some(rows) => limit_meters(rows, at),
        None => legacy_meters(&payload, at),
    }
    .ok()?;
    source::object(vec![
        ("v", SourceJson::from_serializable(&1).ok()?),
        ("observed_at_ms", SourceJson::from_serializable(&at).ok()?),
        ("payload", payload),
        ("meters", source::json_array(&meters).ok()?),
    ])
    .ok()
}
/// Source `readClaudeCodeUsageSnapshot`: recompute projections from validated provenance.
pub fn read_snapshot(value: &SourceJson) -> Option<SourceJson> {
    if !record(Some(value)) || finite(get(value, "v").as_ref()) != Some(1.0) {
        return None;
    }
    let observed = finite(get(value, "observed_at_ms").as_ref())?;
    parse_snapshot(&get(value, "payload")?, observed)
}
/// Snapshot clock used by the history reader's `observed_at_ms <= sampled_at_ms` check.
pub fn observed_at_ms(snapshot: &SourceJson) -> Option<f64> {
    finite(get(snapshot, "observed_at_ms").as_ref())
}
/// The fields of a stored meter that its usage rollup key and bounds depend on.
#[derive(Clone, Debug, PartialEq)]
pub struct MeterAnchor {
    /// Source `JSON.stringify([meter.id, meter.resets_at])`, hashed into the rollup key.
    pub anchor: SourceJson,
    pub used_percent: Option<f64>,
    pub observed_at_ms: f64,
}
/// Source `readClaudeCodeUsageMeter`.
pub fn read_meter(value: &SourceJson) -> Option<MeterAnchor> {
    let leaf = |name| get(value, name);
    if !record(Some(value))
        || !["id", "label", "kind", "group"]
            .iter()
            .all(|name| is(leaf(name).as_ref(), Kind::String))
        || !nullable_number(leaf("used_percent").as_ref())
        || !nullable_string(leaf("resets_at").as_ref())
        || !nullable_string(leaf("severity").as_ref())
        || !is(leaf("is_active").as_ref(), Kind::Boolean)
        || !valid_scope(leaf("scope").as_ref())
    {
        return None;
    }
    let observed =
        finite(leaf("observed_at_ms").as_ref()).filter(|n| json::safe_integer(*n) && *n > 0.0)?;
    let reset = leaf("reset_at_ms")?;
    if json::kind(&reset) != Kind::Null
        && finite(Some(&reset)).is_none_or(|n| !json::safe_integer(n) || n <= 0.0)
    {
        return None;
    }
    let anchor = source::json_array(&[leaf("id")?, leaf("resets_at")?]).ok()?;
    Some(MeterAnchor {
        anchor: source::canonical(&anchor).ok()?,
        used_percent: finite(leaf("used_percent").as_ref()),
        observed_at_ms: observed,
    })
}
