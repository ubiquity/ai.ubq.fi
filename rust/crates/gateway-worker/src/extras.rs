//! Actual fixed embeddings/jobs/SystemOne routes; M15 owns their source behavior.
use crate::authority::RemoteAuthorityStore;
use crate::catalog::CatalogState;
use crate::extras_host::Host;
use crate::host::PlatformClock;
use crate::host::PlatformHost;
use crate::host::PlatformTokens;
use crate::inference::RequestResources;
use crate::routing_host;
use gateway_auth::types::{AuthMethod, AuthPrincipal};
use gateway_authority::kernel::KernelTerminal;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use gateway_contracts::identity::{ApiKeyId, PrincipalId, RequestContext, RequestId};
use gateway_extras_contract::errors::rendering as invalid;
use gateway_extras_contract::family::ExtrasFamily;
use gateway_extras_contract::types::{
    ExtrasRequest, ExtrasResponse, ExtrasRoute, ImageKind, KernelDisposition,
};
use gateway_extras_runtime::ExtrasRuntime;
use gateway_providers::types::ClientSurface;
use gateway_routing_runtime::{session::RoutingSession, types::RoutingInput};
use gateway_worker_http_body::body;
use gateway_worker_storage::store::now_ms;
use gateway_worker_stream::observed::{FinishHook, ObservationHandle, ObservedStream};
use std::rc::Rc;
use worker::{Request, Response};

pub struct Target {
    pub route: ExtrasRoute,
    pub request_id: RequestId,
}

pub async fn run(
    mut request: Request,
    target: Target,
    resources: RequestResources,
    principal: &AuthPrincipal,
    store: RemoteAuthorityStore,
    catalog: CatalogState,
    platform: &PlatformHost,
) -> Result<Response, GatewayError> {
    let Target { route, request_id } = target;
    let RequestResources { context, permit } = resources;
    let image = matches!(route, ExtrasRoute::Images(_));
    let (raw, multipart, raw_body_bytes) = match request_body(&mut request, &route, image).await? {
        ExtrasBody::Parsed(raw, multipart, len) => (raw, multipart, len),
        ExtrasBody::Rejected(response, _bytes) => return crate::runtime::auth_response(response),
    };
    let original = RequestContext {
        request_id,
        principal: PrincipalId(crate::lifecycle::principal_id(principal)),
        key_id: match &principal.method {
            AuthMethod::KvApiKey { key_id, .. } => Some(ApiKeyId(key_id.clone())),
            _ => None,
        },
        route: route.label().into(),
        started_at_ms: now_ms()?,
    };
    let family = match route {
        ExtrasRoute::Embeddings => ExtrasFamily::Embeddings,
        ExtrasRoute::JobCreate => ExtrasFamily::EmbeddingJobCreate,
        ExtrasRoute::JobGet(_) => ExtrasFamily::EmbeddingJobPoll,
        ExtrasRoute::SystemOne => ExtrasFamily::SystemOne,
        ExtrasRoute::Decisions => ExtrasFamily::Decisions,
        ExtrasRoute::Images(ImageKind::Generations) => ExtrasFamily::ImageGenerations,
        ExtrasRoute::Images(ImageKind::Edits) => ExtrasFamily::ImageEdits,
    };
    let default_model =
        gateway_extras_contract::model::default_model(catalog.runtime.as_ref(), &catalog.input);
    let session = RoutingSession::begin_extras(
        RoutingInput {
            context: original.clone(),
            principal: principal.clone(),
            raw: raw.clone(),
            surface: ClientSurface::Responses,
            catalog: catalog.input.clone(),
            runtime: catalog.runtime.clone(),
            client_version: "0.100.0".into(),
            native_session_id: None,
            retry_prohibited: false,
        },
        store.clone(),
        routing_host::Host::new(
            store.clone(),
            platform.clone(),
            context.clone(),
            original.clone(),
        ),
        PlatformClock,
        PlatformTokens,
        family,
    )
    .await
    .map_err(|error| *error.error)?;
    let host = Host::new(
        platform.clone(),
        catalog,
        session.admission_handle(),
        image,
        default_model,
    )
    .await?;
    let runtime = Rc::new(ExtrasRuntime::new(store, host));
    let result = runtime
        .handle(ExtrasRequest {
            route,
            body: raw,
            multipart,
            raw_body_bytes,
            idempotency_key: request
                .headers()
                .get("idempotency-key")
                .map_err(|_| invalid())?,
            principal,
            context: &original,
        })
        .await;
    runtime.host().settle_child().await?;
    let result = completed_result(&runtime, result).await?;
    let (kernel, reason) = completion(result.kernel, result.semantic_terminal);
    let observation = ObservationHandle::default();
    if let Some(terminal) = result.semantic_terminal {
        observation.semantic_terminal(terminal);
    }
    let finish: FinishHook = Box::new(move |observation| {
        Box::pin(async move {
            if let Err(error) = runtime
                .host()
                .admission
                .finish(observation, kernel, reason)
                .await
            {
                worker::console_error!(
                    "Durable extras settlement remains pending: {}",
                    error.error.envelope.error.message
                );
            }
            drop(permit);
        })
    });
    render_response(
        (result.body, result.status, result.headers),
        observation,
        context,
        platform,
        finish,
    )
}

