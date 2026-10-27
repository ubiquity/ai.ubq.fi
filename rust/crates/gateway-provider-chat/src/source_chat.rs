use gateway_core::js_string::JsString16;
use gateway_core::model::ModelId;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;
use gateway_provider_history::source_chat::bound;
use gateway_provider_history::source_chat::error;
use gateway_provider_history::source_chat::messages;
use gateway_provider_protocol::source_chat::Projection;
use gateway_provider_source::source as s;
use gateway_provider_source::types::ChatProfile;

pub fn thinking(raw: &SourceJson, effort: Option<&str>) -> bool {
    effort.filter(|v| !v.trim().is_empty()).map_or_else(
        || {
            !s::present_path(raw, &["thinking", "type"])
                .and_then(|observed| s::control(&observed))
                .is_some_and(|v| v.trim().eq_ignore_ascii_case("disabled"))
        },
        |effort| !effort.trim().eq_ignore_ascii_case("none"),
    )
}

pub fn validate_thinking(raw: &SourceJson, field: &str) -> s::Result<()> {
    if thinking(
        raw,
        s::present(raw, "reasoning_effort")
            .and_then(|observed| s::control(&observed))
            .as_deref(),
    ) && (s::present(raw, "tool_choice").is_some_and(|observed| s::is(&observed, "required"))
        || s::present(raw, "tool_choice")
            .and_then(|observed| json::entries(&observed))
            .is_some())
    {
        let choice = s::present(raw, "tool_choice")
            .and_then(|observed| s::string(&observed))
            .unwrap_or_else(|| {
                s::concat(&[
                    &JsString16::from_utf8("function:"),
                    &s::present_path(raw, &["tool_choice", "function", "name"])
                        .and_then(|observed| s::string(&observed))
                        .unwrap_or_default(),
                ])
            });
        return Err(s::invalid(
            s::concat(&[
                &JsString16::from_utf8("tool_choice '"),
                &choice,
                &JsString16::from_utf8(&format!(
                    "' is not supported while {field} keeps DeepSeek thinking mode active; set {field} to 'none' or use tool_choice 'auto'"
                )),
            ]),
            "tool_choice",
        ));
    }
    Ok(())
}

pub fn effort(value: &JsString16, profile: ChatProfile, _param: &str) -> s::Result<SourceJson> {
    // b592 src/deepseek/index.ts:187,299: only the provider wire alias is mapped.
    let trimmed = s::trim(value);
    let units = trimmed.as_units();
    let ultra = units.len() == b"ultra".len()
        && units.iter().zip(b"ultra").all(|(&unit, &lower)| {
            unit == u16::from(lower) || unit == u16::from(lower.to_ascii_uppercase())
        });
    if profile == ChatProfile::Deepseek && ultra {
        Ok(s::text("max")?)
    } else {
        Ok(s::quoted(value)?)
    }
}

