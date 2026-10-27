//! Retained Claude history record shapes, identities and source downsampling math.
pub mod object;
pub mod types;
use gateway_auth::crypto::sha256_hex;
use gateway_auth::store;
use gateway_core::{
    error::GatewayError,
    js_string::JsString16,
    source_control::decode_source_control_record,
    source_json::SourceJson,
    source_serialization as source,
    state::{RecordKey, StoredRecord},
};
use object::SourceObject;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
use types::{ClaudeAcceptedObservation, ClaudeObservationState, CredentialKind};
pub const BUCKET_MS: i64 = 900_000;
pub const HOUR_MS: i64 = 3_600_000;
pub const RETENTION_MS: i64 = 604_800_000;
pub const STALE_MS: i64 = 1_800_000;
fn error(message: &str) -> GatewayError {
    GatewayError::unavailable(message, "observability_state_invalid")
}
pub const WINDOWS: [&str; 2] = ["five_hour", "seven_day"];
pub const LIMIT: usize = 20_000;
fn safe(n: f64) -> bool {
    n.is_finite() && n.fract() == 0.0 && (0.0..=9_007_199_254_740_991.0).contains(&n)
}
fn optional_time(n: Option<f64>) -> bool {
    n.is_none_or(safe)
}
fn identity(s: &str) -> bool {
    !s.is_empty() && s.encode_utf16().count() <= 200
}
fn workspace(s: &Option<JsString16>) -> bool {
    s.as_ref()
        .is_none_or(|s| !s.as_units().is_empty() && s.as_units().len() <= 200)
}
fn percent(n: Option<f64>) -> bool {
    n.is_none_or(|n| n.is_finite() && (0.0..=100.0).contains(&n))
}
pub fn used_percent(value: Option<f64>) -> Option<f64> {
    value
        .filter(|n| n.is_finite() && *n >= 0.0)
        .map(|n| (n * 100.0).min(100.0))
}
#[derive(Clone, Deserialize, Serialize)]
pub struct Window {
    status: Option<JsString16>,
    used_percent: Option<f64>,
    reset_at_ms: Option<f64>,
    observed_at_ms: Option<f64>,
}
impl Window {
    pub fn valid(&self, sampled: f64) -> bool {
        percent(self.used_percent)
            && optional_time(self.reset_at_ms)
            && optional_time(self.observed_at_ms)
            && self.observed_at_ms.is_none_or(|t| t <= sampled)
    }
}
#[derive(Clone, Deserialize, Serialize)]
pub struct Standard {
    family: String,
    limit: Option<f64>,
    remaining: Option<f64>,
    reset_at_ms: Option<f64>,
    observed_at_ms: Option<f64>,
}
impl Standard {
    pub fn valid(&self, sampled: f64) -> bool {
        ["requests", "input-tokens", "output-tokens"].contains(&self.family.as_str())
            && [self.limit, self.remaining]
                .iter()
                .all(|n| n.is_none_or(|n| n.is_finite() && n >= 0.0))
            && optional_time(self.reset_at_ms)
            && optional_time(self.observed_at_ms)
            && self.observed_at_ms.is_none_or(|t| t <= sampled)
    }
}
#[derive(Clone, Deserialize, Serialize)]
pub struct Windows {
    five_hour: Option<Window>,
    seven_day: Option<Window>,
}
impl Windows {
    pub fn get(&self, name: &str) -> Option<&Window> {
        match name {
            "five_hour" => self.five_hour.as_ref(),
            "seven_day" => self.seven_day.as_ref(),
            _ => None,
        }
    }
}
#[derive(Clone, Deserialize, Serialize)]
pub struct Point {
    pub v: f64,
    pub point_id: String,
    pub bucket_start_at_ms: f64,
    pub sampled_at_ms: f64,
    pub source_observed_at_ms: Option<f64>,
    pub account_id: String,
    pub account_cohort_id: String,
    pub credential_kind: CredentialKind,
    pub workspace_id: Option<JsString16>,
    pub state: ClaudeObservationState,
    pub response_status: Option<f64>,
    pub windows: Windows,
    #[serde(default, skip_serializing_if = "Option::is_none", deserialize_with="present_snapshot")]
    pub usage_snapshot: Option<SourceJson>,
    pub standard: Vec<Standard>,
}
fn present_snapshot<'de,D:serde::Deserializer<'de>>(deserializer:D)->Result<Option<SourceJson>,D::Error>{<SourceJson as Deserialize>::deserialize(deserializer).map(Some)}
impl Point {
    pub fn clock(&self) -> f64 {
        self.source_observed_at_ms.unwrap_or(self.sampled_at_ms)
    }
    pub fn valid(&self) -> bool {
        self.usage_snapshot.as_ref().is_none_or(|s| {
            gateway_claude_usage_protocol::snapshot::provenance(s)
                .is_some_and(|(_, at)| at as f64 <= self.sampled_at_ms)
        }) && self.v == 1.0
            && identity(&self.point_id)
            && identity(&self.account_id)
            && identity(&self.account_cohort_id)
            && workspace(&self.workspace_id)
            && safe(self.bucket_start_at_ms)
            && safe(self.sampled_at_ms)
            && optional_time(self.source_observed_at_ms)
            && self
                .source_observed_at_ms
                .is_none_or(|t| t <= self.sampled_at_ms)
            && (self.clock() / BUCKET_MS as f64).floor() * BUCKET_MS as f64
                == self.bucket_start_at_ms
            && WINDOWS.iter().all(|n| {
                self.windows
                    .get(n)
                    .is_none_or(|w| w.valid(self.sampled_at_ms))
            })
            && self.standard.len() <= 3
            && self.standard.iter().all(|w| w.valid(self.sampled_at_ms))
            && self
                .response_status
                .is_none_or(|n| safe(n) && (100.0..=599.0).contains(&n))
    }
    pub fn history_key(&self) -> RecordKey {
        history_key(
            self.bucket_start_at_ms,
            &self.account_id,
            &self.account_cohort_id,
        )
    }
    fn rollup_bucket(&self, window: &Window) -> f64 {
        (window
            .observed_at_ms
            .or(self.source_observed_at_ms)
            .unwrap_or(self.sampled_at_ms)
            / HOUR_MS as f64)
            .floor()
            * HOUR_MS as f64
    }
    pub fn rollup_key(&self, name: &str) -> RecordKey {
        let w = self.windows.get(name).expect("validated window");
        rollup_key_parts(
            self.rollup_bucket(w),
            &self.account_id,
            &self.account_cohort_id,
            name,
            w.reset_at_ms,
        )
    }
}
#[derive(Clone, Deserialize, Serialize)]
struct Flags {
    normal: f64,
    unavailable: bool,
    recovery: bool,
    five_hour: bool,
    seven_day: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    usage: Option<bool>,
}
impl Default for Flags {
    fn default() -> Self {
        Self {
            normal: 0.0,
            unavailable: false,
            recovery: false,
            five_hour: false,
            seven_day: false,
            usage: None,
        }
    }
}
#[derive(Deserialize, Serialize)]
pub struct Bucket {
    pub v: f64,
    pub bucket_start_at_ms: f64,
    pub account_id: String,
    pub account_cohort_id: String,
    pub points: Vec<SourceJson>,
    flags: Flags,
}
impl Bucket {
    pub fn key(&self) -> RecordKey {
        history_key(
            self.bucket_start_at_ms,
            &self.account_id,
            &self.account_cohort_id,
        )
    }
    pub fn valid(&self) -> bool {
        if self.v != 1.0
            || !safe(self.bucket_start_at_ms)
            || !identity(&self.account_id)
            || !identity(&self.account_cohort_id)
            || !safe(self.flags.normal)
            || self.flags.normal > 2.0
            || self.points.len() > 6
        {
            return false;
        }
        let mut ids = BTreeSet::new();
        self.points.iter().all(|v| {
            v.deserialize::<Point>().is_ok_and(|p| {
                p.valid()
                    && p.bucket_start_at_ms == self.bucket_start_at_ms
                    && p.account_id == self.account_id
                    && p.account_cohort_id == self.account_cohort_id
                    && ids.insert(p.point_id)
            })
        })
    }
}
#[derive(Deserialize, Serialize)]
pub struct Rollup {
    pub v: f64,
    pub bucket_start_at_ms: f64,
    pub account_id: String,
    pub account_cohort_id: String,
    pub credential_kind: CredentialKind,
    pub workspace_id: Option<JsString16>,
    pub window: String,
    pub reset_at_ms: Option<f64>,
    pub samples: f64,
    pub available_sample_count: f64,
    pub unavailable_sample_count: f64,
    pub first_source_observed_at_ms: Option<f64>,
    pub last_source_observed_at_ms: Option<f64>,
    pub first_sampled_at_ms: f64,
    pub last_sampled_at_ms: f64,
    pub min_used_percent: Option<f64>,
    pub max_used_percent: Option<f64>,
    pub last_used_percent: Option<f64>,
    pub last_status: Option<JsString16>,
}
impl Rollup {
    pub fn key(&self) -> RecordKey {
        rollup_key_parts(
            self.bucket_start_at_ms,
            &self.account_id,
            &self.account_cohort_id,
            &self.window,
            self.reset_at_ms,
        )
    }
    pub fn valid(&self) -> bool {
        if self.v != 1.0
            || !safe(self.bucket_start_at_ms)
            || self.bucket_start_at_ms % HOUR_MS as f64 != 0.0
            || !identity(&self.account_id)
            || !identity(&self.account_cohort_id)
            || !workspace(&self.workspace_id)
            || !WINDOWS.contains(&self.window.as_str())
            || !optional_time(self.reset_at_ms)
            || ![
                self.samples,
                self.available_sample_count,
                self.unavailable_sample_count,
                self.first_sampled_at_ms,
                self.last_sampled_at_ms,
            ]
            .into_iter()
            .all(safe)
            || self.samples == 0.0
            || self.available_sample_count + self.unavailable_sample_count != self.samples
            || self.first_sampled_at_ms > self.last_sampled_at_ms
            || !optional_time(self.first_source_observed_at_ms)
            || !optional_time(self.last_source_observed_at_ms)
            || ![
                self.min_used_percent,
                self.max_used_percent,
                self.last_used_percent,
            ]
            .into_iter()
            .all(percent)
        {
            return false;
        }
        let (first, last) = (
            self.first_source_observed_at_ms,
            self.last_source_observed_at_ms,
        );
        if first.is_some() != last.is_some()
            || first.zip(last).is_some_and(|(f, l)| {
                f > l
                    || l > self.last_sampled_at_ms
                    || f < self.bucket_start_at_ms
                    || l >= self.bucket_start_at_ms + HOUR_MS as f64
            })
        {
            return false;
        }
        let (min, max) = (self.min_used_percent, self.max_used_percent);
        min.is_some() == max.is_some()
            && min.zip(max).is_none_or(|(a, b)| a <= b)
            && self
                .last_used_percent
                .is_none_or(|v| min.zip(max).is_some_and(|(a, b)| v >= a && v <= b))
    }
}
pub fn history_key(bucket: f64, account: &str, cohort: &str) -> RecordKey {
    store::key(json!([
        "uos_ai",
        "provider_capacity",
        "claude",
        "v1",
        "history",
        bucket,
        account,
        cohort
    ]))
}
pub fn rollup_key_parts(
    bucket: f64,
    account: &str,
    cohort: &str,
    window: &str,
    reset: Option<f64>,
) -> RecordKey {
    store::key(json!([
        "uos_ai",
        "provider_capacity",
        "claude",
        "v1",
        "rollup",
        bucket,
        account,
        cohort,
        window,
        reset.map_or_else(|| json!("unknown"), |n| json!(n))
    ]))
}
pub fn valid_history_point_source(v: &SourceJson) -> bool {
    v.deserialize::<Point>().is_ok_and(|p| p.valid())
}
pub fn valid_bucket_source(v: &SourceJson) -> bool {
    v.deserialize::<Bucket>().is_ok_and(|p| p.valid())
}
pub fn valid_rollup_source(v: &SourceJson) -> bool {
    v.deserialize::<Rollup>().is_ok_and(|p| p.valid())
}
// Existing scalar custody readers opt into the same finite named validation only.
pub fn valid_history_point(v: &Value) -> bool {
    SourceJson::from_serializable(v).is_ok_and(|v| valid_history_point_source(&v))
}
pub fn valid_bucket(v: &Value) -> bool {
    SourceJson::from_serializable(v).is_ok_and(|v| valid_bucket_source(&v))
}
pub fn valid_rollup(v: &Value) -> bool {
    SourceJson::from_serializable(v).is_ok_and(|v| valid_rollup_source(&v))
}
pub fn claude_history_bucket_record_source(
    key: &RecordKey,
    row: &StoredRecord,
) -> Result<SourceJson, GatewayError> {
    let value = decode_source_control_record(row)?;
    let bucket: Bucket = value.deserialize()?;
    if !bucket.valid() || bucket.key() != *key {
        return Err(error("Claude history evidence is corrupt"));
    }
    Ok(value)
}
pub fn claude_rollup_record_source(
    key: &RecordKey,
    row: &StoredRecord,
) -> Result<SourceJson, GatewayError> {
    let value = decode_source_control_record(row)?;
    let rollup: Rollup = value.deserialize()?;
    if !rollup.valid() || rollup.key() != *key {
        return Err(error("Claude rollup evidence is corrupt"));
    }
    Ok(value)
}
#[derive(Deserialize)]
struct InputWindow {
    status: Option<JsString16>,
    utilization: Option<f64>,
    reset_at_ms: Option<f64>,
    observed_at_ms: f64,
}
#[derive(Deserialize)]
struct InputStandard {
    limit: Option<f64>,
    remaining: Option<f64>,
    reset_at_ms: Option<f64>,
    observed_at_ms: f64,
}
#[derive(Deserialize)]
struct Observation {
    windows: BTreeMap<String, InputWindow>,
    standard: BTreeMap<String, InputStandard>,
    #[serde(rename = "representative_claim")]
    _representative_claim: Option<JsString16>,
    #[serde(rename = "observed_at_ms")]
    _observed_at_ms: f64,
}
/// Only the actual accepted event's safe fields enter the source digest. Raw
/// quota/identity/credential documents and unknown event metadata are not telemetry.
fn safe_observation(event: &ClaudeAcceptedObservation) -> Result<SourceJson, GatewayError> {
    let mut observation = SourceObject::parse(&event.observation)?;
    for (field, names, leaves) in [
        (
            "windows",
            &["5h", "7d"][..],
            &["status", "utilization", "reset_at_ms", "observed_at_ms"][..],
        ),
        (
            "standard",
            &["requests", "input-tokens", "output-tokens"][..],
            &["limit", "remaining", "reset_at_ms", "observed_at_ms"][..],
        ),
    ] {
        let input = observation
            .get(field)
            .ok_or_else(|| error("Accepted Claude observation field is missing"))?;
        let mut output = SourceObject::parse(&SourceObject::parse(input)?.select(names)?)?;
        for name in names {
            if let Some(value) = output.get(name).cloned() {
                output.set(name, &SourceObject::parse(&value)?.select(leaves)?)?;
            }
        }
        observation.set(field, &output.source_json()?)?;
    }
    source::canonical(&observation.select(&[
        "windows",
        "standard",
        "representative_claim",
        "observed_at_ms",
        "usage_snapshot",
    ])?)
}
pub fn point_for_observation(
    event: &ClaudeAcceptedObservation,
) -> Result<Option<SourceJson>, GatewayError> {
    let observation = safe_observation(event)?;
    let parsed: Observation = observation.deserialize()?;
    if source::member(&observation, "usage_snapshot").is_none()
        && parsed.windows.is_empty()
        && parsed.standard.is_empty()
    {
        return Ok(None);
    }
    let standard_order = source::entries(
        &source::member(&observation, "standard")
            .ok_or_else(|| error("Accepted Claude standard metadata is missing"))?,
    )
    .ok_or_else(|| error("Accepted Claude standard metadata is invalid"))?
    .into_iter()
    .map(|(name, _)| name.try_utf8())
    .collect::<Result<Vec<_>, _>>()?;
    let digest = source::object(vec![
        ("account", SourceJson::from_serializable(&event.account_id)?),
        (
            "cohort",
            SourceJson::from_serializable(&event.account_cohort_id)?,
        ),
        (
            "kind",
            SourceJson::from_serializable(&event.credential_kind)?,
        ),
        (
            "workspace",
            SourceJson::from_serializable(&event.workspace_id)?,
        ),
        ("state", SourceJson::from_serializable(&event.state)?),
        (
            "response_status",
            SourceJson::from_serializable(&event.response_status)?,
        ),
        ("observation", observation.clone()),
    ])?;
    let window = |name: &str| {
        parsed.windows.get(name).map(|w| Window {
            status: w.status.clone(),
            used_percent: used_percent(w.utilization),
            reset_at_ms: w.reset_at_ms,
            observed_at_ms: safe(w.observed_at_ms).then_some(w.observed_at_ms),
        })
    };
    let source_time = event
        .source_observed_at_ms
        .filter(|n| (0..=9_007_199_254_740_991).contains(n))
        .map(|n| n as f64);
    let point = Point {
        usage_snapshot: source::member(&observation, "usage_snapshot"),
        v: 1.0,
        point_id: sha256_hex(source::canonical(&digest)?.as_bytes()),
        bucket_start_at_ms: (source_time.unwrap_or(event.sampled_at_ms as f64) / BUCKET_MS as f64)
            .floor()
            * BUCKET_MS as f64,
        sampled_at_ms: event.sampled_at_ms as f64,
        source_observed_at_ms: source_time,
        account_id: event.account_id.clone(),
        account_cohort_id: event.account_cohort_id.clone(),
        credential_kind: event.credential_kind,
        workspace_id: event.workspace_id.clone(),
        state: event.state,
        response_status: event.response_status.map(f64::from),
        windows: Windows {
            five_hour: window("5h"),
            seven_day: window("7d"),
        },
        standard: standard_order
            .iter()
            .map(String::as_str)
            .filter_map(|name| {
                parsed.standard.get(name).map(|w| Standard {
                    family: name.into(),
                    limit: w.limit,
                    remaining: w.remaining,
                    reset_at_ms: w.reset_at_ms,
                    observed_at_ms: Some(w.observed_at_ms),
                })
            })
            .collect(),
    };
    if !point.valid() {
        return Ok(None);
    }
    Ok(Some(source::canonical(&SourceJson::from_serializable(
        &point,
    )?)?))
}
pub fn retain_source(
    previous: Option<&SourceJson>,
    point: &SourceJson,
) -> Result<Option<SourceJson>, GatewayError> {
    let p: Point = point.deserialize()?;
    let old = previous
        .map(SourceJson::deserialize::<Bucket>)
        .transpose()?;
    let points = old
        .as_ref()
        .map(|b| b.points.as_slice())
        .unwrap_or_default();
    let parsed = points
        .iter()
        .map(SourceJson::deserialize::<Point>)
        .collect::<Result<Vec<_>, _>>()?;
    if points.len() >= 6 || parsed.iter().any(|v| v.point_id == p.point_id) {
        return Ok(None);
    }
    let mut flags = old.as_ref().map(|b| b.flags.clone()).unwrap_or_default();
    let mut retain = flags.normal < 2.0;
    if retain {
        flags.normal += 1.0;
    }
    if parsed.last().is_some_and(|v| v.state != p.state) {
        let flag = if p.state == ClaudeObservationState::Unavailable {
            &mut flags.unavailable
        } else {
            &mut flags.recovery
        };
        if !*flag {
            *flag = true;
            retain = true;
        }
    }
    for name in WINDOWS {
        let before = parsed.iter().rev().find_map(|p| p.windows.get(name));
        let after = p.windows.get(name);
        let flag = if name == "five_hour" {
            &mut flags.five_hour
        } else {
            &mut flags.seven_day
        };
        if let Some(after) = after
            && before.is_none_or(|b| b.reset_at_ms != after.reset_at_ms)
            && !*flag
            && !retain
        {
            *flag = true;
            retain = true;
        }
    }
    if !retain && p.usage_snapshot.is_some() && flags.usage != Some(true) {
        let anchors = |value: &SourceJson| -> Option<Vec<(JsString16, Option<JsString16>)>> {
            let rows = source::member(value, "meters")
                .as_ref()
                .and_then(source::array)?;
            rows.iter()
                    .map(|m| {
                        Some((
                            source::member(m, "id").as_ref().and_then(source::string)?,
                            source::member(m, "resets_at")
                                .as_ref()
                                .and_then(source::string),
                        ))
                    })
                    .collect::<Option<Vec<_>>>()
        };
        let before = parsed
            .iter()
            .rev()
            .find_map(|p| p.usage_snapshot.as_ref())
            .and_then(anchors);
        let after = p.usage_snapshot.as_ref().and_then(anchors);
        if before != after {
            flags.usage = Some(true);
            retain = true;
        }
    }
    if !retain {
        return Ok(None);
    }
    let mut timed: Vec<_> = points
        .iter()
        .cloned()
        .zip(
            parsed
                .iter()
                .map(|p| (p.clock() as i64, p.sampled_at_ms as i64)),
        )
        .collect();
    timed.push((point.clone(), (p.clock() as i64, p.sampled_at_ms as i64)));
    timed.sort_by_key(|(_, t)| *t);
    Ok(Some(source::canonical(&SourceJson::from_serializable(
        &Bucket {
            v: 1.0,
            bucket_start_at_ms: p.bucket_start_at_ms,
            account_id: p.account_id,
            account_cohort_id: p.account_cohort_id,
            points: timed.into_iter().map(|(v, _)| v).collect(),
            flags,
        },
    )?)?))
}
pub fn merge_source(
    previous: Option<&SourceJson>,
    point: &SourceJson,
    name: &str,
) -> Result<SourceJson, GatewayError> {
    let p: Point = point.deserialize()?;
    let w = p
        .windows
        .get(name)
        .ok_or_else(|| error("Claude rollup window is missing"))?;
    let bucket = p.rollup_bucket(w);
    let old = previous
        .map(SourceJson::deserialize::<Rollup>)
        .transpose()?;
    if old.as_ref().is_some_and(|v| {
        v.bucket_start_at_ms != bucket
            || v.account_id != p.account_id
            || v.account_cohort_id != p.account_cohort_id
            || v.credential_kind != p.credential_kind
            || v.workspace_id != p.workspace_id
            || v.window != name
            || v.reset_at_ms != w.reset_at_ms
    }) {
        return Err(error("Claude rollup binding mismatch"));
    }
    let source_at = w.observed_at_ms.or(p.source_observed_at_ms);
    let sampled = p.sampled_at_ms;
    let newer = old.as_ref().is_none_or(|v| {
        source_at.unwrap_or(sampled) >= v.last_source_observed_at_ms.unwrap_or(v.last_sampled_at_ms)
    });
    let mut values = Vec::new();
    let mut times = Vec::new();
    if let Some(v) = &old {
        values.extend(
            [v.min_used_percent, v.max_used_percent]
                .into_iter()
                .flatten(),
        );
        times.extend(
            [v.first_source_observed_at_ms, v.last_source_observed_at_ms]
                .into_iter()
                .flatten(),
        );
    }
    values.extend(w.used_percent);
    times.extend(source_at);
    let rollup = Rollup {
        v: 1.0,
        bucket_start_at_ms: bucket,
        account_id: p.account_id,
        account_cohort_id: p.account_cohort_id,
        credential_kind: p.credential_kind,
        workspace_id: p.workspace_id,
        window: name.into(),
        reset_at_ms: w.reset_at_ms,
        samples: old.as_ref().map_or(0.0, |v| v.samples) + 1.0,
        available_sample_count: old.as_ref().map_or(0.0, |v| v.available_sample_count)
            + f64::from(u8::from(p.state == ClaudeObservationState::Available)),
        unavailable_sample_count: old.as_ref().map_or(0.0, |v| v.unavailable_sample_count)
            + f64::from(u8::from(p.state == ClaudeObservationState::Unavailable)),
        first_source_observed_at_ms: times.iter().copied().reduce(f64::min),
        last_source_observed_at_ms: times.iter().copied().reduce(f64::max),
        first_sampled_at_ms: old
            .as_ref()
            .map_or(sampled, |v| v.first_sampled_at_ms.min(sampled)),
        last_sampled_at_ms: old
            .as_ref()
            .map_or(sampled, |v| v.last_sampled_at_ms.max(sampled)),
        min_used_percent: values.iter().copied().reduce(f64::min),
        max_used_percent: values.iter().copied().reduce(f64::max),
        last_used_percent: if newer {
            w.used_percent
        } else {
            old.as_ref()
                .expect("previous observation")
                .last_used_percent
        },
        last_status: if newer {
            w.status.clone()
        } else {
            old.as_ref()
                .expect("previous observation")
                .last_status
                .clone()
        },
    };
    source::canonical(&SourceJson::from_serializable(&rollup)?)
}
