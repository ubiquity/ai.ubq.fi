//! Raw adapter for the fixed private route; all publication policy is shared.
use crate::transport::{HeaderLimits, body::request_bytes, bounded_headers};
use gateway_auth::native::NATIVE_MAX_BODY_BYTES;
use gateway_core::error::GatewayError;
use gateway_publication_policy::protocol::{OwnedNativeAuthHeaders, PublicationRequest};
use gateway_publication_transport::transport::PublicationReceiver;
use gateway_worker_authority::client::RemoteAuthorityStore;
use worker::{Env, Request, Response};

pub async fn handle(
    mut request: Request,
    env: &Env,
    receiver: Option<&PublicationReceiver>,
    native: &crate::native_host::NativeBindings,
    store: &RemoteAuthorityStore,
) -> Result<Response, GatewayError> {
    bounded_headers(
        &request,
        HeaderLimits {
            total_bytes: 16_384,
            value_bytes: 16_384,
            fields: 2000,
        },
    )?;
    let raw_body = request_bytes(&mut request, NATIVE_MAX_BODY_BYTES).await?;
    let operation: serde_json::Value =
        serde_json::from_slice(&raw_body).map_err(|_| unavailable())?;
    let callback = matches!(
        operation
            .get("operation")
            .and_then(serde_json::Value::as_str),
        Some("managed_access_publish" | "codex_auth_import_publish")
    );
    let scoped = if callback {
        crate::native_authority::callback::join(store, native, &raw_body).await?
    } else if matches!(
        store.access(),
        gateway_authority_lifecycle::model::Access::Legacy
    ) {
        store.clone()
    } else {
        let capability =
            gateway_auth::crypto::sha256_hex(crate::transport::random::opaque_token()?.as_bytes());
        let id =
            gateway_auth::crypto::sha256_hex(crate::transport::random::opaque_token()?.as_bytes());
        store
            .acquire(
                gateway_authority_lifecycle::model::Acquire {
                    id,
                    capability_sha256: gateway_auth::crypto::sha256_hex(capability.as_bytes()),
                    kind: gateway_authority_lifecycle::model::LeaseKind::Root {
                        request_id: "native-configuration-publication".into(),
                    },
                    parent: None,
                },
                capability,
            )
            .await?
    };
    let result = dispatch(&request, env, receiver, native, &scoped, &raw_body).await;
    gateway_worker_authority::callbacks::finish(&scoped, result).await
}
async fn dispatch(
    request: &Request,
    env: &Env,
    receiver: Option<&PublicationReceiver>,
    native: &crate::native_host::NativeBindings,
    store: &RemoteAuthorityStore,
    raw_body: &[u8],
) -> Result<Response, GatewayError> {
    let headers = OwnedNativeAuthHeaders::from_pairs(request.headers().entries())?;
    let method = request.method().to_string();
    let path = request.path();
    #[derive(serde::Deserialize)]
    struct Operation {
        operation: String,
    }
    if serde_json::from_slice::<Operation>(&raw_body)
        .ok()
        .is_some_and(|value| value.operation == "managed_access_publish")
    {
        return crate::native_authority::managed_publication(
            &request, env, native, store, &raw_body,
        )
        .await;
    }
    if serde_json::from_slice::<Operation>(&raw_body)
        .ok()
        .is_some_and(|value| value.operation == "codex_auth_import_publish")
    {
        return crate::native_authority::import_dispatch::publication(
            &request, env, native, store, receiver, &raw_body,
        )
        .await;
    }
    let receiver = receiver.ok_or_else(|| {
        GatewayError::unavailable(
            "The native publication receiver is not configured.",
            "native_publication_unavailable",
        )
    })?;
    let response = receiver
        .handle(
            store,
            PublicationRequest {
                method,
                path,
                raw_body: raw_body.to_vec(),
                headers,
            },
        )
        .await;
    let mut wire = Response::from_bytes(response.raw_body)
        .map_err(|_| unavailable())?
        .with_status(response.status);
    for (name, value) in response.headers {
        wire.headers_mut()
            .set(&name, &value)
            .map_err(|_| unavailable())?;
    }
    Ok(wire)
}

fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native publication transport failed.",
        "native_publication_transport_invalid",
    )
}
