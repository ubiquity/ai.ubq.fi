//! Ordinary source keys, source JSON projection and serialized lease records; none grants a campaign permit.
use crate::model::{State, TargetBinding, failed};
use gateway_core::source_serialization as source;
use gateway_core::tuple_key::{ScalarKeyPart::String as Text, TupleKey};
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson, state::RecordKey,
};
use serde::{Deserialize, Serialize};
use serde_json::Value;
pub fn key(parts: &[&str]) -> RecordKey {
    TupleKey::from_scalar_parts(&parts.iter().map(|s| Text(s)).collect::<Vec<_>>()).to_record_key()
}
pub fn catalog_key() -> RecordKey {
    key(&["ubq_ai", "codex_models"])
}
pub fn runtime_key() -> RecordKey {
    key(&["uos_ai", "runtime_config", "v2"])
}
pub fn json<T: serde::Serialize + ?Sized>(value: &T) -> SourceJson {
    SourceJson::from_serializable(value).expect("typed cache control is valid source JSON")
}
pub fn set_member(value: &mut SourceJson, key: &str, next: SourceJson) -> Result<(), GatewayError> {
    let key = JsString16::from_utf8(key);
    let mut fields = source::entries(value)
        .ok_or_else(|| failed("Prompt-cache scope experiment catalog is malformed."))?;
    if let Some((_, old)) = fields.iter_mut().find(|(name, _)| name == &key) {
        *old = next;
    } else {
        fields.push((key, next));
    }
    *value = source::object_keys(fields)?;
    Ok(())
}
pub fn record_key(kind: &str, target: &TargetBinding) -> RecordKey {
    let mut parts = vec![
        "uos_ai",
        "prompt_cache_scope_experiment",
        "v5",
        kind,
        target.provider.as_str(),
        target.telemetry_provider.as_str(),
    ];
    if kind != "campaign" {
        parts.push(&target.topology_kind);
    }
    parts.push(&target.probe_profile);
    if kind != "campaign" {
        parts.push(&target.model);
    }
    key(&parts)
}
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct CycleLease {
    pub owner: String,
    pub lease_until_ms: i64,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct CampaignLease {
    pub owner: String,
    pub target_id: String,
    pub inventory_fingerprint: String,
    pub lease_until_ms: i64,
}
pub fn active(value: &Option<Value>, now: i64) -> bool {
    value
        .as_ref()
        .and_then(|v| v["lease_until_ms"].as_f64())
        .is_some_and(|v| v.fract() == 0.0 && v > now as f64 && v <= 9_007_199_254_740_991.0)
}
pub fn owns(value: &Option<Value>, owner: &str, now: i64) -> bool {
    active(value, now) && value.as_ref().is_some_and(|v| v["owner"] == owner)
}
pub fn campaign_owned(value: &Option<Value>, state: &State, now: i64) -> bool {
    owns(value, &state.campaign_owner, now)
        && value.as_ref().is_some_and(|v| {
            v["target_id"] == state.target.id
                && v["inventory_fingerprint"] == state.target.inventory_fingerprint
        })
}
pub fn release_valid(value: &str) -> bool {
    value.len() == 40
        && value
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}
