use gateway_contracts::lifecycle::SemanticTerminal;
use gateway_contracts::provider::UpstreamProvider;
use gateway_provider_events::event::ProviderEvent;
use gateway_provider_protocol::types::ClientSurface;
use gateway_provider_protocol::types::DecodePlan;
use gateway_provider_protocol::types::DecodedResponse;
use gateway_provider_protocol::types::Headers;
use gateway_provider_source::source as s;
use serde_json::Value;
use serde_json::json;

pub fn buffered_chat_events(
    completion: &Value,
    plan: &DecodePlan,
) -> Result<Vec<ProviderEvent>, gateway_source::error::GatewayError> {
    let mut choices = vec![];
    let mut finishes = vec![];
    for (position, choice) in completion["choices"]
        .as_array()
        .into_iter()
        .flatten()
        .enumerate()
    {
        let index = choice["index"].as_u64().unwrap_or(position as u64);
        let mut delta = choice["message"].clone();
        if let Some(calls) = delta["tool_calls"].as_array_mut() {
            for (index, call) in calls.iter_mut().enumerate() {
                call["index"] = json!(index);
            }
        }
        choices.push(json!({"index":index,"delta":delta,"finish_reason":null}));
        finishes.push(json!({"index":index,"delta":{},"finish_reason":choice["finish_reason"]}));
    }
    let base = |choices: Value| -> Result<
        gateway_source::source_json::SourceJson,
        gateway_source::error::GatewayError,
    > {
        s::object(vec![
            (
                "id",
                s::scalar_json(
                    &completion
                        .get("id")
                        .cloned()
                        .unwrap_or(json!(plan.response_id)),
                )?,
            ),
            ("object", s::text("chat.completion.chunk")?),
            (
                "created",
                s::scalar_json(
                    &completion
                        .get("created")
                        .cloned()
                        .unwrap_or(json!(plan.created_at)),
                )?,
            ),
            ("model", s::quoted(plan.model.as_js_string())?),
            ("choices", s::scalar_json(&choices)?),
        ])
    };
    let mut events = vec![
        ProviderEvent::chat_source(base(json!(choices))?),
        ProviderEvent::chat_source(base(json!(finishes))?),
    ];
    if s::present_path(&plan.request, &["stream_options", "include_usage"])
        .and_then(|observed| s::boolean(&observed))
        == Some(true)
        && let Some(usage) = completion.get("usage")
    {
        let mut chunk = base(json!([]))?;
        s::set(&mut chunk, "usage", s::scalar_json(usage)?)?;
        events.push(ProviderEvent::chat_source(chunk));
    }
    events.push(ProviderEvent::chat(json!("[DONE]"))?);
    Ok(events)
}

pub fn lithos_stream_refusal(
    plan: &DecodePlan,
    status: u16,
    headers: &Headers,
    value: &Value,
) -> Result<Option<DecodedResponse>, gateway_source::error::GatewayError> {
    if plan.provider != UpstreamProvider::Lithos || !plan.stream || status != 429 {
        return Ok(None);
    }
    let detail = value
        .get("error")
        .filter(|v| v.is_object())
        .unwrap_or(value);
    let message = detail["message"]
        .as_str()
        .unwrap_or("Upstream refused the streaming request.");
    let code = detail["code"]
        .as_str()
        .or_else(|| detail["type"].as_str())
        .unwrap_or("lithos_upstream_error");
    let mut output_headers = headers.clone();
    output_headers.insert("Content-Type".into(), "text/event-stream".into());
    output_headers.insert("x-uos-upstream".into(), "lithos".into());
    let event = if plan.surface == ClientSurface::Chat {
        ProviderEvent::chat(
            json!({"error":{"message":message,"type":"server_error","code":code,"param":null}}),
        )?
    } else {
        let mut response = crate::writer::envelope(plan, "failed")?;
        s::set(
            &mut response,
            "error",
            s::scalar_json(&json!({"code":code,"message":message}))?,
        )?;
        gateway_provider_protocol::source_echo::response_event("response.failed", response)?
    };
    Ok(Some(DecodedResponse {
        status: 200,
        headers: output_headers,
        body: s::scalar_json(value)?,
        events: vec![event],
        terminal: Some(SemanticTerminal::Failed),
        usage: None,
        native_replay: None,
        raw_body: None,
    }))
}

/// Native Messages stop vocabulary has its own error identity, distinct from Chat finish reasons.
pub fn native_stop_error(
    events: &mut [ProviderEvent],
    reason: &str,
) -> Result<(), gateway_source::error::GatewayError> {
    if let Some(last) = events.last_mut() {
        let Some(mut response) = s::present(&last.data, "response") else {
            return Ok(());
        };
        if s::present(&response, "status").is_some_and(|observed| s::is(&observed, "failed"))
            && s::present_path(&response, &["error", "code"])
                .and_then(|observed| s::control(&observed))
                .is_some_and(|code| code.starts_with("unrecognized_finish_reason:"))
        {
            s::set(
                &mut response,
                "error",
                s::scalar_json(
                    &json!({"code":"anthropic_unrecognized_stop_reason","message":format!("Anthropic Messages stopped with reason {reason}.")}),
                )?,
            )?;
            s::set(&mut last.data, "response", response)?;
        }
    }
    Ok(())
}
