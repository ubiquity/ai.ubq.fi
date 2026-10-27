//! Complete source historical outage/reset event derivation and fixed event writes.
use crate::{CapacityEvents, capacity_events::event_key, store};
use gateway_capacity_projection::{
    events::valid_event, math as capacity_math, scalars::safe_i64, shapes as shape,
};
use gateway_core::query::QueryStore;
use gateway_observability_contracts::types::ObservabilityHost;
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
impl<S: QueryStore, H: ObservabilityHost> CapacityEvents<'_, S, H> {
    pub async fn historical_rate_limit_events(
        &self,
        history: &[Value],
        events: Vec<Value>,
        at: i64,
    ) -> Result<Vec<Value>, GatewayError> {
        let mut merged: std::collections::BTreeMap<String, Value> = events
            .into_iter()
            .filter_map(|v| Some((v["event_id"].as_str()?.to_owned(), v.clone())))
            .collect();
        let mut ordered = history.to_vec();
        ordered.sort_by_key(|p| safe_i64(&p["sampled_at_ms"]));
        for slot in [1, 2] {
            let mut previous: Option<Value> = None;
            let mut outage = false;
            for point in &ordered {
                let source = shape::source(point, slot);
                if source.is_none_or(|s| s["state"] == "unavailable") {
                    if previous
                        .as_ref()
                        .zip(source)
                        .is_some_and(|(p, s)| capacity_math::same_cohort(&p["source"], s))
                    {
                        outage = true
                    } else {
                        previous = None;
                        outage = false
                    }
                    continue;
                }
                let source = source.expect("present source");
                if let Some(ref p) = previous
                    && outage
                    && capacity_math::same_cohort(&p["source"], source)
                {
                    let current =
                        json!({"snapshot_at_ms":point["sampled_at_ms"],"sources":[source]});
                    for event in
                        capacity_math::rate_limit_events(None, &current, std::slice::from_ref(p))
                    {
                        let id = event["event_id"].as_str().unwrap_or("").to_owned();
                        if let std::collections::btree_map::Entry::Vacant(entry) =
                            merged.entry(id.clone())
                        {
                            let _ = store::commit(
                                self.store,
                                Vec::new(),
                                vec![store::put(
                                    self.host,
                                    event_key("rate_limit_reset_event", &id),
                                    event.clone(),
                                    Some(at + shape::RETENTION_MS),
                                )?],
                            )
                            .await;
                            entry.insert(event);
                        }
                    }
                }
                if source["state"] == "available" {
                    previous =
                        Some(json!({"sampled_at_ms":point["sampled_at_ms"],"source":source}));
                }
                outage = false;
            }
        }
        let mut out: Vec<Value> = merged
            .into_values()
            .filter(|v| valid_event("rate_limit_reset_event", v))
            .collect();
        let mut error = None;
        out.sort_by(|a, b| {
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
        Ok(out)
    }
}
