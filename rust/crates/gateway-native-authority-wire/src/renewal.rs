//! Legacy fence2 and mode DATA; never a renewal grant.
use crate::context::NativeRenewalContext;
use serde::{Deserialize, Serialize};
pub const NATIVE_RENEWAL_FENCE_VERSION: u8 = 2;
fn fence_v2<'de, D: serde::Deserializer<'de>>(d: D) -> Result<u8, D::Error> {
    let v = u8::deserialize(d)?;
    if v != NATIVE_RENEWAL_FENCE_VERSION {
        return Err(serde::de::Error::custom(
            "unsupported native renewal fence version",
        ));
    }
    Ok(v)
}
fn required_nullable_time<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<i64>, D::Error> {
    Option::<i64>::deserialize(d)
}
fn required_nullable_text<'de, D: serde::Deserializer<'de>>(
    d: D,
) -> Result<Option<String>, D::Error> {
    Option::<String>::deserialize(d)
}

/// The source-native generation and the cloud access credential version are distinct.
/// Publication must independently prove both; a managed credential is never relabelled external.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NativeRenewalFence {
    #[serde(deserialize_with = "fence_v2")]
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
    pub principal: String,
    pub principal_policy_digest: String,
    pub original_request_id: String,
    pub context: NativeRenewalContext,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum NativeRenewalMode {
    /// One newly committed claim may attempt its atomic start transition once.
    RenewOnce,
    /// A repeated/started/uncertain operation can only read the registered source and reconcile.
    ReconcileOnly,
}
