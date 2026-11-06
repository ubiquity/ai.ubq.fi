//! Actual schema -> admission -> provider -> terminal transport path.
use crate::{
    authority::RemoteAuthorityStore,
    catalog::CatalogState,
    host::{PlatformClock, PlatformHost, PlatformTokens},
    inference_lifecycle::{Lifecycle, Session, finish_hook},
    routing_host,
    storage::now_ms,
    transport::{
        body,
        outbound::{
            Cancellation, DispatchAttempt, DispatchFailure, DispatchOrigin, PinnedDestination,
            send_constructed, send_http,
        },
        stream::ObservationHandle,
    },
};
use crate::{
    inference_capture::Capture,
    inference_currentness::CurrentnessGuard,
    inference_early::Gate,
    inference_local::Start,
    inference_source::{NativeTurn, error_response},
};
use gateway_auth::types::{AuthMethod, AuthPrincipal};
use gateway_core::{
    claude::ClaudeErrorEvidence,
    error::GatewayError,
    identity::{ApiKeyId, PrincipalId, RequestContext, RequestId},
    model::ModelId,
    provider::UpstreamProvider,
};
use gateway_observability::replay_trace::{AttemptTrace, TraceTerminal};
use gateway_providers::types::{Headers, ProviderEvent};
use gateway_routing_runtime::{
    local_work::LocalRequestMetadata,
    session::RoutingSession,
    types::{
        AttemptFailure, FailureOrigin, PreparedAttempt, PreparedWireRequest, RefusalDecision,
        ResponseDecision, ResponseHead, RoutingError, RoutingInput, TransportState,
        WorkspaceContinuation,
    },
};
use gateway_schema::{
    request::RequestSurface,
    source_request::{SourceRequestDocument, parse_http_body},
};
use std::{cell::RefCell, rc::Rc};
use worker::{Context, Method, Request, Response};

mod stream;

const ATTEMPT_DEADLINE_MS: i32 = 1_800_000;
type ResponseResult = Result<Response, GatewayError>;

pub struct RequestResources {
    pub context: Rc<Context>,
    pub permit: Option<gateway_admission::Permit>,
}