async fn completed_result(
    runtime: &ExtrasRuntime<RemoteAuthorityStore, Host>,
    result: Result<ExtrasResponse, GatewayError>,
) -> Result<ExtrasResponse, GatewayError> {
    match result {
        Ok(result) => Ok(result),
        Err(error) => {
            runtime
                .host()
                .admission
                .finish(
                    Default::default(),
                    KernelTerminal::Released,
                    "extras_gateway_failure",
                )
                .await
                .map_err(|error| *error.error)?;
            Err(error)
        }
    }
}

fn completion(
    kernel: KernelDisposition,
    terminal: Option<gateway_contracts::lifecycle::SemanticTerminal>,
) -> (KernelTerminal, &'static str) {
    match kernel {
        KernelDisposition::Completed => (KernelTerminal::Committed, "extras_source_completed"),
        KernelDisposition::Incomplete(reason) => (KernelTerminal::Released, reason),
        KernelDisposition::FromAggregate
            if terminal == Some(gateway_contracts::lifecycle::SemanticTerminal::Completed) =>
        {
            (KernelTerminal::Committed, "images_source_completed")
        }
        KernelDisposition::FromAggregate => (KernelTerminal::Released, "images_source_incomplete"),
    }
}

enum ExtrasBody {
    Parsed(
        SourceJson,
        Option<Vec<gateway_providers::extras::MultipartFile>>,
        usize,
    ),
    Rejected(gateway_auth::types::AuthResponse, Vec<u8>),
}
async fn request_body(
    request: &mut Request,
    route: &ExtrasRoute,
    image: bool,
) -> Result<ExtrasBody, GatewayError> {
    let edit = *route == ExtrasRoute::Images(ImageKind::Edits);
    let multipart = image
        && request
            .headers()
            .get("content-type")
            .map_err(|_| invalid())?
            .is_some_and(|value| {
                value
                    .to_ascii_lowercase()
                    .starts_with("multipart/form-data")
            });
    if multipart && !edit {
        return Err(GatewayError::new(
            400,
            "Image generation requests must use a JSON object body.",
            Some("invalid_request_error"),
        ));
    }
    let bytes = if matches!(route, ExtrasRoute::JobGet(_)) {
        Vec::new()
    } else {
        body::request_bytes(
            request,
            if multipart {
                gateway_worker_http_body::multipart::MULTIPART_BYTES
            } else if edit {
                gateway_worker_http_body::multipart::IMAGE_JSON_BYTES
            } else if *route == ExtrasRoute::SystemOne {
                body::SYSTEM_ONE_BODY_MAX_BYTES
            } else {
                body::INFERENCE_JSON_MAX_BYTES
            },
        )
        .await
        .map_err(|error| gateway_extras_contract::route::body_error(route, error))?
    };
    let (raw, multipart) = if multipart {
        let (raw, files) = gateway_worker_http_body::multipart::parse(request, &bytes).await?;
        (raw, Some(files))
    } else if matches!(route, ExtrasRoute::JobGet(_)) {
        (SourceJson::parse("null")?, None)
    } else if edit {
        (
            gateway_worker_http_body::multipart::image_json(&bytes)?,
            None,
        )
    } else {
        let raw = match gateway_schema::source_request::parse_http_body(&bytes) {
            Ok(raw) => raw,
            Err(error) => {
                return Ok(ExtrasBody::Rejected(
                    gateway_auth::types::AuthResponse::source_json(
                        error.status,
                        error.body().clone(),
                    ),
                    bytes,
                ));
            }
        };
        (raw, None)
    };
    let raw_body_bytes = bytes.len();
    drop(bytes);
    Ok(ExtrasBody::Parsed(raw, multipart, raw_body_bytes))
}

fn render_response(
    result: (SourceJson, u16, gateway_extras_contract::types::Headers),
    observation: ObservationHandle,
    context: Rc<worker::Context>,
    platform: &PlatformHost,
    finish: FinishHook,
) -> Result<Response, GatewayError> {
    let (body, status, headers) = result;
    let bytes = body.as_bytes().to_vec();
    let source = Box::pin(futures_util::stream::iter([Ok::<_, worker::Error>(bytes)]));
    let mut response = ObservedStream::new(
        source,
        observation,
        context,
        platform.cancellation.clone(),
        None,
        finish,
    )
    .into_response()
    .map_err(|_| invalid())?
    .with_status(status);
    for (name, value) in headers {
        response
            .headers_mut()
            .set(&name, &value)
            .map_err(|_| invalid())?;
    }
    Ok(response)
}
