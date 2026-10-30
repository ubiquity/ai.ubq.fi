//! Approved post-head HTTP failure encoding; original failure DATA stays internal.
use futures_util::StreamExt;
use gateway_source::error::GatewayError;
use gateway_worker_capture::capture::Capture;
use gateway_worker_platform::cancellation::Cancellation;
use gateway_worker_web::http_failure::{error_detail, frames, native_failure_record, safe_headers};
use std::{rc::Rc, task::Poll};
use worker::{Response, ResponseBody};

const ERROR_BYTES: usize = 64 * 1024;
const ERROR_DEADLINE_MS: i32 = 1000;

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
    let original = native_failure_record(status, headers, truncated, &detail, complete)?;
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
