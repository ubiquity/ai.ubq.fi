//! Untrusted selectors join only an already registered operation; normal native proofs still run.
use crate::client::RemoteAuthorityStore;
use gateway_auth::native_auth::NativeTransportIdentity;
use gateway_authority_lifecycle::{
    model::{Access, Blocker, NativeSelector},
    policy,
};
use gateway_core::error::GatewayError;

fn selected(value: &serde_json::Value, fields: &[&str]) -> Option<String> {
    fields
        .iter()
        .find_map(|field| value.pointer(field).and_then(serde_json::Value::as_str))
        .filter(|s| !s.is_empty() && s.len() <= 4096)
        .map(str::to_owned)
}

pub async fn join(
    store: &RemoteAuthorityStore,
    identity: &NativeTransportIdentity,
    bytes: &[u8],
    id: String,
    capability: String,
) -> Result<RemoteAuthorityStore, GatewayError> {
    if matches!(store.access(), Access::Legacy) {
        return Ok(store.clone());
    }
    let value: serde_json::Value = serde_json::from_slice(bytes)
        .map_err(|_| policy::refused("Native selector is malformed"))?;
    let (operation_id, action_ref) = if value.get("operation").and_then(serde_json::Value::as_str)
        == Some("claim")
        && value.get("original_control_body_base64url").is_some()
    {
        let request =
            gateway_authority::native_control::wire::NativeRenewalAuthorityRequest::parse(bytes)?;
        let original = gateway_auth::native_control::NativeControlEnvelope::parse(
            &request.original_control_body()?,
        )?;
        let gateway_auth::native_control::NativeControlRequest::CodexRenewAccess { action_ref } =
            original.request;
        (Some(original.operation_id), Some(action_ref))
    } else {
        (
            selected(
                &value,
                &[
                    "/operation_id",
                    "/command/metadata/operation_id",
                    "/command/state/metadata/operation_id",
                    "/command/started/operation_id",
                    "/state/metadata/operation_id",
                ],
            ),
            selected(
                &value,
                &[
                    "/action_ref",
                    "/command/metadata/action_ref",
                    "/command/state/metadata/action_ref",
                    "/command/started/action_ref",
                    "/state/metadata/action_ref",
                ],
            ),
        )
    };
    let selector = NativeSelector {
        operation_id,
        action_ref,
        owner_id: identity.owner_id().into(),
        owner_epoch: identity.owner_epoch().into(),
        key_fingerprint: identity.key_fingerprint().into(),
    };
    store.join_native(selector, id, capability).await
}

pub async fn finish(
    store: &RemoteAuthorityStore,
    result: Result<worker::Response, GatewayError>,
) -> Result<worker::Response, GatewayError> {
    if matches!(store.access(), Access::Permit(_)) {
        let blocker = result
            .as_ref()
            .err()
            .and_then(|error| error.envelope.error.code.as_deref())
            .filter(|code| code.starts_with("authority_"))
            .map(|_| Blocker::UncertainCommit);
        store.finish_scope(blocker).await?;
    }
    result
}
