//! Existing stream polling, decoding and terminal observation, without routing decisions.
use super::{
    ATTEMPT_DEADLINE_MS, CEREBRAS_RATE_LIMIT_HEADERS, ResponseResult, apply_warnings,
    cerebras_error_body, copy_headers, invalid, observe_event, provider_correlation, set_header,
};
use crate::storage::now_ms;
use crate::transport::stream::FinishHook;
use crate::transport::{body, stream::ObservedStream};
use crate::{
    inference_lifecycle::{Lifecycle, Usage},
    transport::{
        outbound::{Cancellation, Deadline},
        relay_delay::RelayDelay,
        stream::ObservationHandle,
    },
};
use futures_util::StreamExt;
use gateway_core::{error::GatewayError, lifecycle::SemanticTerminal, provider::UpstreamProvider};
use gateway_providers::response::decode_response;
use gateway_providers::types::Headers;
use gateway_providers::{
    sse::StreamDecoder,
    types::{ClientSurface, DecodePlan, ProviderEvent},
};
use gateway_routing_runtime::types::PreparedWireRequest;
use serde_json::json;
use std::{collections::VecDeque, rc::Rc};
use worker::{Response, ResponseBody};
pub(super) struct Relay {
    pub(super) source: worker::ByteStream,
    pub(super) plan: DecodePlan,
    pub(super) decoder: StreamDecoder,
    pub(super) queue: VecDeque<Vec<u8>>,
    pub(super) done: bool,
    pub(super) observation: ObservationHandle,
    pub(super) usage: Usage,
    pub(super) deadline: Option<Deadline>,
    pub(super) cancellation: Rc<Cancellation>,
    pub(super) lifecycle: Rc<Lifecycle>,
    pub(super) next_sequence: u64,
    pub(super) heartbeat: RelayDelay<worker::Delay>,
}

