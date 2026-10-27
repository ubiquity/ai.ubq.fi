//! Bind decoded streams to the original Worker request lifetime.
use super::{ResponseResult, dispatch_attempts};
use crate::inference_lifecycle::{Lifecycle, Usage, finish_hook};
use crate::routing_host;
use gateway_admission::handoff::{Pending, enqueue};
use gateway_core::{error::GatewayError, lifecycle::SemanticTerminal, provider::UpstreamProvider};
use gateway_providers::response::decode_response;
use gateway_providers::types::Headers;
use gateway_providers::types::{DecodePlan, ProviderEvent};
use gateway_routing_runtime::types::PreparedWireRequest;
use gateway_routing_runtime::types::{
    AttemptFailure, FailureOrigin, RefusalDecision, RoutingError, TransportState,
};
use gateway_worker_http_body::body;
use gateway_worker_http_body::protocol::{
    CEREBRAS_RATE_LIMIT_HEADERS, apply_warnings, cerebras_error_body, copy_headers, interrupted,
    invalid, provider_correlation, set_header, stream_failure,
};
use gateway_worker_platform::cancellation::Cancellation;
use gateway_worker_platform::cancellation::Deadline;
use gateway_worker_storage::store::now_ms;
use gateway_worker_stream::observed::{FinishHook, ObservationHandle, ObservedStream};
use gateway_worker_stream::relay::PrepareDisposition;
use gateway_worker_stream::relay::PreparedRelay;
use gateway_worker_stream::relay::TerminalObserver;
use std::{cell::RefCell, rc::Rc};
use worker::{Response, ResponseBody};

pub(super) struct PendingLifetime {
    pub(super) session: Rc<crate::inference_lifecycle::Session>,
    pub(super) currentness: crate::inference_currentness::CurrentnessGuard,
    pub(super) permit: Option<gateway_admission::Permit>,
    pub(super) deadline: Option<Deadline>,
}
pub(super) fn cleanup_error(
    original: gateway_routing_runtime::types::RoutingError,
    cleanup: Result<(), gateway_routing_runtime::types::RoutingError>,
) -> gateway_routing_runtime::types::RoutingError {
    match cleanup {
        Ok(()) => original,
        Err(error) => error,
    }
}
impl PendingLifetime {
    pub(super) async fn cleanup(
        self,
        error: gateway_routing_runtime::types::RoutingError,
        capture: Rc<crate::inference_capture::Capture>,
        cancellation: Rc<Cancellation>,
    ) -> ResponseResult {
        let error = cleanup_error(
            error,
            self.session.finish(Default::default()).await.map(|_| ()),
        );
        let result = capture_error(error, &capture, &cancellation).await;
        drop(self);
        result
    }
}

pub(super) struct Selected {
    pub(super) upstream: Response,
    pub(super) attempt: gateway_routing_runtime::types::PreparedAttempt,
    pub(super) headers: Headers,
    pub(super) plan: DecodePlan,
    pub(super) prepared: Option<PreparedRelay<Cancellation>>,
    pub(super) observation: ObservationHandle,
    pub(super) usage: Usage,
}
impl Selected {
    pub(super) fn new(
        upstream: Response,
        attempt: gateway_routing_runtime::types::PreparedAttempt,
    ) -> Self {
        let headers = upstream.headers().entries().collect::<Headers>();
        let plan = gateway_providers::response::response_plan(attempt.request.decode(), &headers);
        Self {
            upstream,
            attempt,
            headers,
            plan,
            prepared: None,
            observation: ObservationHandle::default(),
            usage: Rc::new(RefCell::new(None)),
        }
    }
    pub(super) fn needs_preparation(&self, session: &crate::inference_lifecycle::Session) -> bool {
        gateway_schema::aliases::is_synthetic(session.selected_model())
            && self.attempt.paid.is_none()
            && self.attempt.request.stream()
            && self.plan.surface == gateway_providers::types::ClientSurface::Responses
            && matches!(
                self.plan.provider,
                UpstreamProvider::Lithos
                    | UpstreamProvider::Deepseek
                    | UpstreamProvider::Openrouter
            )
            && self.upstream.status_code() < 400
            && gateway_providers::response::is_event_stream(
                self.plan.provider,
                self.upstream.status_code(),
                &self.headers,
            )
    }
}
pub(super) struct SelectedLifetime {
    pub(super) lifecycle: Rc<Lifecycle>,
    pub(super) permit: Option<gateway_admission::Permit>,
    pub(super) observation: ObservationHandle,
    pub(super) usage: Usage,
    pub(super) deadline: Option<Deadline>,
}
impl SelectedLifetime {
    async fn cleanup(
        self,
        error: RoutingError,
        capture: Rc<crate::inference_capture::Capture>,
        cancellation: Rc<Cancellation>,
    ) -> ResponseResult {
        let usage = self.usage.borrow().clone();
        let error = cleanup_error(
            error,
            self.lifecycle
                .finish(self.observation.snapshot(), usage)
                .await
                .map_err(Into::into),
        );
        let result = capture_error(error, &capture, &cancellation).await;
        drop(self);
        result
    }
}

