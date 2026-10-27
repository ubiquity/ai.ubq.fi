use crate::types::ClaudeBodyOptions;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;
use gateway_provider_chat::source_anthropic::add_cache;
use gateway_provider_chat::source_anthropic::base;
use gateway_provider_chat::source_anthropic::block;
use gateway_provider_chat::source_anthropic::content;
use gateway_provider_chat::source_anthropic::eq;
use gateway_provider_chat::source_anthropic::merge;
use gateway_provider_chat::source_anthropic::only;
use gateway_provider_chat::source_anthropic::policy;
use gateway_provider_chat::source_anthropic::text;
use gateway_provider_chat::source_anthropic::valid_name;
use gateway_provider_chat::source_anthropic::valid_reasoning;
use gateway_provider_protocol::replay::ReplayEntry;
use gateway_provider_protocol::replay::ReplayTransport;
use gateway_provider_protocol::types::PrepareOptions;
use gateway_provider_source::source as s;

/// The native body algorithm cannot inspect auth material.
struct NativeBodyOptions<'a> {
    restored_native_messages: Option<&'a Vec<SourceJson>>,
    supports_native_effort: bool,
    supports_disabled_thinking: bool,
    native_reasoning_efforts: Option<&'a Vec<gateway_core::js_string::JsString16>>,
    catalog_max_output_tokens: Option<u64>,
}

impl<'a> From<&'a PrepareOptions> for NativeBodyOptions<'a> {
    fn from(options: &'a PrepareOptions) -> Self {
        Self {
            restored_native_messages: options.restored_native_messages.as_ref(),
            supports_native_effort: options.supports_native_effort,
            supports_disabled_thinking: options.supports_disabled_thinking,
            native_reasoning_efforts: options.native_reasoning_efforts.as_ref(),
            catalog_max_output_tokens: options.catalog_max_output_tokens,
        }
    }
}

fn native_messages(
    raw: &SourceJson,
    system: &mut Vec<SourceJson>,
    options: &NativeBodyOptions<'_>,
) -> Result<Option<Vec<SourceJson>>, gateway_core::error::GatewayError> {
    if let Some(restored) = &options.restored_native_messages {
        return Ok(restored_native_messages(restored, system))?;
    }
    let input = represented!(s::present(raw, "input"));
    if s::string(&input).is_some() {
        return Ok(Some(vec![s::object(vec![
            ("role", s::text("user")?),
            ("content", s::array(&[text(input)?])?),
        ])?]));
    }
    let mut messages = vec![];
    for item in represented!(json::array(&input)) {
        represented!(append_native_item(&item, system, &mut messages)?);
    }
    Ok((!messages.is_empty()).then_some(messages))
}

pub fn restored_native_messages(
    restored: &[SourceJson],
    system: &mut Vec<SourceJson>,
) -> Result<Option<Vec<SourceJson>>, gateway_core::error::GatewayError> {
    let mut messages = vec![];
    for item in restored {
        let role =
            represented!(s::present(item, "role").and_then(|observed| s::control(&observed)));
        let blocks =
            represented!(s::present(item, "content").and_then(|observed| json::array(&observed)));
        match role.as_str() {
            "system" | "developer" => system.extend(blocks),
            "user" | "assistant" => merge(&mut messages, &role, blocks)?,
            _ => return Ok(None),
        }
    }
    Ok((!messages.is_empty()).then_some(messages))
}

