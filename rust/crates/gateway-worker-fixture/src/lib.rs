//! Test-only constructor composition. The actual application and coordination
//! authority are gateway-worker; no request handler, ledger or upstream is copied.
mod native_carrier;
mod native_import;
use gateway_auth::native_auth::{NativeCapability, NativeTransportIdentity};
use gateway_core::error::GatewayError;
use gateway_publication_policy::control_publication::ConfiguredPaidInputs;
use gateway_publication_transport::transport::PublicationReceiver;
use gateway_worker_fixture_setup::{initialize, invalid};
use std::collections::BTreeSet;
use worker::{Context, Env, Request};

pub const SURPLUS_SECRET: &str = "fixture-matrix-surplus";
pub const OPENLUX_DISCOVERY_SECRET: &str = "fixture-matrix-openlux-discovery";
pub const OPENLUX_SPEND_SECRET: &str = "fixture-matrix-openlux-spend";

fn now() -> i64 {
    gateway_worker::storage::now_ms().unwrap_or(i64::MAX)
}
fn identity() -> Result<NativeTransportIdentity, GatewayError> {
    NativeTransportIdentity::new(
        "fixture-matrix-key".into(),
        "fixture-matrix-owner".into(),
        "fixture-matrix-epoch".into(),
        [31; 32],
        BTreeSet::from([NativeCapability::ConfigurationPublish]),
    )
}
fn inputs() -> Result<ConfiguredPaidInputs, GatewayError> {
    use gateway_auth::crypto::Secret;
    ConfiguredPaidInputs::new(
        Some(Secret::new(SURPLUS_SECRET.into())),
        Some(Secret::new(OPENLUX_DISCOVERY_SECRET.into())),
        Some(Secret::new(OPENLUX_SPEND_SECRET.into())),
        serde_json::json!({"fixture":"runtime-matrix-v1"}),
    )
}

/// Explicit test-only feature: build this crate alone, never with the production
/// entrypoint feature. Missing fixture binding fails closed before application use.
#[cfg_attr(feature = "entrypoint", worker::event(fetch))]
pub async fn fetch(
    request: Request,
    env: Env,
    context: Context,
) -> worker::Result<gateway_worker::response::GatewayResponse> {
    if env
        .var("UOS_MATRIX_SCENARIO")
        .ok()
        .is_some_and(|v| v.to_string() == "native-carrier")
    {
        return native_carrier::fetch(request, env, context).await;
    }
    if env
        .var("UOS_MATRIX_SCENARIO")
        .ok()
        .is_some_and(|v| v.to_string() == "native-import")
    {
        return native_import::fetch(request, env, context).await;
    }
    let result = async {
        let scenario = env
            .var("UOS_MATRIX_SCENARIO")
            .map_err(|_| invalid())?
            .to_string();
        if !["direct", "codex", "claude", "paid"].contains(&scenario.as_str()) {
            return Err(invalid());
        }
        let identity = identity()?;
        let inputs = inputs()?;
        let receiver =
            PublicationReceiver::configured(identity.clone(), inputs.clone(), Vec::new(), now)?;
        let store = gateway_worker::authority::RemoteAuthorityStore::from_env(&env)?;
        initialize(&store, &identity, &inputs, &receiver, &scenario).await?;
        Ok(receiver)
    }
    .await;
    let receiver = result.map_err(|error: GatewayError| {
        worker::Error::RustError(
            error
                .envelope
                .error
                .code
                .unwrap_or_else(|| "matrix_fixture_failed".into()),
        )
    })?;
    #[cfg(feature = "stream-drop-fixture")]
    let request = {
        let mut request = request;
        if request.method() == worker::Method::Post && request.path() == "/v1/responses" {
            let marker = match request.cloned() {
                Ok(mut cloned) => cloned.json::<serde_json::Value>().await.ok(),
                Err(_) => None,
            };
            if marker
                .as_ref()
                .and_then(|body| body.get("input"))
                .and_then(serde_json::Value::as_str)
                == Some("fixture:matrix:precommit:rust-drop")
            {
                return drop_preparing(request, env, context, &receiver).await;
            }
        }
        request
    };
    gateway_worker::fetch_with_bindings(
        request,
        env,
        context,
        Some(&receiver),
        gateway_worker::native_host::NativeBindings::default(),
    )
    .await
}

#[cfg(test)]
mod tests {
    #[test]
    fn configuration_fixture_does_not_require_an_access_owner() {
        assert!(
            super::PublicationReceiver::configured(
                super::identity().unwrap(),
                super::inputs().unwrap(),
                Vec::new(),
                || 0,
            )
            .is_ok()
        );
    }
}

#[cfg(feature = "stream-drop-fixture")]
async fn drop_preparing(
    request: Request,
    env: Env,
    context: Context,
    receiver: &PublicationReceiver,
) -> worker::Result<gateway_worker::response::GatewayResponse> {
    let signal = request.inner().signal();
    let phase = gateway_worker::stream_fixture::observe_preparation();
    let mut actual = Box::pin(gateway_worker::fetch_with_bindings(
        request,
        env,
        context,
        Some(receiver),
        gateway_worker::native_host::NativeBindings::default(),
    ));
    let request_id =
        std::future::poll_fn(
            |context| match std::future::Future::poll(actual.as_mut(), context) {
                std::task::Poll::Ready(_) => std::task::Poll::Ready(Err(worker::Error::RustError(
                    "The actual handler completed before the preparation barrier".into(),
                ))),
                std::task::Poll::Pending => match phase.request_id() {
                    Some(id) => std::task::Poll::Ready(Ok(id)),
                    None => std::task::Poll::Pending,
                },
            },
        )
        .await?;
    let before = signal.aborted();
    drop(actual);
    worker::Response::from_json(&serde_json::json!({"request_id":request_id,"phase":"presemantic-prefix-pending","rust_future_dropped":true,"caller_aborted_before":before,"caller_aborted_after":signal.aborted()})).map(Into::into)
}
