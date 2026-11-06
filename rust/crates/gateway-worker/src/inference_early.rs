//! Source e9b1b90 with 9e75476: a native Codex streamed Responses turn receives its SSE headers
//! after validation and a successful dispatch gate, before provider output. No heartbeat is
//! written; the selected answer, or its re-encoded late HTTP failure, follows on the same body.
use gateway_core::{error::GatewayError, source_json::SourceJson, source_serialization as json};
use gateway_prompt_admission::native_failure::is_codex_native_turn;
use gateway_schema::{provider_catalog::CEREBRAS_IDS, util::js_whitespace};
use std::{
    cell::Cell,
    future::{Future, poll_fn},
    pin::Pin,
    rc::Rc,
    task::Poll,
};
use worker::{Response, wasm_bindgen_futures::JsFuture, web_sys};

/// Transport readiness only: it grants no authority, emits no API event and settles nothing.
#[derive(Debug, Default)]
pub struct Gate {
    armed: Cell<bool>,
    committed: Cell<bool>,
}
impl Gate {
    /// Source `nativeHeaderUsageContext` prerequisites known before the body is parsed.
    pub fn for_request(responses: bool, turn_metadata: Option<&str>) -> Option<Rc<Self>> {
        (responses && is_codex_native_turn(turn_metadata)).then(Rc::default)
    }
    /// After parsing: only a literal `stream: true` that is not a buffered Cerebras model.
    pub fn arm(&self, stream: Option<&SourceJson>, model: Option<&SourceJson>) {
        let cerebras = model.and_then(json::string).is_some_and(|model| {
            let model = String::from_utf16_lossy(model.as_units());
            CEREBRAS_IDS.contains(&model.trim_matches(js_whitespace).to_lowercase().as_str())
        });
        self.armed
            .set(stream.is_some_and(|value| value.as_str() == "true") && !cerebras);
    }
    /// Runs only after a successful dispatch gate; an aborted caller commits nothing.
    pub fn commit(&self, aborted: bool) {
        if self.armed.get() && !aborted {
            self.committed.set(true);
        }
    }
    pub fn committed(&self) -> bool {
        self.committed.get()
    }
}

/// Drives the unchanged pipeline until it answers or commits headers. After a commit the same
/// pipeline is owned by `waitUntil`, never dropped, and its answer is piped into the open body.
pub(crate) async fn respond(
    gate: Rc<Gate>,
    pipeline: impl Future<Output = Result<Response, GatewayError>> + 'static,
    context: &worker::Context,
) -> Result<Response, GatewayError> {
    let pipeline = match drive(&gate, Box::pin(pipeline)).await {
        Ok(result) => return result,
        Err(pending) => pending,
    };
    let bridge = web_sys::TransformStream::new().map_err(|_| unavailable())?;
    let writable = bridge.writable();
    context.wait_until(async move {
        let body = pipeline
            .await
            .map(|answer| web_sys::Response::from(answer).body());
        // A pipe or close failure is the client's disconnect; the inner body owns its receipt.
        let _settled = match body {
            Ok(Some(body)) => JsFuture::from(body.pipe_to(&writable)).await,
            Ok(None) => JsFuture::from(writable.close()).await,
            Err(_) => JsFuture::from(writable.abort()).await,
        };
    });
    let response = Response::builder().with_status(200);
    sse(response.stream(bridge.readable()))
}
/// Polls the one pipeline until it answers or its dispatch gate commits headers. A committed
/// pipeline is handed back still pending and is never dropped or polled twice concurrently.
pub async fn drive<F: Future + Unpin>(gate: &Gate, mut pipeline: F) -> Result<F::Output, F> {
    let answered = poll_fn(|cx| match Pin::new(&mut pipeline).poll(cx) {
        Poll::Ready(result) => Poll::Ready(Some(result)),
        Poll::Pending if gate.committed() => Poll::Ready(None),
        Poll::Pending => Poll::Pending,
    })
    .await;
    answered.ok_or(pipeline)
}
/// Source early headers: SSE with no caching; CORS and request identity are added by the router.
pub(crate) fn sse(response: Response) -> Result<Response, GatewayError> {
    let headers = response.headers();
    (headers.set("content-type", "text/event-stream"))
        .and_then(|()| headers.set("cache-control", "no-store"))
        .map_err(|_| unavailable())?;
    Ok(response)
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native stream headers are unavailable.",
        "native_stream_unavailable",
    )
}