fn append_native_item(
    item: &SourceJson,
    system: &mut Vec<SourceJson>,
    messages: &mut Vec<SourceJson>,
) -> Result<Option<()>, gateway_core::error::GatewayError> {
    let kind = s::present(item, "type")
        .and_then(|observed| s::control(&observed))
        .unwrap_or_else(|| "message".into());
    match kind.as_str() {
        "message" => {
            let role =
                represented!(s::present(item, "role").and_then(|observed| s::control(&observed)));
            let blocks = represented!(content(&represented!(s::present(item, "content")), true)?);
            if role == "system" || role == "developer" {
                if blocks.iter().any(|block| {
                    !s::present(block, "type").is_some_and(|observed| s::is(&observed, "text"))
                }) {
                    return Ok(None);
                }
                system.extend(blocks);
            } else if role == "user" || role == "assistant" {
                merge(messages, &role, blocks)?;
            } else {
                return Ok(None);
            }
        }
        "function_call" => {
            if s::present(item, "namespace").is_some_and(|v| !s::is_null(&v))
                || !s::present(item, "name").is_some_and(|observed| valid_name(&observed))
            {
                return Ok(None);
            }
            // This is the existing native protocol's JSON.parse operation, not an opaque Chat argument rewrite.
            let arguments = represented!(
                s::present(item, "arguments").and_then(|observed| s::control(&observed))
            );
            let input = represented!(SourceJson::parse(&arguments).ok());
            represented!(json::entries(&input));
            let id = represented!(s::present(item, "call_id"));
            represented!(s::string(&id));
            merge(
                messages,
                "assistant",
                vec![s::object(vec![
                    ("type", s::text("tool_use")?),
                    ("id", id),
                    ("name", represented!(s::present(item, "name"))),
                    ("input", input),
                ])?],
            )?;
        }
        "function_call_output" => {
            let id = represented!(s::present(item, "call_id"));
            represented!(s::string(&id));
            let mut result = s::object(vec![
                ("type", s::text("tool_result")?),
                ("tool_use_id", id),
                (
                    "content",
                    s::array(&represented!(content(
                        &represented!(s::present(item, "output")),
                        true
                    )?))?,
                ),
            ])?;
            if let Some(value) = s::present(item, "is_error") {
                s::set(&mut result, "is_error", value)?;
            }
            merge(messages, "user", vec![result])?;
        }
        _ => return Ok(None),
    }
    Ok(Some(()))
}

fn native_tools(
    raw: &SourceJson,
) -> Result<Option<Vec<SourceJson>>, gateway_core::error::GatewayError> {
    let Some(value) = s::present(raw, "tools") else {
        return Ok(Some(vec![]));
    };
    let mut tools = vec![];
    for tool in represented!(json::array(&value)) {
        if !s::present(&tool, "type").is_some_and(|observed| s::is(&observed, "function"))
            || s::present(&tool, "namespace").is_some()
            || !s::present(&tool, "name").is_some_and(|observed| valid_name(&observed))
        {
            return Ok(None);
        }
        let mut result = s::object(vec![
            ("name", represented!(s::present(&tool, "name"))),
            (
                "input_schema",
                match s::present(&tool, "parameters") {
                    Some(value) => value,
                    None => s::scalar_json(&serde_json::json!({"type":"object"}))?,
                },
            ),
        ])?;
        if let Some(value) = s::present(&tool, "description") {
            s::set(&mut result, "description", value)?;
        }
        tools.push(represented!(block(result, &tool)?));
    }
    Ok(Some(tools))
}

