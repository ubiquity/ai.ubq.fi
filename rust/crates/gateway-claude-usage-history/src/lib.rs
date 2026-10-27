//! Finite retained-history store/query capability. Cannot mint account or quota authority.
mod store;
pub mod usage;
use gateway_auth::store as atomic;
use gateway_claude_usage_history_records::{
    Point,Bucket,Rollup,WINDOWS,BUCKET_MS,HOUR_MS,RETENTION_MS,STALE_MS,LIMIT,point_for_observation,retain_source,claude_history_bucket_record_source,claude_rollup_record_source,merge_source,object::SourceObject,types::ClaudeAcceptedObservation,
};
use gateway_core::{
    error::GatewayError,
    js_string::JsString16,
    query::{QuerySelector, QueryStore},
    source_control::encode_source_control_record,
    source_json::SourceJson,
    source_serialization as source,
    state::{RecordKey, RecordMutation},
};
use serde_json::json;
use store::{error, now, scan_selector, uuid};
pub trait HistoryHost {
    fn now_ms(&self) -> i64;
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError>;
    fn parse_date(&self, value: &JsString16) -> Option<i64>;
}
pub struct History<'a, S, H> {
    pub store: &'a S,
    pub host: &'a H,
}
fn put<H: HistoryHost>(
    host: &H,
    key: RecordKey,
    value: &SourceJson,
    expiry: Option<i64>,
) -> Result<RecordMutation, GatewayError> {
    Ok(RecordMutation::Put {
        key,
        record: encode_source_control_record(uuid(host)?, value, expiry)?,
    })
}
impl<S: QueryStore, H: HistoryHost> History<'_, S, H> {
    pub async fn record_claude_capacity_observation(
        &self,
        event: &ClaudeAcceptedObservation,
    ) -> Result<bool, GatewayError> {
        let mut event = event.clone();
        if let Some(snapshot) = source::member(&event.observation, "usage_snapshot") {
            let snapshot = gateway_claude_usage_protocol::snapshot::read(&snapshot, &|s| {
                self.host.parse_date(s)
            })
            .ok_or_else(|| error("Accepted Claude snapshot provenance is invalid"))?;
            let mut fields = SourceObject::parse(&event.observation)?;
            fields.set("usage_snapshot", &snapshot)?;
            event.observation = fields.source_json()?;
        }
        let Some(point) = point_for_observation(&event)? else {
            return Ok(false);
        };
        let p: Point = point.deserialize()?;
        let key = p.history_key();
        let at = now(self.host)?;
        for _ in 0..4 {
            let row = self.store.read(&key).await?;
            let previous = row
                .as_ref()
                .filter(|r| !r.expires_at_ms.is_some_and(|t| t <= at))
                .map(|r| claude_history_bucket_record_source(&key, r))
                .transpose()?;
            let Some(next) = retain_source(previous.as_ref(), &point)? else {
                return Ok(false);
            };
            let mut checks = vec![atomic::check(&key, row.as_ref())];
            let expiry = (p.bucket_start_at_ms as i64 + BUCKET_MS + RETENTION_MS).max(at + 1);
            let mut writes = vec![put(self.host, key.clone(), &next, Some(expiry))?];
            for window in WINDOWS {
                if p.windows.get(window).is_none() {
                    continue;
                }
                let key = p.rollup_key(window);
                let row = self.store.read(&key).await?;
                let previous = row
                    .as_ref()
                    .filter(|r| !r.expires_at_ms.is_some_and(|t| t <= at))
                    .map(|r| claude_rollup_record_source(&key, r))
                    .transpose()?;
                let merged = merge_source(previous.as_ref(), &point, window)?;
                checks.push(atomic::check(&key, row.as_ref()));
                writes.push(put(self.host, key, &merged, None)?);
            }
            if let Some(snapshot) = source::member(&point, "usage_snapshot") {
                let snapshot = gateway_claude_usage_protocol::snapshot::read(&snapshot, &|s| {
                    self.host.parse_date(s)
                })
                .ok_or_else(|| error("Claude snapshot is invalid"))?;
                for meter in &snapshot.meters {
                    let key = usage::key(&p, meter)?;
                    let row = self.store.read(&key).await?;
                    let previous = row
                        .as_ref()
                        .filter(|r| r.expires_at_ms.is_none_or(|t| t > at))
                        .map(|r| usage::record(&key, r))
                        .transpose()?;
                    let merged = usage::merge(previous.as_ref(), &p, meter)?;
                    checks.push(atomic::check(&key, row.as_ref()));
                    writes.push(put(self.host, key, &merged, None)?);
                }
            }
            if atomic::commit(self.store, checks, writes).await? {
                return Ok(true);
            }
        }
        Err(error("Claude history contention exceeded four attempts"))
    }
    pub async fn claude_capacity_history(&self, at: i64) -> SourceJson {
        let base=source::canonical(&SourceJson::from_serializable(&json!({"v":1,"scan":"unavailable","truncated":false,"bucket_ms":BUCKET_MS,"retention_ms":RETENTION_MS,"stale_after_ms":STALE_MS,"points":[]})).expect("finite history base")).expect("finite history base");
        let read: Result<SourceJson, GatewayError> = async {
            let cutoff = (at - RETENTION_MS).max(0);
            let first = cutoff / BUCKET_MS * BUCKET_MS;
            let end = at / BUCKET_MS * BUCKET_MS + BUCKET_MS;
            let selector = QuerySelector::Range {
                start: atomic::key(json!([
                    "uos_ai",
                    "provider_capacity",
                    "claude",
                    "v1",
                    "history",
                    first
                ])),
                end: atomic::key(json!([
                    "uos_ai",
                    "provider_capacity",
                    "claude",
                    "v1",
                    "history",
                    end
                ])),
            };
            let rows = scan_selector(self.store, selector, true, at, LIMIT + 1).await?;
            let mut points = Vec::new();
            let mut truncated = rows.len() > LIMIT;
            for entry in rows.into_iter().take(LIMIT) {
                let value = claude_history_bucket_record_source(&entry.key, &entry.record)?;
                let bucket: Bucket = value.deserialize()?;
                for point in bucket.points.into_iter().rev() {
                    let p: Point = point.deserialize()?;
                    if p.sampled_at_ms > at as f64 || p.clock() < cutoff as f64 {
                        continue;
                    }
                    if points.len() >= LIMIT {
                        truncated = true;
                        break;
                    }
                    let point = if let Some(snapshot) = &p.usage_snapshot {
                        let snapshot =
                            gateway_claude_usage_protocol::snapshot::read(snapshot, &|s| {
                                self.host.parse_date(s)
                            })
                            .ok_or_else(|| error("Claude snapshot is invalid"))?;
                        let mut projected = SourceObject::parse(&point)?;
                        projected.set("usage_snapshot", &snapshot)?;
                        projected.source_json()?
                    } else {
                        point
                    };
                    points.push((point, p.clock() as i64));
                }
                if points.len() >= LIMIT && truncated {
                    break;
                }
            }
            points.sort_by_key(|(_, at)| *at);
            let mut out = SourceObject::parse(&base)?;
            out.set("scan", "ok")?;
            out.set("truncated", &truncated)?;
            out.set(
                "points",
                &points.into_iter().map(|(p, _)| p).collect::<Vec<_>>(),
            )?;
            out.source_json()
        }
        .await;
        read.unwrap_or(base)
    }
    pub async fn claude_capacity_rollups(&self, since: i64, at: i64) -> SourceJson {
        let base = SourceJson::parse(r#"{"scan":"unavailable","truncated":false,"points":[]}"#)
            .expect("finite rollup base");
        let read: Result<SourceJson, GatewayError> = async {
            let first = since / HOUR_MS * HOUR_MS;
            let last = at / HOUR_MS * HOUR_MS;
            let selector = QuerySelector::Range {
                start: atomic::key(json!([
                    "uos_ai",
                    "provider_capacity",
                    "claude",
                    "v1",
                    "rollup",
                    first
                ])),
                end: atomic::key(json!([
                    "uos_ai",
                    "provider_capacity",
                    "claude",
                    "v1",
                    "rollup",
                    last + HOUR_MS
                ])),
            };
            let rows = scan_selector(self.store, selector, true, at, LIMIT + 1).await?;
            let truncated = rows.len() > LIMIT;
            let mut points = Vec::new();
            for entry in rows.into_iter().take(LIMIT) {
                let value = claude_rollup_record_source(&entry.key, &entry.record)?;
                let p: Rollup = value.deserialize()?;
                if p.last_sampled_at_ms > at as f64 {
                    return Err(error("Claude rollup evidence is corrupt"));
                }
                points.push((value, p.bucket_start_at_ms as i64));
            }
            points.sort_by_key(|(_, at)| *at);
            let mut out = SourceObject::parse(&base)?;
            out.set("scan", "ok")?;
            out.set("truncated", &truncated)?;
            out.set(
                "points",
                &points.into_iter().map(|(p, _)| p).collect::<Vec<_>>(),
            )?;
            out.source_json()
        }
        .await;
        read.unwrap_or(base)
    }
}
