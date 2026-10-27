use gateway_prompt_policy::payload::{PER_MESSAGE_LIMIT as FORWARD_LIMIT, VERSION as FORWARD_VERSION};
use gateway_prompt_policy::summary::{self, SummaryExemption};
use crate::chat::bound_source;
use gateway_provider_source::source as s;
use gateway_provider_protocol::types::Elision;
use gateway_core::js_string::JsString16;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;
use gateway_local_refusal::source_error::SourceRequestError;

pub fn error(message: &str, param: &str) -> SourceRequestError {
    s::invalid(JsString16::from_utf8(message), param)
}


fn bytes(text: &JsString16) -> usize { gateway_prompt_policy::payload::byte_len(text) }


pub fn bound(
    text: &JsString16,
    path: &str,
    id: Option<&JsString16>,
    kind: &str,
    reject: bool,
    elisions: &mut Vec<Elision>,
) -> s::Result<SourceJson> {
    { let text = bound_source(text, path, id, kind, reject, elisions)?; Ok(s::quoted(&text)?) }
}


fn content(
    value: &SourceJson,
    role: &str,
    path: &str,
    reject: bool,
    elisions: &mut Vec<Elision>,
    exempt: bool,
) -> s::Result<SourceJson> {
    if let Some(text) = s::string(value) {
        return bound(&text, path, None, "message", reject, elisions);
    }
    let parts = json::array(value).ok_or_else(|| {
        error(
            "input.content must be a string or an array",
            "input.content",
        )
    })?;
    let mut texts = vec![];
    let mut images = vec![];
    for (index, part) in parts.iter().enumerate() {
        if json::entries(part).is_none() {
            return Err(error(
                "input.content items must be objects",
                "input.content",
            ));
        }
        let kind = s::present(part, "type");
        if ["text", "input_text", "output_text"]
            .iter()
            .any(|kind_name| kind.as_ref().is_some_and(|value| s::is(value, kind_name)))
        {
            texts.push(s::present(part, "text").and_then(|observed| s::string(&observed)).ok_or_else(|| {
                error("input.content text must be a string", "input.content.text")
            })?);
        } else if role == "assistant" && kind.as_ref().is_some_and(|value| s::is(value, "refusal")) {
            texts.push(s::present(part, "refusal").and_then(|observed| s::string(&observed)).ok_or_else(|| {
                error(
                    "input.content refusal must be a string",
                    "input.content.refusal",
                )
            })?);
        } else if kind.as_ref().is_some_and(|value| s::is(value, "input_image")) {
            let url = s::present(part, "image_url").and_then(|observed| s::string(&observed))
                .or_else(|| s::present(part, "file_url").and_then(|observed| s::string(&observed)))
                .filter(|value| !value.as_units().is_empty())
                .ok_or_else(|| {
                    error("input_image requires image_url", "input.content.image_url")
                })?;
            let size = bytes(&url);
            if size > FORWARD_LIMIT {
                if reject {
                    return Err(crate::chat::oversized(
                        &format!("{path}[{index}]"),
                        size,
                        None,
                    ));
                }
                omit_image(&url, &format!("{path}[{index}]"), &mut texts, elisions);
            } else {
                let mut reference = s::object(vec![("url", s::quoted(&url)?)])?;
                if let Some(detail) = s::present(part, "detail").filter(|value| {
                    s::string(value).is_some_and(|value| !value.as_units().is_empty())
                }) {
                    s::set(&mut reference, "detail", detail)?;
                }
                images.push(s::object(vec![
                    ("type", s::text("image_url")?),
                    ("image_url", reference),
                ])?);
            }
        } else {
            let name = kind.as_ref().and_then(s::string).unwrap_or_else(|| JsString16::from_utf8("unknown"));
            return Err(s::invalid(
                s::concat(&[
                    &JsString16::from_utf8("input.content type '"),
                    &name,
                    &JsString16::from_utf8("' is not supported"),
                ]),
                "input.content.type",
            ));
        }
    }
    let text = s::join(&texts, "");
    if images.is_empty() {
        return if exempt { Ok(s::quoted(&text)?) } else { bound(&text, path, None, "message", reject, elisions) };
    }
    if !texts.is_empty() {
        images.insert(
            0,
            s::object(vec![("type", s::text("text")?), ("text", s::quoted(&text)?)])?,
        );
    }
    bounded_parts(images, path, reject, elisions)
}


fn part_bytes(part: &SourceJson) -> usize {
    let text = if s::present(part, "type").is_some_and(|observed| s::is(&observed, "text")) {
        s::present(part, "text").and_then(|observed| s::string(&observed))
    } else if s::present(part, "type").is_some_and(|observed| s::is(&observed, "image_url")) {
        s::present_path(part, &["image_url", "url"]).and_then(|observed| s::string(&observed))
    } else {
        None
    };
    text.as_ref().map_or(0, bytes)
}


