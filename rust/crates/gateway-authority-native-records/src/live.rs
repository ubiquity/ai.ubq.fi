//! Existing live actor action/connection DATA. No deserialization grants a live operation.
use gateway_authority_actor_records::legacy::StoredNativeActor;
use serde::{Deserialize, Serialize};
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ActionRow {
    pub v: u8,
    pub action_ref: String,
    pub request_id: String,
    pub actor: StoredNativeActor,
    pub call_id: String,
    pub account_id: String,
    pub mapping_revision: String,
    pub created_at_ms: i64,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ConnectionRow {
    pub v: u8,
    pub action_ref: String,
    pub connection_id: String,
    pub authorization_ref: String,
    pub owner_id: String,
    pub owner_epoch: String,
    pub key_fingerprint: String,
    pub call_id: String,
    pub created_at_ms: i64,
    pub upstream_status: Option<u16>,
    pub retired: bool,
}

use gateway_kv_codec::{
    key::RecordKey, tuple_key::ScalarKeyPart::String as Text, tuple_key::TupleKey,
};
pub fn mapping_key(call: &str, pending: bool) -> RecordKey {
    let mut parts = vec![Text("uos_ai"), Text("codex_live_calls"), Text("v1")];
    if pending {
        parts.push(Text("pending_retirement"));
    }
    parts.push(Text(call));
    TupleKey::from_scalar_parts(&parts).to_record_key()
}
