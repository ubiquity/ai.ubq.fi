use gateway_provider_source::source as s;
use gateway_provider_tool_grammar::tools;
use gateway_provider_source::types::ChatProfile;
use gateway_provider_protocol::types::ClientSurface;
use gateway_provider_protocol::types::DecodePlan;
use gateway_provider_protocol::types::DecodedResponse;
use gateway_provider_protocol::types::Headers;
use gateway_provider_events::event::ProviderEvent;
use gateway_provider_protocol::types::ResponseDecode;
use gateway_provider_protocol::types::header;
use gateway_provider_source::extras::malformed;
use gateway_provider_source::usage;
use gateway_provider_writer::writer::ResponseWriter;
use gateway_provider_writer::writer::envelope;
use gateway_provider_writer::writer::finish_disposition;
use gateway_core::error::GatewayError;
use gateway_core::lifecycle::SemanticTerminal;
use gateway_core::provider::UpstreamProvider;
use gateway_core::source_json::SourceJson;
use serde_json::Value;
use serde_json::json;
use gateway_provider_stream::response::is_event_stream;
use gateway_provider_stream::response::decode_stream_response;
use gateway_provider_stream::response::terminal_for;
use gateway_provider_protocol::response::translated_response_id;
use gateway_provider_protocol::response::valid_provider_request_id;
use gateway_provider_protocol::response::translated_provider_request_id;
use gateway_provider_protocol::response::response_plan;
use gateway_provider_protocol::response::native_response_id;

pub fn normalize_chat_completion(
    value: &Value,
    plan: &DecodePlan,
) -> Result<SourceJson, GatewayError> {
    let fields = normalize_chat_completion_fragment(value, plan)?;
    Ok(chat_model_body(&fields, plan)?)
}


fn chat_model_body(fields: &Value, plan: &DecodePlan) -> Result<SourceJson, gateway_core::error::GatewayError> {
    Ok({
    let mut body = s::scalar_json(fields)?;
    s::set(&mut body, "model", s::quoted(plan.model.as_js_string())?)?;
    body

    })
}


/// Only actual scalar upstream fragments, without source-derived model/request fields.
fn normalize_chat_completion_fragment(
    value: &Value,
    plan: &DecodePlan,
) -> Result<Value, GatewayError> {
    let id = value["id"]
        .as_str()
        .filter(|s| !s.trim().is_empty())
        .ok_or_else(|| malformed("Upstream Chat Completion is missing an id."))?;
    let created = value["created"]
        .as_u64()
        .filter(|v| *v <= 9_007_199_254_740_991)
        .ok_or_else(|| malformed("Upstream Chat Completion has an invalid creation time."))?;
    if value.get("object").is_some_and(|v| v != "chat.completion") {
        return Err(malformed("Upstream did not return a Chat Completion."));
    }
    let choices = value["choices"]
        .as_array()
        .filter(|v| !v.is_empty())
        .ok_or_else(|| malformed("Upstream Chat Completion has no choices."))?;
    let mut normalized = vec![];
    for (position, choice) in choices.iter().enumerate() {
        let message = &choice["message"];
        if message["role"] != "assistant" {
            return Err(malformed(
                "Upstream choice does not contain an assistant message.",
            ));
        }
        for key in ["content", "refusal", "reasoning_content", "reasoning"] {
            if message
                .get(key)
                .is_some_and(|v| !v.is_null() && !v.is_string())
            {
                return Err(malformed(format!("Upstream choice has invalid {key}.")));
            }
        }
        let calls = if let Some(calls) = message.get("tool_calls").filter(|value| !value.is_null())
        {
            let mut validated = vec![];
            for call in calls
                .as_array()
                .ok_or_else(|| malformed("Upstream tool calls are not an array."))?
            {
                let id = call["id"]
                    .as_str()
                    .filter(|s| !s.is_empty() && s.trim() == *s)
                    .ok_or_else(|| malformed("Upstream tool call id is invalid."))?;
                let name = call["function"]["name"]
                    .as_str()
                    .filter(|s| !s.is_empty() && s.trim() == *s)
                    .ok_or_else(|| malformed("Upstream tool name is invalid."))?;
                let args = call["function"]["arguments"]
                    .as_str()
                    .ok_or_else(|| malformed("Upstream tool arguments are invalid."))?;
                validated.push(
                    json!({"id":id,"type":"function","function":{"name":name,"arguments":args}}),
                );
            }
            validated
        } else {
            vec![]
        };
        let content = message["content"].as_str().unwrap_or("");
        let refusal = message["refusal"].as_str().unwrap_or("");
        let reasoning = message["reasoning_content"]
            .as_str()
            .or_else(|| message["reasoning"].as_str());
        if message.get("content").is_none()
            && refusal.is_empty()
            && calls.is_empty()
            && reasoning.is_none()
        {
            return Err(malformed(
                "Upstream choice has neither content nor a tool call.",
            ));
        }
        let mut mapped = json!({"role":"assistant","content":if content.is_empty()&&(!calls.is_empty()||!refusal.is_empty()){Value::Null}else{json!(content)}});
        if !calls.is_empty() {
            mapped["tool_calls"] = json!(calls);
        }
        if !refusal.is_empty() {
            mapped["refusal"] = json!(refusal);
        }
        if let Some(reasoning) = reasoning {
            mapped[if plan.provider == UpstreamProvider::Cerebras {
                "reasoning"
            } else {
                "reasoning_content"
            }] = json!(reasoning);
        }
        let finish = choice.get("finish_reason").cloned().unwrap_or(Value::Null);
        if !finish.is_null() && !finish.is_string() {
            return Err(malformed("Upstream choice has an invalid finish reason."));
        }
        normalized.push(json!({"index":choice["index"].as_u64().unwrap_or(position as u64),"message":mapped,"finish_reason":finish}));
    }
    let mut result =
        json!({"id":id,"object":"chat.completion","created":created,"choices":normalized});
    if let Some(value) = value.get("usage").filter(|v| !v.is_null()) {
        if !value.is_object() {
            return Err(malformed("Upstream usage is not an object."));
        }
        result["usage"] = value.clone();
    }
    Ok(result)
}


