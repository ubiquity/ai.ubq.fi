use gateway_core::{
    error::GatewayError,
    health::{DEPLOYMENT_ID_HEADER, GIT_SHA_HEADER, ReleaseIdentity},
};
#[cfg(feature = "entrypoint")]
use worker::event;
use worker::{Context, Env, Method, Request, Response, Result};
use {native_host::NativeBindings, response::GatewayResponse, std::rc::Rc};

mod admin_host;
pub mod admin_support_host;
mod admission;
mod agent_messages;
mod assets;
pub mod authority;
mod cache_experiment_host;
mod candidate_bindings;
mod catalog;
mod catalog_host;
mod claude_host;
mod codex_admin_host;
pub mod compaction_host;
mod cors;
mod extras;
mod extras_host;
mod host;
mod image_host;
mod inference;
mod inference_capture;
mod inference_currentness;
pub mod inference_early;
mod inference_lifecycle;
mod inference_local;
mod inference_local_lifecycle;
pub mod inference_source;
pub mod inference_wire;
mod lifecycle;
mod live_host;
mod live_http;
/// Profile/native composition and the authenticated operator state transfer.
pub mod migration_host;
mod multipart;
mod native_authority;
pub mod native_host;
mod native_transport;
mod observability_host;
mod paid_lookup;
pub mod private_installer;
pub mod provider_error;
mod publication;
pub mod response;
mod routing_host;
mod runtime;
mod source_collator;
pub mod transport;

fn error_response(error: GatewayError) -> Result<Response> {
    let mut response = Response::from_json(&error.envelope)?.with_status(error.status);
    if error.envelope.error.code.as_deref()
        == Some(gateway_admission::LOCAL_INFERENCE_OVERLOAD_CODE)
    {
        response.headers_mut().set(
            "Retry-After",
            &gateway_admission::RETRY_AFTER_SECONDS.to_string(),
        )?;
    }
    Ok(response)
}

#[cfg_attr(feature = "entrypoint", event(fetch))]
pub async fn fetch(request: Request, env: Env, context: Context) -> Result<GatewayResponse> {
    let native = match candidate_bindings::native(&env)
        .and_then(|configured| configured.map(|value| value.bindings()).transpose())
    {
        Ok(Some(native)) => native,
        Ok(None) => match NativeBindings::from_worker_binding(&env) {
            Ok(Some(native)) => native,
            Ok(None) => NativeBindings::default(),
            Err(error) => return error_response(error).map(Into::into),
        },
        Err(error) => return error_response(error).map(Into::into),
    };
    fetch_with_bindings(request, env, context, None, native).await
}

/// Trusted publication injection. The exported entrypoint still has no
/// publication receiver until that separate authority is reviewed.
pub async fn fetch_with_publication(
    request: Request,
    env: Env,
    context: Context,
    receiver: Option<&gateway_publication_transport::transport::PublicationReceiver>,
) -> Result<GatewayResponse> {
    fetch_with_bindings(request, env, context, receiver, NativeBindings::default()).await
}

/// Capabilities are supplied by trusted native configuration or a private
/// runtime fixture. Neither a client field nor a provider token constructs them.
pub async fn fetch_with_bindings(
    request: Request,
    env: Env,
    context: Context,
    receiver: Option<&gateway_publication_transport::transport::PublicationReceiver>,
    native: NativeBindings,
) -> Result<GatewayResponse> {
    let identity = match ReleaseIdentity::cloudflare(
        env!("UOS_RUST_BUILD_GIT_SHA"),
        env!("UOS_RUST_BUILD_DEPLOYMENT_ID"),
    ) {
        Ok(identity) => identity,
        Err(error) => return error_response(error).map(Into::into),
    };
    let request_id = match transport::random::random_uuid() {
        Ok(id) => id,
        Err(error) => return error_response(error).map(Into::into),
    };
    let context = Rc::new(context);
    let raw = gateway_worker_authority::client::RemoteAuthorityStore::from_env(&env)
        .map_err(|error| worker::Error::RustError(error.envelope.error.message))?;
    let path = normalized_path(&request.path()).to_owned();
    let pure = request.method() == Method::Options
        || (path == "/health" && matches!(request.method(), Method::Get | Method::Head));
    let operator = gateway_state_transfer::http::operation(request.method().as_ref(), &path)
        .is_some()
        || path == "/admin/state-transfer/control";
    let callback = native_authority::is_path(&request.path())
        || request.path() == gateway_auth::native::NATIVE_PUBLICATION_PATH;
    let (store, writer) = if callback {
        let status = match raw
            .coordination(gateway_authority_lifecycle::model::Operation::Status)
            .await
        {
            Ok(gateway_authority_lifecycle::model::Reply::Status { status }) => status,
            Ok(_) => {
                return error_response(gateway_authority_lifecycle::policy::closed())
                    .map(Into::into);
            }
            Err(error) => return error_response(error).map(Into::into),
        };
        use gateway_authority_lifecycle::model::Phase;
        match status.gate.phase {
            Phase::LegacyRunning => (raw, None),
            Phase::Running | Phase::Draining => (raw.operator_read(), None),
            _ => {
                return error_response(gateway_authority_lifecycle::policy::closed())
                    .map(Into::into);
            }
        }
    } else if pure || operator {
        (raw.operator_read(), None)
    } else {
        let capability = gateway_auth::crypto::sha256_hex(
            transport::random::opaque_token()
                .map_err(|error| worker::Error::RustError(error.envelope.error.message))?
                .as_bytes(),
        );
        let acquire = gateway_authority_lifecycle::model::Acquire {
            id: gateway_auth::crypto::sha256_hex(request_id.as_bytes()),
            capability_sha256: gateway_auth::crypto::sha256_hex(capability.as_bytes()),
            kind: gateway_authority_lifecycle::model::LeaseKind::Root {
                request_id: request_id.clone(),
            },
            parent: None,
        };
        let scoped = match raw.acquire(acquire, capability).await {
            Ok(value) => value,
            Err(error) => return error_response(error).map(Into::into),
        };
        let (scoped, writer) = scoped
            .track_root(&context)
            .map_err(|error| worker::Error::RustError(error.envelope.error.message))?;
        (scoped, Some(writer))
    };
    let result = dispatch_with_store(
        request, env, context, receiver, native, &identity, request_id, store,
    )
    .await;
    if let Some(writer) = writer {
        writer.complete();
    }
    result
}

