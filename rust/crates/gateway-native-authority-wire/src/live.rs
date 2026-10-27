//! Legacy finite live reply DATA; actual access issuer stays with Authority.
use serde::{Deserialize, Serialize};
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NativeLiveFence {
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
    pub principal: String,
    pub principal_policy_digest: String,
    pub access_expires_at_ms: Option<i64>,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum NativeLiveRetirement {
    Retired,
    Pending,
    Unknown,
}

#[derive(Deserialize, Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum NativeLiveAuthorityResponse<A> {
    Authorize {
        v: u8,
        authorization_ref: String,
        fence: NativeLiveFence,
        access: A,
    },
    Recheck {
        v: u8,
        fence: NativeLiveFence,
        access: A,
    },
    Retire {
        v: u8,
        outcome: NativeLiveRetirement,
    },
}