impl Relay {
    fn failure_event(&self) -> ProviderEvent {
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
                json!({"type":"response.failed","sequence_number":self.next_sequence,"response":{"id":self.plan.response_id,"object":"response","status":"failed","error":error}}),
            )
        }
    }

    async fn events(&mut self, events: Vec<ProviderEvent>) -> Result<(), GatewayError> {
        if let Some(usage) = self.decoder.observed_usage() {
            *self.usage.borrow_mut() = Some(usage.clone());
        }
        for event in &events {
            if let Some(usage) = event.observed_usage() {
                *self.usage.borrow_mut() = Some(usage);
            }
        }
        let terminal_output = events.iter().rev().find_map(|event| {
            event
                .field("response")
                .and_then(|response| {
                    serde_json::from_str::<serde_json::Value>(response.as_str()).ok()
                })
                .and_then(|response| {
                    response
                        .get("output")
                        .and_then(serde_json::Value::as_array)
                        .cloned()
                })
        });
        let native_replay = self.decoder.native_replay().cloned();
        if !events.is_empty() {
            match self.cancellation.set_timeout_ms(ATTEMPT_DEADLINE_MS) {
                Ok(deadline) => self.deadline = Some(deadline),
                Err(error) => {
                    self.cancellation.abort();
                    self.done = true;
                    return Err(error);
                }
            }
        }
        for event in events {
            self.next_sequence = event
                .field("sequence_number")
                .and_then(|value| value.deserialize::<u64>().ok())
                .map_or(self.next_sequence + 1, |number| number + 1);
            observe_event(&self.observation, &event);
            self.queue.push_back(event.encode_sse().into_bytes());
        }
        let terminal_usage = self.usage.borrow().clone();
        let terminal = self.observation.snapshot().semantic_terminal;
        if terminal.is_some()
            && let Err(error) = self
                .lifecycle
                .observe_terminal(self.observation.snapshot(), terminal_usage)
                .await
        {
            self.done = true;
            self.queue.clear();
            return Err(error);
        }
        if terminal == Some(SemanticTerminal::Completed)
            && self.plan.provider == UpstreamProvider::ClaudeCode
            && let (Some(output), Some(native)) = (terminal_output, native_replay)
        {
            let _ = crate::claude_host::remember_native(
                &self.lifecycle.principal,
                &self.lifecycle.attempt,
                &output,
                &native,
                crate::storage::now_ms().unwrap_or(i64::MAX),
            );
        }
        if terminal.is_some() {
            self.done = true;
        }
        Ok(())
    }

    async fn failed(&mut self) -> Option<worker::Result<Vec<u8>>> {
        self.done = true;
        if self.cancellation.client_aborted() {
            return None;
        }
        // Source stream-relay.failStream emits a declared
        // failure terminal for read/truncation errors.
        // A Rust stream exception alone tears down HTTP.
        let event = self.failure_event();
        self.observation
            .semantic_terminal(if self.plan.surface == ClientSurface::Chat {
                SemanticTerminal::Error
            } else {
                SemanticTerminal::Failed
            });
        let terminal_usage = self.usage.borrow().clone();
        if let Err(error) = self
            .lifecycle
            .observe_terminal(self.observation.snapshot(), terminal_usage)
            .await
        {
            return Some(Err(worker::Error::RustError(error.envelope.error.message)));
        }
        Some(Ok(event.encode_sse().into_bytes()))
    }

    pub(super) async fn pull(&mut self) -> Option<worker::Result<Vec<u8>>> {
        loop {
            if let Some(bytes) = self.queue.pop_front() {
                return Some(Ok(bytes));
            }
            if self.done {
                return None;
            }
            let incoming = {
                match futures_util::future::select(
                    Box::pin(self.source.next()),
                    std::pin::Pin::new(&mut self.heartbeat),
                )
                .await
                {
                    futures_util::future::Either::Left((incoming, _remaining)) => Some(incoming),
                    futures_util::future::Either::Right(((), remaining)) => {
                        drop(remaining);
                        None
                    }
                }
            };
            let Some(incoming) = incoming else {
                self.heartbeat = RelayDelay::new(worker::Delay::from(
                    std::time::Duration::from_millis(gateway_providers::sse::KEEPALIVE_INTERVAL_MS),
                ));
                return Some(Ok(b": keep-alive\n\n".to_vec()));
            };
            let result = match incoming {
                Some(Ok(bytes)) => self.decoder.push(&bytes),
                Some(Err(_)) => Err(invalid("Upstream Chat Completions stream failed.")),
                None => {
                    self.done = true;
                    self.decoder.finish()
                }
            };
            match result {
                Ok(events) => {
                    if let Err(error) = self.events(events).await {
                        return Some(Err(worker::Error::RustError(error.envelope.error.message)));
                    }
                }
                Err(_) => return self.failed().await,
            }
        }
    }
}

