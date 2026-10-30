//! One decoder survives private preparation and the observed downstream body.
use crate::{
    control::{ATTEMPT_DEADLINE_MS, StreamControl},
    delay::RelayDelay,
    observed::ObservationHandle,
};
use futures_util::Stream;
use gateway_contracts::{
    lifecycle::SemanticTerminal, lifecycle::TransportObservation, provider::UpstreamProvider,
};
use gateway_provider_events::event::ProviderEvent;
use gateway_provider_protocol::{types::ClientSurface, types::DecodePlan};
use gateway_provider_stream::{sse::KEEPALIVE_INTERVAL_MS, sse::StreamDecoder};
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::{
    cell::RefCell,
    collections::VecDeque,
    future::{Future, poll_fn},
    pin::Pin,
    rc::Rc,
    task::{Context, Poll},
    time::Duration,
};

pub trait TerminalObserver: 'static {
    fn observe_terminal(
        &self,
        observation: TransportObservation,
        usage: Option<Value>,
    ) -> impl Future<Output = Result<(), GatewayError>>;
    fn remember_native(&self, output: &[Value], native: &Value) -> Result<bool, GatewayError>;
}

pub enum PrepareDisposition {
    Commit,
    PresemanticFailure(GatewayError),
}

struct Core<C: StreamControl> {
    source: Option<worker::ByteStream>,
    plan: DecodePlan,
    decoder: StreamDecoder,
    raw: VecDeque<ProviderEvent>,
    queue: VecDeque<Vec<u8>>,
    done: bool,
    observation: ObservationHandle,
    usage: Rc<RefCell<Option<Value>>>,
    deadline: Option<C::Deadline>,
    control: Rc<C>,
    terminal_pending: bool,
    terminal_output: Option<Vec<Value>>,
    native_replay: Option<Value>,
    error: Option<GatewayError>,
}

pub struct PreparedRelay<C: StreamControl> {
    core: Core<C>,
    next_sequence: u64,
    heartbeat: RelayDelay<worker::Delay>,
}

pub struct Relay<C: StreamControl, O: TerminalObserver> {
    core: Core<C>,
    observer: Rc<O>,
    next_sequence: u64,
    heartbeat: RelayDelay<worker::Delay>,
}

enum Input {
    Bytes(Vec<u8>),
    Eof,
    Heartbeat,
}

impl<C: StreamControl> PreparedRelay<C> {
    pub fn new(
        source: worker::ByteStream,
        plan: DecodePlan,
        control: Rc<C>,
        deadline: Option<C::Deadline>,
        observation: ObservationHandle,
        usage: Rc<RefCell<Option<Value>>>,
    ) -> Self {
        Self {
            core: Core {
                source: Some(source),
                decoder: StreamDecoder::new(plan.clone()),
                plan,
                raw: VecDeque::new(),
                queue: VecDeque::new(),
                done: false,
                observation,
                usage,
                deadline,
                control,
                terminal_pending: false,
                terminal_output: None,
                native_replay: None,
                error: None,
            },
            next_sequence: 0,
            heartbeat: heartbeat(),
        }
    }

    #[cfg(feature = "fixture-phase")]
    pub fn presemantic_prefix_observed(&self) -> bool {
        !self.core.raw.is_empty()
            && !self.core.decoder.semantic_committed()
            && self.core.observation.snapshot().semantic_terminal.is_none()
    }

    pub async fn prepare(&mut self) -> Result<PrepareDisposition, GatewayError> {
        loop {
            self.core.check_cancelled()?;
            let terminal = self.core.observation.snapshot().semantic_terminal;
            if self.core.decoder.semantic_committed()
                || matches!(
                    terminal,
                    Some(SemanticTerminal::Completed | SemanticTerminal::Incomplete)
                )
            {
                return Ok(PrepareDisposition::Commit);
            }
            if matches!(
                terminal,
                Some(SemanticTerminal::Failed | SemanticTerminal::Error)
            ) {
                return Ok(PrepareDisposition::PresemanticFailure(invalid(
                    "Upstream stream failed before semantic output.",
                )));
            }
            if let Err(error) = self
                .core
                .next(&mut self.next_sequence, &mut self.heartbeat)
                .await
            {
                self.core.check_cancelled()?;
                if self.core.decoder.semantic_committed() {
                    self.core.error = Some(error);
                    return Ok(PrepareDisposition::Commit);
                }
                return Ok(PrepareDisposition::PresemanticFailure(error));
            }
        }
    }