async fn dispatch_with_store(
    request: Request,
    env: Env,
    context: Rc<Context>,
    receiver: Option<&gateway_publication_transport::transport::PublicationReceiver>,
    native: NativeBindings,
    identity: &ReleaseIdentity,
    request_id: String,
    store: gateway_worker_authority::client::RemoteAuthorityStore,
) -> Result<GatewayResponse> {
    if native_authority::is_path(&request.path()) {
        let response = match native_authority::handle(request, &env, &native, &store).await {
            Ok(response) => response,
            Err(error) => error_response(error)?,
        };
        return Ok(response.into());
    }
    if request.path() == gateway_auth::native::NATIVE_PUBLICATION_PATH {
        let mut response = match publication::handle(request, &env, receiver, &native, &store).await
        {
            Ok(response) => response,
            Err(error) => error_response(error)?,
        };
        response.headers_mut().set("Cache-Control", "no-store")?;
        response
            .headers_mut()
            .set("x-uos-request-id", &request_id)?;
        response
            .headers_mut()
            .set(GIT_SHA_HEADER, identity.git_sha())?;
        response
            .headers_mut()
            .set(DEPLOYMENT_ID_HEADER, identity.deployment_id())?;
        return Ok(response.into());
    }
    let cors = cors::Cors::from_request(&request, &env)?;
    let mut response = if request.method() == Method::Options {
        Response::empty()?.with_status(204).into()
    } else if normalized_path(&request.path()) == "/health"
        && matches!(request.method(), Method::Get | Method::Head)
    {
        if request.method() == Method::Head {
            Response::empty()?.into()
        } else {
            match crate::observability_host::health_response().and_then(runtime::auth_response) {
                Ok(response) => response.into(),
                Err(error) => return error_response(error).map(Into::into),
            }
        }
    } else {
        match runtime::route(
            request,
            env,
            context.clone(),
            identity.git_sha(),
            gateway_core::identity::RequestId(request_id.clone()),
            native,
            store,
        )
        .await
        {
            Ok(response) => response,
            Err(error) => error_response(error)?.into(),
        }
    };
    // A trusted native sideband returns the actual switching response. Keep its
    // WebSocket and upgrade headers together without ordinary HTTP decoration.
    if response.status_code() == 101 {
        return Ok(response);
    }
    if response.headers().get("Cache-Control")?.is_none() {
        response.headers_mut().set("Cache-Control", "no-store")?;
    }
    cors.apply(&mut response)?;
    response
        .headers_mut()
        .set("x-uos-request-id", &request_id)?;
    response
        .headers_mut()
        .set(GIT_SHA_HEADER, identity.git_sha())?;
    response
        .headers_mut()
        .set(DEPLOYMENT_ID_HEADER, identity.deployment_id())?;
    Ok(response)
}

/// The source public router removes every trailing slash except the root slash.
pub(crate) fn normalized_path(path: &str) -> &str {
    if path == "/" {
        path
    } else {
        path.trim_end_matches('/')
    }
}

#[cfg(test)]
mod route_tests {
    #[test]
    fn normalization_matches_source_without_decoding_segments() {
        for (source, expected) in [
            ("/", "/"),
            ("/health///", "/health"),
            ("/v1/responses/", "/v1/responses"),
            ("/api/auth/login/start//", "/api/auth/login/start"),
            ("/v1/live/rtc_%2F/", "/v1/live/rtc_%2F"),
            ("///", ""),
        ] {
            assert_eq!(super::normalized_path(source), expected);
        }
    }
}
