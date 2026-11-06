//! Outgoing-body outcomes: a native pipe result never replaces a recorded Rust-side cause,
//! exactly one terminal is handed off, and a gateway deadline writes the source failure frame.
use gateway_core::lifecycle::{
    BodyOutcome, DeliveryOutcome, ReaderTermination, SemanticTerminal, TransportObservation,
};
use gateway_core::provider::UpstreamProvider;
use gateway_providers::types::ClientSurface;
use gateway_worker::inference_source::{abort_frame, stream_failure};
use gateway_worker::transport::stream::{
    FinishSignal, ObservationHandle, SourceEnd, constructor_result, end_facts, piped,
};
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::task::{Poll, Wake, Waker};

type TestResult = Result<(), Box<dyn std::error::Error>>;

#[derive(Default)]
struct WakeCount(AtomicUsize);
impl Wake for WakeCount {
    fn wake(self: Arc<Self>) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}

fn recorded(end: SourceEnd, client: bool, deadline: bool) -> TransportObservation {
    let (reader, body) = end_facts(end, client, deadline);
    TransportObservation {
        reader_termination: Some(reader),
        body: Some(body),
        ..TransportObservation::default()
    }
}

fn facts(observed: TransportObservation) -> (Option<ReaderTermination>, Option<BodyOutcome>) {
    (observed.reader_termination, observed.body)
}

fn failure_settles_once(error: &'static str) -> TestResult {
    let signal = Rc::new(FinishSignal::default());
    let count = Arc::new(WakeCount::default());
    let waker = Waker::from(count.clone());
    assert_eq!(signal.poll_take(&waker), Poll::Pending);
    let handle = ObservationHandle::default();
    handle.semantic_terminal(SemanticTerminal::Completed);
    let result: Result<(), _> = constructor_result(Err(error), &Some(signal.clone()), &handle);
    assert_eq!(result, Err(error));
    assert!(signal.completed());
    assert_eq!(count.0.load(Ordering::SeqCst), 1);
    let Poll::Ready(observed) = signal.poll_take(&waker) else {
        return Err("the constructor failure was not handed off".into());
    };
    assert_eq!(
        observed.semantic_terminal,
        Some(SemanticTerminal::Completed)
    );
    let abandoned = (
        Some(ReaderTermination::Cancelled),
        Some(BodyOutcome::Interrupted),
    );
    assert_eq!(facts(observed), abandoned);
    assert_eq!(observed.delivery, DeliveryOutcome::Unobserved);
    signal.complete(TransportObservation::default());
    assert_eq!(signal.poll_take(&waker), Poll::Pending);
    assert_eq!(count.0.load(Ordering::SeqCst), 1);
    Ok(())
}

#[test]
fn failed_response_constructor_settles_registered_signal_once() -> TestResult {
    failure_settles_once("response constructor")
}

#[test]
fn failed_bridge_constructor_settles_registered_signal_once() -> TestResult {
    failure_settles_once("bridge constructor")
}

#[test]
fn successful_constructor_keeps_existing_waiter_pending() {
    let signal = Rc::new(FinishSignal::default());
    let result =
        constructor_result::<_, ()>(Ok(7), &Some(signal.clone()), &ObservationHandle::default());
    assert_eq!(result, Ok(7));
    assert!(!signal.completed());
    assert_eq!(signal.poll_take(Waker::noop()), Poll::Pending);
}

#[test]
fn deadline_or_read_error_after_partial_delivery_keeps_its_cause_when_the_pipe_fails() {
    let deadline = piped(recorded(SourceEnd::ReadError, false, true), true, false);
    let deadline_facts = (
        Some(ReaderTermination::Deadline),
        Some(BodyOutcome::Interrupted),
    );
    assert_eq!(facts(deadline), deadline_facts);
    assert_eq!(deadline.delivery, DeliveryOutcome::Unobserved);
    let read_error = piped(recorded(SourceEnd::ReadError, false, false), true, false);
    let read_error_facts = (
        Some(ReaderTermination::ReadError),
        Some(BodyOutcome::Failed),
    );
    assert_eq!(facts(read_error), read_error_facts);
    // A deadline that already wrote its frame and closed cleanly stays a deadline.
    let closed = piped(recorded(SourceEnd::Eof, false, true), false, false);
    assert_eq!(facts(closed), deadline_facts);
}

