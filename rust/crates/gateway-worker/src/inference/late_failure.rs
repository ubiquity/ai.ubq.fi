//! Approved post-head HTTP failure encoding; original failure DATA stays internal.
use crate::inference_capture::Capture;
use futures_util::StreamExt;
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use gateway_worker_platform::cancellation::Cancellation;
use gateway_worker_web::http_failure::{error_detail, frames};
use std::{rc::Rc, task::Poll};
use worker::{Response, ResponseBody};

const SAFE_HEADERS: &[&str] = &[
    "cache-control",
    "content-length",
    "content-type",
    "openai-processing-ms",
    "retry-after",
    "x-ratelimit-limit-requests",
    "x-ratelimit-limit-tokens",
    "x-ratelimit-remaining-requests",
    "x-ratelimit-remaining-tokens",
    "x-ratelimit-reset-requests",
    "x-ratelimit-reset-tokens",
];
const HEADER_UNITS: usize = 512;
const ERROR_BYTES: usize = 64 * 1024;
const ERROR_DEADLINE_MS: i32 = 1000;

fn scalar(value: &serde_json::Value) -> Result<SourceJson, GatewayError> {
    SourceJson::from_serializable(value)
}
fn object(fields: Vec<(&str, SourceJson)>) -> Result<SourceJson, GatewayError> {
    source::object_keys(
        fields
            .into_iter()
            .map(|(name, value)| (JsString16::from_utf8(name), value))
            .collect(),
    )
}
fn safe_headers(response: &Response) -> Result<(SourceJson, bool), GatewayError> {
    let mut values = Vec::new();
    let mut truncated = false;
    for name in SAFE_HEADERS {
        if let Ok(Some(value)) = response.headers().get(name) {
            let units = value.encode_utf16().collect::<Vec<_>>();
            truncated |= units.len() > HEADER_UNITS;
            values.push((
                JsString16::from_utf8(name),
                SourceJson::from_serializable(&JsString16::from_units(
                    units.into_iter().take(HEADER_UNITS).collect(),
                ))?,
            ));
        }
    }
    Ok((source::object_keys(values)?, truncated))
}
async fn bounded_body(
    response: &mut Response,
    cancellation: &Cancellation,
) -> Result<(Vec<u8>, bool), GatewayError> {
    match response.body() {
        ResponseBody::Empty => return Ok((Vec::new(), true)),
        ResponseBody::Body(bytes) => {
            return Ok((
                bytes[..bytes.len().min(ERROR_BYTES)].to_vec(),
                bytes.len() <= ERROR_BYTES,
            ));
        }
        ResponseBody::Stream(_) => {}
    }
    let child = cancellation.child()?;
    let _deadline = child.set_timeout_ms(ERROR_DEADLINE_MS)?;
    let mut reader = response.stream().map_err(|_| unavailable())?;
    let mut bytes = Vec::new();
    let mut complete = false;
    loop {
        let next = std::future::poll_fn(|context| {
            child.register_waker(context.waker());
            if child.signal().aborted() {
                Poll::Ready(None)
            } else {
                reader.poll_next_unpin(context)
            }
        })
        .await;
        match next {
            None => {
                complete = !child.signal().aborted();
                break;
            }
            Some(Err(_)) => break,
            Some(Ok(chunk)) => {
                let keep = chunk.len().min(ERROR_BYTES - bytes.len());
                bytes.extend_from_slice(&chunk[..keep]);
                if keep != chunk.len() {
                    break;
                }
            }
        }
    }
    drop(reader);
    Ok((bytes, complete))
}
pub(super) async fn convert(
    mut response: Response,
    capture: &Rc<Capture>,
    cancellation: &Cancellation,
) -> Result<Response, GatewayError> {
    let status = response.status_code();
    let (headers, truncated) = safe_headers(&response)?;
    let (bytes, complete) = bounded_body(&mut response, cancellation).await?;
    let detail = error_detail(&bytes, complete, status)?;
    let original = object(vec![
        ("status", scalar(&serde_json::json!(status))?),
        ("headers", headers),
        ("headers_truncated", scalar(&serde_json::json!(truncated))?),
        (
            "error",
            if complete {
                detail.clone()
            } else {
                scalar(&serde_json::Value::Null)?
            },
        ),
        ("body_complete", scalar(&serde_json::json!(complete))?),
    ])?;
    let payload = frames(&detail, status)?;
    capture.late_http_wire(original);
    let mut response = Response::from_bytes(payload).map_err(|_| unavailable())?;
    response
        .headers_mut()
        .set("content-type", "text/event-stream")
        .map_err(|_| unavailable())?;
    response
        .headers_mut()
        .set("cache-control", "no-store")
        .map_err(|_| unavailable())?;
    Ok(response)
}
pub(super) fn from_error(error: &GatewayError) -> Result<Response, GatewayError> {
    Response::from_json(&error.envelope)
        .map(|response| response.with_status(error.status))
        .map_err(|_| unavailable())
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "The selected HTTP failure could not be rendered.",
        "source_refusal_unavailable",
    )
}