struct Dispatch<'a> {
    session: &'a Rc<Session>,
    routing: &'a routing_host::Host,
    cancellation: &'a Rc<Cancellation>,
    capture: &'a Rc<Capture>,
    early: Option<Rc<Gate>>,
}
impl Dispatch<'_> {
    async fn send(
        &self,
        attempt: &PreparedAttempt,
        continuation: Option<WorkspaceContinuation>,
        target: &PinnedDestination,
        wire: &[u8],
    ) -> Result<Option<(Response, i64)>, RoutingError> {
        let continuing = continuation.is_some();
        let gate_session = self.session.clone();
        let gate_attempt = attempt.clone();
        let gate_cancel = self.cancellation.clone();
        let gate_capture = self.capture.clone();
        let gate_early = self.early.clone();
        let passive_trace = Rc::new(RefCell::new(None));
        let gate_trace = passive_trace.clone();
        // Claude actual body exists before any same-binding material/host constructor.
        let physical = match &attempt.request {
            PreparedWireRequest::Claude(_) => Some(self.routing.physical_claude(attempt)?),
            PreparedWireRequest::Ready(_) => None,
        };
        let before_dispatch = || async move {
            if let Some(continuation) = continuation {
                gate_session
                    .before_workspace_dispatch(continuation)
                    .await
                    .map_err(|error| *error.error)?;
            } else {
                gate_session
                    .before_dispatch(&gate_attempt)
                    .await
                    .map_err(|error| *error.error)?;
            }
            // No await follows this real dispatch gate before the actual fetch.
            if !gate_cancel.signal().aborted() {
                *gate_trace.borrow_mut() = gate_capture.start(gate_attempt.request.provider());
            }
            if let Some(gate) = gate_early {
                gate.commit(gate_cancel.signal().aborted());
            }
            Ok(())
        };
        let result = match &attempt.request {
            PreparedWireRequest::Ready(request) => {
                send_http(
                    target,
                    Method::Post,
                    &request.headers,
                    wire,
                    self.cancellation,
                    before_dispatch,
                )
                .await
            }
            PreparedWireRequest::Claude(_) => {
                send_constructed(
                    physical.ok_or_else(crate::inference_local_lifecycle::wrapper_error)?,
                    self.cancellation,
                    before_dispatch,
                )
                .await
            }
        };

        let trace = passive_trace.borrow_mut().take();
        match result {
            Ok(response) => {
                let observed_at_ms = now_ms()?;
                let response = crate::inference_capture::upstream(
                    response,
                    trace,
                    self.cancellation.clone(),
                    observed_at_ms,
                )?;
                Ok(Some((response, observed_at_ms)))
            }
            Err(failure) => self.failed(attempt, continuing, trace, failure).await,
        }
    }
    async fn failed(
        &self,
        attempt: &PreparedAttempt,
        continuing: bool,
        trace: Option<AttemptTrace>,
        failure: DispatchFailure,
    ) -> Result<Option<(Response, i64)>, RoutingError> {
        if let Some(trace) = trace
            && let Ok(at) = now_ms()
        {
            if self.cancellation.signal().aborted() {
                trace.finish(TraceTerminal::Cancelled, at);
            } else {
                trace.fetch_error(at);
            }
        }
        let decision = self
            .session
            .refusal(
                attempt,
                AttemptFailure {
                    origin: if failure.origin == DispatchOrigin::Gateway {
                        FailureOrigin::Gateway
                    } else if self.cancellation.signal().aborted()
                        && !self.cancellation.client_aborted()
                    {
                        FailureOrigin::GatewayDeadline
                    } else {
                        FailureOrigin::Transport
                    },
                    status: None,
                    headers: Default::default(),
                    body: Vec::new(),
                    body_complete: false,
                    failure: if self.cancellation.client_aborted() {
                        gateway_providers::policy::HopFailure::Cancelled
                    } else if self.cancellation.signal().aborted() {
                        gateway_providers::policy::HopFailure::Deadline
                    } else {
                        gateway_providers::policy::HopFailure::Transport
                    },
                    transport: if continuing || failure.attempt == DispatchAttempt::Started {
                        TransportState::Started
                    } else {
                        TransportState::NotStarted
                    },
                    semantic_committed: false,
                    cancelled: self.cancellation.signal().aborted(),
                },
            )
            .await?;
        if decision == RefusalDecision::Continue {
            return Ok(None);
        }
        Err(failure.error.into())
    }
    async fn resolve(
        &self,
        attempt: &PreparedAttempt,
        response: &mut Response,
        observed_at_ms: i64,
        continuing: bool,
    ) -> Result<ResponseDecision, RoutingError> {
        // Header time was captured before the first observer or ledger await.
        let status = response.status_code();
        let headers = response.headers().entries().collect::<Headers>();
        if !continuing {
            self.session.transport_started(attempt).await?;
        }
        let observed = self
            .session
            .observe_response(
                attempt,
                ResponseHead {
                    status,
                    headers: headers.clone(),
                    provider_request_id: crate::inference_wire::provider_request_id(
                        attempt.request.provider(),
                        &headers,
                    )
                    .map(str::to_owned),
                    observed_at_ms,
                },
            )
            .await?;
        if status < 400 {
            self.session
                .resolve_response(
                    attempt,
                    observed,
                    ClaudeErrorEvidence::from_prefix(None, false)?,
                    None,
                )
                .await?;
            return Ok(ResponseDecision::Return);
        }
        let (capture, evidence) = if attempt.request.provider() == UpstreamProvider::ClaudeCode {
            let captured =
                crate::transport::claude_body::capture(response, self.cancellation).await;
            (
                body::ResponseCapture::default(),
                ClaudeErrorEvidence::from_prefix(captured.prefix, captured.complete)?,
            )
        } else {
            let capture = match (attempt.request.provider(), status) {
                (UpstreamProvider::ChatgptCodex, 400) => {
                    body::refusal_probe(response, self.cancellation, 250).await
                }
                (UpstreamProvider::ChatgptCodex, 429)
                | (UpstreamProvider::Metered, 403)
                | (UpstreamProvider::Surplus, 402) => {
                    body::refusal_probe(response, self.cancellation, body::REFUSAL_PROBE_TIMEOUT_MS)
                        .await
                }
                _ => body::ResponseCapture::default(),
            };
            (capture, ClaudeErrorEvidence::from_prefix(None, false)?)
        };
        let decision = self
            .session
            .resolve_response(
                attempt,
                observed,
                evidence,
                Some(AttemptFailure {
                    origin: FailureOrigin::Provider,
                    status: Some(status),
                    headers,
                    body: capture.bytes,
                    body_complete: capture.complete,
                    failure: gateway_providers::policy::HopFailure::Status(status),
                    transport: TransportState::Started,
                    semantic_committed: false,
                    cancelled: self.cancellation.signal().aborted(),
                }),
            )
            .await;
        if !matches!(&decision, Ok(ResponseDecision::Return)) {
            body::discard_response(response);
        }
        decision
    }
}
async fn dispatch_attempts(
    session: Rc<Session>,
    routing: routing_host::Host,
    cancellation: Rc<Cancellation>,
    capture: Rc<Capture>,
    early: Option<Rc<Gate>>,
) -> Result<(Response, PreparedAttempt), RoutingError> {
    let dispatch = Dispatch {
        session: &session,
        routing: &routing,
        cancellation: &cancellation,
        capture: &capture,
        early,
    };
    let mut pending: Option<(WorkspaceContinuation, Vec<u8>)> = None;
    loop {
        let (attempt, continuation, retained_wire) =
            if let Some((continuation, wire)) = pending.take() {
                (
                    continuation.attempt().clone(),
                    Some(continuation),
                    Some(wire),
                )
            } else {
                let attempt = session.next_attempt().await?.ok_or_else(|| {
                    GatewayError::unavailable(
                        "No eligible provider remains for this request.",
                        "provider_routing_unavailable",
                    )
                })?;
                (attempt, None, None)
            };
        let continuing = continuation.is_some();
        let target = PinnedDestination::new(attempt.request.url())?;
        let wire = match retained_wire {
            Some(wire) => wire,
            None => attempt.request.body().as_bytes().to_vec(),
        };
        let Some((mut response, observed_at_ms)) = dispatch
            .send(&attempt, continuation, &target, &wire)
            .await?
        else {
            continue;
        };
        match dispatch
            .resolve(&attempt, &mut response, observed_at_ms, continuing)
            .await?
        {
            ResponseDecision::Return => {
                return Ok((response, attempt));
            }
            ResponseDecision::Continue => {}
            ResponseDecision::WorkspaceContinuation(continuation) => {
                pending = Some((*continuation, wire));
            }
        }
    }
}

