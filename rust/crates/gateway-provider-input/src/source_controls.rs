use gateway_provider_source::source as s;
use gateway_provider_protocol::types::ClientSurface;
use gateway_provider_protocol::types::OrdinaryPreparationContext;
use gateway_core::js_string::JsString16;
use gateway_core::model::ModelId;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;
use gateway_schema::source_request::SourceField;
use gateway_schema::source_request::SourceRequestDocument;
pub fn replay(input: &SourceJson) -> Result<SourceJson, gateway_core::error::GatewayError> {
    let Some(items) = json::array(input) else { return Ok(input.clone()); };
    let mut output = Vec::new();
    for mut item in items {
        let synthetic = s::present(&item, "id").and_then(|value| s::control(&value)).is_some_and(|id| gateway_provider_policy::request::codex_synthetic_id(&id));
        if !synthetic { output.push(item); continue; }
        if s::present(&item, "type").is_some_and(|value| s::is(&value, "reasoning"))
            && s::present(&item, "encrypted_content").and_then(|value| s::string(&value)).is_none_or(|value| value.as_units().is_empty()) {
            continue;
        }
        if json::entries(&item).is_some() { s::remove(&mut item, "id")?; }
        output.push(item);
    }
    s::array(&output)
}



pub fn codex(raw: &SourceJson) -> s::Result<SourceJson> {
    let model = s::present(raw, "model")
        .filter(|value| s::string(value).is_some())
        .ok_or_else(|| s::invalid(JsString16::from_utf8("model must be a string"), "model"))?;
    let input = s::present(raw, "input")
        .ok_or_else(|| s::invalid(JsString16::from_utf8("input is required"), "input"))?;
    let mut body = s::object(vec![
        ("model", model),
        ("input", replay(&input)?),
        ("stream", s::scalar_json(&serde_json::json!(true))?),
        ("store", s::scalar_json(&serde_json::json!(false))?),
    ])?;
    for key in [
        "instructions",
        "reasoning",
        "max_output_tokens",
        "tools",
        "tool_choice",
        "parallel_tool_calls",
        "prompt_cache_key",
        "prompt_cache_options",
        "prompt_cache_retention",
        "text",
        "include",
        "context_management",
    ] {
        if let Some(value) = s::present(raw, key) {
            s::set(&mut body, key, value)?;
        }
    }
    ultra(&mut body)?;
    if let Some(mut tools) = s::present(&body, "tools").and_then(|observed| json::array(&observed)) {
        for tool in &mut tools {
            if s::present(tool, "type").is_some_and(|observed| s::is(&observed, "function"))
                && let Some(mut function) = s::present(tool, "function").filter(|value| json::entries(value).is_some()) {
                s::set(&mut function, "type", s::text("function")?)?;
                *tool = function;
            }
        }
        s::set(&mut body, "tools", s::array(&tools)?)?;
    }
    if let Some(choice) = s::present(&body, "tool_choice")
        && s::present(&choice, "type").is_some_and(|value| s::is(&value, "function"))
        && s::present(&choice, "function").and_then(|value| json::entries(&value)).is_some() {
        let mut fields = vec![("type", s::text("function")?)];
        // JS omits an absent `name`; an explicitly supplied null remains a null field.
        if let Some(name) = s::present_path(&choice, &["function", "name"]) { fields.push(("name", name)); }
        s::set(&mut body, "tool_choice", s::object(fields)?)?;
    }
    Ok(body)
}


