//! src/decisions/handlers.ts handleDecisions over the original extras admission.
use crate::{
    host::{DECISIONS_TIMEOUT_MS, DecisionsCall, DecisionsHost},
    request,
};
use gateway_core::{
    error::GatewayError, js_string::JsString16, lifecycle::SemanticTerminal, query::QueryStore,
    source_json::SourceJson, source_serialization as source, tuple_key::TupleKey,
};
use gateway_extras_runtime::types::{
    DispatchFailure, DispatchFailureOrigin, ExtrasProvider, ExtrasRequest, ExtrasResponse, Headers,
    HealthKind, HealthObservation, KernelDisposition, UpstreamResponse,
};
use gateway_schema::{provider_catalog::openrouter_endpoints, selection::ProviderSelection};
use serde_json::{Value, json};

const JEV_ALIAS: &str = "typesafe/jev-latest";
const JEV_UPSTREAM: &str = "~typesafe/jev-latest";

fn health(status: Option<u16>, kind: HealthKind) -> HealthObservation {
    HealthObservation {
        provider: ExtrasProvider::SystemOne,
        kind,
        upstream_status: status,
    }
}
/// recordDecisionsResponseHealth: the provider-health classification of a status.
pub fn observed(status: u16) -> HealthObservation {
    health(
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
/// The client status for a non-2xx upstream: 400, 413 and 429 pass through.
pub fn upstream_status(status: u16) -> u16 {
    if matches!(status, 400 | 413 | 429) {
        status
    } else {
        502
    }
}
fn same(value: &JsString16, literal: &str) -> bool {
    value.as_units().iter().copied().eq(literal.encode_utf16())
}
/// The decision-output OpenRouter ids, in cached catalogue order.
pub fn decision_model_ids(rows: &[SourceJson]) -> Vec<JsString16> {
    rows.iter()
        .filter(|row| openrouter_endpoints(row).contains(&"/v1/decisions"))
        .filter_map(|row| source::member(row, "id").and_then(|id| source::string(&id)))
        .collect()
}
/// openRouterDecisionUpstreamModelFor: the stable Jev alias maps to the rolling
/// tilde row; a text-only or unknown id is never sent to the decision wire.
pub fn resolve_model(model: &JsString16, rows: &[SourceJson]) -> Option<JsString16> {
    let ids = decision_model_ids(rows);
    if model.as_units().is_empty() {
        return None;
    }
    if ids.contains(model) {
        return Some(model.clone());
    }
    (same(model, JEV_ALIAS) && ids.iter().any(|id| same(id, JEV_UPSTREAM)))
        .then(|| JsString16::from_utf8(JEV_UPSTREAM))
}
fn refusal(status: u16, message: &str, code: &str) -> ExtrasResponse {
    ExtrasResponse::error(GatewayError::new(status, message, Some(code)))
}
fn finished(mut response: ExtrasResponse) -> ExtrasResponse {
    response.kernel = if (200..300).contains(&response.status) {
        KernelDisposition::Completed
    } else {
        KernelDisposition::Incomplete("decisions_failed")
    };
    response
}
async fn provider_enabled<S: QueryStore>(store: &S, now: i64) -> Result<bool, GatewayError> {
    let parts = [json!("uos_ai"), json!("provider_selection"), json!("v1")];
    let row = store
        .read(&TupleKey::from_json_parts(&parts)?.to_record_key())
        .await?;
    let mut value: Option<Value> = None;
    if let Some(row) = row {
        row.validate_version()?;
        if row.expires_at_ms.is_none_or(|expiry| expiry > now) {
            value = Some(row.json_value()?.clone());
        }
    }
    let selection = value.as_ref().and_then(ProviderSelection::normalize);
    Ok(ProviderSelection::enabled(selection.as_ref(), "openrouter"))
}
fn call<H: DecisionsHost>(
    host: &H,
    secret: &str,
    body: SourceJson,
) -> Result<DecisionsCall, GatewayError> {
    let now = host.now_ms();
    if !(0..=gateway_core::identity::MAX_SAFE_INTEGER).contains(&now) {
        return Err(GatewayError::unavailable(
            "Decisions clock is invalid.",
            "invalid_clock",
        ));
    }
    Ok(DecisionsCall {
        headers: Headers::from([
            ("accept".into(), "application/json".into()),
            ("authorization".into(), format!("Bearer {secret}")),
            ("content-type".into(), "application/json".into()),
        ]),
        body,
        deadline_ms: now.saturating_add(DECISIONS_TIMEOUT_MS),
    })
}

/// One Decisions request. `openrouter` is the request's cached OpenRouter rows.
pub async fn handle<S: QueryStore, H: DecisionsHost>(
    store: &S,
    host: &H,
    request: &ExtrasRequest<'_>,
    openrouter: &[SourceJson],
) -> Result<ExtrasResponse, GatewayError> {
    let parsed = match request::parse(&request.body) {
        Ok(parsed) => parsed,
        Err(error) => return Ok(finished(ExtrasResponse::source_error(error))),
    };
    if !provider_enabled(store, host.now_ms()).await? {
        return Ok(finished(refusal(
            503,
            "Decisions provider is disabled by the provider selection",
            "decisions_provider_disabled",
        )));
    }
    let Some(upstream) = resolve_model(&parsed.model, openrouter) else {
        return Ok(finished(ExtrasResponse::source_error(request::invalid(
            "The requested model is not served by the OpenRouter Decisions API",
            Some("model"),
        ))));
    };
    let Some(options) = host.provider_options(ExtrasProvider::SystemOne).await? else {
        return Ok(finished(refusal(
            503,
            "Decisions upstream is not configured",
            "openrouter_api_key_missing",
        )));
    };
    let body = request::forward_body(&parsed, &upstream)?;
    let result = host
        .dispatch_decisions(call(host, &options.auth.secret, body)?)
        .await;
    Ok(finished(match result {
        Err(failure) => dispatch_failure(host, failure),
        Ok(result) => upstream_response(result)?,
    }))
}

fn dispatch_failure<H: DecisionsHost>(host: &H, failure: DispatchFailure) -> ExtrasResponse {
    if failure.origin == DispatchFailureOrigin::Gateway {
        let mut response = ExtrasResponse::error(failure.error.as_ref().clone());
        response.headers.extend(failure.headers.clone());
        response.upstream_errors.push(failure);
        return response;
    }
    // A caller abort after transport began is a bounded cancellation, never quota.
    if host.cancelled() && !failure.before_transport {
        let mut response = ExtrasResponse::error(
            GatewayError::new(499, "Request was cancelled.", Some("request_cancelled"))
                .with_type("server_error")
                .with_param(None),
        );
        response.upstream_errors.push(failure);
        return response;
    }
    let mut response = refusal(
        502,
        "Decisions upstream unreachable",
        "openrouter_upstream_unreachable",
    );
    if !failure.before_transport {
        response
            .health
            .push(health(None, HealthKind::UpstreamError));
    }
    response.upstream_errors.push(failure);
    response
}
fn invalid_response() -> ExtrasResponse {
    let mut response = refusal(
        502,
        "Decisions upstream invalid response",
        "openrouter_upstream_invalid_response",
    );
    response
        .health
        .push(health(None, HealthKind::UpstreamError));
    response
}

fn upstream_response(result: UpstreamResponse) -> Result<ExtrasResponse, GatewayError> {
    if !(200..300).contains(&result.status) {
        let status = upstream_status(result.status);
        let error = GatewayError::new(
            status,
            "Decisions upstream error",
            Some("openrouter_upstream_error"),
        );
        let mut response = ExtrasResponse::error(error.clone());
        response.health.push(observed(result.status));
        response.upstream_errors.push(DispatchFailure {
            origin: DispatchFailureOrigin::Upstream,
            error: Box::new(error),
            headers: result.headers,
            quota_refusal: false,
            upstream_status: Some(result.status),
            retry_after_ms: None,
            before_transport: false,
            raw_error_body: Some(result.body),
        });
        return Ok(response);
    }
    let Some(payload) = SourceJson::parse_bytes(&result.body)
        .ok()
        .filter(|payload| payload.as_str().trim_start().starts_with('{'))
    else {
        return Ok(invalid_response());
    };
    let answers = source::member(&payload, "answers");
    if !answers.is_some_and(|answers| answers.as_str().trim_start().starts_with('{')) {
        return Ok(invalid_response());
    }
    let usage = source::member(&payload, "usage")
        .as_ref()
        .and_then(gateway_providers::extras::source_systemone_usage);
    let mut response = ExtrasResponse::source_json(200, source::canonical(&payload)?);
    response
        .headers
        .insert("x-uos-upstream".into(), "openrouter".into());
    response.semantic_terminal = Some(SemanticTerminal::Completed);
    response.usage = usage;
    response.health.push(observed(200));
    Ok(response)
}