pub async fn run(
    mut request: Request,
    resources: RequestResources,
    principal: &AuthPrincipal,
    store: RemoteAuthorityStore,
    catalog: CatalogState,
    host: &PlatformHost,
    request_id: RequestId,
) -> Result<Response, GatewayError> {
    let RequestResources { context, permit } = resources;
    let responses = crate::normalized_path(&request.path()) == "/v1/responses";
    let bytes = body::inference_bytes(&mut request).await?;
    let client_version = request
        .headers()
        .get("user-agent")
        .ok()
        .flatten()
        .and_then(|value| {
            value.strip_prefix("codex_cli_rs/").map(|value| {
                value
                    .split_whitespace()
                    .next()
                    .unwrap_or("0.100.0")
                    .to_owned()
            })
        })
        .unwrap_or_else(|| "0.100.0".into());
    let turn_metadata = request
        .headers()
        .get("x-codex-turn-metadata")
        .ok()
        .flatten();
    let early = Gate::for_request(responses, turn_metadata.as_deref());
    let capture = Capture::new(request, &bytes, store.clone(), host, context.clone());
    let (principal, host, owner) = (principal.clone(), host.clone(), context.clone());
    let committed = early.clone();
    // Owned so that, after admitted native headers, `waitUntil` can finish this same pipeline.
    let pipeline = async move {
        let turn = NativeTurn {
            stream: None,
            turn_metadata,
            early: committed.clone(),
        };
        let inference = Inference {
            principal: &principal,
            store: &store,
            host: &host,
            context,
            capture: &capture,
            responses,
            turn,
        };
        // Compaction requests use the selected provider, like ordinary inference.
        let result = inference
            .execute(&bytes, catalog, client_version, request_id, false, permit)
            .await;
        let cancelled = host.cancellation.client_aborted();
        match result {
            Err(error) if committed.is_some_and(|gate| gate.committed()) => {
                crate::inference_source::late_error(error, &capture, cancelled).await
            }
            Err(error) => {
                capture.failure(&error, cancelled).await;
                Err(error)
            }
            answer => answer,
        }
    };
    match early {
        Some(gate) => crate::inference_early::respond(gate, pipeline, &owner).await,
        None => pipeline.await,
    }
}

