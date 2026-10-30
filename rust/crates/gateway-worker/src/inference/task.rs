//! Closed actual request execution. A task is not admission or semantic proof.
use super::{Inference, OwnedRequest, ResponseResult, late_failure};
use futures_util::{Stream, future::poll_fn};
use gateway_auth_contracts::types::AuthPrincipal;
use gateway_contracts::lifecycle::TransportObservation;
use gateway_source::error::GatewayError;
use gateway_worker_capture::capture::Capture;
use gateway_worker_platform::cancellation::Cancellation;
use gateway_worker_stream::observed::{ObservationHandle, ObservedStream};
use gateway_worker_wire_observation::head::HeadState;
use std::{
    cell::Cell,
    future::Future,
    pin::Pin,
    rc::Rc,
    task::{Context, Poll},
};
use worker::{ByteStream, Response, ResponseBody};

type Execution = Pin<Box<dyn Future<Output = Result<TaskReply, GatewayError>>>>;
struct TaskReply {
    response: Response,
    late_failure: bool,
}
#[derive(Clone, Copy, PartialEq)]
enum Phase {
    Running,
    Complete,
    Transferred,
}
#[derive(Clone, Copy, PartialEq)]
enum BodyStage {
    Selecting,
    Forwarding,
    LateFailure,
    Failed,
}
pub(super) enum StartDisposition {
    Complete(ResponseResult),
    AcceptedHead,
}

pub(super) struct RequestTask {
    execution: Option<Execution>,
    outcome: Option<Result<TaskReply, GatewayError>>,
    phase: Phase,
    head: Rc<HeadState>,
    cancellation: Rc<Cancellation>,
    capture: Rc<Capture>,
    context: Rc<worker::Context>,
    request_id: String,
}
impl RequestTask {
    pub(super) fn start(input: OwnedRequest) -> Self {
        let head = HeadState::new();
        input.capture.bind_head(head.clone());
        let cancellation = input.host.cancellation.clone();
        let capture = input.capture.clone();
        let context = input.context.clone();
        let request_id = input.request_id.0.clone();
        let executing_head = head.clone();
        let execution = Box::pin(async move {
            let result = Inference {
                principal: &input.principal,
                store: &input.store,
                host: &input.host,
                context: input.context,
                capture: &input.capture,
                responses: input.responses,
                head: executing_head.clone(),
            }
            .execute(
                &input.bytes,
                input.catalog,
                input.client_version,
                input.request_id,
                input.local_compaction,
                input.permit,
            )
            .await;
            if let Err(error) = &result {
                input
                    .capture
                    .failure(error, input.host.cancellation.client_aborted())
                    .await;
            }
            if !executing_head.expects_stream() || executing_head.is_delivery_lost() {
                return result.map(|response| TaskReply {
                    response,
                    late_failure: false,
                });
            }
            let response = match result {
                Ok(response) => response,
                Err(error) => late_failure::from_error(&error)?,
            };
            let is_sse = response
                .headers()
                .get("content-type")
                .ok()
                .flatten()
                .is_some_and(|value| value.to_ascii_lowercase().starts_with("text/event-stream"));
            if response.status_code() >= 400 && !is_sse {
                return late_failure::convert(response, &input.capture, &input.host.cancellation)
                    .await
                    .map(|response| TaskReply {
                        response,
                        late_failure: true,
                    });
            }
            Ok(TaskReply {
                response,
                late_failure: false,
            })
        });
        Self {
            execution: Some(execution),
            outcome: None,
            phase: Phase::Running,
            head,
            cancellation,
            capture,
            context,
            request_id,
        }
    }
    fn poll_execution(&mut self, context: &mut Context<'_>) {
        if let Some(execution) = &mut self.execution
            && let Poll::Ready(result) = execution.as_mut().poll(context)
        {
            self.execution = None;
            self.outcome = Some(result);
            self.phase = Phase::Complete;
        }
    }
    fn take_reply(&mut self) -> Option<Result<TaskReply, GatewayError>> {
        let result = self.outcome.take();
        if result.is_some() {
            self.phase = Phase::Transferred;
        }
        result
    }
    pub(super) fn poll_start(&mut self, context: &mut Context<'_>) -> Poll<StartDisposition> {
        self.poll_execution(context);
        // A real accepted head wins even when immediate EOF/fallback also finished in this poll.
        if self.head.seen() {
            return Poll::Ready(StartDisposition::AcceptedHead);
        }
        self.take_reply().map_or(Poll::Pending, |result| {
            Poll::Ready(StartDisposition::Complete(
                result.map(|reply| reply.response),
            ))
        })
    }
    fn into_provisional_response(self) -> ResponseResult {
        let head = self.head.clone();
        let capture = self.capture.clone();
        let context = self.context.clone();
        let cancellation = self.cancellation.clone();
        let request_id = self.request_id.clone();
        let stage = Rc::new(Cell::new(BodyStage::Selecting));
        let finish_stage = stage.clone();
        let finish_capture = capture.clone();
        let finish_head = head.clone();
        let source = PendingBody {
            task: Some(self),
            stream: None,
            bytes: None,
            done: false,
            stage,
        };
        let finish = Box::new(move |observation: TransportObservation| {
            Box::pin(async move {
                if finish_stage.get() == BodyStage::Selecting
                    && observation.reader_termination
                        == Some(gateway_contracts::lifecycle::ReaderTermination::Cancelled)
                    && finish_head.is_published()
                {
                    finish_head.delivery_lost();
                }
                if finish_stage.get() != BodyStage::Forwarding {
                    finish_capture.finish_provisional(observation).await;
                }
            }) as gateway_worker_stream::observed::FinishFuture
        });
        let response = ObservedStream::new(
            source,
            ObservationHandle::default(),
            context,
            cancellation,
            None,
            finish,
        )
        .into_response();
        let mut response = match response {
            Ok(response) => response,
            Err(_) => {
                head.publication_failed();
                return Err(unavailable());
            }
        };
        let headers = response.headers_mut();
        let result = headers
            .set("content-type", "text/event-stream")
            .and_then(|()| headers.set("cache-control", "no-store"))
            .and_then(|()| headers.set("x-uos-request-id", &request_id));
        if result.is_err() {
            head.publication_failed();
            drop(response.stream());
            return Err(unavailable());
        }
        head.published();
        capture.provisional_wire();
        Ok(response)
    }
}
impl Drop for RequestTask {
    fn drop(&mut self) {
        if self.phase == Phase::Running && self.execution.is_some() {
            if self.head.is_published() {
                self.head.delivery_lost();
            }
            if !self.cancellation.signal().aborted() {
                self.cancellation.abort();
            }
            // Drop the one actual execution; its armed lifetime schedules one cleanup.
            drop(self.execution.take());
        } else if self.phase == Phase::Complete
            && let Some(Ok(mut reply)) = self.outcome.take()
        {
            // A completed but abandoned body still has its own selected-body owner.
            drop(reply.response.stream());
        }
    }
}

