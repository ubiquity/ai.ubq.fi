//! Authenticated operator state transfer for the deployed Worker authority.
//!
//! Import wraps the existing private staged begin/append/seal/bind protocol in
//! `gateway_migration::staged` with a one-time operator intent, a resumable
//! status and exact-replay idempotency. It never writes outside that protocol,
//! so the empty-target, sequence, digest-chain, absent-key and one-commit-per-
//! chunk fences stay authoritative. Export pages the authority's raw canonical
//! records; `gateway-state-reverse` rebuilds a stopped Bun source from them.
//! Neither direction activates a service, grants ownership or refreshes a
//! credential.
pub mod export;
pub mod http;
pub mod import;
pub mod intent;

use gateway_core::error::GatewayError;
use gateway_core::state::RecordKey;

pub(crate) fn refused(message: impl Into<String>) -> GatewayError {
    GatewayError::new(409, message, Some("state_transfer_refused"))
}

/// The private staged progress row owned by `gateway_migration::staged`.
pub(crate) fn progress_key() -> RecordKey {
    gateway_auth::store::key(serde_json::json!(["uos_ai", "migration", "v3", "staged"]))
}

/// The sealed adoption manifest row owned by `gateway_migration::engine`.
pub(crate) fn manifest_key() -> RecordKey {
    gateway_auth::store::key(serde_json::json!(["uos_ai", "migration", "v3", "manifest"]))
}

/// The empty tuple prefix: every authority row is its strict descendant.
pub(crate) fn root() -> Result<RecordKey, GatewayError> {
    Ok(gateway_core::tuple_key::TupleKey::from_parts(&[])?.to_record_key())
}

pub(crate) fn decode<T: serde::de::DeserializeOwned>(
    value: &serde_json::Value,
) -> Result<T, GatewayError> {
    T::deserialize(value).map_err(|_| refused("Stored migration control row has an unknown shape"))
}

/// Every record crossing either direction keeps its typed value, a non-empty
/// opaque revision and an absolute (non-negative) expiry.
pub fn validate_record(record: &gateway_core::state::StoredRecord) -> Result<(), GatewayError> {
    record.validate_version()?;
    if record.revision.is_empty()
        || record.revision.len() > 4096
        || record.expires_at_ms.is_some_and(|expiry| expiry < 0)
    {
        return Err(refused(
            "Transferred record revision or absolute expiry is invalid",
        ));
    }
    Ok(())
}