/// One ordinary phase coordinator; direct/OpenRouter branches never enter it.
pub fn ordinary(
    raw: &SourceJson,
    surface: ClientSurface,
    model: &ModelId,
    context: Option<&OrdinaryPreparationContext>,
) -> s::Result<SourceJson> {
    let document = SourceRequestDocument::new(raw.clone())?;
    let model_raw = match document.model_request()? {
        gateway_schema::source_request::SourceModelRequest::Default => model.as_js_string().clone(),
        gateway_schema::source_request::SourceModelRequest::Named(name) => name,
    };
    let mut body = if surface == ClientSurface::Chat {
        let view = document.for_surface(gateway_schema::request::RequestSurface::ChatCompletions);
        let messages = s::present(raw, "messages").and_then(|value| json::array(&value)).ok_or_else(|| {
            gateway_local_refusal::source_error::SourceRequestError::new(
                400,
                "messages must be an array",
                "invalid_request_error",
                Some("invalid_request_error"),
                None,
            )
        })?;
        if messages.is_empty() {
            return Err(
                gateway_local_refusal::source_error::SourceRequestError::new(
                    400,
                    "messages must be a non-empty array",
                    "invalid_request_error",
                    Some("invalid_request_error"),
                    None,
                ),
            );
        }
        // Chat effort syntax and stream options precede content conversion; metadata mapping does not.
        let effort = view.reasoning_effort()?;
        let stream = view.stream()?;
        view.chat_stream_options()?;
        let normalized = document.normalize_chat_for_codex()?;
        validate_cache(context, &model_raw, raw, &normalized.input, "messages")?;
        let mut body = s::object(vec![
            ("model", s::quoted(model.as_js_string())?),
            ("input", s::array(&normalized.input)?),
            ("stream", s::scalar_json(&serde_json::json!(stream))?),
            ("store", s::scalar_json(&serde_json::json!(false))?),
        ])?;
        if let Some(instructions) = normalized.instructions {
            s::set(&mut body, "instructions", s::quoted(&instructions)?)?;
        }
        for key in [
            "tools",
            "tool_choice",
            "parallel_tool_calls",
            "prompt_cache_key",
            "prompt_cache_options",
            "prompt_cache_retention",
        ] {
            if let Some(value) = s::present(raw, key) {
                s::set(&mut body, key, value)?;
            }
        }
        let effort = effort.or_else(|| context.map(|context| context.default_effort().clone()));
        if let Some(effort) = effort {
            s::set(
                &mut body,
                "reasoning",
                s::object(vec![("effort", map_effort(&effort, context)?)])?,
            )?;
        }
        if let Some(max) =
            s::present(raw, "max_completion_tokens").filter(|value| !s::is_null(value))
        {
            s::set(&mut body, "max_output_tokens", max)?;
        }
        if let Some(format) = s::present(raw, "response_format")
            && s::canonical(&format)?.as_str() == r#"{"type":"json_object"}"# {
            s::set(
                &mut body,
                "text",
                s::scalar_json(&serde_json::json!({"format":{"type":"json_object"}}))?,
            )?;
        }
        body
    } else {
        let input = document.normalize_responses_input()?;
        validate_cache(context, &model_raw, raw, &input, "input")?;
        let reasoning = document.reasoning()?;
        let instructions = document.instructions()?;
        // Both syntax phases finish before selected-model effort mapping/default injection.
        let reasoning = match reasoning {
            Some(mut value) => {
                if let Some(effort) = s::present(&value, "effort").and_then(|observed| s::string(&observed)) {
                    s::set(&mut value, "effort", map_effort(&effort, context)?)?;
                }
                Some(value)
            }
            None => match context {
                Some(context) => Some(s::object(vec![("effort", map_effort(context.default_effort(), Some(context))?)])?),
                None => None,
            },
        };
        let mut body = raw.clone();
        s::set(&mut body, "input", s::array(&input)?)?;
        if let Some(value) = reasoning {
            s::set(&mut body, "reasoning", value)?;
        } else {
            s::remove(&mut body, "reasoning")?;
        }
        match instructions {
            SourceField::Missing => (),
            SourceField::Null => s::remove(&mut body, "instructions")?,
            SourceField::Value(text) => s::set(&mut body, "instructions", s::quoted(&text)?)?,
        }
        body
    };
    s::set(&mut body, "model", s::quoted(model.as_js_string())?)?;
    Ok(body)
}


fn map_effort(effort: &JsString16, context: Option<&OrdinaryPreparationContext>) -> Result<SourceJson, gateway_core::error::GatewayError> {
    Ok(s::quoted(&context.map_or_else(
        || effort.clone(),
        |context| gateway_schema::reasoning::effort_for_codex(effort, context.reasoning()),
    ))?)
}


fn validate_cache(
    context: Option<&OrdinaryPreparationContext>,
    model: &JsString16,
    raw: &SourceJson,
    input: &[SourceJson],
    input_key: &str,
) -> s::Result<()> {
    if let Some(context) = context {
        gateway_schema::source_request::cache_capability::validate_known_unsupported_prompt_cache(
            model,
            context.prompt_cache(),
            raw,
            input,
            input_key,
        )?;
    }
    Ok(())
}


