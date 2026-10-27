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
    },
};
use crate::{
    inference_capture::Capture, inference_currentness::CurrentnessGuard, inference_local::Start,
};
use gateway_auth::types::{AuthMethod, AuthPrincipal};
use gateway_core::{
    claude::ClaudeErrorEvidence,
    error::GatewayError,
    identity::{ApiKeyId, PrincipalId, RequestContext, RequestId},
    provider::UpstreamProvider,
};
use gateway_local_refusal::routing_error::RoutingError;
use gateway_observability::replay_trace::{AttemptTrace, TraceTerminal};
use gateway_providers::types::Headers;
use gateway_routing_runtime::{
    local_work::LocalRequestMetadata, session::RoutingSession, types::AttemptFailure,
    types::FailureOrigin, types::PreparedAttempt, types::PreparedWireRequest,
    types::RefusalDecision, types::ResponseDecision, types::ResponseHead, types::RoutingInput,
    types::TransportState, types::WorkspaceContinuation,
};
use gateway_schema::{
    request::RequestSurface,
    source_request::{SourceRequestDocument, parse_http_body},
};
use serde_json::json;
use std::{cell::RefCell, rc::Rc};
use worker::{Context, Method, Request, Response};

mod stream;

use gateway_worker_stream::control::ATTEMPT_DEADLINE_MS;
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
                    provider_request_id: correlation(attempt.request.provider(), &headers)
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
    routing: &routing_host::Host,
    cancellation: Rc<Cancellation>,
    capture: Rc<Capture>,
) -> Result<(Response, PreparedAttempt), RoutingError> {
    let dispatch = Dispatch {
        session: &session,
        routing,
        cancellation: &cancellation,
        capture: &capture,
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
    let local_compaction = responses
        && crate::host::setting(&host.env, "JEV_COMPACTION_DISABLED").as_deref() != Some("1")
        && gateway_compaction::is_responses_compaction(
            request
                .headers()
                .get("x-codex-turn-metadata")
                .ok()
                .flatten()
                .as_deref(),
        );
    let capture = Capture::new(request, &bytes, store.clone(), host, context.clone());
    let result = Inference {
        principal,
        store: &store,
        host,
        context,
        capture: &capture,
        responses,
    }
    .execute(
        &bytes,
        catalog,
        client_version,
        request_id,
        local_compaction,
        permit,
    )
    .await;
    if let Err(error) = &result {
        capture
            .failure(error, host.cancellation.client_aborted())
            .await;
    }
    result
}

struct Inference<'a> {
    principal: &'a AuthPrincipal,
    store: &'a RemoteAuthorityStore,
    host: &'a PlatformHost,
    context: Rc<Context>,
    capture: &'a Rc<Capture>,
    responses: bool,
}
impl Inference<'_> {
    async fn execute(
        self,
        bytes: &[u8],
        catalog: CatalogState,
        client_version: String,
        request_id: RequestId,
        local_compaction: bool,
        mut permit: Option<gateway_admission::Permit>,
    ) -> ResponseResult {
        let document = match parse_http_body(bytes).and_then(SourceRequestDocument::new) {
            Ok(document) => document,
            Err(error) => {
                return crate::inference_source::error_response(error.into(), self.capture).await;
            }
        };
        let catalog = if local_compaction {
            catalog
        } else {
            catalog.resolve_inference(&document).await?
        };
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
                return crate::inference_source::error_response(error, self.capture).await;
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
                return crate::inference_source::error_response(error, self.capture).await;
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
    async fn cleanup_pending(
        &self,
        owner: &mut gateway_admission::handoff::Pending<stream::PendingLifetime>,
        error: RoutingError,
    ) -> ResponseResult {
        let Some(lifetime) = owner.take() else {
            return crate::inference_source::error_response(
                crate::inference_local_lifecycle::wrapper_error(),
                self.capture,
            )
            .await;
        };
        gateway_admission::handoff::enqueue(
            lifetime.cleanup(error, self.capture.clone(), self.host.cancellation.clone()),
            |task| self.context.wait_until(task),
        )
        .await
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
        let mut owner = gateway_admission::handoff::Pending::new(stream::PendingLifetime {
            session,
            currentness,
            permit,
            deadline: Some(deadline),
        });
        let selected = match self.select(&mut owner, &routing).await {
            Ok(value) => value,
            Err(error) => {
                if owner.armed() {
                    return self.cleanup_pending(&mut owner, error).await;
                }
                let Some(lifetime) = owner.get() else {
                    return crate::inference_source::error_response(
                        crate::inference_local_lifecycle::wrapper_error(),
                        self.capture,
                    )
                    .await;
                };
                lifetime
                    .session
                    .finish(Default::default())
                    .await
                    .map_err(|error| *error.error)?;
                return crate::inference_source::error_response(error, self.capture).await;
            }
        };
        self.respond_selected(owner, selected, client_stream).await
    }

    async fn respond_selected(
        self,
        mut owner: gateway_admission::handoff::Pending<stream::PendingLifetime>,
        selected: stream::Selected,
        client_stream: bool,
    ) -> ResponseResult {
        let stream::Selected {
            upstream,
            attempt,
            headers,
            plan: decode_plan,
            prepared: ready,
            observation,
            usage,
        } = selected;
        let status = upstream.status_code();
        let provider = attempt.request.decode().provider;
        let prepared = attempt.request.clone();
        self.capture.correlate(correlation(provider, &headers));
        let Some(lifetime) = owner.get() else {
            return crate::inference_source::error_response(
                crate::inference_local_lifecycle::wrapper_error(),
                self.capture,
            )
            .await;
        };
        let pricing = match crate::inference_source::pricing_metadata(
            lifetime.session.surplus_billing(),
            prepared.url().ends_with("/messages"),
        ) {
            Ok(value) => value,
            Err(error) => {
                if owner.armed() {
                    return self.cleanup_pending(&mut owner, error.into()).await;
                }
                return Err(error);
            }
        };
        let event_stream = gateway_providers::response::is_event_stream(provider, status, &headers);
        // Everything fallible has finished before this synchronous owner transfer.
        let Some(stream::PendingLifetime {
            session,
            currentness,
            permit,
            deadline,
        }) = owner.take()
        else {
            return crate::inference_source::error_response(
                crate::inference_local_lifecycle::wrapper_error(),
                self.capture,
            )
            .await;
        };
        let lifecycle = Rc::new(Lifecycle {
            session,
            currentness,
            attempt,
            principal: self.principal.clone(),
            provider_request_id: correlation(provider, &headers).map(str::to_owned),
            status,
            store: self.store.clone(),
            pricing,
        });
        let mut deadline = deadline;
        let finish = if owner.armed() {
            stream::ReplyFinish::selected(
                stream::SelectedLifetime {
                    lifecycle: lifecycle.clone(),
                    permit,
                    observation: observation.clone(),
                    usage: usage.clone(),
                    deadline: deadline.take(),
                },
                self.context.clone(),
                self.capture.clone(),
                self.host.cancellation.clone(),
            )
        } else {
            stream::ReplyFinish::Ordinary(Some(finish_hook(
                lifecycle.clone(),
                usage.clone(),
                permit,
                self.capture.clone(),
            )))
        };
        let reply = stream::Reply {
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
        };
        match ready {
            Some(ready) => match reply.streaming_from(ready) {
                Ok(response) => Ok(response),
                Err(error) => crate::inference_source::error_response(error, self.capture).await,
            },
            None => reply.send().await,
        }
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

/// Source Cerebras errors forward only message/code from a bounded body.
fn cerebras_error_detail(bytes: &[u8]) -> (String, String) {
    let fallback_message = "Cerebras upstream returned an error.".to_owned();
    let fallback_code = "cerebras_upstream_error".to_owned();
    let Ok(value) = serde_json::from_slice::<serde_json::Value>(bytes) else {
        return (fallback_message, fallback_code);
    };
    let error = value.get("error").filter(|value| value.is_object());
    let text = |name: &str, limit: usize| {
        error
            .and_then(|error| error.get(name))
            .or_else(|| value.get(name))
            .and_then(serde_json::Value::as_str)
            .map(|value| {
                value
                    .chars()
                    .take(limit)
                    .collect::<String>()
                    .trim()
                    .to_owned()
            })
            .filter(|value| !value.is_empty())
    };
    (
        text("message", 1_000).unwrap_or(fallback_message),
        text("code", 200).unwrap_or(fallback_code),
    )
}

fn cerebras_error_type(status: u16) -> &'static str {
    if status >= 500 {
        "server_error"
    } else if status == 429 {
        "rate_limit_error"
    } else {
        "invalid_request_error"
    }
}

fn cerebras_error_body(status: u16, bytes: &[u8]) -> Vec<u8> {
    let (message, code) = cerebras_error_detail(bytes);
    serde_json::to_vec(&json!({
        "error": {
            "message": message,
            "type": cerebras_error_type(status),
            "code": code,
        }
    }))
    .unwrap_or_else(|_| {
        br#"{"error":{"message":"Cerebras upstream returned an error.","type":"invalid_request_error","code":"cerebras_upstream_error"}}"#.to_vec()
    })
}

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

fn correlation(
    provider: UpstreamProvider,
    headers: &gateway_providers::types::Headers,
) -> Option<&str> {
    let names: &[&str] = if provider == UpstreamProvider::Deepseek {
        &["x-request-id", "x-ds-request-id", "x-deepseek-request-id"]
    } else if provider == UpstreamProvider::Cerebras {
        &[
            "x-request-id",
            "x-api-request-id",
            "x-oneapi-request-id",
            "x-cerebras-request-id",
        ]
    } else {
        &["x-request-id", "request-id"]
    };
    names
        .iter()
        .find_map(|wanted| {
            headers
                .iter()
                .find(|(name, _)| name.eq_ignore_ascii_case(wanted))
                .map(|(_, value)| value.trim())
        })
        .filter(|id| {
            !id.is_empty() && id.len() <= 256 && !id.chars().any(|c| c <= '\u{1f}' || c == '\u{7f}')
        })
}

fn provider_correlation(
    response: &mut Response,
    provider: UpstreamProvider,
    headers: &gateway_providers::types::Headers,
) -> Result<(), GatewayError> {
    let id = correlation(provider, headers);
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

fn interrupted() -> GatewayError {
    crate::transport::platform_error(
        "The upstream request was interrupted.",
        "upstream_request_interrupted",
    )
}
fn stream_failure(provider: UpstreamProvider) -> GatewayError {
    GatewayError::new(
        502,
        format!(
            "DeepSeek waterfall provider '{}' ended its stream before producing output.",
            gateway_providers::policy::direct_provider_label(provider)
        ),
        Some("deepseek_waterfall_stream_failure"),
    )
    .with_type("server_error")
}
fn invalid(message: &str) -> GatewayError {
    GatewayError::new(502, message, Some("upstream_transport_error"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cerebras_payment_required_body_forwards_only_source_detail_fields() {
        let input = br#"{"error":{"message":"Payment required","code":"payment_required","param":"secret","type":"vendor"},"secret":"never"}"#;
        let body = cerebras_error_body(402, input);
        let text = String::from_utf8(body.clone()).unwrap();
        assert_eq!(
            text,
            r#"{"error":{"message":"Payment required","type":"invalid_request_error","code":"payment_required"}}"#
        );
        assert!(!text.contains("vendor"));
        assert!(!text.contains("secret"));
        assert!(!text.contains("param"));
    }

    #[test]
    fn cerebras_payment_required_146_byte_source_shape() {
        let message = "x".repeat(65);
        let input = serde_json::to_vec(&json!({
            "error": {"message": message, "code": "payment_required"}
        }))
        .unwrap();
        let body = cerebras_error_body(402, &input);
        assert_eq!(body.len(), 146);
        assert_eq!(
            body,
            br#"{"error":{"message":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","type":"invalid_request_error","code":"payment_required"}}"#
        );
    }

    #[test]
    fn cerebras_status_types_and_bounds_follow_the_source() {
        let body = cerebras_error_body(429, br#"{"error":{"message":"slow down","code":"rate"}}"#);
        assert!(
            String::from_utf8(body)
                .unwrap()
                .contains("rate_limit_error")
        );
        let body = cerebras_error_body(503, br#"{"message":"down","code":"unavailable"}"#);
        assert!(String::from_utf8(body).unwrap().contains("server_error"));
        let oversized = serde_json::to_vec(&json!({
            "error": {"message": "m".repeat(2_000), "code": "c".repeat(400)}
        }))
        .unwrap();
        let body = String::from_utf8(cerebras_error_body(402, &oversized)).unwrap();
        assert!(!body.contains(&"m".repeat(1_001)));
        assert!(!body.contains(&"c".repeat(201)));
    }
}

#[cfg(test)]
mod pending_tests;