pub fn project(
    raw: &SourceJson,
    model: &ModelId,
    stream: bool,
    profile: ChatProfile,
) -> s::Result<Projection> {
    let (mut messages, mut elisions, exemptions) = messages(raw)?;
    if messages.is_empty() {
        return Err(error("input must contain at least one message", "input"));
    }
    let (mut body, warnings) = controls(raw, model, stream, profile)?;
    let (tools, names) = gateway_provider_tool_grammar::source_tools::flatten(
        s::present(raw, "tools").as_ref(),
        128,
    )?;
    if let Some(choice) = gateway_provider_tool_grammar::source_tools::choice(
        s::present(raw, "tool_choice").as_ref(),
        &names,
    )? {
        s::set(&mut body, "tool_choice", choice)?;
    }
    if let Some(parallel) =
        s::present(raw, "parallel_tool_calls").filter(|value| s::boolean(value).is_some())
    {
        s::set(&mut body, "parallel_tool_calls", parallel)?;
    }
    if let Some(text) = s::present(raw, "text").filter(|value| !s::is_null(value)) {
        if json::entries(&text).is_none() {
            return Err(error("text must be an object", "text"));
        }
        if let Some(format) = s::present(&text, "format").filter(|value| !s::is_null(value)) {
            if json::entries(&format).is_none() {
                return Err(error("text.format must be an object", "text.format"));
            }
            let kind = match s::present(&format, "type").filter(|value| s::string(value).is_some())
            {
                Some(value) => value,
                None => s::text("text")?,
            };
            if s::is(&kind, "json_object") {
                s::set(
                    &mut body,
                    "response_format",
                    s::scalar_json(&serde_json::json!({"type":"json_object"}))?,
                )?;
            } else if !s::is(&kind, "text") {
                return Err(s::invalid(
                    s::concat(&[
                        &JsString16::from_utf8("text.format type '"),
                        &s::string(&kind).unwrap_or_default(),
                        &JsString16::from_utf8("' is not supported upstream"),
                    ]),
                    "text.format.type",
                ));
            }
        }
    }
    if profile == ChatProfile::Deepseek {
        validate_thinking(&body, "reasoning.effort")?;
    }
    if !tools.is_empty() {
        if !s::present(raw, "tool_choice").is_some_and(|observed| s::is(&observed, "none")) {
            let continuation = JsString16::from_utf8(gateway_provider_history::chat::CONTINUATION);
            if let Some(index) = messages.iter().position(|m| {
                s::present(m, "role").is_some_and(|observed| s::is(&observed, "system"))
                    && s::present(m, "content")
                        .and_then(|observed| s::string(&observed))
                        .is_some()
            }) {
                let old = s::present(&messages[index], "content")
                    .and_then(|observed| s::string(&observed))
                    .unwrap_or_default();
                let text = s::concat(&[&old, &JsString16::from_utf8("\n\n"), &continuation]);
                s::set(
                    &mut messages[index],
                    "content",
                    bound(
                        &text,
                        "instructions",
                        None,
                        "message",
                        s::present(raw, "truncation")
                            .is_some_and(|observed| s::is(&observed, "disabled")),
                        &mut elisions,
                    )?,
                )?;
            } else {
                messages.insert(
                    0,
                    s::object(vec![
                        ("role", s::text("system")?),
                        ("content", s::quoted(&continuation)?),
                    ])?,
                );
            }
        }
        let start = messages
            .iter()
            .rposition(|m| s::present(m, "role").is_some_and(|observed| s::is(&observed, "user")))
            .map_or(0, |i| i + 1);
        for message in messages.iter_mut().skip(start) {
            if s::present(message, "role").is_some_and(|observed| s::is(&observed, "assistant"))
                && s::present(message, "reasoning_content")
                    .and_then(|observed| s::string(&observed))
                    .is_none()
            {
                s::set(message, "reasoning_content", s::text("")?)?;
            }
        }
        s::set(&mut body, "tools", s::array(&tools)?)?;
    }
    s::set(&mut body, "messages", s::array(&messages)?)?;
    let admission = admit(&body, model, profile)?;
    Ok(Projection {
        body,
        names,
        elisions,
        warnings,
        exemptions,
        admission: Some(admission),
    })
}