struct Inference<'a> {
    principal: &'a AuthPrincipal,
    store: &'a RemoteAuthorityStore,
    host: &'a PlatformHost,
    context: Rc<Context>,
    capture: &'a Rc<Capture>,
    responses: bool,
    turn: NativeTurn,
}
impl Inference<'_> {
    async fn execute(
        mut self,
        bytes: &[u8],
        mut catalog: CatalogState,
        client_version: String,
        request_id: RequestId,
        local_compaction: bool,
        mut permit: Option<gateway_admission::Permit>,
    ) -> ResponseResult {
        let document = match parse_http_body(bytes).and_then(SourceRequestDocument::new) {
            Ok(document) => document,
            Err(error) => {
                return error_response(error.into(), self.capture, &self.turn).await;
            }
        };
        self.turn.stream =
            gateway_core::source_serialization::member(document.original_source(), "stream");
        if let Some(gate) = &self.turn.early {
            let model =
                gateway_core::source_serialization::member(document.original_source(), "model");
            gate.arm(self.turn.stream.as_ref(), model.as_ref());
        }
        if let Ok(gateway_schema::source_request::SourceModelRequest::Named(model)) =
            document.model_request()
            && let Err(error) = catalog
                .resolve_model(&ModelId::from_source(model), self.responses)
                .await
        {
            return error_response(error.into(), self.capture, &self.turn).await;
        }
        let original = RequestContext {
            request_id,
            principal: PrincipalId(crate::lifecycle::principal_id(self.principal)),
            key_id: match &self.principal.method {
                AuthMethod::KvApiKey { key_id, .. } => Some(ApiKeyId(key_id.clone())),
                _ => None,
            },
            route: if self.responses {
                "responses"
            } else {
                "chat.completions"
            }
            .to_owned(),
            started_at_ms: now_ms()?,
        };
        let metadata = LocalRequestMetadata {
            context: original.clone(),
            principal: self.principal.clone(),
            catalog: catalog.input,
            runtime: catalog.runtime,
            client_version,
            native_session_id: None,
            retry_prohibited: false,
        };
        // This clone shares the original Claude Binding owner across local fallback and dispatch.
        let routing = routing_host::Host::new(
            self.store.clone(),
            self.host.clone(),
            self.context.clone(),
            original.clone(),
        );
        let client_stream = document.stream().unwrap_or(false);

        let session = match self
            .begin(metadata, &document, local_compaction, &mut permit, &routing)
            .await
        {
            Ok(Start::Response(response)) => return Ok(response),
            Ok(Start::Ordinary(session)) => session,
            Err(error) => {
                return error_response(error, self.capture, &self.turn).await;
            }
        };

        let currentness = match CurrentnessGuard::start(
            session.clone(),
            self.host.cancellation.clone(),
            self.context.clone(),
        )
        .await
        {
            Ok(guard) => guard,
            Err(error) => {
                session
                    .finish(Default::default())
                    .await
                    .map_err(|error| *error.error)?;
                return error_response(error, self.capture, &self.turn).await;
            }
        };
        self.capture
            .begin(
                &original,
                session.selected_model(),
                document.original_source(),
                client_stream,
            )
            .await;

        self.deliver(permit, routing, session, currentness, client_stream)
            .await
    }
    async fn begin(
        &self,
        metadata: LocalRequestMetadata,
        document: &SourceRequestDocument,
        local_compaction: bool,
        permit: &mut Option<gateway_admission::Permit>,
        routing: &routing_host::Host,
    ) -> Result<Start, RoutingError> {
        if local_compaction {
            let started = crate::inference_local::start(
                metadata,
                document.clone(),
                self.store.clone(),
                routing.clone(),
                self.context.clone(),
                self.capture.clone(),
                permit.take(),
            )
            .await?;
            *permit = started.permit;
            Ok(started.route)
        } else {
            let input = RoutingInput::from_source_request(
                metadata,
                document.for_surface(if self.responses {
                    RequestSurface::Responses
                } else {
                    RequestSurface::ChatCompletions
                }),
            );
            let session = RoutingSession::begin(
                input,
                self.store.clone(),
                routing.clone(),
                PlatformClock,
                PlatformTokens,
            )
            .await?;
            Ok(Start::Ordinary(Rc::new(session)))
        }
    }
    async fn deliver(
        self,
        permit: Option<gateway_admission::Permit>,
        routing: routing_host::Host,
        session: Rc<Session>,
        currentness: CurrentnessGuard,
        client_stream: bool,
    ) -> ResponseResult {
        let deadline = self.host.cancellation.set_timeout_ms(ATTEMPT_DEADLINE_MS)?;
        let (upstream, attempt) = match dispatch_attempts(
            session.clone(),
            routing,
            self.host.cancellation.clone(),
            self.capture.clone(),
            self.turn.early.clone(),
        )
        .await
        {
            Ok(dispatched) => dispatched,
            Err(error) => {
                session
                    .finish(Default::default())
                    .await
                    .map_err(|error| *error.error)?;
                return error_response(error, self.capture, &self.turn).await;
            }
        };

        let status = upstream.status_code();
        let provider = attempt.request.decode().provider;
        let prepared = attempt.request.clone();
        let headers = upstream.headers().entries().collect::<Headers>();
        let decode_plan = gateway_providers::response::response_plan(prepared.decode(), &headers);
        let provider_request_id =
            crate::inference_wire::provider_request_id(provider, &headers).map(str::to_owned);
        self.capture.correlate(provider_request_id.as_deref());
        let pricing = crate::inference_source::pricing_metadata(
            session.surplus_billing(),
            prepared.url().ends_with("/messages"),
        )?;
        let lifecycle = Rc::new(Lifecycle {
            session,
            currentness,
            attempt,
            principal: self.principal.clone(),
            provider_request_id: RefCell::new(provider_request_id),
            status,
            store: self.store.clone(),
            pricing,
        });
        let event_stream = gateway_providers::response::is_event_stream(provider, status, &headers);
        let observation = ObservationHandle::default();
        let usage = Rc::new(RefCell::new(None));
        let finish = finish_hook(
            lifecycle.clone(),
            usage.clone(),
            permit,
            self.capture.clone(),
        );

        stream::Reply {
            finish,
            usage,
            observation,
            lifecycle,
            decode_plan,
            headers,
            prepared,
            upstream,
            deadline,
            context: self.context,
            host: self.host,
            capture: self.capture,
            responses: self.responses,
            principal: self.principal,
            client_stream,
            event_stream,
            status,
            provider,
            early: self.turn.early_committed(),
        }
        .send()
        .await
    }
}

