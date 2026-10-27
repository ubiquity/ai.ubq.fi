use gateway_core::{
    error::GatewayError, lifecycle::SemanticTerminal, query::QueryStore, source_json::SourceJson,
};
use gateway_extras_contract::types::{
    DispatchFailure, DispatchFailureOrigin, ExtrasDispatch, ExtrasHost, ExtrasProvider,
    ExtrasRequest, ExtrasResponse, ExtrasRoute, HealthKind, HealthObservation, PreparedCall,
    UpstreamResponse,
};
mod state;
use gateway_provider_services::extras;
use gateway_schema::selection::ProviderSelection;
use state::{now, selection};
fn health(provider: ExtrasProvider, status: Option<u16>, kind: HealthKind) -> HealthObservation {
    HealthObservation {
        provider,
        kind,
        upstream_status: status,
    }
}
fn observed(provider: ExtrasProvider, status: u16) -> HealthObservation {
    health(
        provider,
        Some(status),
        match status {
            401 | 403 => HealthKind::AuthInvalid,
            402 | 429 => HealthKind::QuotaExhausted,
            500.. => HealthKind::UpstreamError,
            400.. => HealthKind::Reachable,
            _ => HealthKind::Success,
        },
    )
}
pub async fn handle<S: QueryStore, H: ExtrasHost>(
    store: &S,
    host: &H,
    request: &ExtrasRequest<'_>,
) -> Result<ExtrasResponse, GatewayError> {
    let decisions = request.route == ExtrasRoute::Decisions;
    let provider = if decisions {
        ExtrasProvider::Decisions
    } else {
        ExtrasProvider::SystemOne
    };
    let label = if decisions { "Decisions" } else { "System One" };
    if decisions && request.raw_body_bytes > 32 * 1024 * 1024 {
        return Ok(ExtrasResponse::error(GatewayError::new(
            413,
            "The Decisions request body is too large",
            Some("decisions_request_too_large"),
        )));
    }
    if !decisions && request.raw_body_bytes > 262_144 {
        return Ok(ExtrasResponse::error(GatewayError::new(
            400,
            "Invalid JSON body",
            Some("invalid_request_error"),
        )));
    }
    // Envelope errors precede configuration, discovery and dispatch.
    let decision = if decisions {
        match gateway_decision_protocol::request::parse(&request.body) {
            Ok(request) => Some(request),
            Err(error) => return Ok(ExtrasResponse::source_json(error.status, error.body()?)),
        }
    } else {
        if let Err(error) = gateway_provider_services::extras::prepare_source_systemone(&request.body, &Default::default()) {
            return Ok(ExtrasResponse::source_error(error));
        }
        None
    };
    let selection = selection(store, now(host)?).await?;
    if !ProviderSelection::enabled(selection.as_ref(), "openrouter") {
        return Ok(ExtrasResponse::error(GatewayError::new(
            503,
            format!("{label} provider is disabled by the provider selection"),
            Some(if decisions {
                "decisions_provider_disabled"
            } else {
                "systemone_provider_disabled"
            }),
        )));
    }
    let models = if decisions {
        host.decision_models().await?
    } else {
        Vec::new()
    };
    if let Some(decision) = &decision
        && gateway_decision_protocol::catalog::resolve_decision(&decision.model, &models).is_none()
    {
        let error = gateway_decision_protocol::request::RequestError::invalid(
            "The requested model is not served by the OpenRouter Decisions API",
            Some("model"),
        );
        return Ok(ExtrasResponse::source_json(error.status, error.body()?));
    }
    let Some(mut options) = host.provider_options(provider).await? else {
        return Ok(ExtrasResponse::error(GatewayError::new(
            503,
            format!("{label} upstream is not configured"),
            Some("openrouter_api_key_missing"),
        )));
    };
    options.allowed_models = models;
    let deadline_ms = now(host)? + if decisions { 60_000 } else { 20_000 };
    let call = match PreparedCall::prepare(provider, &request.body, &options, deadline_ms) {
        Ok(call) => call,
        Err(error) => return Ok(ExtrasResponse::source_error(error)),
    };
    let result = host
        .dispatch(if decisions {
            ExtrasDispatch::Decisions(call)
        } else {
            ExtrasDispatch::SystemOne(call)
        })
        .await;
    finish(host, provider, label, decisions, result)
}
fn finish<H: ExtrasHost>(
    host: &H,
    provider: ExtrasProvider,
    label: &str,
    decisions: bool,
    result: Result<UpstreamResponse, DispatchFailure>,
) -> Result<ExtrasResponse, GatewayError> {
    match result {
        Err(failure) => {
            if failure.origin == DispatchFailureOrigin::Gateway {
                let mut r = ExtrasResponse::error(failure.error.as_ref().clone());
                r.headers.extend(failure.headers.clone());
                r.upstream_errors.push(failure);
                return Ok(r);
            }
            if decisions && host.cancelled() {
                return Ok(ExtrasResponse::error(state::cancelled()));
            }
            let mut r = ExtrasResponse::error(GatewayError::new(
                502,
                format!("{label} upstream unreachable"),
                Some("openrouter_upstream_unreachable"),
            ));
            if !failure.before_transport {
                r.health
                    .push(health(provider, None, HealthKind::UpstreamError));
            }
            r.upstream_errors.push(failure);
            Ok(r)
        }
        Ok(result) => {
            if !(200..300).contains(&result.status) {
                let status =
                    if matches!(result.status, 400 | 429) || decisions && result.status == 413 {
                        result.status
                    } else {
                        502
                    };
                let mut r = ExtrasResponse::error(GatewayError::new(
                    status,
                    format!("{label} upstream error"),
                    Some("openrouter_upstream_error"),
                ));
                r.health.push(observed(provider, result.status));
                r.upstream_errors.push(DispatchFailure {
                    origin: DispatchFailureOrigin::Upstream,
                    error: Box::new(GatewayError::new(
                        status,
                        format!("{label} upstream error"),
                        Some("openrouter_upstream_error"),
                    )),
                    headers: result.headers,
                    quota_refusal: false,
                    upstream_status: Some(result.status),
                    retry_after_ms: None,
                    before_transport: false,
                    raw_error_body: Some(result.body),
                });
                return Ok(r);
            }
            let payload = SourceJson::parse_bytes(&result.body).ok();
            let decoded = payload
                .as_ref()
                .ok_or_else(|| {
                    GatewayError::new(
                        502,
                        format!("{label} upstream invalid response"),
                        Some("openrouter_upstream_invalid_response"),
                    )
                })
                .and_then(|body| {
                    if decisions {
                        gateway_decision_protocol::response::decode(body)
                    } else {
                        gateway_provider_services::extras::decode_source_systemone(body)
                    }
                });
            match decoded {
                Err(e) => {
                    let mut r = ExtrasResponse::error(e);
                    r.health
                        .push(health(provider, None, HealthKind::UpstreamError));
                    Ok(r)
                }
                Ok(payload) => {
                    let usage = if decisions {
                        gateway_decision_protocol::response::usage(&payload)
                    } else {
                        gateway_core::source_serialization::member(&payload, "usage")
                            .as_ref()
                            .and_then(gateway_provider_services::extras::source_systemone_usage)
                    };
                    let mut r = ExtrasResponse::source_json(200, payload);
                    if decisions {
                        r.headers
                            .insert("x-uos-upstream".into(), "openrouter".into());
                    }
                    r.semantic_terminal = Some(SemanticTerminal::Completed);
                    r.usage = usage;
                    r.health.push(observed(provider, 200));
                    Ok(r)
                }
            }
        }
    }
}
