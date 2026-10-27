use gateway_provider_protocol::replay::ReplayEntry;
use gateway_provider_protocol::replay::ReplayTransport;
use gateway_provider_source::source as s;
use gateway_provider_tool_grammar::source_tools as source_tools;
use gateway_provider_tool_grammar::source_tools::ToolMap;
use gateway_provider_protocol::types::PrepareOptions;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;

pub fn eq(value: &SourceJson, literal: &str) -> Result<bool, gateway_core::error::GatewayError> {
    Ok(s::canonical(value)?.as_str() == literal)
}


pub fn only(value: &SourceJson, keys: &[&str]) -> bool {
    json::entries(value).is_some_and(|entries| {
        entries.iter().all(|(key, _)| {
            keys.iter()
                .any(|name| key.as_units().iter().copied().eq(name.encode_utf16()))
        })
    })
}


pub fn base(raw: &SourceJson) -> bool {
    s::present(raw, "model").and_then(|observed| s::string(&observed)).is_some_and(|model| {
        gateway_provider_source::anthropic::is_claude_model(&gateway_core::model::ModelId::from_source(model))
    }) && s::present(raw, "store").is_none_or(|v| s::boolean(&v) == Some(false))
        && s::present(raw, "previous_response_id").is_none_or(|v| s::is_null(&v))
        && [
            "context_management",
            "max_tool_calls",
            "truncation",
            "metadata",
            "service_tier",
            "safety_identifier",
            "user",
        ]
        .iter()
        .all(|key| s::present(raw, key).is_none())
        && s::present(raw, "include").is_none_or(|value| {
            json::array(&value).is_some_and(|values| {
                values
                    .iter()
                    .all(|value| s::is(value, "reasoning.encrypted_content"))
            })
        })
}


fn cache(value: &SourceJson) -> Option<SourceJson> {
    (only(value, &["type", "ttl"])
        && s::present(value, "type").is_some_and(|observed| s::is(&observed, "ephemeral"))
        && s::present(value, "ttl").is_none_or(|v| s::is(&v, "5m") || s::is(&v, "1h")))
    .then(|| value.clone())
}


pub fn block(mut block: SourceJson, source: &SourceJson) -> Result<Option<SourceJson>, gateway_core::error::GatewayError> {
    Ok({
    if let Some(marker) = s::present(source, "prompt_cache_breakpoint") {
        if !eq(&marker, r#"{"mode":"explicit"}"#)? {
            return Ok(None);
        }
        s::set(
            &mut block,
            "cache_control",
            s::scalar_json(&serde_json::json!({"type":"ephemeral"}))?,
        )?;
    }
    if let Some(control) = s::present(source, "cache_control") {
        s::set(&mut block, "cache_control", represented!(cache(&control)))?;
    }
    Some(block)

    })
}


pub fn policy(raw: &SourceJson) -> bool {
    s::present(raw, "prompt_cache_retention").is_none_or(|v| s::is(&v, "in_memory"))
        && s::present(raw, "prompt_cache_options").is_none_or(|v| {
            only(&v, &["mode"])
                && s::present(&v, "mode")
                    .is_none_or(|v| s::is(&v, "implicit") || s::is(&v, "explicit"))
        })
        && s::present(raw, "cache_control").is_none_or(|v| cache(&v).is_some())
}


fn count(value: &SourceJson) -> usize {
    if let Some(values) = json::array(value) {
        values.iter().map(count).sum()
    } else if let Some(fields) = json::entries(value) {
        usize::from(s::present(value, "cache_control").is_some())
            + fields
                .iter()
                .filter(|(key, _)| {
                    !["parameters", "input_schema"]
                        .iter()
                        .any(|name| key.as_units().iter().copied().eq(name.encode_utf16()))
                })
                .map(|(_, value)| count(value))
                .sum::<usize>()
    } else {
        0
    }
}


pub fn text(value: SourceJson) -> Result<SourceJson, gateway_core::error::GatewayError> {
    Ok({
    s::object(vec![("type", s::text("text")?), ("text", value)])?

    })
}


