//! Authoritative source-wire errors; diagnostics never replace the original body.
use crate::{inference_capture::Capture, inference_early::Gate};
use gateway_core::provider::UpstreamProvider;
use gateway_core::{error::GatewayError, source_json::SourceJson};
use gateway_observability::replay_trace::SAFE_HEADERS;
use gateway_prompt_admission::native_failure::{
    CONTENT_TYPE, encode_native_context_failure, selects_native_encoding,
};
use gateway_prompt_admission::native_http_failure::{SelectedAnswer, late_failure, reencodes};
use gateway_providers::types::{ClientSurface, ProviderEvent};
use gateway_routing_runtime::types::{RoutingError, SourceRefusalBody};
use serde_json::json;
use std::rc::Rc;
use worker::Response;

/// Encoding selectors only: no field grants authority or bypasses validation.
#[derive(Default)]
pub struct NativeTurn {
    pub stream: Option<SourceJson>,
    pub turn_metadata: Option<String>,
    /// Present only for a native Responses turn; committed after a successful dispatch gate.
    pub early: Option<Rc<Gate>>,
}
impl NativeTurn {
    pub fn early_committed(&self) -> bool {
        self.early.as_ref().is_some_and(|gate| gate.committed())
    }
}
/// One local refusal answer. A native aggregate overflow is 200 SSE; all else keeps its body.
pub struct Reply {
    pub status: u16,
    pub content_type: &'static str,
    pub body: Vec<u8>,
    pub native_context_failure: bool,
}
/// `None` keeps the gateway error path. `mint` runs only for the native encoding, which is
/// selected from the request itself and never dispatches or proves quota or capacity.
pub fn reply(
    error: &RoutingError,
    turn: &NativeTurn,
    mint: impl FnOnce() -> Result<(String, i64), GatewayError>,
) -> Result<Option<Reply>, GatewayError> {
    let stream = turn.stream.as_ref();
    if selects_native_encoding(stream, turn.turn_metadata.as_deref(), &error.error) {
        let (id, created_at) = mint()?;
        return Ok(Some(Reply {
            status: 200,
            content_type: CONTENT_TYPE,
            body: encode_native_context_failure(&id, created_at, &error.error).into_bytes(),
            native_context_failure: true,
        }));
    }
    Ok(error.source_body.as_ref().map(|body| Reply {
        status: error.error.status,
        content_type: match body {
            SourceRefusalBody::Json(_) => "application/json",
            SourceRefusalBody::InternalServerError => "text/plain; charset=utf-8",
        },
        body: body.as_bytes().to_vec(),
        native_context_failure: false,
    }))
}
fn mint() -> Result<(String, i64), GatewayError> {
    let id = crate::transport::random::random_uuid()?.replace('-', "");
    Ok((
        format!("resp_{id}"),
        crate::storage::now_ms()?.div_euclid(1000),
    ))
}
pub(crate) async fn error_response(
    error: RoutingError,
    capture: &Rc<Capture>,
    turn: &NativeTurn,
) -> Result<Response, GatewayError> {
    let Some(reply) = reply(&error, turn, mint)? else {
        return Err(*error.error);
    };
    if !reply.native_context_failure
        && turn.early_committed()
        && reencodes(reply.status, Some(reply.content_type))
    {
        let mut headers = error.headers;
        headers.insert("content-type".into(), reply.content_type.into());
        let answer = SelectedAnswer {
            status: reply.status,
            headers: &headers,
            body: &reply.body,
            complete: true,
        };
        return late_answer(capture, &answer).await;
    }
    let mut response = Response::from_bytes(reply.body.clone())
        .map_err(|_| unavailable())?
        .with_status(reply.status);
    response
        .headers_mut()
        .set("content-type", reply.content_type)
        .map_err(|_| unavailable())?;
    response
        .headers_mut()
        .set("cache-control", "no-store")
        .map_err(|_| unavailable())?;
    for (name, value) in error.headers {
        response
            .headers_mut()
            .set(&name, &value)
            .map_err(|_| unavailable())?;
    }
    if reply.native_context_failure {
        capture.native_context_failure(&reply.body).await;
    } else {
        capture.immediate(reply.status, &reply.body).await;
    }
    Ok(response)
}
/// Mints the response id and records the internal evidence; returns the two client frames.
pub(crate) fn late_frames(
    capture: &Capture,
    answer: &SelectedAnswer<'_>,
) -> Result<Vec<u8>, GatewayError> {
    let (id, created_at) = mint()?;
    Ok(capture.late_http_failure(late_failure(answer, SAFE_HEADERS, &id, created_at)?))
}
async fn late_answer(
    capture: &Capture,
    answer: &SelectedAnswer<'_>,
) -> Result<Response, GatewayError> {
    let frames = late_frames(capture, answer)?;
    capture.local_stream(&frames).await;
    crate::inference_early::sse(Response::from_bytes(frames).map_err(|_| unavailable())?)
}
/// Source `nativeFailureAfterEarlyHeaders` for a gateway error after headers were committed.
pub(crate) async fn late_error(
    error: GatewayError,
    capture: &Capture,
    cancelled: bool,
) -> Result<Response, GatewayError> {
    let headers = [("content-type".to_owned(), "application/json".to_owned())].into();
    if let Ok(body) = serde_json::to_vec(&error.envelope) {
        let answer = SelectedAnswer {
            status: error.status,
            headers: &headers,
            body: &body,
            complete: true,
        };
        if let Ok(response) = late_answer(capture, &answer).await {
            return Ok(response);
        }
    }
    capture.failure(&error, cancelled).await;
    Err(error)
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "The source refusal could not be rendered.",
        "source_refusal_unavailable",
    )
}

