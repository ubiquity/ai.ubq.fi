//! Explicit source response2 DATA with streamed source-string decoding.
use crate::{
    live::NativeLiveRetirement,
    renewal::NativeRenewalMode,
    source_fence::{SourceNativeLiveFenceV2, SourceNativeRenewalFenceV3},
};
use serde::Serialize;
mod live_codec;
mod renewal_codec;
#[derive(Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum SourceNativeRenewalAuthorityResponseV2 {
    Claim {
        v: u8,
        action_ref: String,
        operation_id: String,
        claim_ref: String,
        start_permit: Option<String>,
        publication_revision: Option<String>,
        mode: NativeRenewalMode,
        deadline_ms: i64,
        fence: SourceNativeRenewalFenceV3,
    },
    Start {
        v: u8,
        mode: NativeRenewalMode,
    },
    Recheck {
        v: u8,
        fence: SourceNativeRenewalFenceV3,
    },
    Published {
        v: u8,
        publication_revision: String,
    },
    Unknown {
        v: u8,
    },
}

#[derive(Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum SourceNativeLiveAuthorityResponseV2<A> {
    Authorize {
        v: u8,
        authorization_ref: String,
        fence: SourceNativeLiveFenceV2,
        access: A,
    },
    Recheck {
        v: u8,
        fence: SourceNativeLiveFenceV2,
        access: A,
    },
    Retire {
        v: u8,
        outcome: NativeLiveRetirement,
    },
}