fn controls(
    raw: &SourceJson,
    model: &ModelId,
    stream: bool,
    profile: ChatProfile,
) -> s::Result<(SourceJson, Vec<String>)> {
    let mut body = s::object(vec![
        ("model", s::quoted(model.as_js_string())?),
        ("messages", s::array(&[])?),
        ("stream", s::scalar_json(&serde_json::json!(stream))?),
    ])?;
    if let Some(max) = s::present(raw, "max_output_tokens").filter(|value| !s::is_null(value)) {
        if !json::number(&max)
            .is_some_and(|v| v > 0.0 && v <= 9_007_199_254_740_991.0 && v.fract() == 0.0)
        {
            return Err(error(
                "max_output_tokens must be a positive integer",
                "max_output_tokens",
            ));
        }
        s::set(&mut body, "max_tokens", max)?;
    }
    if let Some(reasoning) = s::present(raw, "reasoning").filter(|value| !s::is_null(value))
        && json::entries(&reasoning).is_none()
    {
        return Err(error("reasoning must be an object", "reasoning"));
    }
    if let Some(value) = s::present_path(raw, &["reasoning", "effort"])
        .and_then(|observed| s::string(&observed))
        .filter(|value| !value.as_units().is_empty())
    {
        s::set(
            &mut body,
            "reasoning_effort",
            effort(&value, profile, "reasoning.effort")?,
        )?;
    } else if profile == ChatProfile::Deepseek && !thinking(raw, None) {
        s::set(&mut body, "reasoning_effort", s::text("none")?)?;
    }
    let active = profile == ChatProfile::Deepseek
        && thinking(
            raw,
            s::present(&body, "reasoning_effort")
                .and_then(|observed| s::control(&observed))
                .as_deref(),
        );
    let mut warnings = vec![];
    for field in ["temperature", "top_p"] {
        if let Some(value) = s::present(raw, field) {
            if active {
                warnings.push(format!("{field}_ignored"));
            } else {
                s::set(&mut body, field, value)?;
            }
        }
    }
    if stream && profile == ChatProfile::Deepseek {
        s::set(
            &mut body,
            "stream_options",
            s::scalar_json(&serde_json::json!({"include_usage":true}))?,
        )?;
    }
    if profile == ChatProfile::Cerebras
        && let Some(key) = s::present(raw, "prompt_cache_key")
    {
        s::set(&mut body, "prompt_cache_key", key)?;
    }
    Ok((body, warnings))
}

fn admit(
    body: &SourceJson,
    model: &ModelId,
    profile: ChatProfile,
) -> s::Result<gateway_prompt_policy::admission::PromptAdmission> {
    // The pure source projection keeps its original fields. Admission measures the later real transport default.
    let mut measured_body = body.clone();
    if s::present(&measured_body, "reasoning_effort").is_none() {
        s::set(
            &mut measured_body,
            "reasoning_effort",
            s::text(gateway_provider_source::types::default_effort(
                profile, model,
            ))?,
        )?;
    }
    if profile == ChatProfile::Cerebras
        && model.matches_scalar("gpt-oss-120b")
        && s::present(body, "reasoning_effort").is_some_and(|value| s::is(&value, "none"))
    {
        return Err(error(
            "reasoning_effort 'none' is not supported by gpt-oss-120b",
            "reasoning_effort",
        ));
    }
    let admission_profile = match profile {
        ChatProfile::Deepseek => gateway_prompt_policy::admission::PromptProfile::Deepseek,
        ChatProfile::Lithos => gateway_prompt_policy::admission::PromptProfile::Lithos,
        ChatProfile::Cerebras => gateway_prompt_policy::admission::PromptProfile::Cerebras,
        ChatProfile::Claude => {
            return Err(error("Claude has no verified Chat prompt window", "model"));
        }
    };
    let admission = if profile == ChatProfile::Cerebras {
        gateway_prompt_policy::admission::estimate_projected(
            &measured_body,
            admission_profile,
            |original| {
                let mut projected = original.clone();
                gateway_provider_chat_controls::projection::prepare(&mut projected)
                    .map_err(s::scalar_error)?;
                Ok(projected)
            },
        )?
    } else {
        gateway_prompt_policy::admission::estimate(&measured_body, admission_profile)?
    };
    if !admission.admitted {
        let label = match profile {
            ChatProfile::Deepseek => "DeepSeek",
            ChatProfile::Lithos => "LithosAI",
            _ => "Cerebras",
        };
        return Err(
            gateway_local_refusal::source_error::SourceRequestError::aggregate_context_overflow(
                JsString16::from_utf8(&gateway_prompt_policy::admission::failure_message(
                    &admission, label,
                )),
            ),
        );
    }
    Ok(admission)
}