fn bounded_parts(
    mut parts: Vec<SourceJson>,
    path: &str,
    reject: bool,
    elisions: &mut Vec<Elision>,
) -> s::Result<SourceJson> {
    let original = parts.iter().map(part_bytes).sum::<usize>();
    if original <= FORWARD_LIMIT {
        return Ok(s::array(&parts)?);
    }
    if reject {
        return Err(crate::chat::oversized(path, original, None));
    }
    while parts.iter().map(part_bytes).sum::<usize>() > FORWARD_LIMIT {
        let Some(index) = parts
            .iter()
            .rposition(|part| s::present(part, "type").is_some_and(|observed| s::is(&observed, "image_url")))
        else {
            break;
        };
        let url = s::present_path(&parts[index], &["image_url", "url"]).and_then(|observed| s::string(&observed)).unwrap_or_default();
        let mut marker = vec![];
        omit_image(&url, &format!("{path}[{index}]"), &mut marker, elisions);
        parts[index] = s::object(vec![
            ("type", s::text("text")?),
            ("text", s::quoted(&marker[0])?),
        ])?;
    }
    let total = parts.iter().map(part_bytes).sum::<usize>();
    if total > FORWARD_LIMIT
        && let Some(index) = parts.iter().position(|part| {
            s::present(part, "type").is_some_and(|observed| s::is(&observed, "text")) && s::present(part, "text").and_then(|observed| s::string(&observed)).is_some()
        })
    {
        let text = s::present(&parts[index], "text").and_then(|observed| s::string(&observed)).unwrap_or_default();
        let available = (FORWARD_LIMIT + bytes(&text)).saturating_sub(total);
        let text = crate::chat::bound_source_limit(
            &text, path, None, "message", false, elisions, available,
        )?;
        parts[index] = s::object(vec![("type", s::text("text")?), ("text", s::quoted(&text)?)])?;
    }
    if parts.iter().map(part_bytes).sum::<usize>() > FORWARD_LIMIT {
        let marker = format!(
            "\n[gateway: {FORWARD_VERSION} elided {original} bytes omitted; the model did not receive the elided bytes; this route forwards at most {FORWARD_LIMIT} bytes per message because the provider counts forwarded payloads as text tokens]"
        );
        elisions.push(Elision {
            path: path.into(),
            call_id: None,
            kind: "message".into(),
            original_bytes: original,
            forwarded_bytes: marker.len(),
            omitted_bytes: original,
        });
        return Ok(s::text(&marker)?);
    }
    Ok(s::array(&parts)?)
}



fn omit_image(
    url: &JsString16,
    path: &str,
    text: &mut Vec<JsString16>,
    elisions: &mut Vec<Elision>,
) {
    let original = bytes(url);
    let marker = format!(
        "[gateway: {FORWARD_VERSION} image omitted ({original} bytes); the model did not receive this image; this route forwards at most {FORWARD_LIMIT} bytes per message because the provider counts forwarded payloads as text tokens]"
    );
    elisions.push(Elision {
        path: path.into(),
        kind: "image".into(),
        original_bytes: original,
        forwarded_bytes: marker.len(),
        omitted_bytes: original,
        call_id: None,
    });
    text.push(JsString16::from_utf8(&marker));
}


pub fn reasoning(item: &SourceJson) -> JsString16 {
    let mut parts = vec![];
    for key in ["summary", "content"] {
        for part in s::present(item, key).and_then(|observed| json::array(&observed)).into_iter().flatten() {
            if let Some(text) = s::present(&part, "text").and_then(|observed| s::string(&observed)) {
                parts.push(text);
            }
        }
    }
    if let Some(text) = s::present(item, "text").and_then(|observed| s::string(&observed)) {
        parts.push(text);
    }
    s::join(&parts, "")
}