pub fn chat_completion_to_responses(
    completion: &Value,
    plan: &DecodePlan,
    profile: ChatProfile,
) -> Result<SourceJson, gateway_core::error::GatewayError> {
    Ok({
    let (reason, incomplete, error) =
        finish_disposition(completion["choices"][0]["finish_reason"].as_str());
    let status = match reason {
        SemanticTerminal::Completed => "completed",
        SemanticTerminal::Incomplete => "incomplete",
        _ => "failed",
    };
    let mut body = envelope(plan, status)?;
    s::set(
        &mut body,
        "created_at",
        s::scalar_json(&completion
                .get("created")
                .cloned()
                .unwrap_or(json!(plan.created_at)))?,
    )?;
    let mut output = vec![];
    for (choice_index, choice) in completion["choices"]
        .as_array()
        .into_iter()
        .flatten()
        .enumerate()
    {
        let message = &choice["message"];
        if let Some(reason) = message["reasoning_content"]
            .as_str()
            .or_else(|| message["reasoning"].as_str())
            .filter(|s| !s.is_empty())
            && !plan
                .reasoning_summary
                .as_ref()
                .is_some_and(|value| value.as_units().iter().copied().eq("none".encode_utf16()))
        {
            output.push(s::scalar_json(&json!({"id":format!("rs_{}_{choice_index}",plan.response_id),"type":"reasoning","status":"completed","summary":[{"type":"summary_text","text":reason}]}))?);
        }
        let mut parts = vec![];
        if let Some(text) = message["content"].as_str().filter(|s| !s.is_empty()) {
            parts.push(json!({"type":"output_text","text":text,"annotations":[]}));
        }
        if let Some(refusal) = message["refusal"].as_str().filter(|s| !s.is_empty()) {
            parts.push(json!({"type":"refusal","refusal":refusal}));
        }
        if !parts.is_empty() {
            output.push(s::scalar_json(&json!({"id":format!("msg_{}_{choice_index}",plan.response_id),"type":"message","status":"completed","role":"assistant","content":parts}))?);
        }
        for (call_index, call) in message["tool_calls"]
            .as_array()
            .into_iter()
            .flatten()
            .enumerate()
        {
            let name = call["function"]["name"].as_str().unwrap_or("");
            let identity = gateway_provider_tool_grammar::tools::original(name, &plan.tool_names);
            let prefix = if identity.custom { "ctc" } else { "fc" };
            let id = format!("{prefix}_{}_{choice_index}_{call_index}", plan.response_id);
            let fallback = format!("{}_call_{call_index}", plan.response_id);
            let call_id = call["id"].as_str().unwrap_or(&fallback);
            output.push(gateway_provider_tool_grammar::tools::function_item(
                &id,
                call_id,
                name,
                call["function"]["arguments"].as_str().unwrap_or(""),
                &plan.tool_names,
                "completed",
            )?);
        }
    }
    s::set(&mut body, "output", s::array(&output)?)?;
    s::set(
        &mut body,
        "usage",
        s::scalar_json(&gateway_provider_source::usage::chat_to_responses(&completion["usage"], profile))?,
    )?;
    if let Some(incomplete) = incomplete {
        s::set(
            &mut body,
            "incomplete_details",
            s::scalar_json(&json!({"reason":incomplete}))?,
        )?;
    }
    if let Some(error) = error {
        s::set(
            &mut body,
            "error",
            s::scalar_json(&json!({"code":error,"message":format!("Upstream stopped generating: {error}")}))?,
        )?;
    }
    body

    })
}


