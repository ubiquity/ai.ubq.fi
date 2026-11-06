use std::cell::{Cell, RefCell};
use std::future::{Future, poll_fn};
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context as PollContext, Poll};

use futures_util::Stream;
use futures_util::task::AtomicWaker;
use gateway_core::lifecycle::{
    BodyOutcome, DeliveryOutcome, ReaderTermination, SemanticTerminal, TransportObservation,
};
use worker::wasm_bindgen_futures::JsFuture;
use worker::{Context, Response, ResponseBody};

use super::outbound::{Cancellation, Deadline};

pub type FinishFuture = Pin<Box<dyn Future<Output = ()>>>;
pub type FinishHook = Box<dyn FnOnce(TransportObservation) -> FinishFuture>;

/// The one terminal hand-off from an outgoing body to its `waitUntil` finisher.
#[derive(Default)]
pub struct FinishSignal {
    observation: RefCell<Option<TransportObservation>>,
    waker: AtomicWaker,
    completed: Cell<bool>,
}

impl FinishSignal {
    /// Exactly one completion is published and woken; later completions are ignored.
    pub fn complete(&self, observation: TransportObservation) {
        if self.completed.replace(true) {
            return;
        }
        *self.observation.borrow_mut() = Some(observation);
        self.waker.wake();
    }
    pub fn completed(&self) -> bool {
        self.completed.get()
    }
    pub fn poll_take(&self, waker: &std::task::Waker) -> Poll<TransportObservation> {
        self.waker.register(waker);
        let taken = self.observation.borrow_mut().take();
        taken.map_or(Poll::Pending, Poll::Ready)
    }
}

/// A failed wrapper constructor has abandoned its body, not observed native delivery.
pub fn constructor_result<T, E>(
    result: std::result::Result<T, E>,
    signal: &Option<Rc<FinishSignal>>,
    observation: &ObservationHandle,
) -> std::result::Result<T, E> {
    if result.is_err()
        && let Some(signal) = signal
    {
        signal.complete(piped(observation.snapshot(), true, false));
    }
    result
}

/// How the outgoing body's own source ended.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SourceEnd {
    Eof,
    ReadError,
}

/// Rust-side facts: a client abort outranks the gateway deadline, which outranks the source end.
pub fn end_facts(end: SourceEnd, client: bool, deadline: bool) -> (ReaderTermination, BodyOutcome) {
    match (client, deadline, end) {
        (true, ..) => (ReaderTermination::Cancelled, BodyOutcome::Interrupted),
        (false, true, _) => (ReaderTermination::Deadline, BodyOutcome::Interrupted),
        (.., SourceEnd::Eof) => (ReaderTermination::Eof, BodyOutcome::Drained),
        (.., SourceEnd::ReadError) => (ReaderTermination::ReadError, BodyOutcome::Failed),
    }
}

/// Delivery is observed, never inferred: only the platform's client abort is evidence.
pub fn observed_delivery(client: bool) -> DeliveryOutcome {
    if client {
        DeliveryOutcome::Interrupted
    } else {
        DeliveryOutcome::Unobserved
    }
}

/// A native pipe result only fills a missing fact or downgrades a weaker one; it never
/// replaces a recorded deadline, read error or client abort.
pub fn piped(
    mut observed: TransportObservation,
    failed: bool,
    client: bool,
) -> TransportObservation {
    if failed {
        if matches!(
            observed.reader_termination,
            None | Some(ReaderTermination::Eof)
        ) {
            observed.reader_termination = Some(ReaderTermination::Cancelled);
        }
        if observed.body != Some(BodyOutcome::Failed) {
            observed.body = Some(BodyOutcome::Interrupted);
        }
    } else {
        observed
            .reader_termination
            .get_or_insert(ReaderTermination::Eof);
        observed.body.get_or_insert(BodyOutcome::Drained);
    }
    observed.delivery = observed_delivery(client);
    observed
}

#[derive(Clone, Default)]
pub struct ObservationHandle(Rc<RefCell<TransportObservation>>);

impl ObservationHandle {
    /// Only the actual provider decoder may publish its observed semantic event.
    pub fn semantic_terminal(&self, terminal: SemanticTerminal) {
        self.0.borrow_mut().semantic_terminal = Some(terminal);
    }

    pub fn snapshot(&self) -> TransportObservation {
        *self.0.borrow()
    }
}

/// One outgoing body with one finalization hook. It never clones/tees the body.
/// Finalization uses waitUntil but must leave durable pending state recoverable
/// when the platform's post-disconnect lifetime expires.
pub struct ObservedStream<S> {
    source: S,
    observation: ObservationHandle,
    context: Rc<Context>,
    cancellation: Rc<Cancellation>,
    _deadline: Option<Deadline>,
    finish: Option<Rc<FinishSignal>>,
    native_pipe: bool,
    deadline_frame: bool,
}