pub(super) enum ReplyFinish {
    Ordinary(Option<FinishHook>),
    Selected(Pending<SelectedLifetime>),
}
impl ReplyFinish {
    pub(super) fn selected(
        value: SelectedLifetime,
        context: Rc<worker::Context>,
        capture: Rc<crate::inference_capture::Capture>,
        cancellation: Rc<Cancellation>,
    ) -> Self {
        let mut owner = Pending::new(value);
        owner.arm(move |value| {
            context.wait_until(async move {
                let _ = value
                    .cleanup(interrupted().into(), capture, cancellation)
                    .await;
            })
        });
        Self::Selected(owner)
    }
    fn take_hook(
        &mut self,
        capture: &Rc<crate::inference_capture::Capture>,
    ) -> Result<FinishHook, RoutingError> {
        match self {
            Self::Ordinary(hook) => hook
                .take()
                .ok_or_else(crate::inference_local_lifecycle::wrapper_error),
            Self::Selected(owner) => {
                let value = owner
                    .take()
                    .ok_or_else(crate::inference_local_lifecycle::wrapper_error)?;
                Ok(finish_hook(
                    value.lifecycle,
                    value.usage,
                    value.permit,
                    capture.clone(),
                ))
            }
        }
    }
    fn selected_armed(&self) -> bool {
        matches!(self, Self::Selected(owner) if owner.get().is_some())
    }
}

async fn capture_error(
    error: RoutingError,
    capture: &Rc<crate::inference_capture::Capture>,
    cancellation: &Cancellation,
) -> ResponseResult {
    let result = crate::inference_source::error_response(error, capture).await;
    if let Err(error) = &result {
        capture.failure(error, cancellation.client_aborted()).await;
    }
    result
}