pub fn response_to_chat(payload: &Value, plan: &DecodePlan) -> Result<SourceJson, GatewayError> {
    gateway_provider_protocol::source_response::to_chat(&s::scalar_json(payload)?, plan)
}


fn replay_chat(completion: &Value, plan: &DecodePlan, profile: ChatProfile) -> Result<Vec<ProviderEvent>, gateway_core::error::GatewayError> {
    Ok({
    let mut writer = ResponseWriter::new(plan.clone());
    let mut delta = completion["choices"][0]["message"].clone();
    if let Some(calls) = delta["tool_calls"].as_array_mut() {
        for (index, call) in calls.iter_mut().enumerate() {
            call["index"] = json!(index);
        }
    }
    let mut events=writer.chat_chunk(&json!({"choices":[{"delta":delta,"finish_reason":completion["choices"][0]["finish_reason"]}],"usage":completion["usage"]}),profile)?;
    events.extend(writer.finish()?);
    events

    })
}


fn decode_anthropic(
    value: &Value,
    plan: &DecodePlan,
) -> Result<(SourceJson, Vec<ProviderEvent>), GatewayError> {
    if value["type"] != "message" || value["role"] != "assistant" {
        return Err(malformed(
            "Anthropic Messages returned an invalid assistant message.",
        ));
    }
    let mut native_plan = plan.clone();
    if let Some(id) = value["id"].as_str().filter(|s| !s.is_empty()) {
        native_plan.response_id = native_response_id(id, &plan.response_id);
    }
    let mut writer = ResponseWriter::new(native_plan);
    let blocks = value["content"]
        .as_array()
        .ok_or_else(|| malformed("Anthropic Messages content is not an array."))?;
    let mut events = writer.start()?;
    for (index, block) in blocks.iter().enumerate() {
        match block["type"].as_str().unwrap_or("") {
            "text" => {
                events.extend(
                    writer.text(
                        block["text"]
                            .as_str()
                            .ok_or_else(|| malformed("Anthropic text is invalid."))?,
                        false,
                    )?,
                );
                events.extend(writer.close()?);
            }
            "thinking" => {
                events.extend(
                    writer.reasoning(
                        block["thinking"]
                            .as_str()
                            .ok_or_else(|| malformed("Anthropic thinking is invalid."))?,
                    )?,
                );
                events.extend(writer.close()?);
            }
            "tool_use" => {
                if !block["input"].is_object() {
                    return Err(malformed("Anthropic tool input is not object."));
                }
                events.extend(
                    writer.tool_delta(
                        index,
                        Some(
                            block["id"]
                                .as_str()
                                .ok_or_else(|| malformed("Anthropic tool id is invalid."))?,
                        ),
                        Some(
                            block["name"]
                                .as_str()
                                .ok_or_else(|| malformed("Anthropic tool name is invalid."))?,
                        ),
                        &block["input"].to_string(),
                    )?,
                );
            }
            "redacted_thinking" => {
                if !block["data"].is_string() {
                    return Err(malformed("Anthropic redacted thinking is invalid."));
                }
            }
            _ => return Err(malformed("Anthropic content block is unsupported.")),
        }
    }
    writer.finish_reason = Some(gateway_provider_writer::writer::anthropic_reason(
        value["stop_reason"].as_str().unwrap_or("unknown"),
    ));
    writer.usage = gateway_provider_source::usage::anthropic_to_responses(&value["usage"]);
    events.extend(writer.finish()?);
    gateway_provider_writer::finalize::native_stop_error(
        &mut events,
        value["stop_reason"].as_str().unwrap_or("unknown"),
    )?;
    let body = events
        .last()
        .and_then(|event| event.field("response"))
        .ok_or_else(|| malformed("Anthropic terminal is absent."))?;
    Ok((body, events))
}