pub fn messages(raw: &SourceJson) -> s::Result<(Vec<SourceJson>, Vec<Elision>, Vec<SummaryExemption>)> {
    let truncation = s::present(raw, "truncation");
    let reject = truncation.as_ref().is_some_and(|value| s::is(value, "disabled"));
    if let Some(truncation) = truncation.filter(|value| !s::is_null(value) && !reject && !s::is(value, "auto")) {
        let value = s::string(&truncation).unwrap_or_else(|| {
            JsString16::from_utf8(if json::number(&truncation).is_some() {
                "number"
            } else if s::boolean(&truncation).is_some() {
                "boolean"
            } else {
                "object"
            })
        });
        return Err(s::invalid(
            s::concat(&[
                &JsString16::from_utf8("truncation '"),
                &value,
                &JsString16::from_utf8("' is not supported; expected 'auto' or 'disabled'"),
            ]),
            "truncation",
        ));
    }
    let mut messages = vec![];
    let mut elisions = vec![];
    let mut pending = JsString16::default();
    if let Some(instructions) =
        s::present(raw, "instructions").and_then(|observed| s::string(&observed)).filter(|text| !s::trim(text).as_units().is_empty())
    {
        messages.push(s::object(vec![
            ("role", s::text("system")?),
            (
                "content",
                bound(
                    &instructions,
                    "instructions",
                    None,
                    "message",
                    reject,
                    &mut elisions,
                )?,
            ),
        ])?);
    }
    let input = s::present(raw, "input").ok_or_else(|| error("input must be a string or an array", "input"))?;
    let summary_index = summary::inspect_input(&input).map_err(|failure| SourceRequestError::new(400, format!("input item {}", failure.message), "invalid_request_error", Some("context_length_exceeded"), Some(Some(failure.path.into()))))?;
    let mut exemptions = Vec::new();
    let items = if s::string(&input).is_some() {
        vec![s::object(vec![
            ("role", s::text("user")?),
            ("content", input),
        ])?]
    } else {
        json::array(&input).ok_or_else(|| error("input must be a string or an array", "input"))?
    };
    for (index, item) in items.iter().enumerate() {
        let path = format!("input[{index}]");
        if let Some(text) = s::string(item) {
            pending = JsString16::default();
            messages.push(s::object(vec![
                ("role", s::text("user")?),
                (
                    "content",
                    bound(&text, &path, None, "message", reject, &mut elisions)?,
                ),
            ])?);
            continue;
        }
        let kind = match s::present(item, "type").filter(|value| s::string(value).is_some()) { Some(value) => value, None => s::text("message")? };
        if s::is(&kind, "reasoning") {
            let text = bound_source(&reasoning(item), &path, None, "reasoning", reject, &mut elisions)?;
            pending = s::concat(&[&pending, &text]);
        } else if s::is(&kind, "message") {
            let role_source =
                s::present(item, "role").and_then(|observed| s::string(&observed)).unwrap_or_else(|| JsString16::from_utf8("user"));
            let role = role_source
                .try_utf8()
                .ok()
                .filter(|value| {
                    ["user", "assistant", "developer", "system"].contains(&value.as_str())
                })
                .ok_or_else(|| {
                    s::invalid(
                        s::concat(&[
                            &JsString16::from_utf8("input role '"),
                            &role_source,
                            &JsString16::from_utf8("' is not supported"),
                        ]),
                        "input.role",
                    )
                })?;
            if role == "user" {
                pending = JsString16::default();
            }
            let role = if role == "developer" { "system" } else { &role };
            let source_content = s::present(item, "content").ok_or_else(|| error("input.content must be a string or an array", "input.content"))?;
            let projected_content = content(&source_content, role, &format!("{path}.content"), reject, &mut elisions, summary_index == Some(index))?;
            if summary_index == Some(index)
                && let Some(text) = s::string(&projected_content)
                && bytes(&text) > FORWARD_LIMIT {
                exemptions.push(summary::exemption(&format!("{path}.content"), bytes(&text)));
            }
            let mut message = s::object(vec![
                ("role", s::text(role)?),
                (
                    "content",
                    projected_content,
                ),
            ])?;
            if role == "assistant" && !pending.as_units().is_empty() {
                s::set(
                    &mut message,
                    "reasoning_content",
                    s::quoted(&std::mem::take(&mut pending))?,
                )?;
            }
            messages.push(message);
        } else if s::is(&kind, "function_call") || s::is(&kind, "custom_tool_call") {
            let label = if s::is(&kind, "custom_tool_call") {
                "custom_tool_call"
            } else {
                "function_call"
            };
            let call_id = s::present(item, "call_id")
                .filter(|value| s::string(value).is_some())
                .or_else(|| s::present(item, "id").filter(|value| s::string(value).is_some()))
                .ok_or_else(|| {
                    error(&format!("{label} items require call_id and name"), "input")
                })?;
            let name = s::present(item, "name")
                .filter(|value| s::string(value).is_some_and(|value| !value.as_units().is_empty()))
                .ok_or_else(|| {
                    error(&format!("{label} items require call_id and name"), "input")
                })?;
            let arguments = if label == "custom_tool_call" {
                let input = match s::present(item, "input").filter(|value| s::string(value).is_some()) { Some(value) => value, None => s::text("")? };
                let object = s::canonical(&s::object(vec![("input", input)])?)?;
                s::text(object.as_str())?
            } else {
                match s::present(item, "arguments").filter(|value| s::string(value).is_some()) { Some(value) => value, None => s::text("{}")? }
            };
            let call_id_text = s::string(&call_id).ok_or_else(|| error(&format!("{label} items require call_id and name"), "input"))?;
            let arguments_text = s::string(&arguments).ok_or_else(|| error("tool arguments must be a string", "input"))?;
            let arguments = s::quoted(&crate::chat::bound_arguments(&arguments_text, &format!("{path}.arguments"), &call_id_text, reject, &mut elisions)?)?;
            let call = s::object(vec![
                ("id", call_id),
                ("type", s::text("function")?),
                (
                    "function",
                    s::object(vec![("name", name), ("arguments", arguments)])?,
                ),
            ])?;
            if let Some(previous) = messages.last_mut().filter(|message| {
                s::present(message, "role").is_some_and(|observed| s::is(&observed, "assistant"))
                    && s::present(message, "content").is_none_or(|observed| s::is_null(&observed))
                    && s::present(message, "tool_calls").and_then(|observed| json::array(&observed)).is_some()
            }) {
                let mut calls = s::present(previous, "tool_calls").and_then(|observed| json::array(&observed)).unwrap_or_default();
                calls.push(call);
                s::set(previous, "tool_calls", s::array(&calls)?)?;
                if !pending.as_units().is_empty() {
                    let old = s::present(previous, "reasoning_content").and_then(|observed| s::string(&observed)).unwrap_or_default();
                    s::set(
                        previous,
                        "reasoning_content",
                        s::quoted(&s::concat(&[&old, &std::mem::take(&mut pending)]))?,
                    )?;
                }
            } else {
                let mut message = s::object(vec![
                    ("role", s::text("assistant")?),
                    ("content", s::null()?),
                    ("tool_calls", s::array(&[call])?),
                ])?;
                if !pending.as_units().is_empty() {
                    s::set(
                        &mut message,
                        "reasoning_content",
                        s::quoted(&std::mem::take(&mut pending))?,
                    )?;
                }
                messages.push(message);
            }
        } else if s::is(&kind, "function_call_output") || s::is(&kind, "custom_tool_call_output") {
            let Some(call_id) =
                s::present(item, "call_id").and_then(|observed| s::string(&observed)).filter(|value| !value.as_units().is_empty())
            else {
                continue;
            };
            let output = match s::present(item, "output").filter(|value| !s::is_null(value)) { Some(value) => value, None => s::text("")? };
            let text = match s::string(&output) {
                Some(text) => text,
                None => JsString16::from_utf8(s::canonical(&output)?.as_str()),
            };
            messages.push(s::object(vec![
                ("role", s::text("tool")?),
                ("tool_call_id", s::quoted(&call_id)?),
                (
                    "content",
                    bound(
                        &text,
                        &format!("{path}.output"),
                        Some(&call_id),
                        "tool_output",
                        reject,
                        &mut elisions,
                    )?,
                ),
            ])?);
        } else if s::is(&kind, "agent_message") {
            pending = JsString16::default();
            let author = s::present(item, "author").and_then(|observed| s::string(&observed))
                .unwrap_or_else(|| JsString16::from_utf8("agent"));
            let recipient = s::present(item, "recipient").and_then(|observed| s::string(&observed))
                .unwrap_or_else(|| JsString16::from_utf8("root"));
            let mut texts = vec![];
            let mut omitted = 0;
            for part in s::present(item, "content").and_then(|observed| json::array(&observed)).into_iter().flatten() {
                if let Some(text) = s::present(&part, "text").and_then(|observed| s::string(&observed)) {
                    texts.push(text);
                } else if let Some(text) = s::present(&part, "encrypted_content").and_then(|observed| s::string(&observed)) {
                    if text
                        .as_units()
                        .starts_with(&"gAAAAA".encode_utf16().collect::<Vec<_>>())
                        && text.as_units().len() >= 100
                    {
                        omitted += 1;
                    } else {
                        texts.push(text);
                    }
                }
            }
            let notice = if omitted > 0 {
                format!(
                    "\n[gateway: {omitted} sealed agent-message part(s) were not readable and omitted]"
                )
            } else {
                String::new()
            };
            let text = s::concat(&[
                &JsString16::from_utf8("[agent message] "),
                &author,
                &JsString16::from_utf8(" -> "),
                &recipient,
                &JsString16::from_utf8(":\n"),
                &s::join(&texts, "\n"),
                &JsString16::from_utf8(&notice),
            ]);
            messages.push(s::object(vec![
                ("role", s::text("user")?),
                (
                    "content",
                    bound(&text, &path, None, "agent_message", reject, &mut elisions)?,
                ),
            ])?);
        } else {
            return Err(s::invalid(
                s::concat(&[
                    &JsString16::from_utf8("input item type '"),
                    &s::string(&kind).unwrap_or_default(),
                    &JsString16::from_utf8("' is not supported"),
                ]),
                "input.type",
            ));
        }
    }
    Ok((messages, elisions, exemptions))
}
