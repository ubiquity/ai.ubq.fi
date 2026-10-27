use gateway_provider_source::source as s;
use gateway_provider_protocol::types::ClientSurface;
use gateway_provider_protocol::types::DecodePlan;
use gateway_provider_protocol::types::DecodedResponse;
use gateway_provider_protocol::types::Headers;
use gateway_provider_protocol::types::header;
use gateway_core::error::GatewayError;
use gateway_core::lifecycle::SemanticTerminal;
use gateway_core::provider::UpstreamProvider;
use gateway_core::source_json::SourceJson;
use serde_json::json;

/// The fixed Codex Responses request always speaks SSE. The source consumer
/// reads every successful Codex body as SSE even when the upstream omits its
/// media type; no other provider or explicit non-SSE media type is inferred.
pub fn is_event_stream(provider: UpstreamProvider, status: u16, headers: &Headers) -> bool {
    match header(headers, "content-type") {
        Some(value) => value.contains("text/event-stream"),
        None => provider == UpstreamProvider::ChatgptCodex && (200..300).contains(&status),
    }
}


pub fn decode_stream_response(
    plan: &DecodePlan,
    status: u16,
    bytes: &[u8],
    mut output_headers: Headers,
) -> Result<DecodedResponse, GatewayError> {
    let mut decoder = crate::sse::StreamDecoder::new(plan.clone());
    let mut events = decoder.push(bytes)?;
    events.extend(decoder.finish()?);
    // The decoder stops at the first terminal. An error must not fall back to
    // an earlier response.created payload or the generic completed sentinel.
    if plan.surface == ClientSurface::Responses
        && let Some(event) = events
            .iter()
            .find(|event| s::present(event.observation(), "type").is_some_and(|kind| s::is(&kind, "error")))
    {
        let body = if plan.stream {
            output_headers.insert("Content-Type".into(), "text/event-stream".into());
            event.data.clone()
        } else {
            s::scalar_json(&json!({"error":{
                "message":s::present(&event.data, "message").and_then(|observed| s::control(&observed)).unwrap_or_else(||"Upstream Responses stream ended unexpectedly.".into()),
                "type":"invalid_request_error",
                "code":s::present(&event.data, "code").and_then(|observed| s::control(&observed)).unwrap_or_else(||"server_error".into())
            }}))?
        };
        return Ok(DecodedResponse {
            status: if plan.stream { status } else { 502 },
            headers: output_headers,
            body,
            events,
            terminal: Some(SemanticTerminal::Error),
            usage: None,
            native_replay: None,
            raw_body: None,
        });
    }
    let body = match events.iter().rev().find_map(|event| event.field("response")) {
        Some(body) => body,
        None => s::null()?,
    };
    let mut body = if plan.surface == ClientSurface::Chat {
        gateway_provider_collector::source_response::collect(&events, plan)?
    } else {
        body
    };
    if plan.surface == ClientSurface::Chat
        && let Some(usage) = decoder.observed_usage().and_then(gateway_provider_source::usage::responses_to_chat)
    {
        s::set(&mut body, "usage", s::scalar_json(&usage)?)?;
    }
    if plan.stream {
        output_headers.insert("Content-Type".into(), "text/event-stream".into());
    }
    let terminal = events.iter().rev().find_map(|event| event.terminal()).or_else(|| terminal_for(&body));
    let usage = s::present(&body, "usage").as_ref().and_then(gateway_provider_source::source_collector::usage);
    Ok(DecodedResponse {
        status,
        headers: output_headers,
        body,
        events,
        terminal,
        usage,
        native_replay: decoder.native_replay().cloned(),
        raw_body: None,
    })
}


pub fn terminal_for(value: &SourceJson) -> Option<SemanticTerminal> {
    match s::present(value, "status").and_then(|observed| s::control(&observed)).as_deref() {
        Some("completed") => Some(SemanticTerminal::Completed),
        Some("incomplete") => Some(SemanticTerminal::Incomplete),
        Some("failed") => Some(SemanticTerminal::Failed),
        _ => None,
    }
}
