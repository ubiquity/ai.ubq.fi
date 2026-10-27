//! Exact legacy finite response DATA.
use crate::renewal::{NativeRenewalFence, NativeRenewalMode};
use serde::Serialize;
mod codec;
#[derive(Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum NativeRenewalAuthorityResponse {
    Claim {
        v: u8,
        action_ref: String,
        operation_id: String,
        claim_ref: String,
        start_permit: Option<String>,
        publication_revision: Option<String>,
        mode: NativeRenewalMode,
        deadline_ms: i64,
        fence: NativeRenewalFence,
    },
    Start {
        v: u8,
        mode: NativeRenewalMode,
    },
    Recheck {
        v: u8,
        fence: NativeRenewalFence,
    },
    Published {
        v: u8,
        publication_revision: String,
    },
    Unknown {
        v: u8,
    },
}