fn native_reasoning(
    raw: &SourceJson,
    options: &NativeBodyOptions<'_>,
) -> Result<Option<SourceJson>, gateway_core::error::GatewayError> {
    let Some(reasoning) = s::present(raw, "reasoning").filter(|v| !s::is_null(v)) else {
        return Ok(Some(s::object(vec![])?));
    };
    if !valid_reasoning(&reasoning) {
        return Ok(None);
    }
    let Some(effort) = s::present(&reasoning, "effort") else {
        return Ok(Some(s::object(vec![])?));
    };
    if s::is(&effort, "none") {
        return Ok(if options.supports_disabled_thinking {
            Some(s::scalar_json(
                &serde_json::json!({"thinking":{"type":"disabled"}}),
            )?)
        } else {
            None
        });
    }
    if !options.supports_native_effort {
        return Ok(None);
    }
    let effort = represented!(s::string(&effort));
    let effort = if effort.as_units().iter().copied().eq("ultra".encode_utf16())
        && options.native_reasoning_efforts.is_none()
    {
        gateway_core::js_string::JsString16::from_utf8("max")
    } else {
        effort
    };
    let allowed = options
        .native_reasoning_efforts
        .cloned()
        .unwrap_or_else(|| {
            ["low", "medium", "high", "xhigh", "max"]
                .map(gateway_core::js_string::JsString16::from_utf8)
                .to_vec()
        });
    if !allowed.contains(&effort) {
        return Ok(None);
    }
    Ok(Some(s::object(vec![
        (
            "thinking",
            s::scalar_json(&serde_json::json!({"type":"adaptive"}))?,
        ),
        (
            "output_config",
            s::object(vec![("effort", s::quoted(&effort)?)])?,
        ),
    ])?))
}

fn native_choice(
    raw: &SourceJson,
) -> Result<Option<SourceJson>, gateway_core::error::GatewayError> {
    let mut choice = match s::present(raw, "tool_choice") {
        None => s::object(vec![])?,
        Some(value) if s::is(&value, "auto") || s::is(&value, "none") => {
            s::object(vec![("type", value)])?
        }
        Some(value) if s::is(&value, "required") => s::object(vec![("type", s::text("any")?)])?,
        Some(value)
            if s::present(&value, "type").is_some_and(|observed| s::is(&observed, "function"))
                && s::present(&value, "namespace").is_none()
                && s::present(&value, "name").is_some_and(|observed| valid_name(&observed)) =>
        {
            s::object(vec![
                ("type", s::text("tool")?),
                ("name", represented!(s::present(&value, "name"))),
            ])?
        }
        _ => return Ok(None),
    };
    if let Some(parallel) = s::present(raw, "parallel_tool_calls") {
        let parallel = represented!(s::boolean(&parallel));
        if s::present(&choice, "type").is_none() {
            s::set(&mut choice, "type", s::text("auto")?)?;
        }
        s::set(
            &mut choice,
            "disable_parallel_tool_use",
            s::scalar_json(&serde_json::json!(!parallel))?,
        )?;
    }
    Ok(Some(choice))
}

pub fn native(
    raw: &SourceJson,
    options: &PrepareOptions,
) -> Result<Option<SourceJson>, gateway_core::error::GatewayError> {
    native_body(raw, &NativeBodyOptions::from(options))
}

pub fn native_claude(
    raw: &SourceJson,
    options: &ClaudeBodyOptions,
) -> Result<Option<SourceJson>, gateway_core::error::GatewayError> {
    native_body(
        raw,
        &NativeBodyOptions {
            restored_native_messages: options.restored_native_messages.as_ref(),
            supports_native_effort: options
                .native_reasoning_efforts
                .as_ref()
                .is_some_and(|values| !values.is_empty()),
            supports_disabled_thinking: options.supports_disabled_thinking,
            native_reasoning_efforts: options.native_reasoning_efforts.as_ref(),
            catalog_max_output_tokens: Some(32_000),
        },
    )
}