struct PendingBody {
    task: Option<RequestTask>,
    stream: Option<ByteStream>,
    bytes: Option<Vec<u8>>,
    done: bool,
    stage: Rc<Cell<BodyStage>>,
}
impl Stream for PendingBody {
    type Item = worker::Result<Vec<u8>>;
    fn poll_next(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        if self.done {
            return Poll::Ready(None);
        }
        if let Some(task) = &mut self.task {
            task.poll_execution(context);
            let Some(result) = task.take_reply() else {
                return Poll::Pending;
            };
            match result {
                Err(error) => {
                    self.stage.set(BodyStage::Failed);
                    self.done = true;
                    return Poll::Ready(Some(Err(worker::Error::RustError(
                        error.envelope.error.message,
                    ))));
                }
                Ok(reply) => {
                    self.stage.set(if reply.late_failure {
                        BodyStage::LateFailure
                    } else {
                        BodyStage::Forwarding
                    });
                    let (_, body) = reply.response.into_parts();
                    match body {
                        ResponseBody::Empty => self.done = true,
                        ResponseBody::Body(bytes) => self.bytes = Some(bytes),
                        ResponseBody::Stream(stream) => {
                            self.stream = Some(ByteStream::from(stream))
                        }
                    }
                }
            }
            // take_reply disarmed task Drop before a legitimate selected stream is forwarded.
            if self.stage.get() == BodyStage::Forwarding {
                drop(self.task.take());
            }
        }
        if let Some(bytes) = self.bytes.take() {
            if self.stage.get() == BodyStage::LateFailure
                && let Some(task) = &self.task
            {
                task.capture.observe_wire_bytes(&bytes);
            }
            self.done = true;
            return Poll::Ready(Some(Ok(bytes)));
        }
        if let Some(stream) = &mut self.stream {
            let result = Pin::new(stream).poll_next(context);
            if matches!(result, Poll::Ready(None)) {
                self.done = true;
            }
            return result;
        }
        self.done = true;
        Poll::Ready(None)
    }
}
pub(super) async fn drive(mut task: RequestTask, principal: &AuthPrincipal) -> ResponseResult {
    match poll_fn(|context| task.poll_start(context)).await {
        StartDisposition::Complete(result) => {
            result.and_then(|response| crate::runtime::decorate_api_policy(response, principal))
        }
        StartDisposition::AcceptedHead => task.into_provisional_response(),
    }
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "The pending downstream response is unavailable.",
        "source_refusal_unavailable",
    )
}