    pub fn attach<O: TerminalObserver>(self, observer: Rc<O>) -> Relay<C, O> {
        Relay {
            core: self.core,
            observer,
            next_sequence: self.next_sequence,
            heartbeat: self.heartbeat,
        }
    }

    pub fn into_deadline(mut self) -> Option<C::Deadline> {
        drop(self.core.source.take());
        self.core.deadline.take()
    }
}

impl<C: StreamControl> Core<C> {
    fn check_cancelled(&self) -> Result<(), GatewayError> {
        if self.control.signal().aborted() {
            Err(interrupted())
        } else {
            Ok(())
        }
    }

    fn poll_input(
        &mut self,
        timer: &mut RelayDelay<worker::Delay>,
        context: &mut Context<'_>,
    ) -> Poll<Result<Input, GatewayError>> {
        self.control.register_waker(context.waker());
        if let Err(error) = self.check_cancelled() {
            return Poll::Ready(Err(error));
        }
        let result = match self.source.as_mut() {
            Some(source) => match Pin::new(source).poll_next(context) {
                Poll::Ready(Some(Ok(bytes))) => Poll::Ready(Ok(Input::Bytes(bytes))),
                Poll::Ready(Some(Err(_))) => {
                    Poll::Ready(Err(invalid("Upstream Chat Completions stream failed.")))
                }
                Poll::Ready(None) => Poll::Ready(Ok(Input::Eof)),
                Poll::Pending => Pin::new(timer).poll(context).map(|()| Ok(Input::Heartbeat)),
            },
            None => Poll::Ready(Ok(Input::Eof)),
        };
        if let Err(error) = self.check_cancelled() {
            return Poll::Ready(Err(error));
        }
        result
    }

    async fn next(
        &mut self,
        sequence: &mut u64,
        timer: &mut RelayDelay<worker::Delay>,
    ) -> Result<bool, GatewayError> {
        let input = poll_fn(|context| self.poll_input(timer, context)).await?;
        let events = match input {
            Input::Bytes(bytes) => self.decoder.push(&bytes)?,
            Input::Eof => {
                self.done = true;
                self.decoder.finish()?
            }
            Input::Heartbeat => {
                *timer = heartbeat();
                return Ok(false);
            }
        };
        self.events(events, sequence)?;
        Ok(true)
    }

    fn events(
        &mut self,
        events: Vec<ProviderEvent>,
        sequence: &mut u64,
    ) -> Result<(), GatewayError> {
        if let Some(usage) = self.decoder.observed_usage() {
            *self.usage.borrow_mut() = Some(usage.clone());
        }
        for event in &events {
            if let Some(usage) = event.observed_usage() {
                *self.usage.borrow_mut() = Some(usage);
            }
        }
        self.terminal_output = events.iter().rev().find_map(|event| {
            event
                .field("response")
                .and_then(|response| serde_json::from_str::<Value>(response.as_str()).ok())
                .and_then(|response| response.get("output").and_then(Value::as_array).cloned())
        });
        self.native_replay = self.decoder.native_replay().cloned();
        if !events.is_empty() {
            match self.control.set_timeout_ms(ATTEMPT_DEADLINE_MS) {
                Ok(deadline) => self.deadline = Some(deadline),
                Err(error) => {
                    self.control.abort();
                    self.done = true;
                    return Err(error);
                }
            }
        }
        for event in events {
            *sequence = event
                .field("sequence_number")
                .and_then(|value| value.deserialize::<u64>().ok())
                .map_or(*sequence + 1, |number| number + 1);
            if let Some(terminal) = event.terminal() {
                self.observation.semantic_terminal(terminal);
            }
            self.raw.push_back(event);
        }
        if self.observation.snapshot().semantic_terminal.is_some() {
            self.terminal_pending = true;
            self.done = true;
        }
        Ok(())
    }