const CEREBRAS_RATE_LIMIT_HEADERS: &[&str] = &[
    "x-ratelimit-limit-requests-minute",
    "x-ratelimit-remaining-requests-minute",
    "x-ratelimit-reset-requests-minute",
    "x-ratelimit-limit-tokens-minute",
    "x-ratelimit-remaining-tokens-minute",
    "x-ratelimit-reset-tokens-minute",
    "x-ratelimit-limit-requests-day",
    "x-ratelimit-remaining-requests-day",
    "x-ratelimit-reset-requests-day",
    "x-ratelimit-limit-tokens-day",
    "x-ratelimit-remaining-tokens-day",
    "x-ratelimit-reset-tokens-day",
];

fn set_header(response: &mut Response, name: &str, value: &str) -> Result<(), GatewayError> {
    response
        .headers_mut()
        .set(name, value)
        .map_err(|_| invalid("Invalid response header."))
}

fn apply_warnings(response: &mut Response, warnings: &[String]) -> Result<(), GatewayError> {
    if warnings.is_empty() {
        return Ok(());
    }
    let mut values = response
        .headers()
        .get("x-uos-warning")
        .map_err(|_| invalid("Invalid provider warning header."))?
        .unwrap_or_default()
        .split(',')
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_owned)
        .collect::<Vec<_>>();
    for warning in warnings {
        if !values.contains(warning) {
            values.push(warning.clone());
        }
    }
    response
        .headers_mut()
        .set("x-uos-warning", &values.join(", "))
        .map_err(|_| invalid("Invalid provider warning header."))
}