/// Retain actual plan pricing data and the existing Messages cost requirement.
pub(crate) fn pricing_metadata(
    pricing: Option<&gateway_core::source_json::SourceJson>,
    require_provider_cost: bool,
) -> Result<Option<gateway_core::source_json::SourceJson>, GatewayError> {
    use gateway_core::{
        js_string::JsString16, source_json::SourceJson, source_serialization as source,
    };
    if !require_provider_cost {
        return Ok(pricing.cloned());
    }
    let key = JsString16::from_utf8("require_provider_cost");
    let mut fields = match pricing {
        Some(pricing) => pricing.object_entries()?,
        None => Vec::new(),
    };
    fields.retain(|(name, _)| name != &key);
    fields.push((key, SourceJson::from_serializable(&true)?));
    Ok(Some(source::object_keys(fields)?))
}

/// Source `chatStreamErrorValue` and the Responses `response.failed` frame of `failStream`,
/// with each relay adapter's own `streamErrorCode`.
pub fn stream_failure(
    provider: UpstreamProvider,
    surface: ClientSurface,
    response_id: &str,
    sequence: u64,
) -> ProviderEvent {
    let code = match provider {
        UpstreamProvider::Deepseek => "deepseek_upstream_stream_error",
        UpstreamProvider::Lithos => "lithos_upstream_stream_error",
        UpstreamProvider::Openrouter => "openrouter_upstream_stream_error",
        _ => "upstream_stream_error",
    };
    let message = "Upstream Chat Completions stream failed.";
    if surface == ClientSurface::Chat {
        return ProviderEvent::chat(
            json!({"error":{"message":message,"type":"server_error","code":code,"param":null}}),
        );
    }
    let error = json!({"code":code,"message":message});
    ProviderEvent::responses(
        json!({"type":"response.failed","response":{"id":response_id,"object":"response","status":"failed","error":error},"sequence_number":sequence}),
    )
}
/// Source `failStream`: the caller's own cancellation ends with no frame; any other abort
/// (the gateway deadline) writes the declared failure frame before the body closes.
pub fn abort_frame(
    client: bool,
    provider: UpstreamProvider,
    surface: ClientSurface,
    response_id: &str,
    sequence: u64,
) -> Option<ProviderEvent> {
    (!client).then(|| stream_failure(provider, surface, response_id, sequence))
}