pub fn ultra(body: &mut SourceJson) -> Result<(), gateway_core::error::GatewayError> {
    if let Some(mut reasoning) = s::present(body, "reasoning")
        && s::present(&reasoning, "effort").is_some_and(|observed| s::is(&observed, "ultra")) {
        s::set(&mut reasoning, "effort", s::text("max")?)?;
        s::set(body, "reasoning", reasoning)?;
    }
    Ok(())
}


pub fn openrouter(body: &mut SourceJson, model: &ModelId) -> Result<(), gateway_core::error::GatewayError> {
    if (s::starts(model.as_js_string(), "openai/") || s::starts(model.as_js_string(), "~openai/"))
        && !model
            .as_js_string()
            .as_units()
            .split(|&unit| unit == 58)
            .any(|part| part.iter().copied().eq("batch".encode_utf16()))
        && s::present(body, "tool_choice").is_none_or(|v| s::is(&v, "auto"))
        && s::present(body, "functions").is_none()
        && s::present(body, "function_call").is_none()
        && s::present(body, "tools").is_none_or(|v| json::array(&v).is_some_and(|a| !a.is_empty()))
    {
        let mut tools = s::present(body, "tools").and_then(|observed| json::array(&observed)).unwrap_or_default();
        for kind in ["web_search", "web_fetch"] {
            if !tools.iter().any(|tool| {
                s::present(tool, "type").and_then(|observed| s::control(&observed)).is_some_and(|name| {
                    name == kind
                        || name == format!("openrouter:{kind}")
                        || name.starts_with(&format!("{kind}_"))
                })
            }) {
                tools.push(s::object(vec![(
                    "type",
                    s::text(&format!("openrouter:{kind}"))?,
                )])?);
            }
        }
        s::set(body, "tools", s::array(&tools)?)?;
    }
    if (s::starts(model.as_js_string(), "anthropic/claude-")
        || s::starts(model.as_js_string(), "~anthropic/claude-"))
        && s::present(body, "cache_control").is_none()
        && !s::present_path(body, &["prompt_cache_options", "mode"]).is_some_and(|observed| s::is(&observed, "explicit"))
        && !cache_block(body)
    {
        s::set(
            body,
            "cache_control",
            s::scalar_json(&serde_json::json!({"type":"ephemeral"}))?,
        )?;
    }
    Ok(())
}


fn cache_block(value: &SourceJson) -> bool {
    if let Some(array) = json::array(value) {
        array.iter().any(cache_block)
    } else if let Some(fields) = json::entries(value) {
        s::present(value, "cache_control").is_some()
            || s::present(value, "prompt_cache_breakpoint").is_some()
            || fields
                .iter()
                .filter(|(key, _)| {
                    [
                        "instructions",
                        "input",
                        "messages",
                        "tools",
                        "content",
                        "output",
                        "function",
                    ]
                    .iter()
                    .any(|name| key.as_units().iter().copied().eq(name.encode_utf16()))
                })
                .any(|(_, value)| cache_block(value))
    } else {
        false
    }
}


pub fn strip_codex(body: &mut SourceJson, warnings: &mut Vec<String>) -> Result<(), gateway_core::error::GatewayError> {
    for (field, warning) in [
        ("prompt_cache_options", "prompt_cache_options_ignored"),
        ("prompt_cache_retention", "prompt_cache_retention_ignored"),
        ("max_output_tokens", "max_output_tokens_ignored"),
        ("max_completion_tokens", "max_output_tokens_ignored"),
    ] {
        if s::present(body, field).is_some() {
            s::remove(body, field)?;
            if !warnings.iter().any(|value| value == warning) { warnings.push(warning.into()); }
        }
    }
    if let Some(mut input) = s::present(body, "input") {
        if strip_breakpoints(&mut input)? { warnings.push("prompt_cache_breakpoint_ignored".into()); }
        s::set(body, "input", input)?;
    }
    Ok(())
}

fn strip_breakpoints(value: &mut SourceJson) -> Result<bool, gateway_core::error::GatewayError> {
    let mut removed = false;
    if let Some(mut array) = json::array(value) {
        for child in &mut array { removed = strip_breakpoints(child)? || removed; }
        *value = s::array(&array)?;
    } else if json::entries(value).is_some() {
        removed = s::present(value, "prompt_cache_breakpoint").is_some();
        s::remove(value, "prompt_cache_breakpoint")?;
        for key in ["content", "output"] {
            if let Some(mut child) = s::present(value, key) {
                removed = strip_breakpoints(&mut child)? || removed;
                s::set(value, key, child)?;
            }
        }
    }
    Ok(removed)
}
