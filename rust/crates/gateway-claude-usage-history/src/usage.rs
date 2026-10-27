use crate::{
    History, HistoryHost,
    store::{error, scan_selector},
};
use gateway_auth::{crypto::sha256_hex, store};
use gateway_claude_usage_history_records::{HOUR_MS, LIMIT, Point, object::SourceObject};
use gateway_claude_usage_protocol::snapshot::{Meter, read_meter, safe_time};
use gateway_source::{
    error::GatewayError, source_json::SourceJson, source_serialization as source,
};
use gateway_core::{
    query::QuerySelector, query::QueryStore, source_control::decode_source_control_record,
    state::StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use serde::{Deserialize, Serialize};
use serde_json::json;
#[derive(Clone, Deserialize, Serialize)]
struct Rollup {
    v: u8,
    bucket_start_at_ms: i64,
    account_id: String,
    account_cohort_id: String,
    credential_kind: gateway_claude_usage_history_records::types::CredentialKind,
    workspace_id: Option<gateway_source::js_string::JsString16>,
    meter: Meter,
    samples: u64,
    first_observed_at_ms: i64,
    last_observed_at_ms: i64,
    min_used_percent: Option<f64>,
    max_used_percent: Option<f64>,
}
fn valid(v: &SourceJson) -> Option<Rollup> {
    let meter = read_meter(&source::member(v, "meter")?)?;
    let r: Rollup = source::canonical(v).ok()?.deserialize().ok()?;
    let identity = |s: &str| !s.is_empty() && s.encode_utf16().count() <= 200;
    if r.v != 1
        || !identity(&r.account_id)
        || !identity(&r.account_cohort_id)
        || r.workspace_id
            .as_ref()
            .is_some_and(|s| s.as_units().is_empty() || s.as_units().len() > 200)
        || ![
            r.bucket_start_at_ms,
            r.first_observed_at_ms,
            r.last_observed_at_ms,
        ]
        .iter()
        .all(|n| safe_time(*n as f64))
        || r.samples == 0
        || r.samples > gateway_contracts::identity::MAX_SAFE_INTEGER as u64
        || r.bucket_start_at_ms % HOUR_MS != 0
        || r.first_observed_at_ms > r.last_observed_at_ms
        || meter.observed_at_ms != r.last_observed_at_ms
        || r.first_observed_at_ms < r.bucket_start_at_ms
        || r.last_observed_at_ms >= r.bucket_start_at_ms + HOUR_MS
    {
        return None;
    }
    if r.min_used_percent.is_some() != r.max_used_percent.is_some()
        || [r.min_used_percent, r.max_used_percent]
            .iter()
            .flatten()
            .any(|n| !n.is_finite())
        || r.min_used_percent
            .zip(r.max_used_percent)
            .is_some_and(|(min, max)| {
                min > max || meter.used_percent.is_some_and(|n| n < min || n > max)
            })
    {
        return None;
    }
    Some(r)
}
pub(crate) fn key(point: &Point, meter: &Meter) -> Result<RecordKey, GatewayError> {
    key_parts(&point.account_id, &point.account_cohort_id, meter)
}
fn key_parts(account: &str, cohort: &str, meter: &Meter) -> Result<RecordKey, GatewayError> {
    let tuple = source::canonical(&SourceJson::from_serializable(&(
        meter.id.clone(),
        meter.resets_at.clone(),
    ))?)?;
    Ok(store::key(json!([
        "uos_ai",
        "provider_capacity",
        "claude",
        "v1",
        "usage_rollup",
        meter.observed_at_ms / HOUR_MS * HOUR_MS,
        account,
        cohort,
        sha256_hex(tuple.as_bytes())
    ])))
}
pub(crate) fn record(key: &RecordKey, row: &StoredRecord) -> Result<SourceJson, GatewayError> {
    let value = decode_source_control_record(row)?;
    let r = valid(&value).ok_or_else(|| error("Claude usage rollup is invalid"))?;
    if *key != key_parts(&r.account_id, &r.account_cohort_id, &r.meter)? {
        return Err(error("Claude usage rollup key is corrupt"));
    }
    Ok(value)
}
pub(crate) fn merge(
    previous: Option<&SourceJson>,
    point: &Point,
    meter: &Meter,
) -> Result<SourceJson, GatewayError> {
    let old = previous
        .map(|v| valid(v).ok_or_else(|| error("Claude usage rollup is invalid")))
        .transpose()?;
    let bucket = meter.observed_at_ms / HOUR_MS * HOUR_MS;
    if old.as_ref().is_some_and(|r| {
        r.account_id != point.account_id
            || r.account_cohort_id != point.account_cohort_id
            || r.bucket_start_at_ms != bucket
            || r.credential_kind != point.credential_kind
            || r.workspace_id != point.workspace_id
            || r.meter.id != meter.id
            || r.meter.resets_at != meter.resets_at
    }) {
        return Err(error("Claude usage rollup binding mismatch"));
    }
    let values: Vec<f64> = [
        old.as_ref().and_then(|r| r.min_used_percent),
        old.as_ref().and_then(|r| r.max_used_percent),
        meter.used_percent,
    ]
    .into_iter()
    .flatten()
    .collect();
    let r = Rollup {
        v: 1,
        bucket_start_at_ms: bucket,
        account_id: point.account_id.clone(),
        account_cohort_id: point.account_cohort_id.clone(),
        credential_kind: point.credential_kind,
        workspace_id: point.workspace_id.clone(),
        meter: old
            .as_ref()
            .filter(|r| meter.observed_at_ms < r.last_observed_at_ms)
            .map(|r| r.meter.clone())
            .unwrap_or_else(|| meter.clone()),
        samples: old.as_ref().map_or(0, |r| r.samples) + 1,
        first_observed_at_ms: old.as_ref().map_or(meter.observed_at_ms, |r| {
            r.first_observed_at_ms.min(meter.observed_at_ms)
        }),
        last_observed_at_ms: old.as_ref().map_or(meter.observed_at_ms, |r| {
            r.last_observed_at_ms.max(meter.observed_at_ms)
        }),
        min_used_percent: values.iter().copied().reduce(f64::min),
        max_used_percent: values.iter().copied().reduce(f64::max),
    };
    source::canonical(&SourceJson::from_serializable(&r)?)
}
impl<S: QueryStore, H: HistoryHost> History<'_, S, H> {
    pub async fn usage_rollups(&self, since: i64, at: i64) -> SourceJson {
        let base = SourceJson::parse(r#"{"scan":"unavailable","truncated":false,"points":[]}"#)
            .unwrap_or_else(|_| unreachable!("fixed source rollup literal"));
        let read: Result<SourceJson, GatewayError> = async {
            let prefix = |hour| {
                store::key(json!([
                    "uos_ai",
                    "provider_capacity",
                    "claude",
                    "v1",
                    "usage_rollup",
                    hour
                ]))
            };
            let rows = scan_selector(
                self.store,
                QuerySelector::Range {
                    start: prefix(since / HOUR_MS * HOUR_MS),
                    end: prefix(at / HOUR_MS * HOUR_MS + HOUR_MS),
                },
                false,
                at,
                LIMIT + 1,
            )
            .await?;
            let truncated = rows.len() > LIMIT;
            let mut points = Vec::new();
            for entry in rows.into_iter().take(LIMIT) {
                let value = record(&entry.key, &entry.record)?;
                let point = valid(&value).ok_or_else(|| error("Claude usage rollup is invalid"))?;
                if point.last_observed_at_ms > at {
                    return Err(error("Claude usage rollup evidence is corrupt"));
                }
                points.push(value);
            }
            let mut out = SourceObject::parse(&base)?;
            out.set("scan", "ok")?;
            out.set("truncated", &truncated)?;
            out.set("points", &points)?;
            out.source_json()
        }
        .await;
        read.unwrap_or(base)
    }
}