pub fn decode_response(
    plan: &DecodePlan,
    status: u16,
    headers: &Headers,
    bytes: &[u8],
) -> Result<DecodedResponse, GatewayError> {
    let response_plan = response_plan(plan, headers);
    let plan = &response_plan;
    let mut output_headers = Headers::from([
        ("Content-Type".into(), "application/json".into()),
        (
            "x-uos-upstream".into(),
            gateway_provider_source::policy::direct_provider_label(plan.provider).into(),
        ),
    ]);
    for (key, value) in headers {
        let lower = key.to_lowercase();
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
            output_headers.insert(key.clone(), value.clone());
        }
    }
    if status >= 400 && plan.provider == UpstreamProvider::ChatgptCodex {
        if let Some(content_type) = header(headers, "content-type") {
            output_headers.insert("Content-Type".into(), content_type.into());
        }
        return Ok(DecodedResponse {
            status,
            headers: output_headers,
            body: s::scalar_json(&serde_json::from_slice::<Value>(bytes).unwrap_or(Value::Null))?,
            events: vec![],
            terminal: Some(SemanticTerminal::Error),
            usage: None,
            native_replay: None,
            raw_body: Some(bytes.to_vec()),
        });
    }
    if is_event_stream(plan.provider, status, headers) {
        decode_stream_response(plan, status, bytes, output_headers)
    } else {
        decode_buffered_response(plan, status, headers, bytes, output_headers)
    }
}


fn decode_buffered_response(
    plan: &DecodePlan,
    status: u16,
    headers: &Headers,
    bytes: &[u8],
    mut output_headers: Headers,
) -> Result<DecodedResponse, GatewayError> {
    let value: Value = serde_json::from_slice(bytes)
        .map_err(|_| malformed("Upstream response was not valid JSON."))?;
    let mut buffered_plan = plan.clone();
    if plan.decode == ResponseDecode::AnthropicChatToResponses {
        if let Some(id) = value["id"].as_str() {
            buffered_plan.response_id = translated_response_id(id);
        }
    } else if plan.decode == ResponseDecode::ChatToResponses(ChatProfile::Cerebras)
        && translated_provider_request_id(plan.provider, headers).is_none()
        && let Some(id) = value["id"]
            .as_str()
            .filter(|id| valid_provider_request_id(id))
    {
        buffered_plan.response_id = translated_response_id(id);
    }
    let plan = &buffered_plan;
    if let Some(refusal) =
        gateway_provider_writer::finalize::lithos_stream_refusal(plan, status, &output_headers, &value)?
    {
        return Ok(refusal);
    }
    if status >= 400 {
        return Ok(DecodedResponse {
            status,
            headers: output_headers,
            body: s::scalar_json(&if value["error"].is_object() {
                value
            } else {
                json!({"error":{"message":format!("Upstream request failed ({status})."),"type":"server_error","code":"upstream_error","param":null}})
            })?,
            events: vec![],
            terminal: Some(SemanticTerminal::Error),
            usage: None,
            native_replay: None,
            raw_body: None,
        });
    }
    let mut events = vec![];
    let mut native_replay = None;
    let value = &value;
    let body = match plan.decode {
        ResponseDecode::Chat
        | ResponseDecode::ChatToResponses(_)
        | ResponseDecode::AnthropicChatToResponses => {
            decode_buffered_chat(value, plan, &mut events, &mut native_replay)?
        }
        ResponseDecode::AnthropicToResponses => {
            let (body, translated) = decode_anthropic(value, plan)?;
            events = if plan.stream { translated } else { vec![] };
            if s::present(&body, "status").is_some_and(|observed| s::is(&observed, "completed")) {
                native_replay = Some(json!({"role":"assistant","content":value["content"]}));
            }
            if plan.surface == ClientSurface::Chat {
                gateway_provider_protocol::source_response::to_chat(&body, plan)?
            } else {
                body
            }
        }
        ResponseDecode::Responses | ResponseDecode::ResponsesToChat => {
            if !value.is_object() {
                return Err(malformed("Upstream Responses body is invalid."));
            }
            if value["status"] == "completed" && !gateway_provider_source::policy::response_has_answer(value)? {
                return Err(empty_completion());
            }
            if plan.decode == ResponseDecode::ResponsesToChat {
                response_to_chat(value, plan)?
            } else {
                s::scalar_json(value)?
            }
        }
        ResponseDecode::Voyage => crate::source_embeddings::decode_embeddings(value, plan)?,
        ResponseDecode::SystemOne => s::scalar_json(&gateway_provider_services::extras::decode_systemone(value)?)?,
        ResponseDecode::Decisions => {
            gateway_decision_protocol::response::decode(&s::scalar_json(value)?)?
        }
    };
    if plan.stream && !events.is_empty() {
        output_headers.insert("Content-Type".into(), "text/event-stream".into());
    }
    let terminal = terminal_for(&body).or_else(|| {
        matches!(
            plan.surface,
            ClientSurface::Chat
                | ClientSurface::Embeddings
                | ClientSurface::SystemOne
                | ClientSurface::Decisions
        )
        .then_some(SemanticTerminal::Completed)
    });
    let usage = s::present(&body, "usage").as_ref().and_then(gateway_provider_source::source_collector::usage);
    Ok(DecodedResponse {
        status,
        headers: output_headers,
        body,
        events,
        terminal,
        usage,
        native_replay,
        raw_body: None,
    })
}


