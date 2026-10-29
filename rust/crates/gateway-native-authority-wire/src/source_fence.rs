//! Explicit lossless fence DATA; deserialization never grants a live phase.
use crate::context::NativeRenewalContext;
use gateway_authority_actor_records::versions::{LIVE_FENCE_V2, RENEWAL_FENCE_V3};
use gateway_source::js_string::JsString16;
use serde::{Deserialize, Serialize};
fn required_nullable_time<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<i64>, D::Error> {
    Option::<i64>::deserialize(d)
}
fn required_nullable_text<'de, D: serde::Deserializer<'de>>(
    d: D,
) -> Result<Option<String>, D::Error> {
    Option::<String>::deserialize(d)
}
fn fence_v3<'de, D: serde::Deserializer<'de>>(d: D) -> Result<u8, D::Error> {
    let v = u8::deserialize(d)?;
    if v != RENEWAL_FENCE_V3 {
        return Err(serde::de::Error::custom(
            "unsupported source renewal fence version",
        ));
    }
    Ok(v)
}
fn live_v2<'de, D: serde::Deserializer<'de>>(d: D) -> Result<u8, D::Error> {
    let v = u8::deserialize(d)?;
    if v != LIVE_FENCE_V2 {
        return Err(serde::de::Error::custom(
            "unsupported source live fence version",
        ));
    }
    Ok(v)
}
/// The source-native generation and the cloud access credential version are distinct.
/// Publication must independently prove both; a managed credential is never relabelled external.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SourceNativeRenewalFenceV3 {
    #[serde(deserialize_with = "fence_v3")]
    pub v: u8,
    pub owner_id: String,
    pub owner_epoch: String,
    pub owner_revision: String,
    pub native_owner_id: String,
    pub source_revision: String,
    pub normalized_revision: String,
    pub account_id_hash: String,
    pub credential_version: String,
    pub source_native_generation: String,
    pub access_generation: String,
    #[serde(deserialize_with = "required_nullable_time")]
    pub access_expires_at_ms: Option<i64>,
    #[serde(deserialize_with = "required_nullable_text")]
    pub native_refreshed_at: Option<String>,
    pub principal: JsString16,
    pub principal_policy_digest: String,
    pub original_request_id: String,
    pub context: NativeRenewalContext,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SourceNativeLiveFenceV2 {
    #[serde(deserialize_with = "live_v2")]
    pub v: u8,
    pub call_id: String,
    pub account_id: String,
    pub account_id_hash: String,
    pub credential_version: String,
    pub source_revision: String,
    pub normalized_revision: String,
    pub mapping_revision: String,
    pub owner_id: String,
    pub owner_epoch: String,
    pub owner_revision: String,
    pub principal: JsString16,
    pub principal_policy_digest: String,
    pub access_expires_at_ms: Option<i64>,
}