pub(super) struct Reply<'a> {
    pub(super) finish: FinishHook,
    pub(super) usage: Usage,
    pub(super) observation: ObservationHandle,
    pub(super) lifecycle: Rc<Lifecycle>,
    pub(super) decode_plan: DecodePlan,
    pub(super) headers: Headers,
    pub(super) prepared: PreparedWireRequest,
    pub(super) upstream: Response,
    pub(super) deadline: Deadline,
    pub(super) context: Rc<worker::Context>,
    pub(super) host: &'a crate::host::PlatformHost,
    pub(super) capture: &'a Rc<crate::inference_capture::Capture>,
    pub(super) responses: bool,
    pub(super) principal: &'a gateway_auth::types::AuthPrincipal,
    pub(super) client_stream: bool,
    pub(super) event_stream: bool,
    pub(super) status: u16,
    pub(super) provider: UpstreamProvider,
}
impl Reply<'_> {
    pub(super) async fn send(self) -> ResponseResult {
        if self.status >= 400 {
            self.capture.configure(
                self.status,
                self.event_stream,
                self.responses,
                self.provider,
            );
            if gateway_providers::sse::is_lithos_stream_refusal(&self.decode_plan, self.status) {
                return self.lithos_refusal().await;
            }
            if self.provider == UpstreamProvider::Cerebras {
                return self.cerebras_refusal().await;
            }
            return self.refusal();
        }
        if self.client_stream && self.event_stream && self.status < 400 {
            return self.streaming();
        }
        self.buffered().await
    }
    async fn lithos_refusal(mut self) -> ResponseResult {
        let probe = body::refusal_probe(
            &mut self.upstream,
            &self.host.cancellation,
            body::REFUSAL_PROBE_TIMEOUT_MS,
        )
        .await;
        body::discard_response(&mut self.upstream);
        let decoded = decode_response(&self.decode_plan, self.status, &self.headers, &probe.bytes)?;
        if decoded.status != 200 || decoded.events.is_empty() {
            return Err(invalid("Lithos stream refusal could not be translated."));
        }
        self.observation
            .semantic_terminal(decoded.terminal.unwrap_or(SemanticTerminal::Failed));
        self.capture
            .configure(200, true, self.responses, self.provider);
        let payload = decoded
            .events
            .iter()
            .map(ProviderEvent::encode_sse)
            .collect::<String>()
            .into_bytes();
        let source = futures_util::stream::iter(vec![Ok(payload)]);
        let stream = ObservedStream::new(
            source,
            self.observation.clone(),
            self.context,
            self.host.cancellation.clone(),
            Some(self.deadline),
            self.finish,
        );
        let (_, raw_body) = stream
            .into_response()
            .map_err(|_| invalid("The downstream refusal body is unavailable."))?
            .into_parts();
        let mut response = Response::builder().with_status(200).body(raw_body);
        for (name, value) in decoded.headers {
            set_header(&mut response, &name, &value)?;
        }
        Ok(response)
    }
    async fn cerebras_refusal(mut self) -> ResponseResult {
        let probe = body::refusal_probe(
            &mut self.upstream,
            &self.host.cancellation,
            body::REFUSAL_PROBE_TIMEOUT_MS,
        )
        .await;
        body::discard_response(&mut self.upstream);
        let bytes = cerebras_error_body(self.status, &probe.bytes);
        let observation = ObservationHandle::default();
        let source = futures_util::stream::iter(vec![Ok(bytes)]);
        let stream = ObservedStream::new(
            source,
            observation,
            self.context,
            self.host.cancellation.clone(),
            Some(self.deadline),
            self.finish,
        );
        let (_, raw_body) = stream
            .into_response()
            .map_err(|_| invalid("The downstream refusal body is unavailable."))?
            .into_parts();
        let mut response = Response::builder().with_status(self.status).body(raw_body);
        set_header(&mut response, "Content-Type", "application/json")?;
        set_header(&mut response, "x-uos-upstream", "cerebras")?;
        if let Some(retry) = self
            .headers
            .iter()
            .find(|(name, _)| name.eq_ignore_ascii_case("retry-after"))
            .map(|(_, value)| value)
        {
            set_header(&mut response, "Retry-After", retry)?;
        }
        if self.status == 429 {
            for name in CEREBRAS_RATE_LIMIT_HEADERS {
                if let Some((_, value)) = self
                    .headers
                    .iter()
                    .find(|(header, _)| header.eq_ignore_ascii_case(name))
                {
                    set_header(&mut response, name, value)?;
                }
            }
        }
        provider_correlation(&mut response, self.provider, &self.headers)?;
        Ok(response)
    }
    fn refusal(self) -> ResponseResult {
        let observation = ObservationHandle::default();
        let (builder, raw_body) = self.upstream.into_parts();
        let source: std::pin::Pin<Box<dyn futures_util::Stream<Item = worker::Result<Vec<u8>>>>> =
            match raw_body {
                ResponseBody::Empty => Box::pin(futures_util::stream::empty()),
                ResponseBody::Body(bytes) => Box::pin(futures_util::stream::iter([Ok(bytes)])),
                ResponseBody::Stream(source) => Box::pin(worker::ByteStream::from(source)),
            };
        let stream = ObservedStream::new(
            self.capture.client(source),
            observation,
            self.context,
            self.host.cancellation.clone(),
            Some(self.deadline),
            self.finish,
        );
        let (_, raw_body) = stream
            .into_response()
            .map_err(|_| invalid("The downstream refusal body is unavailable."))?
            .into_parts();
        let mut response = builder.body(raw_body);
        provider_correlation(&mut response, self.provider, &self.headers)?;
        apply_warnings(&mut response, self.prepared.warnings())?;
        Ok(response)
    }
    fn streaming(mut self) -> ResponseResult {
        self.capture
            .configure(self.status, true, self.responses, self.provider);
        let source = self
            .upstream
            .stream()
            .map_err(|_| invalid("Upstream stream is unavailable."))?;
        let relay = Relay {
            source,
            plan: self.decode_plan.clone(),
            decoder: StreamDecoder::new(self.decode_plan.clone()),
            queue: VecDeque::new(),
            done: false,
            observation: self.observation.clone(),
            usage: self.usage,
            deadline: Some(self.deadline),
            cancellation: self.host.cancellation.clone(),
            lifecycle: self.lifecycle.clone(),
            next_sequence: 0,
            heartbeat: RelayDelay::new(worker::Delay::from(std::time::Duration::from_millis(
                gateway_providers::sse::KEEPALIVE_INTERVAL_MS,
            ))),
        };
        let source = Box::pin(futures_util::stream::unfold(
            relay,
            |mut state| async move {
                let item = state.pull().await;
                item.map(|bytes| (bytes, state))
            },
        ));
        let stream = ObservedStream::new(
            self.capture.client(source),
            self.observation,
            self.context,
            self.host.cancellation.clone(),
            None,
            self.finish,
        );
        let mut response = stream
            .into_response()
            .map_err(|_| invalid("The downstream stream is unavailable."))?
            .with_status(self.status);
        set_header(&mut response, "Content-Type", "text/event-stream")?;
        copy_headers(&mut response, &self.headers)?;
        set_header(
            &mut response,
            "x-uos-upstream",
            gateway_providers::policy::direct_provider_label(self.provider),
        )?;
        provider_correlation(&mut response, self.provider, &self.headers)?;
        apply_warnings(&mut response, self.prepared.warnings())?;
        Ok(response)
    }
    async fn buffered(mut self) -> ResponseResult {
        let bytes = match body::response_bytes(
            &mut self.upstream,
            body::INFERENCE_JSON_MAX_BYTES,
            &self.host.cancellation.signal(),
        )
        .await
        {
            Ok(bytes) => bytes,
            Err(error) => {
                self.lifecycle.finish(Default::default(), None).await?;
                return Err(error);
            }
        };
        let decoded = match decode_response(&self.decode_plan, self.status, &self.headers, &bytes) {
            Ok(decoded) => decoded,
            Err(error) => {
                self.lifecycle.finish(Default::default(), None).await?;
                return Err(error);
            }
        };
        if let Some(terminal) = decoded.terminal {
            self.observation.semantic_terminal(terminal);
        }
        if decoded.terminal == Some(SemanticTerminal::Completed)
            && self.provider == UpstreamProvider::ClaudeCode
            && let Some(native) = decoded.native_replay.as_ref()
            && let Some(output) = serde_json::from_str::<serde_json::Value>(decoded.body.as_str())
                .ok()
                .and_then(|body| {
                    body.get("output")
                        .and_then(serde_json::Value::as_array)
                        .cloned()
                })
        {
            let _ = crate::claude_host::remember_native(
                self.principal,
                &self.lifecycle.attempt,
                &output,
                native,
                now_ms().unwrap_or(i64::MAX),
            );
        }
        if self.observation.snapshot().semantic_terminal.is_some() {
            self.lifecycle
                .observe_terminal(self.observation.snapshot(), decoded.usage.clone())
                .await?;
        }
        *self.usage.borrow_mut() = decoded.usage;
        let payload = if self.client_stream && !decoded.events.is_empty() {
            decoded
                .events
                .iter()
                .map(ProviderEvent::encode_sse)
                .collect::<String>()
                .into_bytes()
        } else if let Some(raw) = decoded.raw_body {
            raw
        } else {
            decoded.body.as_bytes().to_vec()
        };
        self.capture.configure(
            decoded.status,
            self.client_stream && !decoded.events.is_empty(),
            self.responses,
            self.provider,
        );
        let source = futures_util::stream::iter(vec![Ok(payload)]);
        let stream = ObservedStream::new(
            self.capture.client(source),
            self.observation,
            self.context,
            self.host.cancellation.clone(),
            Some(self.deadline),
            self.finish,
        );
        let mut response = stream
            .into_response()
            .map_err(|_| invalid("The downstream body is unavailable."))?
            .with_status(decoded.status);
        for (name, value) in decoded.headers {
            set_header(&mut response, &name, &value)?;
        }
        provider_correlation(&mut response, self.provider, &self.headers)?;
        apply_warnings(&mut response, self.prepared.warnings())?;
        Ok(response)
    }
}