fn decode_buffered_chat(
    value: &Value,
    plan: &DecodePlan,
    events: &mut Vec<ProviderEvent>,
    native_replay: &mut Option<Value>,
) -> Result<SourceJson, GatewayError> {
    let profile = match plan.decode {
        ResponseDecode::ChatToResponses(profile) => profile,
        _ => ChatProfile::Claude,
    };
    let mut projected = value.clone();
    if plan.decode == ResponseDecode::AnthropicChatToResponses {
        let reasoning =
            gateway_provider_collector::claude_chat_stream::readable_reasoning(&value["choices"][0]["message"]);
        if !reasoning.is_empty() {
            projected["choices"][0]["message"]["reasoning_content"] = json!(reasoning);
        }
    }
    let completion = normalize_chat_completion_fragment(&projected, plan)?;
    if plan.provider == UpstreamProvider::Cerebras && !gateway_provider_source::policy::chat_has_answer(&completion) {
        return Err(cerebras_empty());
    }
    if plan.decode == ResponseDecode::Chat {
        if plan.stream {
            *events = gateway_provider_writer::finalize::buffered_chat_events(&completion, plan)?;
        }
        return Ok(chat_model_body(&completion, plan)?);
    }
    let body = chat_completion_to_responses(&completion, plan, profile)?;
    let completed = s::present(&body, "status").is_some_and(|observed| s::is(&observed, "completed"));
    if completed && !gateway_provider_source::source_policy::response_has_answer(&body) {
        return Err(empty_completion());
    }
    if plan.stream {
        *events = replay_chat(&completion, plan, profile)?;
    }
    if plan.decode == ResponseDecode::AnthropicChatToResponses && completed {
        *native_replay = value["choices"][0].get("message").cloned();
    }
    if plan.surface == ClientSurface::Chat {
        gateway_provider_protocol::source_response::to_chat(&body, plan)
    } else {
        Ok(body)
    }
}


fn empty_completion() -> GatewayError {
    GatewayError::new(
        502,
        "Upstream response completed with no translated semantic output.",
        Some("empty_upstream_completion"),
    )
    .with_type("server_error")
}


fn cerebras_empty() -> GatewayError {
    GatewayError::new(
        502,
        "Cerebras upstream returned an invalid completion.",
        Some("cerebras_upstream_invalid_response"),
    )
    .with_type("server_error")
}
