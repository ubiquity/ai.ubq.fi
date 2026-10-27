//! Approved post-head HTTP failure encoding; original failure DATA stays internal.
use crate::inference_capture::Capture;
use futures_util::StreamExt;
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use gateway_worker_platform::{cancellation::Cancellation, random};
use gateway_worker_storage::store::now_ms;
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
fn string_field(value: &SourceJson, name: &str) -> Option<JsString16> {
    source::member(value, name)
        .as_ref()
        .and_then(source::string)
}
fn error_detail(bytes: &[u8], complete: bool, status: u16) -> Result<SourceJson, GatewayError> {
    let decoded = String::from_utf8_lossy(bytes);
    let text = if complete {
        decoded
            .strip_prefix('\u{feff}')
            .unwrap_or(&decoded)
            .to_owned()
    } else {
        String::new()
    };
    if let Ok(parsed) = SourceJson::parse(&text)
        && let Some(error) = source::member(&parsed, "error")
        && error.object_entries().is_ok()
    {
        return Ok(error);
    }
    scalar(
        &serde_json::json!({"message":if text.is_empty(){format!("Upstream request failed with HTTP {status}.")}else{text}}),
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
fn frames(error: &SourceJson, status: u16) -> Result<Vec<u8>, GatewayError> {
    let code = string_field(error, "code").unwrap_or_else(|| "server_error".into());
    let message = string_field(error, "message").unwrap_or_else(|| {
        JsString16::from_utf8(&format!("Upstream request failed with HTTP {status}."))
    });
    let id = format!("resp_{}", random::random_uuid()?.replace('-', ""));
    let base = scalar(
        &serde_json::json!({"id":id,"object":"response","created_at":now_ms()? / 1000,"status":"in_progress","error":null,"output":[],"usage":null}),
    )?;
    let mut failed = base.object_entries()?;
    for (name, value) in &mut failed {
        if name.as_units() == JsString16::from_utf8("status").as_units() {
            *value = scalar(&serde_json::json!("failed"))?;
        }
        if name.as_units() == JsString16::from_utf8("error").as_units() {
            *value = object(vec![
                ("code", SourceJson::from_serializable(&code)?),
                ("message", SourceJson::from_serializable(&message)?),
            ])?;
        }
    }
    let created = object(vec![
        ("type", scalar(&serde_json::json!("response.created"))?),
        ("sequence_number", scalar(&serde_json::json!(0))?),
        ("response", base),
    ])?;
    let failed = object(vec![
        ("type", scalar(&serde_json::json!("response.failed"))?),
        ("sequence_number", scalar(&serde_json::json!(1))?),
        ("response", source::object_keys(failed)?),
    ])?;
    Ok(format!(
        "event: response.created\ndata: {}\n\nevent: response.failed\ndata: {}\n\n",
        created.as_str(),
        failed.as_str()
    )
    .into_bytes())
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