    fn encode(&mut self) {
        for event in self.raw.drain(..) {
            self.queue.push_back(event.encode_sse().into_bytes());
        }
    }

    fn failure_event(&self, sequence: u64) -> Result<ProviderEvent, GatewayError> {
        let code = match self.plan.provider {
            UpstreamProvider::Deepseek => "deepseek_upstream_stream_error",
            UpstreamProvider::Lithos => "lithos_upstream_stream_error",
            _ => "upstream_stream_error",
        };
        let error = json!({"code":code,"message":"Upstream Chat Completions stream failed."});
        if self.plan.surface == ClientSurface::Chat {
            ProviderEvent::chat(json!({"error":error}))
        } else {
            ProviderEvent::responses(
                json!({"type":"response.failed","sequence_number":sequence,"response":{"id":self.plan.response_id,"object":"response","status":"failed","error":error}}),
            )
        }
    }
}

impl<C: StreamControl, O: TerminalObserver> Relay<C, O> {
    async fn publish(&mut self) -> Result<(), GatewayError> {
        self.core.encode();
        if !self.core.terminal_pending {
            return Ok(());
        }
        let usage = self.core.usage.borrow().clone();
        if let Err(error) = self
            .observer
            .observe_terminal(self.core.observation.snapshot(), usage)
            .await
        {
            self.core.queue.clear();
            self.core.done = true;
            drop(self.core.source.take());
            return Err(error);
        }
        let remembered = if self.core.observation.snapshot().semantic_terminal
            == Some(SemanticTerminal::Completed)
            && self.core.plan.provider == UpstreamProvider::ClaudeCode
            && let (Some(output), Some(native)) = (
                self.core.terminal_output.take(),
                self.core.native_replay.take(),
            ) {
            self.observer.remember_native(&output, &native)
        } else {
            Ok(false)
        };
        self.core.terminal_pending = false;
        drop(self.core.source.take());
        if remembered.is_err() {
            self.core.queue.clear();
            self.core.done = true;
        }
        remembered.map(|_| ())
    }

    async fn failed(&mut self) -> Option<worker::Result<Vec<u8>>> {
        self.core.done = true;
        if self.core.control.client_aborted() {
            drop(self.core.source.take());
            return None;
        }
        let event = self.core.failure_event(self.next_sequence);
        self.core
            .observation
            .semantic_terminal(if self.core.plan.surface == ClientSurface::Chat {
                SemanticTerminal::Error
            } else {
                SemanticTerminal::Failed
            });
        let usage = self.core.usage.borrow().clone();
        let observed = self
            .observer
            .observe_terminal(self.core.observation.snapshot(), usage)
            .await;
        drop(self.core.source.take());
        Some(
            observed
                .and_then(|()| event.map(|event| event.encode_sse().into_bytes()))
                .map_err(|error| worker::Error::RustError(error.envelope.error.message)),
        )
    }

    pub async fn pull(&mut self) -> Option<worker::Result<Vec<u8>>> {
        loop {
            if let Err(error) = self.publish().await {
                return Some(Err(worker::Error::RustError(error.envelope.error.message)));
            }
            if let Some(bytes) = self.core.queue.pop_front() {
                return Some(Ok(bytes));
            }
            if self.core.error.take().is_some() {
                return self.failed().await;
            }
            if self.core.done {
                return None;
            }
            match self
                .core
                .next(&mut self.next_sequence, &mut self.heartbeat)
                .await
            {
                Ok(false) => return Some(Ok(b": keep-alive\n\n".to_vec())),
                Ok(true) => {}
                Err(_) => return self.failed().await,
            }
        }
    }
}

fn heartbeat() -> RelayDelay<worker::Delay> {
    RelayDelay::new(worker::Delay::from(Duration::from_millis(
        KEEPALIVE_INTERVAL_MS,
    )))
}
fn invalid(message: &str) -> GatewayError {
    GatewayError::new(502, message, Some("upstream_transport_error"))
}
fn interrupted() -> GatewayError {
    GatewayError::new(
        502,
        "The upstream request was interrupted.",
        Some("upstream_request_interrupted"),
    )
    .with_type("server_error")
}
