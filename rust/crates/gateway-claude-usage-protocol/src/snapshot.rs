use gateway_core::{
    error::GatewayError, identity::MAX_SAFE_INTEGER, js_string::JsString16,
    source_json::SourceJson, source_serialization as source,
};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

pub const LEGACY: [&str; 5] = [
    "five_hour",
    "seven_day",
    "seven_day_oauth_apps",
    "seven_day_opus",
    "seven_day_sonnet",
];
const LABELS: [&str; 5] = [
    "5-hour window",
    "Weekly window",
    "Weekly OAuth apps",
    "Weekly Opus",
    "Weekly Sonnet",
];
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Snapshot {
    pub v: u8,
    pub observed_at_ms: i64,
    pub payload: SourceJson,
    pub meters: Vec<Meter>,
}
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Meter {
    pub id: JsString16,
    pub label: JsString16,
    pub kind: JsString16,
    pub group: JsString16,
    pub scope: SourceJson,
    pub severity: Option<JsString16>,
    pub is_active: bool,
    pub used_percent: Option<f64>,
    pub resets_at: Option<JsString16>,
    pub reset_at_ms: Option<i64>,
    pub observed_at_ms: i64,
}
pub fn safe_time(value: f64) -> bool {
    value.is_finite() && value.fract() == 0.0 && (0.0..=MAX_SAFE_INTEGER as f64).contains(&value)
}
fn object_like(value: &SourceJson) -> bool {
    source::entries(value).is_some() || source::array(value).is_some()
}
fn nullable(value: Option<&SourceJson>, predicate: impl Fn(&SourceJson) -> bool) -> bool {
    value.is_some_and(|v| v.as_str() == "null" || predicate(v))
}
fn optional(value: Option<&SourceJson>, predicate: impl Fn(&SourceJson) -> bool) -> bool {
    value.is_none_or(|v| v.as_str() == "null" || predicate(v))
}
fn finite(value: &SourceJson) -> bool {
    source::number(value).is_some_and(f64::is_finite)
}
fn text(value: &SourceJson) -> bool {
    source::string(value).is_some()
}
fn window(value: &SourceJson) -> bool {
    object_like(value)
        && nullable(source::member(value, "utilization").as_ref(), finite)
        && nullable(source::member(value, "resets_at").as_ref(), text)
}
fn scope(value: &SourceJson) -> bool {
    if !object_like(value) {
        return false;
    }
    ["model", "surface"].iter().all(|name| {
        optional(source::member(value, name).as_ref(), |v| {
            object_like(v) && source::member(v, "display_name").is_some_and(|s| text(&s))
        })
    })
}
fn limit(value: &SourceJson) -> bool {
    object_like(value)
        && ["kind", "group", "severity"]
            .iter()
            .all(|n| source::member(value, n).is_some_and(|v| text(&v)))
        && source::member(value, "percent").is_some_and(|v| finite(&v))
        && nullable(source::member(value, "resets_at").as_ref(), text)
        && source::member(value, "is_active")
            .is_some_and(|v| matches!(v.as_str(), "true" | "false"))
        && optional(source::member(value, "scope").as_ref(), scope)
}
fn extra(value: &SourceJson) -> bool {
    object_like(value)
        && source::member(value, "is_enabled")
            .is_some_and(|v| matches!(v.as_str(), "true" | "false"))
        && ["monthly_limit", "used_credits", "utilization"]
            .iter()
            .all(|n| nullable(source::member(value, n).as_ref(), finite))
        && ["currency", "disabled_reason"]
            .iter()
            .all(|n| optional(source::member(value, n).as_ref(), text))
}
fn credit(value: &SourceJson) -> bool {
    window(value)
        && optional(source::member(value, "label").as_ref(), text)
        && ["limit_dollars", "used_dollars", "remaining_dollars"]
            .iter()
            .all(|n| optional(source::member(value, n).as_ref(), finite))
        && optional(source::member(value, "periods").as_ref(), |v| {
            source::array(v).is_some_and(|rows| {
                rows.iter().all(|p| {
                    object_like(p)
                        && ["starts_at", "ends_at"]
                            .iter()
                            .all(|n| source::member(p, n).is_some_and(|s| text(&s)))
                })
            })
        })
}
pub fn valid_payload(value: &SourceJson) -> bool {
    let recognized =
        LEGACY
            .into_iter()
            .chain(["limits", "extra_usage", "cinder_cove", "wattle_ember"]);
    if !object_like(value)
        || !recognized
            .into_iter()
            .any(|n| source::member(value, n).is_some())
    {
        return false;
    }
    LEGACY
        .iter()
        .all(|n| optional(source::member(value, n).as_ref(), window))
        && optional(source::member(value, "limits").as_ref(), |v| {
            source::array(v).is_some_and(|rows| rows.iter().all(limit))
        })
        && optional(source::member(value, "extra_usage").as_ref(), extra)
        && optional(source::member(value, "cinder_cove").as_ref(), window)
        && optional(source::member(value, "wattle_ember").as_ref(), credit)
}
/// Validated provenance only. Stored derived meters are deliberately ignored.
pub fn provenance(value: &SourceJson) -> Option<(SourceJson, i64)> {
    if source::member(value, "v").as_ref().and_then(source::number) != Some(1.0) {
        return None;
    }
    let at = source::member(value, "observed_at_ms")
        .as_ref()
        .and_then(source::number)?;
    let payload = source::member(value, "payload")?;
    (safe_time(at) && at > 0.0 && valid_payload(&payload)).then_some((payload, at as i64))
}
pub fn read(
    value: &SourceJson,
    parse_date: &impl Fn(&JsString16) -> Option<i64>,
) -> Option<Snapshot> {
    let (payload, at) = provenance(value)?;
    parse(&payload, at, parse_date)
}
pub fn parse(
    payload: &SourceJson,
    at: i64,
    parse_date: &impl Fn(&JsString16) -> Option<i64>,
) -> Option<Snapshot> {
    if !(1..=MAX_SAFE_INTEGER).contains(&at) || !valid_payload(payload) {
        return None;
    }
    let payload = source::canonical(payload).ok()?;
    let mut meters = Vec::new();
    if let Some(rows) = source::member(&payload, "limits")
        .as_ref()
        .and_then(source::array)
    {
        let mut occurrences = BTreeMap::<String, usize>::new();
        for row in rows {
            let kind = source::member(&row, "kind")
                .as_ref()
                .and_then(source::string)?;
            let group = source::member(&row, "group")
                .as_ref()
                .and_then(source::string)?;
            let scope = source::member(&row, "scope").unwrap_or(SourceJson::parse("null").ok()?);
            let tuple = source::canonical(
                &source::json_array(&[
                    source::quoted(&kind).ok()?,
                    source::quoted(&group).ok()?,
                    scope.clone(),
                ])
                .ok()?,
            )
            .ok()?;
            let occurrence = occurrences.entry(tuple.as_str().into()).or_default();
            let id = JsString16::from_utf8(&format!("limits:{}:{}", tuple.as_str(), occurrence));
            *occurrence += 1;
            let mut labels = vec![group.clone(), kind.clone()];
            for name in ["model", "surface"] {
                if let Some(label) = source::member(&scope, name)
                    .and_then(|v| source::member(&v, "display_name"))
                    .as_ref()
                    .and_then(source::string)
                {
                    labels.push(label);
                }
            }
            let resets_at = source::member(&row, "resets_at")
                .as_ref()
                .and_then(source::string);
            meters.push(Meter {
                id,
                label: join(&labels),
                kind,
                group,
                scope,
                severity: source::member(&row, "severity")
                    .as_ref()
                    .and_then(source::string),
                is_active: source::member(&row, "is_active").is_some_and(|v| v.as_str() == "true"),
                used_percent: source::member(&row, "percent")
                    .as_ref()
                    .and_then(source::number),
                reset_at_ms: reset(&resets_at, parse_date),
                resets_at,
                observed_at_ms: at,
            });
        }
    } else {
        for (index, name) in LEGACY.iter().enumerate() {
            let Some(row) = source::member(&payload, name).filter(|v| v.as_str() != "null") else {
                continue;
            };
            let resets_at = source::member(&row, "resets_at")
                .as_ref()
                .and_then(source::string);
            meters.push(Meter {
                id: JsString16::from_utf8(&format!("legacy:{name}")),
                label: LABELS[index].into(),
                kind: (*name).into(),
                group: if index == 0 { "session" } else { "weekly" }.into(),
                scope: SourceJson::parse("null").ok()?,
                severity: None,
                is_active: false,
                used_percent: source::member(&row, "utilization")
                    .as_ref()
                    .and_then(source::number),
                reset_at_ms: reset(&resets_at, parse_date),
                resets_at,
                observed_at_ms: at,
            });
        }
    }
    Some(Snapshot {
        v: 1,
        observed_at_ms: at,
        payload,
        meters,
    })
}
fn join(labels: &[JsString16]) -> JsString16 {
    let mut units = Vec::new();
    for (i, label) in labels.iter().enumerate() {
        if i > 0 {
            units.extend(" · ".encode_utf16());
        }
        units.extend(label.as_units());
    }
    JsString16::from_units(units)
}
fn reset(
    value: &Option<JsString16>,
    parse_date: &impl Fn(&JsString16) -> Option<i64>,
) -> Option<i64> {
    value
        .as_ref()
        .and_then(parse_date)
        .filter(|at| (1..=MAX_SAFE_INTEGER).contains(at))
}
/// Strict required-field validation for independently stored rollup meters.
pub fn read_meter(value: &SourceJson) -> Option<Meter> {
    if !["id", "label", "kind", "group"]
        .iter()
        .all(|n| source::member(value, n).is_some_and(|v| text(&v)))
        || !nullable(source::member(value, "used_percent").as_ref(), finite)
        || !["resets_at", "severity"]
            .iter()
            .all(|n| nullable(source::member(value, n).as_ref(), text))
        || !source::member(value, "is_active")
            .is_some_and(|v| matches!(v.as_str(), "true" | "false"))
        || !optional(source::member(value, "scope").as_ref(), scope)
    {
        return None;
    }
    let at = source::member(value, "observed_at_ms")
        .as_ref()
        .and_then(source::number)?;
    let reset = source::member(value, "reset_at_ms")?;
    if !safe_time(at)
        || at <= 0.0
        || !(reset.as_str() == "null"
            || source::number(&reset).is_some_and(|n| safe_time(n) && n > 0.0))
    {
        return None;
    }
    let mut fields = source::entries(value)?;
    if source::member(value, "scope").is_none() {
        fields.push(("scope".into(), SourceJson::parse("null").ok()?));
    }
    source::canonical(&source::object_keys(fields).ok()?)
        .ok()?
        .deserialize()
        .ok()
}
pub fn serialize(snapshot: &Snapshot) -> Result<SourceJson, GatewayError> {
    source::canonical(&SourceJson::from_serializable(snapshot)?)
}
