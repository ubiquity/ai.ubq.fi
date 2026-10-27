//! Ordinary paid control rows and source codecs; these records mint no routing receipt.
mod model_map;
use gateway_core::{
    model::ModelId, provider::PaidProvider, source_json::SourceJson,
    source_serialization as source, state::RevisionCheck,
};
use gateway_providers::types::ClientSurface;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct PaidRoutingPolicy {
    pub v: u32,
    pub codex_enabled: bool,
    pub codex_models: Vec<ModelId>,
    #[serde(with = "model_map")]
    pub models: BTreeMap<ModelId, Vec<PaidProvider>>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct PaidSourceCheck {
    pub key: String,
    pub expected_revision: Option<String>,
}

impl From<&RevisionCheck> for PaidSourceCheck {
    fn from(value: &RevisionCheck) -> Self {
        Self {
            key: value.key.0.clone(),
            expected_revision: value.expected_revision.clone(),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct PaidControlOwner {
    pub v: u32,
    pub owner_id: String,
    pub owner_epoch: String,
    pub key_fingerprint: String,
}

pub const PAID_CONTROL_CONFIG_VERSION: u32 = 2;

fn required_nullable_binding<'de, D: serde::Deserializer<'de>>(
    deserializer: D,
) -> Result<Option<String>, D::Error> {
    Option::<String>::deserialize(deserializer)
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PaidControlConfig {
    pub v: u32,
    pub owner_id: String,
    pub owner_epoch: String,
    pub generation: String,
    pub configuration_fingerprint: String,
    #[serde(deserialize_with = "required_nullable_binding")]
    pub surplus_binding: Option<String>,
    #[serde(deserialize_with = "required_nullable_binding")]
    pub openlux_discovery_binding: Option<String>,
    #[serde(deserialize_with = "required_nullable_binding")]
    pub openlux_binding: Option<String>,
    pub source_checks: Vec<PaidSourceCheck>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct PaidProviderObservation {
    pub v: u32,
    pub provider: PaidProvider,
    pub owner_id: String,
    pub owner_epoch: String,
    pub configuration_generation: String,
    pub configuration_fingerprint: String,
    pub credential_binding: String,
    pub observed_at_ms: i64,
    pub source_valid_until_ms: Option<i64>,
    pub stale: bool,
    pub models: Vec<SourceJson>,
    pub source_checks: Vec<PaidSourceCheck>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct PaidProjectionProvenance {
    pub v: u32,
    pub owner_revision: String,
    pub config_revision: String,
    pub surplus_observation_revision: Option<String>,
    pub openlux_observation_revision: Option<String>,
    pub observed_at_ms: i64,
    pub source_valid_until_ms: Option<i64>,
    pub source_checks: Vec<PaidSourceCheck>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct PaidRoutingProjection {
    pub policy: PaidRoutingPolicy,
    pub provenance: PaidProjectionProvenance,
}

// Serde flatten materializes intermediate content, which cannot retain RawValue
// model leaves. Decode the complete source tree and each exact finite DTO directly.
impl Serialize for PaidRoutingProjection {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let policy =
            SourceJson::from_serializable(&self.policy).map_err(serde::ser::Error::custom)?;
        let mut fields = source::entries(&policy)
            .ok_or_else(|| serde::ser::Error::custom("Invalid paid policy"))?;
        fields.push((
            "provenance".into(),
            SourceJson::from_serializable(&self.provenance).map_err(serde::ser::Error::custom)?,
        ));
        source::object_keys(fields)
            .map_err(serde::ser::Error::custom)?
            .serialize(serializer)
    }
}
impl<'de> Deserialize<'de> for PaidRoutingProjection {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let raw = <SourceJson as serde::Deserialize>::deserialize(deserializer)?;
        let policy = raw.deserialize().map_err(serde::de::Error::custom)?;
        let provenance = source::member(&raw, "provenance")
            .ok_or_else(|| serde::de::Error::custom("Missing paid provenance"))?
            .deserialize()
            .map_err(serde::de::Error::custom)?;
        Ok(Self { policy, provenance })
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PaidSyntheticHop {
    Surplus,
    Openlux,
}

/// Construct only from the validated server request and its server-owned pinned hop.
/// The normalized body carries original_model, never a client synthetic-hop flag.
#[derive(Clone)]
pub struct PaidRequestContext {
    pub original_model: ModelId,
    pub body: SourceJson,
    pub surface: ClientSurface,
    pub synthetic_hop: Option<PaidSyntheticHop>,
}