fn observe_event(observation: &ObservationHandle, event: &ProviderEvent) {
    if let Some(terminal) = event.terminal() {
        observation.semantic_terminal(terminal);
    }
}

fn copy_headers(
    response: &mut Response,
    headers: &gateway_providers::types::Headers,
) -> Result<(), GatewayError> {
    for (name, value) in headers {
        let lower = name.to_ascii_lowercase();
        if lower == "retry-after"
            || lower.starts_with("x-ratelimit-")
            || [
                "x-request-id",
                "request-id",
                "x-uos-warning",
                "x-should-retry",
            ]
            .contains(&lower.as_str())
        {
            set_header(response, name, value)?;
        }
    }
    Ok(())
}

/// Source buffered DeepSeek/Cerebras: a header-less completion is correlated by its own id.
fn completion_correlation(
    lifecycle: &Lifecycle,
    capture: &Capture,
    plan: &gateway_providers::types::DecodePlan,
    bytes: &[u8],
) -> Option<String> {
    if lifecycle.provider_request_id.borrow().is_some() {
        return None;
    }
    let id = gateway_providers::response::completion_provider_request_id(plan, bytes)?;
    capture.correlate(Some(&id));
    *lifecycle.provider_request_id.borrow_mut() = Some(id.clone());
    Some(id)
}

fn provider_correlation(
    response: &mut Response,
    provider: UpstreamProvider,
    headers: &gateway_providers::types::Headers,
) -> Result<(), GatewayError> {
    let id = crate::inference_wire::provider_request_id(provider, headers);
    for name in [
        "x-request-id",
        "x-api-request-id",
        "x-oneapi-request-id",
        "x-cerebras-request-id",
        "x-ds-request-id",
        "x-deepseek-request-id",
        "x-uos-provider-request-id",
        "request-id",
    ] {
        response
            .headers_mut()
            .delete(name)
            .map_err(|_| invalid("Invalid response header."))?;
    }
    if let Some(id) = id {
        set_header(response, "x-uos-provider-request-id", id)?;
    }
    Ok(())
}

fn invalid(message: &str) -> GatewayError {
    GatewayError::new(502, message, Some("upstream_transport_error"))
}
