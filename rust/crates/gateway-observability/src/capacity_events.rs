//! Redacted source capacity markers and verified reset backfill; no reset execution.
use crate::{ObservabilityRuntime, store, types::ObservabilityHost};
use gateway_capacity_projection::scalars::safe_i64;
use gateway_capacity_projection::{
    events::{parse_shadow, valid_event, valid_redemption},
    shapes::{BUCKET_MS, RETENTION_MS},
};
use gateway_core::query::QueryStore;
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::collections::BTreeMap;
pub fn event_key(kind: &str, id: &str) -> gateway_kv_codec::key::RecordKey {
    store::key(json!(["uos_ai", "provider_capacity", "v1", kind, id]))
}
fn public_event(kind: &str, v: &Value) -> Value {
    let mut out = json!({"v":1,"event_id":v["event_id"],"observed_at_ms":v["observed_at_ms"]});
    let fields: &[&str] = match kind {
        "reset_event" => &["slot"],
        "downtime_event" => &["provider", "failure_kind", "status"],
        _ => &[
            "provider",
            "slot",
            "window",
            "previous_sampled_at_ms",
            "previous_reset_at_ms",
            "reset_at_ms",
            "previous_used_percent",
            "current_used_percent",
            "capacity_gain_percentage_points",
        ],
    };
    for field in fields {
        out[*field] = v[*field].clone()
    }
    out
}
impl<S: QueryStore, H: ObservabilityHost> ObservabilityRuntime<S, H> {
    pub async fn record_capacity_reset_event(&self, event: &Value) -> bool {
        if !valid_event("reset_event", event) {
            return false;
        }
        self.write_capacity_event("reset_event", event, false)
            .await
            .unwrap_or(false)
    }
    pub async fn record_capacity_downtime(
        &self,
        failure_kind: &str,
        status: Option<u16>,
        observed_at_ms: i64,
    ) -> bool {
        let event = json!({"v":1,"event_id":format!("openai-{}",observed_at_ms/BUCKET_MS*BUCKET_MS),"provider":"openai","failure_kind":failure_kind,"status":status,"observed_at_ms":observed_at_ms});
        if !valid_event("downtime_event", &event) {
            return false;
        }
        self.write_capacity_event("downtime_event", &event, true)
            .await
            .unwrap_or(false)
    }
    async fn write_capacity_event(
        &self,
        kind: &str,
        event: &Value,
        any_existing: bool,
    ) -> Result<bool, GatewayError> {
        let at = store::now(self.host.as_ref())?;
        let key = event_key(
            kind,
            event["event_id"].as_str().expect("validated event ID"),
        );
        for _ in 0..4 {
            let row = self.store.read(&key).await?;
            if let Some(v) = store::value(row.as_ref(), at)? {
                if any_existing {
                    return Ok(valid_event(kind, v));
                }
                if valid_event(kind, v) {
                    return Ok(v["slot"] == event["slot"]
                        && v["observed_at_ms"] == event["observed_at_ms"]);
                }
            }
            if store::commit(
                self.store.as_ref(),
                vec![store::check(&key, row.as_ref())],
                vec![store::put(
                    self.host.as_ref(),
                    key.clone(),
                    event.clone(),
                    Some(at + RETENTION_MS),
                )?],
            )
            .await?
            {
                return Ok(true);
            }
        }
        Ok(false)
    }
    pub async fn capacity_events(&self, kind: &str, at: i64) -> Result<Vec<Value>, GatewayError> {
        if !["reset_event", "rate_limit_reset_event", "downtime_event"].contains(&kind) {
            return Err(store::invalid("Invalid capacity event family"));
        }
        let cutoff = (at - RETENTION_MS).max(0);
        let mut events = BTreeMap::new();
        for entry in store::scan(
            self.store.as_ref(),
            store::key(json!(["uos_ai", "provider_capacity", "v1", kind])),
            false,
            at,
            usize::MAX,
        )
        .await?
        {
            let v = entry.record.json_value()?;
            if valid_event(kind, v)
                && safe_i64(&v["observed_at_ms"]).is_some_and(|t| t >= cutoff && t <= at)
            {
                events.insert(
                    v["event_id"].as_str().unwrap_or("").to_owned(),
                    public_event(kind, v),
                );
            }
        }
        if kind == "reset_event" {
            let derivation:Result<Vec<Value>,GatewayError>=async {
                let mut fences=Vec::new();
                for row in store::scan(self.store.as_ref(),store::key(json!(["uos_ai","codex_reset_shadow_decision","v1"])),false,at,usize::MAX).await? {
                    if let Some(value)=parse_shadow(row.record.json_value()?){fences.extend(value["fences"].as_array().expect("source fences").iter().cloned());}
                }
                let mut derived=Vec::new();
                for row in store::scan(self.store.as_ref(),store::key(json!(["uos_ai","codex_reset_redemption","v1"])),false,at,usize::MAX).await? {
                    let value=row.record.json_value()?;
                    if !valid_redemption(value)||value["state"]!="verified"||!safe_i64(&value["verified_at_ms"]).is_some_and(|t|t>=cutoff&&t<=at){continue;}
                    if let Some(fence)=fences.iter().find(|f|f["account_id_hash"]==value["account_id_hash"]&&f["quota_generation"]==value["quota_generation"])
                        && matches!(safe_i64(&fence["slot"]),Some(1|2)) {
                        derived.push(json!({"v":1,"event_id":value["idempotency_key_hash"],"slot":fence["slot"],"observed_at_ms":value["verified_at_ms"]}));
                    }
                }
                Ok(derived)
            }.await;
            for event in derivation.unwrap_or_else(|_: GatewayError| Vec::new()) {
                let id = event["event_id"].as_str().unwrap_or("").to_owned();
                if let std::collections::btree_map::Entry::Vacant(entry) = events.entry(id) {
                    let _ = self.record_capacity_reset_event(&event).await;
                    entry.insert(event);
                }
            }
        }
        let mut events: Vec<Value> = events.into_values().collect();
        let mut error = None;
        events.sort_by(|a, b| {
            safe_i64(&a["observed_at_ms"])
                .cmp(&safe_i64(&b["observed_at_ms"]))
                .then_with(|| {
                    match self.host.compare_labels(
                        a["event_id"].as_str().unwrap_or(""),
                        b["event_id"].as_str().unwrap_or(""),
                    ) {
                        Ok(order) => order,
                        Err(e) => {
                            error = Some(e);
                            std::cmp::Ordering::Equal
                        }
                    }
                })
        });
        if let Some(error) = error {
            return Err(error);
        }
        Ok(events)
    }
}
