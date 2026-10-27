//! Ordinary source-shaped capacity data and its existing finite decoder.
use gateway_authority_contracts::invalid;
use gateway_authority_routing_records::codec::{safe_integer, safe_ms};
use gateway_core::identity::RoutingAccountHash;
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct CapacityStore {
    pub v: u32,
    pub updated_at_ms: i64,
    pub observations: Vec<CapacityObservation>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct CapacityWindow {
    pub limit_window_seconds: Option<i64>,
    pub used_percent: Option<f64>,
    pub reset_at_ms: Option<i64>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct CapacityAdditionalRateLimit {
    pub limit_name: String,
    pub metered_feature: Option<String>,
    pub windows: CapacityWindows,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq, Serialize)]
pub struct CapacityWindows {
    pub primary: Option<CapacityWindow>,
    pub secondary: Option<CapacityWindow>,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum CapacityState {
    Available,
    Stale,
    Unavailable,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct CapacityObservation {
    pub slot: u8,
    pub account_id_hash: RoutingAccountHash,
    pub state: CapacityState,
    pub source_observed_at_ms: Option<i64>,
    pub snapshot_at_ms: i64,
    pub windows: CapacityWindows,
    pub additional_rate_limits: Vec<CapacityAdditionalRateLimit>,
}

impl Default for CapacityStore {
    fn default() -> Self {
        Self {
            v: 1,
            updated_at_ms: 0,
            observations: Vec::new(),
        }
    }
}

fn optional_time(value: &Value, name: &str) -> Option<i64> {
    value.get(name).and_then(safe_ms)
}

pub fn normalize_capacity_window(raw: &Value) -> Option<CapacityWindow> {
    raw.as_object()?;
    Some(CapacityWindow {
        limit_window_seconds: optional_time(raw, "limit_window_seconds"),
        used_percent: raw
            .get("used_percent")
            .and_then(Value::as_f64)
            .filter(|n| n.is_finite() && (0.0..=100.0).contains(n)),
        reset_at_ms: optional_time(raw, "reset_at_ms"),
    })
}

fn windows(raw: &Value) -> CapacityWindows {
    CapacityWindows {
        primary: raw.get("primary").and_then(normalize_capacity_window),
        secondary: raw.get("secondary").and_then(normalize_capacity_window),
    }
}

fn additional(raw: &Value) -> Vec<CapacityAdditionalRateLimit> {
    raw.as_array()
        .into_iter()
        .flatten()
        .filter_map(|row| {
            let name = row.get("limit_name")?.as_str()?.trim();
            if name.is_empty() {
                return None;
            }
            let raw_windows = row.get("windows")?.as_object()?;
            let feature = match row.get("metered_feature") {
                None | Some(Value::Null) => None,
                Some(Value::String(s)) => {
                    Some(s.trim()).filter(|s| !s.is_empty()).map(str::to_owned)
                }
                _ => return None,
            };
            Some(CapacityAdditionalRateLimit {
                limit_name: name.into(),
                metered_feature: feature,
                windows: windows(&Value::Object(raw_windows.clone())),
            })
        })
        .collect()
}

pub fn parse_capacity_observation(raw: &Value) -> Result<CapacityObservation, GatewayError> {
    let slot = raw
        .get("slot")
        .and_then(safe_ms)
        .filter(|n| *n <= 1)
        .ok_or_else(|| invalid("Malformed capacity slot."))? as u8;
    let hash = raw
        .get("account_id_hash")
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())
        .ok_or_else(|| invalid("Missing capacity account hash."))?;
    let state = match raw.get("state").and_then(Value::as_str) {
        Some("available") => CapacityState::Available,
        Some("stale") => CapacityState::Stale,
        Some("unavailable") => CapacityState::Unavailable,
        _ => return Err(invalid("Malformed capacity observation state.")),
    };
    let snapshot_at_ms = raw
        .get("snapshot_at_ms")
        .and_then(safe_ms)
        .ok_or_else(|| invalid("Malformed capacity snapshot time."))?;
    let raw_windows = raw
        .get("windows")
        .filter(|v| v.is_object())
        .ok_or_else(|| invalid("Missing named capacity windows."))?;
    Ok(CapacityObservation {
        slot,
        account_id_hash: RoutingAccountHash(hash.into()),
        state,
        source_observed_at_ms: optional_time(raw, "source_observed_at_ms"),
        snapshot_at_ms,
        windows: windows(raw_windows),
        additional_rate_limits: additional(
            raw.get("additional_rate_limits").unwrap_or(&Value::Null),
        ),
    })
}

pub fn parse_capacity_store(raw: &Value) -> Result<CapacityStore, GatewayError> {
    if raw.get("v").and_then(safe_integer) != Some(1) {
        return Err(invalid("Unsupported capacity observation schema."));
    }
    let updated_at_ms = raw
        .get("updated_at_ms")
        .and_then(safe_ms)
        .ok_or_else(|| invalid("Malformed capacity store time."))?;
    let observations = raw
        .get("observations")
        .and_then(Value::as_array)
        .ok_or_else(|| invalid("Malformed capacity observation rows."))?
        .iter()
        .map(parse_capacity_observation)
        .collect::<Result<Vec<_>, _>>()?;
    Ok(CapacityStore {
        v: 1,
        updated_at_ms,
        observations,
    })
}