#[test]
fn client_abort_then_clean_end_reports_the_abort() {
    let observed = piped(recorded(SourceEnd::Eof, true, false), false, true);
    let cancelled = (
        Some(ReaderTermination::Cancelled),
        Some(BodyOutcome::Interrupted),
    );
    assert_eq!(facts(observed), cancelled);
    assert_eq!(observed.delivery, DeliveryOutcome::Interrupted);
}

#[test]
fn pipe_result_only_fills_or_downgrades_weaker_facts() {
    let drained = (Some(ReaderTermination::Eof), Some(BodyOutcome::Drained));
    let cancelled = (
        Some(ReaderTermination::Cancelled),
        Some(BodyOutcome::Interrupted),
    );
    assert_eq!(
        facts(piped(TransportObservation::default(), false, false)),
        drained
    );
    assert_eq!(
        facts(piped(TransportObservation::default(), true, false)),
        cancelled
    );
    assert_eq!(
        facts(piped(recorded(SourceEnd::Eof, false, false), true, true)),
        cancelled
    );
    // Delivery is never inferred as delivered: a clean pipe stays unobserved.
    let clean = piped(recorded(SourceEnd::Eof, false, false), false, false);
    assert_eq!(clean.delivery, DeliveryOutcome::Unobserved);
}

#[test]
fn deadline_writes_the_source_failure_frame_and_client_abort_writes_none() -> TestResult {
    let chat = abort_frame(
        false,
        UpstreamProvider::Deepseek,
        ClientSurface::Chat,
        "resp_1",
        4,
    )
    .ok_or("deadline frame")?;
    assert_eq!(
        chat.encode_sse(),
        "data: {\"error\":{\"message\":\"Upstream Chat Completions stream failed.\",\"type\":\"server_error\",\"code\":\"deepseek_upstream_stream_error\",\"param\":null}}\n\n"
    );
    let responses = abort_frame(
        false,
        UpstreamProvider::Openrouter,
        ClientSurface::Responses,
        "resp_1",
        4,
    )
    .ok_or("deadline frame")?;
    assert_eq!(
        responses.encode_sse(),
        "event: response.failed\ndata: {\"type\":\"response.failed\",\"response\":{\"id\":\"resp_1\",\"object\":\"response\",\"status\":\"failed\",\"error\":{\"code\":\"openrouter_upstream_stream_error\",\"message\":\"Upstream Chat Completions stream failed.\"}},\"sequence_number\":4}\n\n"
    );
    assert!(
        abort_frame(
            true,
            UpstreamProvider::Lithos,
            ClientSurface::Chat,
            "resp_1",
            4
        )
        .is_none()
    );
    let lithos =
        stream_failure(UpstreamProvider::Lithos, ClientSurface::Chat, "resp_1", 0).encode_sse();
    assert!(lithos.contains("\"code\":\"lithos_upstream_stream_error\""));
    Ok(())
}

#[test]
fn deadline_outranks_source_end_and_client_abort_outranks_deadline() {
    let deadline = (ReaderTermination::Deadline, BodyOutcome::Interrupted);
    assert_eq!(end_facts(SourceEnd::Eof, false, true), deadline);
    assert_eq!(end_facts(SourceEnd::ReadError, false, true), deadline);
    let cancelled = (ReaderTermination::Cancelled, BodyOutcome::Interrupted);
    assert_eq!(end_facts(SourceEnd::Eof, true, true), cancelled);
    assert_eq!(
        end_facts(SourceEnd::ReadError, false, false),
        (ReaderTermination::ReadError, BodyOutcome::Failed)
    );
}