pub fn content(value: &SourceJson, native: bool) -> Result<Option<Vec<SourceJson>>, gateway_core::error::GatewayError> {
    Ok({
    if s::string(value).is_some() {
        return Ok(Some(vec![text(value.clone())?]));
    }
    let mut blocks = vec![];
    for part in represented!(json::array(value)) {
        let kind = represented!(s::present(&part, "type").and_then(|observed| s::control(&observed)));
        let result = match kind.as_str() {
            "input_text" | "output_text" | "text" => {
                let value = represented!(s::present(&part, "text"));
                represented!(s::string(&value));
                text(value)?
            }
            "input_image" => {
                if s::present(&part, "file_id").is_some() {
                    return Ok(None);
                }
                let url = represented!(s::present(&part, "image_url"));
                represented!(s::string(&url));
                if native {
                    if s::present(&part, "detail")
                        .is_some_and(|v| !s::is_null(&v) && !s::is(&v, "auto"))
                    {
                        return Ok(None);
                    }
                    let value = represented!(s::control(&url));
                    let source = if let Some(data) = value.strip_prefix("data:") {
                        let (kind, data) = represented!(data.split_once(";base64,"));
                        if !["image/jpeg", "image/png", "image/gif", "image/webp"].contains(&kind) {
                            return Ok(None);
                        }
                        s::object(vec![
                            ("type", s::text("base64")?),
                            ("media_type", s::text(kind)?),
                            ("data", s::text(data)?),
                        ])?
                    } else if value.starts_with("https://") || value.starts_with("http://") {
                        s::object(vec![("type", s::text("url")?), ("url", url)])?
                    } else {
                        return Ok(None);
                    };
                    s::object(vec![("type", s::text("image")?), ("source", source)])?
                } else {
                    let mut reference = s::object(vec![("url", url)])?;
                    if let Some(detail) = s::present(&part, "detail") {
                        s::set(&mut reference, "detail", detail)?;
                    }
                    s::object(vec![
                        ("type", s::text("image_url")?),
                        ("image_url", reference),
                    ])?
                }
            }
            _ => return Ok(None),
        };
        blocks.push(represented!(block(result, &part)?));
    }
    Some(blocks)

    })
}


pub fn merge(messages: &mut Vec<SourceJson>, role: &str, blocks: Vec<SourceJson>) -> Result<(), gateway_core::error::GatewayError> {
    Ok({
    if let Some(last) = messages
        .last_mut()
        .filter(|value| s::present(value, "role").is_some_and(|observed| s::is(&observed, role)))
    {
        let mut old = s::present(last, "content").and_then(|observed| json::array(&observed)).unwrap_or_default();
        old.extend(blocks);
        s::set(last, "content", s::array(&old)?)?;
    } else {
        messages.push(s::object(vec![
            ("role", s::text(role)?),
            ("content", s::array(&blocks)?),
        ])?);
    }

    })
}


pub fn valid_name(value: &SourceJson) -> bool {
    s::control(value).is_some_and(|name| {
        !name.is_empty()
            && name.len() <= 64
            && name
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
    })
}


pub fn valid_reasoning(reasoning: &SourceJson) -> bool {
    only(reasoning, &["effort", "summary"])
        && s::present(reasoning, "summary").is_none_or(|v| {
            ["auto", "detailed", "none"]
                .iter()
                .any(|value| s::is(&v, value))
        })
}


pub fn add_cache(body: &mut SourceJson, raw: &SourceJson, native: bool) -> Result<Option<()>, gateway_core::error::GatewayError> {
    Ok({
    let count = count(body);
    if count > 4 {
        return Ok(None);
    }
    if count == 0 && !s::present_path(raw, &["prompt_cache_options", "mode"]).is_some_and(|observed| s::is(&observed, "explicit")) {
        let control = match s::present(raw, "cache_control") { Some(value) => value, None => s::scalar_json(&serde_json::json!({"type":"ephemeral"}))? };
        let mut messages = represented!(s::present(body, "messages").and_then(|observed| json::array(&observed)));
        if let Some(last) = messages.last_mut()
            && let Some(mut blocks) =
                s::present(last, "content").and_then(|observed| json::array(&observed)).filter(|blocks| !blocks.is_empty())
        {
            s::set(represented!(blocks.last_mut()), "cache_control", control)?;
            s::set(last, "content", s::array(&blocks)?)?;
            s::set(body, "messages", s::array(&messages)?)?;
            return Ok(Some(()));
        }
        if native {
            let mut system = represented!(s::present(body, "system").and_then(|observed| json::array(&observed)));
            s::set(represented!(system.last_mut()), "cache_control", control)?;
            s::set(body, "system", s::array(&system)?)?;
        } else {
            let mut tools = represented!(s::present(body, "tools").and_then(|observed| json::array(&observed)));
            let last = represented!(tools.last_mut());
            let mut function = represented!(s::present(last, "function"));
            s::set(&mut function, "cache_control", control)?;
            s::set(last, "function", function)?;
            s::set(body, "tools", s::array(&tools)?)?;
        }
    }
    Some(())

    })
}


