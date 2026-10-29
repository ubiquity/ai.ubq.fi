//! Source lease-guarded event/admin sampler. No timer, alarm, or inference operation.
use crate::CapacitySampler;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_capacity_projection::{scalars::safe_i64, scalars::trim, shapes as shape};
use gateway_core::query::QueryStore;
use gateway_observability_contracts::{
    data, host_types::CapacityAccountRef, host_types::CapacityProbe,
    host_types::ClaudeAccountReadback, source_object::SourceObject, types::ObservabilityHost,
};
use gateway_source::{
    error::GatewayError, source_json::SourceJson, source_serialization as source,
};
use serde_json::{Value, json};

fn view(
    snapshot: Option<Value>,
    state: &str,
    history: Vec<Value>,
    reset: Vec<Value>,
    rate: Vec<Value>,
    downtime: Vec<Value>,
    at: i64,
) -> Value {
    let exists = snapshot.is_some();
    let mut out = shape::stale_snapshot(
        &snapshot.unwrap_or_else(|| shape::unavailable_snapshot(at)),
        at,
    );
    let stale = out["sources"]
        .as_array()
        .is_some_and(|a| a.iter().any(|s| s["state"] == "stale"));
    out["cache_state"] = json!(if !exists {
        "unavailable"
    } else if stale {
        "stale"
    } else {
        state
    });
    out["history"] = json!(history);
    out["reset_events"] = json!(reset);
    out["rate_limit_reset_events"] = json!(rate);
    out["downtime_events"] = json!(downtime);
    out
}
pub fn unavailable_claude(at: i64) -> Value {
    json!({"source":"claude_code","label":"Claude subscription","state":"unavailable","source_observed_at_ms":null,"snapshot_at_ms":at,"failure_kind":"not_configured","failure_status":null,"credential_kind":null,"workspace_id":null,"windows":{"five_hour":null,"seven_day":null},"standard":[],"usage":null,"blocked_until_ms":null,"cooldown_until_ms":null,"last_error":null})
}
pub fn claude_source(
    account: &ClaudeAccountReadback,
    health: &Value,
    at: i64,
    dates: &impl Fn(&gateway_source::js_string::JsString16) -> Option<i64>,
) -> Result<SourceJson, GatewayError> {
    let empty = SourceJson::parse("{}")?;
    let quota = SourceObject::parse(account.quota.as_ref().unwrap_or(&empty))?;
    let null = SourceJson::parse("null")?;
    let field = |name: &str| quota.get(name).cloned().unwrap_or_else(|| null.clone());
    let number = |value: &SourceJson| source::number(value).filter(|n| n.is_finite());
    let integer = |value: &SourceJson| {
        number(value).filter(|n| n.fract() == 0.0 && (0.0..=9_007_199_254_740_991.0).contains(n))
    };
    let blocked = integer(&field("blocked_until_ms")).is_some_and(|t| t > at as f64);
    let cooling = integer(&field("cooldown_until_ms")).is_some_and(|t| t > at as f64);
    let error = field("last_error");
    let error_fields = if error.as_str().trim_start().starts_with('{') {
        Some(SourceObject::parse(&error)?)
    } else {
        None
    };
    let failing = blocked || cooling || error_fields.is_some();
    let windows = field("windows");
    let standard = field("standard");
    let mut observed = Vec::new();
    for document in [&windows, &standard] {
        if document.as_str().trim_start().starts_with('{') {
            for (_, value) in source::entries(document)
                .ok_or_else(|| data::error("Invalid Claude numeric observation metadata"))?
            {
                if value.as_str().trim_start().starts_with('{')
                    && let Some(at) = SourceObject::parse(&value)?
                        .get("observed_at_ms")
                        .and_then(number)
                {
                    observed.push(at);
                }
            }
        }
    }
    let projected = quota
        .get("usage_snapshot")
        .and_then(|s| gateway_claude_usage_protocol::snapshot::read(s, dates))
        .map(|s| gateway_claude_usage_protocol::snapshot::serialize(&s))
        .transpose()?;
    let snapshot = projected.as_ref();
    let usage = quota
        .get("usage")
        .and_then(gateway_claude_usage_protocol::state::read);
    let usage_stale = snapshot.is_some_and(|s| {
        usage.as_ref().is_none_or(|u| u.state != "ok")
            || gateway_claude_usage_protocol::snapshot::provenance(s)
                .is_some_and(|(_, t)| at - t >= 30_000)
    });
    let observed = snapshot
        .and_then(gateway_claude_usage_protocol::snapshot::provenance)
        .map(|(_, t)| t as f64)
        .or_else(|| observed.into_iter().reduce(f64::max));
    let state = if failing {
        "unavailable"
    } else if usage_stale || observed.is_some_and(|t| at as f64 - t > shape::STALE_MS as f64) {
        "stale"
    } else {
        "available"
    };
    let failure_status = error_fields
        .as_ref()
        .and_then(|e| e.get("status"))
        .and_then(number)
        .filter(|n| *n >= 0.0 && n.fract() == 0.0)
        .or_else(|| {
            if blocked {
                Some(429.0)
            } else if cooling {
                health["last_status"].as_u64().map(|n| n as f64)
            } else {
                None
            }
        });
    let window = |name| gateway_claude_usage_history_records::window_source(&windows, name);
    let mut standard_rows = Vec::new();
    if standard.as_str().trim_start().starts_with('{') {
        for (family, value) in source::entries(&standard)
            .ok_or_else(|| data::error("Invalid Claude standard metadata"))?
        {
            let value = SourceObject::parse(&value)?;
            let leaf = |name| value.get(name).cloned().unwrap_or_else(|| null.clone());
            standard_rows.push(source::object(vec![
                ("family", family.to_source_json()?),
                ("limit", leaf("limit")),
                ("remaining", leaf("remaining")),
                ("reset_at_ms", leaf("reset_at_ms")),
            ])?);
        }
    }
    let mut document = SourceObject::parse(&SourceJson::from_serializable(
        &json!({"source":"claude_code","label":"Claude subscription","account_id":account.account_id,"account_cohort_id":account.account_cohort_id,"state":state,"source_observed_at_ms":observed,"snapshot_at_ms":at,"failure_kind":if failing{Some("upstream_error")}else{None},"failure_status":failure_status,"credential_kind":account.credential.kind}),
    )?)?;
    document.set("workspace_id", &account.workspace.as_ref().map(|w| &w.id))?;
    document.set(
        "windows",
        &source::object(vec![
            ("five_hour", SourceJson::from_serializable(&window("5h")?)?),
            ("seven_day", SourceJson::from_serializable(&window("7d")?)?),
        ])?,
    )?;
    document.set("standard", &standard_rows)?;
    document.set("usage", &usage)?;
    if let Some(snapshot) = snapshot {
        document.set("usage_snapshot", snapshot)?;
        document.set("usage_stale", &usage_stale)?;
    }
    document.set("blocked_until_ms", &field("blocked_until_ms"))?;
    document.set("cooldown_until_ms", &field("cooldown_until_ms"))?;
    document.set("last_error", &error)?;
    document.source_json()
}
impl<S: QueryStore, H: ObservabilityHost> CapacitySampler<'_, S, H> {
    async fn slot_capacity(
        &self,
        account: Option<&CapacityAccountRef>,
        slot: u8,
        fence: Option<&gateway_authority_capacity::CodexCapacityObservationFence>,
        at: i64,
    ) -> Result<Value, GatewayError> {
        let Some(account) = account else {
            return Ok(shape::unavailable_codex(
                slot,
                at,
                "not_configured",
                None,
                false,
                None,
                None,
            ));
        };
        let fence = fence.ok_or_else(|| data::missing("capacity_authority_source_fence"))?;
        if !fence.fetch_bindings().any(|binding| {
            binding.source_slot() == slot && binding.account_id() == account.account_id
        }) {
            return Err(data::error(
                "Capacity GET does not match the captured source",
            ));
        }
        let cohort = sha256_hex(
            format!("uos-prompt-cache-account-cohort-v1\0{}", account.account_id).as_bytes(),
        );
        let failed = |kind: &str, status: Option<u16>| {
            shape::unavailable_codex(
                slot,
                at,
                kind,
                status,
                true,
                account.label.as_deref(),
                Some(&cohort),
            )
        };
        let probe = self
            .host
            .probe_codex_capacity(account, fence, at + 8000)
            .await?;
        match probe {
            CapacityProbe::Unreachable => Ok(failed("unreachable", None)),
            CapacityProbe::Http { status, .. } if !(200..300).contains(&status) => Ok(failed(
                if status >= 500 {
                    "upstream_error"
                } else {
                    "http_error"
                },
                Some(status),
            )),
            CapacityProbe::Http { status, json } => {
                let Some(windows) = json.as_ref().and_then(shape::parse_codex_usage) else {
                    return Ok(failed("invalid_response", Some(status)));
                };
                Ok(
                    json!({"source":"codex","label":shape::codex_label(slot,account.label.as_deref()),"slot":slot,"account_cohort_id":cohort,"state":"available","source_observed_at_ms":at,"snapshot_at_ms":at,"failure_kind":null,"failure_status":null,"windows":{"primary":windows["primary"],"secondary":windows["secondary"]},"additional_rate_limits":windows["additional_rate_limits"]}),
                )
            }
        }
    }
    async fn capture_capacity_snapshot(
        &self,
        at: i64,
        force_metered: bool,
    ) -> Result<Value, GatewayError> {
        let accounts = self.host.codex_capacity_accounts().await?;
        let mut slots = std::collections::BTreeSet::new();
        if accounts.len() > 2
            || accounts.iter().any(|a| {
                !(1..=2).contains(&a.slot)
                    || trim(&a.account_id).is_empty()
                    || !slots.insert(a.slot)
            })
        {
            return Err(data::error(
                "Capacity owner published an invalid account roster",
            ));
        }
        let fence = if accounts.is_empty() {
            None
        } else {
            Some(
                self.host
                    .begin_codex_capacity_observation(&accounts)
                    .await?,
            )
        };
        let at = fence.as_ref().map_or(at, |f| f.observed_at_ms());
        let codex = futures_util::future::join(
            self.slot_capacity(accounts.iter().find(|a| a.slot == 1), 1, fence.as_ref(), at),
            self.slot_capacity(accounts.iter().find(|a| a.slot == 2), 2, fence.as_ref(), at),
        );
        let (((one, two), metered), _usage) = futures_util::future::join(
            futures_util::future::join(
                codex,
                self.host.metered_quota_snapshot(force_metered, at + 8000),
            ),
            self.host.collect_claude_usage(),
        )
        .await;
        let one = one?;
        let two = two?;
        let metered = metered?;
        let mut observations = Vec::new();
        for account in accounts {
            let source = if account.slot == 1 { &one } else { &two };
            let invalid =
                || data::error("Capacity source observation does not match authority DTO");
            observations.push(gateway_authority_capacity::CodexCapacitySample {
                account: gateway_authority_capacity::CodexCapacityAccountRef {
                    source_slot: account.slot,
                    account_id: account.account_id,
                },
                state: serde_json::from_value(source["state"].clone()).map_err(|_| invalid())?,
                source_observed_at_ms: safe_i64(&source["source_observed_at_ms"]),
                snapshot_at_ms: at,
                windows: serde_json::from_value(source["windows"].clone())
                    .map_err(|_| invalid())?,
                additional_rate_limits: serde_json::from_value(json!(
                    shape::additional_for_routing(&source["additional_rate_limits"], at)
                ))
                .map_err(|_| invalid())?,
            });
        }
        if let Some(fence) = fence.as_ref() {
            self.host
                .accept_capacity_observations(fence, &observations)
                .await?;
        }
        Ok(
            json!({"snapshot_at_ms":at,"stale_after_ms":shape::STALE_MS,"sources":[one,two,shape::metered_source(metered.as_ref(),at)]}),
        )
    }
    pub async fn persisted_capacity_view(&self) -> Result<Value, GatewayError> {
        let at = data::now(self.host)?;
        let snapshot = self.read_capacity_snapshot(at).await.unwrap_or(None);
        let history = self.capacity_history(at).await.unwrap_or_default();
        let reset = self
            .events()
            .capacity_events("reset_event", at)
            .await
            .unwrap_or_default();
        let rate = self
            .events()
            .capacity_events("rate_limit_reset_event", at)
            .await
            .unwrap_or_default();
        let downtime = self
            .events()
            .capacity_events("downtime_event", at)
            .await
            .unwrap_or_default();
        let rate = self
            .events()
            .historical_rate_limit_events(&history, rate, at)
            .await
            .unwrap_or_default();
        Ok(view(
            snapshot,
            "persisted",
            history,
            reset,
            rate,
            downtime,
            at,
        ))
    }
    pub async fn refresh_capacity_view(&self, force_metered: bool) -> Result<Value, GatewayError> {
        let at = data::now(self.host)?;
        let before = self.persisted_capacity_view().await?;
        let cached = (before["cache_state"] != "unavailable").then(|| {
            let mut v = before.clone();
            if let Some(o) = v.as_object_mut() {
                for f in [
                    "cache_state",
                    "history",
                    "reset_events",
                    "rate_limit_reset_events",
                    "downtime_events",
                ] {
                    o.remove(f);
                }
            }
            v
        });
        let owner = data::uuid(self.host)?;
        let lease = self
            .acquire_capacity_lease(&owner, at)
            .await
            .unwrap_or(None);
        let Some(lease) = lease else {
            let snapshot = self
                .wait_capacity_snapshot(
                    cached.as_ref().and_then(|s| safe_i64(&s["snapshot_at_ms"])),
                )
                .await
                .unwrap_or(None)
                .or(cached);
            let refreshed = self
                .persisted_capacity_view()
                .await
                .unwrap_or_else(|_| before.clone());
            return Ok(view(
                snapshot,
                "persisted",
                refreshed["history"].as_array().cloned().unwrap_or_default(),
                refreshed["reset_events"]
                    .as_array()
                    .cloned()
                    .unwrap_or_default(),
                refreshed["rate_limit_reset_events"]
                    .as_array()
                    .cloned()
                    .unwrap_or_default(),
                refreshed["downtime_events"]
                    .as_array()
                    .cloned()
                    .unwrap_or_default(),
                at,
            ));
        };
        let captured = async {
            let snapshot = self.capture_capacity_snapshot(at, force_metered).await?;
            let persisted = self
                .persist_capacity_snapshot(&lease, &snapshot)
                .await
                .unwrap_or(false);
            let refreshed = self
                .persisted_capacity_view()
                .await
                .unwrap_or_else(|_| before.clone());
            let mut history = refreshed["history"].as_array().cloned().unwrap_or_default();
            if !persisted {
                let point = shape::history_point(&snapshot);
                history.retain(|p| {
                    p["bucket_start_at_ms"] != point["bucket_start_at_ms"]
                        || p["sampled_at_ms"] != point["sampled_at_ms"]
                });
                history.push(point);
                history.sort_by_key(|p| {
                    (
                        safe_i64(&p["bucket_start_at_ms"]),
                        safe_i64(&p["sampled_at_ms"]),
                    )
                });
            }
            Ok(view(
                Some(snapshot),
                "live",
                history,
                refreshed["reset_events"]
                    .as_array()
                    .cloned()
                    .unwrap_or_default(),
                refreshed["rate_limit_reset_events"]
                    .as_array()
                    .cloned()
                    .unwrap_or_default(),
                refreshed["downtime_events"]
                    .as_array()
                    .cloned()
                    .unwrap_or_default(),
                at,
            ))
        }
        .await;
        self.release_capacity_lease(&owner).await;
        captured
    }
    pub async fn sample_capacity_on_event(&self) -> Result<bool, GatewayError> {
        let at = data::now(self.host)?;
        let owner = data::uuid(self.host)?;
        let Some(lease) = self.acquire_capacity_lease(&owner, at).await? else {
            return Ok(false);
        };
        let sample = async {
            let snapshot = self.capture_capacity_snapshot(at, true).await?;
            self.persist_capacity_snapshot(&lease, &snapshot).await
        }
        .await;
        if !matches!(sample, Ok(true)) {
            self.release_capacity_lease(&owner).await;
        }
        sample
    }
    pub async fn trigger_capacity_sample(&self) -> Result<bool, GatewayError> {
        let at = data::now(self.host)?;
        let bucket = at / shape::BUCKET_MS * shape::BUCKET_MS;
        if bucket <= self.sampled_bucket.get() {
            return Ok(false);
        }
        self.sampled_bucket.set(bucket);
        self.sample_capacity_on_event().await
    }
}