fn native_body(
    raw: &SourceJson,
    options: &NativeBodyOptions<'_>,
) -> Result<Option<SourceJson>, gateway_core::error::GatewayError> {
    if !base(raw) || !policy(raw) {
        return Ok(None);
    }
    if let Some(text) = s::present(raw, "text")
        && (!only(&text, &["format"])
            || match s::present(&text, "format") {
                Some(value) => !eq(&value, r#"{"type":"text"}"#)?,
                None => false,
            })
    {
        return Ok(None);
    }
    let max = represented!(
        s::present(raw, "max_output_tokens")
            .and_then(|value| json::number(&value))
            .filter(|v| *v >= 0.0 && v.fract() == 0.0)
            .map(|v| v as u64)
            .or(options.catalog_max_output_tokens)
            .or_else(|| s::present(raw, "model")
                .is_some_and(|observed| s::is(&observed, "claude-opus-5-5"))
                .then_some(128_000))
    );
    if max == 0 || max > 9_007_199_254_740_991 {
        return Ok(None);
    }
    let mut system = match s::present(raw, "instructions").filter(|v| !s::is_null(v)) {
        Some(value) => represented!(content(&value, true)?),
        None => vec![],
    };
    if system
        .iter()
        .any(|value| !s::present(value, "type").is_some_and(|observed| s::is(&observed, "text")))
    {
        return Ok(None);
    }
    let messages = represented!(native_messages(raw, &mut system, options)?);
    let tools = represented!(native_tools(raw)?);
    let choice = represented!(native_choice(raw)?);
    let reasoning = represented!(native_reasoning(raw, options)?);
    let mut body = s::object(vec![
        ("model", represented!(s::present(raw, "model"))),
        ("max_tokens", s::scalar_json(&serde_json::json!(max))?),
        ("messages", s::array(&messages)?),
        (
            "stream",
            s::scalar_json(&serde_json::json!(
                s::present(raw, "stream").and_then(|observed| s::boolean(&observed)) == Some(true)
            ))?,
        ),
    ])?;
    if !system.is_empty() {
        s::set(&mut body, "system", s::array(&system)?)?;
    }
    if !tools.is_empty() {
        s::set(&mut body, "tools", s::array(&tools)?)?;
    }
    if !represented!(json::entries(&choice)).is_empty() {
        s::set(&mut body, "tool_choice", choice)?;
    }
    for (key, value) in represented!(json::entries(&reasoning)) {
        s::set(&mut body, &represented!(key.try_utf8().ok()), value)?;
    }
    for key in ["temperature", "top_p", "prompt_cache_key"] {
        if let Some(value) = s::present(raw, key) {
            s::set(&mut body, key, value)?;
        }
    }
    represented!(add_cache(&mut body, raw, true)?);
    Ok(Some(body))
}

pub fn replay_messages(
    raw: &SourceJson,
    entries: &[ReplayEntry],
) -> Result<Option<Vec<SourceJson>>, gateway_core::error::GatewayError> {
    let (_, _names) = match gateway_provider_tool_grammar::source_tools::flatten(
        s::present(raw, "tools").as_ref(),
        64,
    ) {
        Ok(value) => value,
        Err(gateway_provider_tool_grammar::source_tools::ToolError::Request(_)) => return Ok(None),
        Err(gateway_provider_tool_grammar::source_tools::ToolError::Construction(error)) => {
            return Err(error);
        }
    };
    let mut messages = vec![];

    let mut system = vec![];
    for entry in entries {
        match entry {
            ReplayEntry::Original(item) => {
                let item = item.clone();
                let role = s::present(&item, "role")
                    .and_then(|observed| s::control(&observed))
                    .unwrap_or_default();
                if ["system", "developer"].contains(&role.as_str()) && !messages.is_empty() {
                    return Ok(None);
                }
                represented!(append_native_item(&item, &mut system, &mut messages)?);
            }
            ReplayEntry::NativeAssistant {
                transport: ReplayTransport::Messages,
                payload,
            } => {
                let blocks = if let Some(blocks) = json::array(payload) {
                    blocks
                } else if json::entries(payload).is_some() {
                    represented!(
                        s::present(payload, "content").and_then(|observed| json::array(&observed))
                    )
                } else {
                    return Ok(None);
                };
                merge(&mut messages, "assistant", blocks)?;
            }
            ReplayEntry::NativeAssistant { .. } => return Ok(None),
        }
    }
    if !system.is_empty() {
        messages.insert(
            0,
            s::object(vec![
                ("role", s::text("system")?),
                ("content", s::array(&system)?),
            ])?,
        );
    }

    if messages.is_empty() {
        return Ok(None);
    }
    Ok(Some(messages))
}