fn chat_tool(tool: &SourceJson) -> Result<bool, gateway_core::error::GatewayError> {
    Ok({
    let valid = s::present(tool, "name").is_some_and(|observed| valid_name(&observed));
    match s::present(tool, "type").and_then(|observed| s::control(&observed)).as_deref() {
        Some("function") => valid,
        Some("custom") => {
            valid && s::present(tool, "format").is_none_or(|value| eq(&value, r#"{"type":"text"}"#)?)
        }
        Some("namespace") => {
            valid
                && s::present(tool, "tools").and_then(|observed| json::array(&observed)).is_some_and(|tools| {
                    tools.iter().all(|tool| {
                        s::present(tool, "type").is_some_and(|observed| s::is(&observed, "function"))
                            && s::present(tool, "name").is_some_and(|observed| valid_name(&observed))
                    })
                })
        }
        _ => false,
    }

    })
}


pub fn chat(
    raw: &SourceJson,
    options: &PrepareOptions,
) -> Result<Option<gateway_provider_protocol::source_chat::Projection>, gateway_core::error::GatewayError> {
    Ok({
    if !base(raw) || !policy(raw) {
        return Ok(None);
    }
    if let Some(tools) = s::present(raw, "tools")
        && represented!(json::array(&tools)).iter().any(|tool| !chat_tool(tool)?)
    {
        return Ok(None);
    }
    let (tools, names) = match gateway_provider_tool_grammar::source_tools::flatten(s::present(raw, "tools").as_ref(), 64) { Ok(value) => value, Err(gateway_provider_tool_grammar::source_tools::ToolError::Request(_)) => return Ok(None), Err(gateway_provider_tool_grammar::source_tools::ToolError::Construction(error)) => return Err(error) };
    let mut messages = vec![];
    if let Some(instructions) = s::present(raw, "instructions").filter(|v| !s::is_null(v)) {
        messages.push(s::object(vec![
            ("role", s::text("system")?),
            ("content", s::array(&represented!(content(&instructions, false)?))?),
        ])?);
    }
    let input = represented!(s::present(raw, "input"));
    if let Some(restored) = &options.restored_native_messages {
        messages.extend(restored.iter().cloned());
    } else if s::string(&input).is_some() {
        messages.push(s::object(vec![
            ("role", s::text("user")?),
            ("content", s::array(&[text(input)?])?),
        ])?);
    } else {
        for item in represented!(json::array(&input)) {
            represented!(append_chat_item(&item, &mut messages, &names)?);
        }
    }
    let stream = s::present(raw, "stream").and_then(|observed| s::boolean(&observed)) == Some(true);
    let mut body = s::object(vec![
        ("model", represented!(s::present(raw, "model"))),
        ("messages", s::array(&messages)?),
        ("stream", s::scalar_json(&serde_json::json!(stream))?),
    ])?;
    if stream {
        s::set(
            &mut body,
            "stream_options",
            s::scalar_json(&serde_json::json!({"include_usage":true}))?,
        )?;
    }
    if !tools.is_empty() {
        s::set(&mut body, "tools", s::array(&tools)?)?;
    }
    if let Some(max) = s::present(raw, "max_output_tokens")
        .and_then(|value| json::number(&value))
        .filter(|v| *v >= 0.0 && v.fract() == 0.0)
        .map(|v| v as u64)
        .or(options.catalog_max_output_tokens)
    {
        if max == 0 {
            return Ok(None);
        }
        s::set(&mut body, "max_tokens", s::scalar_json(&serde_json::json!(max))?)?;
    }
    if let Some(reasoning) = s::present(raw, "reasoning").filter(|v| !s::is_null(v)) {
        if !valid_reasoning(&reasoning) {
            return Ok(None);
        }
        if let Some(effort) = s::present(&reasoning, "effort") {
            if s::trim(&represented!(s::string(&effort))).as_units().is_empty() {
                return Ok(None);
            }
            s::set(&mut body, "reasoning_effort", effort)?;
        }
    }
    if let Some(choice) =
        match gateway_provider_tool_grammar::source_tools::choice(s::present(raw, "tool_choice").as_ref(), &names) { Ok(value) => value, Err(gateway_provider_tool_grammar::source_tools::ToolError::Request(_)) => return Ok(None), Err(gateway_provider_tool_grammar::source_tools::ToolError::Construction(error)) => return Err(error) }
    {
        s::set(&mut body, "tool_choice", choice)?;
    }
    if let Some(format) = s::present_path(raw, &["text", "format"]) {
        match represented!(s::present(&format, "type").and_then(|observed| s::control(&observed)).as_deref()) {
            "text" => (),
            "json_object" => s::set(
                &mut body,
                "response_format",
                s::scalar_json(&serde_json::json!({"type":"json_object"}))?,
            )?,
            "json_schema" => {
                let mut schema = format;
                s::remove(&mut schema, "type")?;
                s::set(
                    &mut body,
                    "response_format",
                    s::object(vec![
                        ("type", s::text("json_schema")?),
                        ("json_schema", schema),
                    ])?,
                )?;
            }
            _ => return Ok(None),
        }
    }
    for key in [
        "parallel_tool_calls",
        "temperature",
        "top_p",
        "prompt_cache_key",
    ] {
        if let Some(value) = s::present(raw, key) {
            s::set(&mut body, key, value)?;
        }
    }
    represented!(add_cache(&mut body, raw, false)?);
    Some(gateway_provider_protocol::source_chat::Projection {
        body,
        names,
        elisions: vec![],
        warnings: vec![],
        exemptions: vec![],
        admission: None,
    })

    })
}


fn append_chat_item(
    item: &SourceJson,
    messages: &mut Vec<SourceJson>,
    names: &ToolMap,
) -> Result<Option<()>, gateway_core::error::GatewayError> {
    Ok({
    let kind = s::present(item, "type").and_then(|observed| s::control(&observed)).unwrap_or_else(|| "message".into());
    match kind.as_str() {
        "message" => {
            let role = represented!(s::present(item, "role").and_then(|observed| s::control(&observed)));
            messages.push(s::object(vec![
                (
                    "role",
                    s::text(if role == "developer" { "system" } else { &role })?,
                ),
                (
                    "content",
                    s::array(&represented!(content(&represented!(s::present(item, "content")), false)?))?,
                ),
            ])?);
        }
        "function_call" | "custom_tool_call" => {
            let name = represented!(s::present(item, "name").and_then(|observed| s::string(&observed)));
            let namespace = s::present(item, "namespace").and_then(|observed| s::string(&observed));
            let projected = names
                .iter()
                .find(|(_, identity)| identity.name == name && identity.namespace == namespace)
                .map(|(wire, _)| wire.clone())
                .unwrap_or(name);
            let arguments = if kind == "custom_tool_call" {
                let input = represented!(s::present(item, "input"));
                represented!(s::string(&input));
                s::text(
                    s::canonical(&s::object(vec![("input", input)])?)
                        ?
                        .as_str(),
                )?
            } else {
                let value = represented!(s::present(item, "arguments"));
                represented!(s::string(&value));
                value
            };
            let id = represented!(s::present(item, "call_id"));
            represented!(s::string(&id));
            let call = s::object(vec![
                ("id", id),
                ("type", s::text("function")?),
                (
                    "function",
                    s::object(vec![
                        ("name", s::quoted(&projected)?),
                        ("arguments", arguments),
                    ])?,
                ),
            ])?;
            if let Some(previous) = messages.last_mut().filter(|value| {
                s::present(value, "role").is_some_and(|observed| s::is(&observed, "assistant"))
                    && s::present(value, "tool_calls").and_then(|observed| json::array(&observed)).is_some()
            }) {
                let mut calls = represented!(s::present(previous, "tool_calls").and_then(|observed| json::array(&observed)));
                calls.push(call);
                s::set(previous, "tool_calls", s::array(&calls)?)?;
            } else {
                messages.push(s::object(vec![
                    ("role", s::text("assistant")?),
                    ("content", s::null()?),
                    ("tool_calls", s::array(&[call])?),
                ])?);
            }
        }
        "function_call_output" | "custom_tool_call_output" => {
            let id = represented!(s::present(item, "call_id"));
            represented!(s::string(&id));
            messages.push(s::object(vec![
                ("role", s::text("tool")?),
                ("tool_call_id", id),
                (
                    "content",
                    s::array(&represented!(content(&represented!(s::present(item, "output")), false)?))?,
                ),
            ])?);
        }
        _ => return Ok(None),
    }
    Some(())

    })
}


pub fn replay_messages(
    raw: &SourceJson,
    entries: &[ReplayEntry],
) -> Result<Option<Vec<SourceJson>>, gateway_core::error::GatewayError> {
    Ok({
    let (_, names) = match gateway_provider_tool_grammar::source_tools::flatten(s::present(raw, "tools").as_ref(), 64) { Ok(value) => value, Err(gateway_provider_tool_grammar::source_tools::ToolError::Request(_)) => return Ok(None), Err(gateway_provider_tool_grammar::source_tools::ToolError::Construction(error)) => return Err(error) };
    let mut messages = vec![];

for entry in entries {
                match entry {
                    ReplayEntry::Original(item) => {
                        represented!(append_chat_item(item, &mut messages, &names)?);
                    }
                    ReplayEntry::NativeAssistant {
                        transport: ReplayTransport::Chat,
                        payload,
                    } if s::present(payload, "role").is_some_and(|observed| s::is(&observed, "assistant")) => {
                        messages.push(payload.clone());
                    }
                    ReplayEntry::NativeAssistant { .. } => return Ok(None),
                }
            }

    if messages.is_empty() {
        return Ok(None);
    }
    Some(messages)

    })
}