pub(super) struct Reply<'a> {
    pub(super) finish: ReplyFinish,
    pub(super) usage: Usage,
    pub(super) observation: ObservationHandle,
    pub(super) lifecycle: Rc<Lifecycle>,
    pub(super) decode_plan: DecodePlan,
    pub(super) headers: Headers,
    pub(super) prepared: PreparedWireRequest,
    pub(super) upstream: Response,
    pub(super) deadline: Option<Deadline>,
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
    pub(super) async fn send(mut self) -> ResponseResult {
        match self.materialize().await {
            Err(error) if self.finish.selected_armed() => {
                let ReplyFinish::Selected(owner) = &mut self.finish else {
                    return crate::inference_source::error_response(error, self.capture).await;
                };
                let Some(value) = owner.take() else {
                    return crate::inference_source::error_response(
                        crate::inference_local_lifecycle::wrapper_error(),
                        self.capture,
                    )
                    .await;
                };
                enqueue(
                    value.cleanup(error, self.capture.clone(), self.host.cancellation.clone()),
                    |task| self.context.wait_until(task),
                )
                .await
            }
            Err(error) => crate::inference_source::error_response(error, self.capture).await,
            Ok(response) => Ok(response),
        }
    }
    async fn materialize(&mut self) -> Result<Response, RoutingError> {
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
    async fn lithos_refusal(&mut self) -> Result<Response, RoutingError> {
        let probe = body::refusal_probe(
            &mut self.upstream,
            &self.host.cancellation,
            body::REFUSAL_PROBE_TIMEOUT_MS,
        )
        .await;
        body::discard_response(&mut self.upstream);
        let decoded = decode_response(&self.decode_plan, self.status, &self.headers, &probe.bytes)?;
        if decoded.status != 200 || decoded.events.is_empty() {
            return Err(invalid("Lithos stream refusal could not be translated.").into());
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
        let stream = self.register(source, self.observation.clone(), true)?;
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
    async fn cerebras_refusal(&mut self) -> Result<Response, RoutingError> {
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
        let stream = self.register(source, observation, true)?;
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
    fn refusal(&mut self) -> Result<Response, RoutingError> {
        let observation = ObservationHandle::default();
        let upstream = std::mem::replace(
            &mut self.upstream,
            Response::builder().body(ResponseBody::Empty),
        );
        let (builder, raw_body) = upstream.into_parts();
        let source: std::pin::Pin<Box<dyn futures_util::Stream<Item = worker::Result<Vec<u8>>>>> =
            match raw_body {
                ResponseBody::Empty => Box::pin(futures_util::stream::empty()),
                ResponseBody::Body(bytes) => Box::pin(futures_util::stream::iter([Ok(bytes)])),
                ResponseBody::Stream(source) => Box::pin(worker::ByteStream::from(source)),
            };
        let stream = self.register(self.capture.client(source), observation, true)?;
        let (_, raw_body) = stream
            .into_response()
            .map_err(|_| invalid("The downstream refusal body is unavailable."))?
            .into_parts();
        let mut response = builder.body(raw_body);
        provider_correlation(&mut response, self.provider, &self.headers)?;
        apply_warnings(&mut response, self.prepared.warnings())?;
        Ok(response)
    }
    fn streaming(&mut self) -> Result<Response, RoutingError> {
        let source = self
            .upstream
            .stream()
            .map_err(|_| invalid("Upstream stream is unavailable."))?;
        let prepared = PreparedRelay::new(
            source,
            self.decode_plan.clone(),
            self.host.cancellation.clone(),
            self.take_deadline(),
            self.observation.clone(),
            self.usage.clone(),
        );
        self.streaming_ready(prepared)
    }

    pub(super) fn streaming_from(
        mut self,
        prepared: PreparedRelay<Cancellation>,
    ) -> Result<Response, RoutingError> {
        self.streaming_ready(prepared)
    }
    fn streaming_ready(
        &mut self,
        prepared: PreparedRelay<Cancellation>,
    ) -> Result<Response, RoutingError> {
        self.capture
            .configure(self.status, true, self.responses, self.provider);
        let relay = prepared.attach(self.lifecycle.clone());
        let source = Box::pin(futures_util::stream::unfold(
            relay,
            |mut state| async move {
                let item = state.pull().await;
                item.map(|bytes| (bytes, state))
            },
        ));
        let stream = self.register(self.capture.client(source), self.observation.clone(), false)?;
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

    async fn buffered(&mut self) -> Result<Response, RoutingError> {
        let bytes = match body::response_bytes(
            &mut self.upstream,
            body::INFERENCE_JSON_MAX_BYTES,
            &self.host.cancellation.signal(),
        )
        .await
        {
            Ok(bytes) => bytes,
            Err(error) => {
                if !self.finish.selected_armed() {
                    self.lifecycle.finish(Default::default(), None).await?;
                }
                return Err(error.into());
            }
        };
        let decoded = match decode_response(&self.decode_plan, self.status, &self.headers, &bytes) {
            Ok(decoded) => decoded,
            Err(error) => {
                if !self.finish.selected_armed() {
                    self.lifecycle.finish(Default::default(), None).await?;
                }
                return Err(error.into());
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
        if self.finish.selected_armed() {
            *self.usage.borrow_mut() = decoded.usage.clone();
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
        let stream = self.register(self.capture.client(source), self.observation.clone(), true)?;
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
    fn take_deadline(&mut self) -> Option<Deadline> {
        match &mut self.finish {
            ReplyFinish::Selected(owner) => owner.get_mut().and_then(|value| value.deadline.take()),
            ReplyFinish::Ordinary(_) => self.deadline.take(),
        }
    }
    fn register<S: futures_util::Stream<Item = worker::Result<Vec<u8>>> + Unpin + 'static>(
        &mut self,
        source: S,
        observation: ObservationHandle,
        include_deadline: bool,
    ) -> Result<ObservedStream<S, Cancellation>, RoutingError> {
        let deadline = if include_deadline {
            self.take_deadline()
        } else {
            None
        };
        let finish = self.finish.take_hook(self.capture)?;
        Ok(ObservedStream::new(
            source,
            observation,
            self.context.clone(),
            self.host.cancellation.clone(),
            deadline,
            finish,
        ))
    }
}

impl TerminalObserver for Lifecycle {
    fn observe_terminal(
        &self,
        observation: gateway_core::lifecycle::TransportObservation,
        usage: Option<serde_json::Value>,
    ) -> impl std::future::Future<Output = Result<(), GatewayError>> {
        Lifecycle::observe_terminal(self, observation, usage)
    }
    fn remember_native(&self, output: &[serde_json::Value], native: &serde_json::Value) {
        let _ = crate::claude_host::remember_native(
            &self.principal,
            &self.attempt,
            output,
            native,
            now_ms().unwrap_or(i64::MAX),
        );
    }
}

impl super::Inference<'_> {
    fn arm_pending(&self, owner: &mut Pending<PendingLifetime>) {
        if owner.armed() {
            return;
        }
        let (context, capture, cancellation) = (
            self.context.clone(),
            self.capture.clone(),
            self.host.cancellation.clone(),
        );
        owner.arm(move |lifetime| {
            context.wait_until(async move {
                let _ = lifetime
                    .cleanup(interrupted().into(), capture, cancellation)
                    .await;
            })
        });
    }

    pub(super) async fn select(
        &self,
        owner: &mut Pending<PendingLifetime>,
        routing: &routing_host::Host,
    ) -> Result<Selected, RoutingError> {
        let mut last_failure = None;
        loop {
            let lifetime = owner
                .get()
                .ok_or_else(crate::inference_local_lifecycle::wrapper_error)?;
            let dispatched = dispatch_attempts(
                lifetime.session.clone(),
                routing,
                self.host.cancellation.clone(),
                self.capture.clone(),
            )
            .await;
            let (upstream, attempt) = match dispatched {
                Ok(value) => value,
                Err(error) => {
                    if error.error.envelope.error.code.as_deref()
                        == Some("provider_routing_unavailable")
                        && let Some(failure) = last_failure
                    {
                        return Err(failure);
                    }
                    return Err(error);
                }
            };
            let mut selected = Selected::new(upstream, attempt);
            if !selected.needs_preparation(&lifetime.session) {
                return Ok(selected);
            }
            self.arm_pending(owner);
            let source = selected
                .upstream
                .stream()
                .map_err(|_| invalid("Upstream stream is unavailable."))?;
            // The existing predicate above checked the genuine synthetic/free-hop
            // session and actual SSE head. No translated refusal or request flag
            // can publish readiness; the real dispatch/admission already ran.
            if selected.upstream.status_code() == 200 {
                self.head.accepted_head();
            }
            let deadline = owner
                .get_mut()
                .ok_or_else(crate::inference_local_lifecycle::wrapper_error)?
                .deadline
                .take();
            let mut prepared = PreparedRelay::new(
                source,
                selected.plan.clone(),
                self.host.cancellation.clone(),
                deadline,
                selected.observation.clone(),
                selected.usage.clone(),
            );
            #[cfg(not(feature = "fixture-stream-drop"))]
            let disposition = prepared.prepare().await?;
            #[cfg(feature = "fixture-stream-drop")]
            let disposition = std::future::poll_fn(|context| {
                let result = {
                    let mut future = Box::pin(prepared.prepare());
                    std::future::Future::poll(future.as_mut(), context)
                };
                if result.is_pending()
                    && prepared.presemantic_prefix_observed()
                    && let Some(value) = owner.get()
                {
                    crate::stream_fixture::note_preparation(
                        &value.session.original_context().request_id.0,
                    );
                }
                result
            })
            .await?;
            if self.host.cancellation.signal().aborted() {
                return Err(interrupted().into());
            }
            match disposition {
                PrepareDisposition::Commit => {
                    selected.prepared = Some(prepared);
                    return Ok(selected);
                }
                PrepareDisposition::PresemanticFailure(_) => {
                    let lifetime = owner
                        .get_mut()
                        .ok_or_else(crate::inference_local_lifecycle::wrapper_error)?;
                    lifetime.deadline = prepared.into_deadline();
                    let failure = stream_failure(selected.plan.provider);
                    let decision = lifetime
                        .session
                        .refusal(
                            &selected.attempt,
                            AttemptFailure {
                                origin: FailureOrigin::Provider,
                                status: None,
                                headers: selected.headers,
                                body: Vec::new(),
                                body_complete: false,
                                failure:
                                    gateway_providers::policy::HopFailure::StreamBeforeSemantic,
                                transport: TransportState::Started,
                                semantic_committed: false,
                                cancelled: self.host.cancellation.signal().aborted(),
                            },
                        )
                        .await?;
                    if self.host.cancellation.signal().aborted() {
                        return Err(interrupted().into());
                    }
                    if decision != RefusalDecision::Continue {
                        return Err(failure.into());
                    }
                    last_failure = Some(failure.into());
                }
            }
        }
    }
}