impl<S> ObservedStream<S> {
    pub fn new(
        source: S,
        observation: ObservationHandle,
        context: Rc<Context>,
        cancellation: Rc<Cancellation>,
        deadline: Option<Deadline>,
        finish: FinishHook,
    ) -> Self {
        let signal = Rc::new(FinishSignal::default());
        let receiver = signal.clone();
        // Register while the invocation is alive. Registering only after a
        // disconnect can be too late to retain the durable terminal writer.
        context.wait_until(async move {
            let observed = poll_fn(|context| receiver.poll_take(context.waker())).await;
            finish(observed).await;
        });
        Self {
            source,
            observation,
            context,
            cancellation,
            _deadline: deadline,
            finish: Some(signal),
            native_pipe: false,
            deadline_frame: false,
        }
    }

    /// Source `failStream`: after a gateway deadline the source still writes its declared
    /// failure frame and ends; a client abort still tears the body down with no frame.
    pub fn with_deadline_frame(mut self) -> Self {
        self.deadline_frame = true;
        self
    }

    fn finalize(&mut self, reader: ReaderTermination, body: BodyOutcome) {
        let Some(signal) = self.finish.take() else {
            return;
        };
        if signal.completed() {
            return;
        }
        let observed = {
            let mut state = self.observation.0.borrow_mut();
            state.reader_termination = Some(reader);
            state.body = Some(body);
            state.delivery = observed_delivery(self.cancellation.client_aborted());
            *state
        };
        if !self.native_pipe {
            signal.complete(observed);
        }
    }
}

impl<S> ObservedStream<S>
where
    S: Stream<Item = worker::Result<Vec<u8>>> + Unpin + 'static,
{
    pub fn into_response(mut self) -> worker::Result<Response> {
        self.native_pipe = true;
        let context = self.context.clone();
        let signal = self.finish.as_ref().cloned();
        let observation = self.observation.clone();
        let cancellation = self.cancellation.clone();
        let response = constructor_result(Response::from_stream(self), &signal, &observation)?;
        let (builder, body) = response.into_parts();
        let ResponseBody::Stream(source) = body else {
            return constructor_result(
                Err(worker::Error::RustError(
                    "Missing streaming response body".into(),
                )),
                &signal,
                &observation,
            );
        };
        let bridge = constructor_result(
            worker::web_sys::TransformStream::new()
                .map_err(|_| worker::Error::RustError("Native stream bridge unavailable".into())),
            &signal,
            &observation,
        )?;
        let pipe = source.pipe_to(&bridge.writable());
        // Observe native pipe cancellation independently of a pending Rust pull.
        // Pipe completion remains producer/body evidence, never delivery proof.
        context.wait_until(async move {
            let failed = JsFuture::from(pipe).await.is_err();
            if let Some(signal) = signal
                && !signal.completed()
            {
                if failed {
                    cancellation.abort();
                }
                let client = cancellation.client_aborted();
                signal.complete(piped(observation.snapshot(), failed, client));
            }
        });
        Ok(builder.stream(bridge.readable()))
    }
}

impl<S> Stream for ObservedStream<S>
where
    S: Stream<Item = worker::Result<Vec<u8>>> + Unpin,
{
    type Item = worker::Result<Vec<u8>>;

    fn poll_next(
        mut self: Pin<&mut Self>,
        context: &mut PollContext<'_>,
    ) -> Poll<Option<Self::Item>> {
        if self.finish.is_none() {
            return Poll::Ready(None);
        }
        self.cancellation.register_waker(context.waker());
        let client = self.cancellation.client_aborted();
        let deadline = !client && self.cancellation.signal().aborted();
        if client || (deadline && !self.deadline_frame) {
            let (reader, body) = end_facts(SourceEnd::Eof, client, deadline);
            self.finalize(reader, body);
            return Poll::Ready(Some(Err(worker::Error::RustError(
                "Upstream stream interrupted".to_owned(),
            ))));
        }
        let end = match Pin::new(&mut self.source).poll_next(context) {
            Poll::Ready(None) => SourceEnd::Eof,
            Poll::Ready(Some(Err(error))) => {
                let (reader, body) = end_facts(SourceEnd::ReadError, client, deadline);
                self.finalize(reader, body);
                return Poll::Ready(Some(Err(error)));
            }
            result => return result,
        };
        let (reader, body) = end_facts(end, client, deadline);
        self.finalize(reader, body);
        Poll::Ready(None)
    }
}

impl<S> Drop for ObservedStream<S> {
    fn drop(&mut self) {
        if self.finish.is_some() {
            self.cancellation.abort();
            self.finalize(ReaderTermination::Cancelled, BodyOutcome::Interrupted);
        }
    }
}
