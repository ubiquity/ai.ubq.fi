//! Ordinary original import observations, not custody or a currentness proof.
use crate::required::required_nullable;
use serde::{Deserialize, Serialize};
#[derive(Clone, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ImportFacts {
    pub owner_id: String,
    pub owner_epoch: String,
    pub native_owner_id: String,
    pub slot: u64,
    pub account_id_hash: String,
    pub credential_version: String,
    pub access_generation: String,
    pub source_native_generation: String,
    pub owner_revision: String,
    pub source_revision: String,
    pub normalized_revision: String,
    #[serde(deserialize_with = "required_nullable")]
    pub runtime_revision: Option<String>,
    pub control_checks_digest: String,
    pub account_digests: Vec<String>,
    pub checks_digest: String,
}
