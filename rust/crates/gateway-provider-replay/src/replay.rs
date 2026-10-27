use gateway_provider_source::source as s;
use gateway_provider_tool_grammar::source_tools::ToolMap;
use gateway_provider_protocol::types::PrepareOptions;
use gateway_core::js_string::JsString16;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as source;
use std::collections::BTreeSet;
use std::collections::VecDeque;
use gateway_provider_protocol::replay::ReplayTransport;
use gateway_provider_protocol::replay::ReplayEntry;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ReplayContext {
    pub scope: String,
    pub provider: String,
    pub model: String,
}


#[derive(Clone)]
struct Receipt {
    context: ReplayContext,
    ids: Vec<String>,
    fingerprints: Vec<SourceJson>,
    native: SourceJson,
    transport: ReplayTransport,
    expires: i64,
    bytes: usize,
}


#[derive(Clone)]
pub struct ReplayStore {
    rows: VecDeque<Receipt>,
    pub ttl_ms: i64,
    pub max_entries: usize,
    pub max_bytes: usize,
    pub max_entry_bytes: usize,
}


impl Default for ReplayStore {
    fn default() -> Self {
        Self {
            rows: VecDeque::new(),
            ttl_ms: 1_800_000,
            max_entries: 64,
            max_bytes: 8 * 1024 * 1024,
            max_entry_bytes: 1024 * 1024,
        }
    }
}


fn context_valid(c: &ReplayContext) -> bool {
    !c.scope.is_empty()
        && c.scope.len() <= 512
        && !c.provider.is_empty()
        && c.provider.len() <= 64
        && !c.model.is_empty()
        && c.model.len() <= 256
}


fn fingerprint(item: &SourceJson) -> Result<Option<SourceJson>, gateway_core::error::GatewayError> {
    Ok({
    let id = represented!(s::present(item, "id").and_then(|observed| s::control(&observed)));
    let (prefix, tail) = represented!(id.split_once('_'));
    if !["rs", "msg", "fc", "ctc"].contains(&prefix)
        || tail.is_empty()
        || tail.len() > 240
        || !tail
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
    {
        return Ok(None);
    }
    match represented!(s::present(item, "type").and_then(|observed| s::control(&observed))).as_str() {
        "reasoning" => {
            if s::present(item, "encrypted_content").is_some_and(|v| !s::is_null(&v))
                || s::present(item, "content").is_some_and(|v| {
                    !s::is_null(&v) && !source::array(&v).is_some_and(|values| values.is_empty())
                })
            {
                return Ok(None);
            }
            let mut summary = vec![];
            for part in represented!(s::present(item, "summary").and_then(|observed| source::array(&observed))) {
                if !s::present(&part, "type").is_some_and(|observed| s::is(&observed, "summary_text")) {
                    return Ok(None);
                }
                summary.push(s::object(vec![
                    ("type", s::text("summary_text")?),
                    ("text", s::quoted(&represented!(s::present(&part, "text").and_then(|observed| s::string(&observed))))?),
                ])?);
            }
            Some(s::object(vec![
                ("id", s::text(&id)?),
                ("type", s::text("reasoning")?),
                ("summary", s::array(&summary)?),
            ])?)
        }
        "message" => {
            if !s::present(item, "role").is_some_and(|observed| s::is(&observed, "assistant")) {
                return Ok(None);
            }
            let mut parts = vec![];
            for part in represented!(s::present(item, "content").and_then(|observed| source::array(&observed))) {
                parts.push(match represented!(s::present(&part, "type").and_then(|observed| s::control(&observed))).as_str() {
                    "output_text" => s::object(vec![
                        ("type", s::text("output_text")?),
                        ("text", s::quoted(&represented!(s::present(&part, "text").and_then(|observed| s::string(&observed))))?),
                    ])?,
                    "refusal" => s::object(vec![
                        ("type", s::text("refusal")?),
                        ("refusal", s::quoted(&represented!(s::present(&part, "refusal").and_then(|observed| s::string(&observed))))?),
                    ])?,
                    _ => return Ok(None),
                });
            }
            Some(s::object(vec![
                ("id", s::text(&id)?),
                ("type", s::text("message")?),
                ("role", s::text("assistant")?),
                ("content", s::array(&parts)?),
            ])?)
        }
        "function_call" | "custom_tool_call" => {
            let kind = represented!(s::present(item, "type").and_then(|observed| s::control(&observed)));
            let call = represented!(s::present(item, "call_id").and_then(|observed| s::string(&observed)).filter(|s| !s.as_units().is_empty()));
            let name = represented!(s::present(item, "name").and_then(|observed| s::string(&observed)).filter(|s| !s.as_units().is_empty()));
            if s::present(item, "encrypted_function_args").is_some_and(|v| !s::is_null(&v))
                || s::present(item, "namespace")
                    .is_some_and(|v| !s::is_null(&v) && s::string(&v).is_none())
            {
                return Ok(None);
            }
            let mut result = s::object(vec![
                ("id", s::text(&id)?),
                ("type", s::text(&kind)?),
                ("call_id", s::quoted(&call)?),
                ("name", s::quoted(&name)?),
                (
                    "namespace",
                    match s::present(item, "namespace") { Some(value) => value, None => s::null()? },
                ),
            ])?;
            if kind == "custom_tool_call" {
                s::set(
                    &mut result,
                    "input",
                    s::quoted(&represented!(s::present(item, "input").and_then(|observed| s::string(&observed))))?,
                )?;
            } else {
                let arguments = represented!(s::present(item, "arguments").and_then(|observed| s::string(&observed)));
                let arguments = represented!(SourceJson::parse(&represented!(arguments.try_utf8().ok())).ok());
                represented!(source::entries(&arguments));
                s::set(&mut result, "arguments", arguments)?;
            }
            Some(result)
        }
        _ => None,
    }

    })
}


fn native_valid(native: &SourceJson, transport: ReplayTransport) -> bool {
    match transport {
        ReplayTransport::Messages => source::array(native).is_some_and(|blocks| {
            !blocks.is_empty()
                && blocks.len() <= 256
                && blocks
                    .iter()
                    .all(|b| match s::present(b, "type").and_then(|observed| s::control(&observed)).as_deref() {
                        Some("text") => s::present(b, "text").and_then(|observed| s::string(&observed)).is_some(),
                        Some("thinking") => {
                            s::present(b, "thinking").and_then(|observed| s::string(&observed)).is_some()
                                && s::present(b, "signature").and_then(|observed| s::string(&observed))
                                    .is_some_and(|s| !s.as_units().is_empty())
                        }
                        Some("redacted_thinking") => {
                            s::present(b, "data").and_then(|observed| s::string(&observed)).is_some_and(|s| !s.as_units().is_empty())
                        }
                        Some("tool_use") => {
                            s::present(b, "id").and_then(|observed| s::string(&observed)).is_some_and(|s| !s.as_units().is_empty())
                                && s::present(b, "name").and_then(|observed| s::string(&observed))
                                    .is_some_and(|s| !s.as_units().is_empty())
                                && s::present(b, "input").and_then(|observed| source::entries(&observed)).is_some()
                        }
                        _ => false,
                    })
        }),
        ReplayTransport::Chat => {
            s::present(native, "role").is_some_and(|observed| s::is(&observed, "assistant"))
                && source::entries(native).is_some()
                && ["content", "refusal", "reasoning_content", "reasoning"]
                    .iter()
                    .all(|key| {
                        s::present(native, key).is_none_or(|value| s::string(&value).is_some())
                    })
                && s::present(native, "tool_calls").is_none_or(|value| {
                    source::array(&value).is_some_and(|calls| calls.len() <= 256)
                })
                && chat_readable_reasoning(native).is_some()
        }
    }
}


fn chat_readable_reasoning(native: &SourceJson) -> Option<JsString16> {
    let detail_text = chat_detail_text(native)?;
    for key in ["reasoning_content", "reasoning"] {
        let Some(value) = s::present(native, key) else {
            continue;
        };
        let value = s::string(&value)?;
        if !value.as_units().is_empty() {
            return Some(value);
        }
    }
    Some(detail_text)
}


fn chat_detail_text(native: &SourceJson) -> Option<JsString16> {
    let Some(details) = s::present(native, "reasoning_details") else {
        return Some(JsString16::from_units(vec![]));
    };
    let details = source::array(&details)?;
    if details.len() > 256 {
        return None;
    }
    let mut text = JsString16::from_units(vec![]);
    for detail in details {
        if !s::present(&detail, "type").is_some_and(|observed| s::is(&observed, "reasoning.text"))
            || s::present(&detail, "signature").and_then(|observed| s::string(&observed))
                .is_none_or(|signature| signature.as_units().is_empty())
            || s::present(&detail, "index")
                .is_some_and(|index| !s::is_null(&index) && !safe_index(&index))
        {
            return None;
        }
        text = s::concat(&[&text, &s::present(&detail, "text").and_then(|observed| s::string(&observed))?]);
    }
    Some(text)
}


fn safe_index(value: &SourceJson) -> bool {
    source::number(value).is_some_and(|value| {
        value.is_finite()
            && value >= 0.0
            && value.fract() == 0.0
            && value <= 9_007_199_254_740_991.0
    })
}


fn empty_string() -> JsString16 {
    JsString16::from_units(vec![])
}


fn parsed_arguments(value: &SourceJson) -> Option<SourceJson> {
    SourceJson::parse(&s::string(value)?.try_utf8().ok()?).ok()
}


fn native_messages_match(native: &SourceJson, output: &[SourceJson]) -> Result<bool, gateway_core::error::GatewayError> {
    Ok({
    let Some(blocks) = source::array(native) else {
        return Ok(false);
    };
    let mut position = 0;
    for block in blocks {
        match s::present(&block, "type").and_then(|observed| s::control(&observed)).as_deref().unwrap_or("") {
            "redacted_thinking" => (),
            "thinking" => {
                if output
                    .get(position)
                    .is_some_and(|item| s::present(item, "type").is_some_and(|observed| s::is(&observed, "reasoning")))
                {
                    let expected = s::object(vec![
                        ("type", s::text("summary_text")?),
                        ("text", match s::present(&block, "thinking") { Some(value) => value, None => return Ok(false) }),
                    ])?;
                    if s::present(&output[position], "summary").and_then(|observed| source::array(&observed))
                        .is_none_or(|summary| summary != [expected])
                    {
                        return Ok(false);
                    }
                    position += 1;
                }
            }
            "text" => {
                let Some(item) = output.get(position) else {
                    return Ok(false);
                };
                let Some(content) = s::present(item, "content").and_then(|observed| source::array(&observed)) else {
                    return Ok(false);
                };
                if !s::present(item, "type").is_some_and(|observed| s::is(&observed, "message"))
                    || content.len() != 1
                    || !s::present(&content[0], "type").is_some_and(|observed| s::is(&observed, "output_text"))
                    || s::present(&content[0], "text").and_then(|observed| s::string(&observed)) != s::present(&block, "text").and_then(|observed| s::string(&observed))
                {
                    return Ok(false);
                }
                position += 1;
            }
            "tool_use" => {
                let Some(item) = output.get(position) else {
                    return Ok(false);
                };
                if !s::present(item, "type").is_some_and(|observed| s::is(&observed, "function_call"))
                    || s::present(item, "call_id").and_then(|observed| s::string(&observed)) != s::present(&block, "id").and_then(|observed| s::string(&observed))
                    || s::present(item, "name").and_then(|observed| s::string(&observed)) != s::present(&block, "name").and_then(|observed| s::string(&observed))
                    || s::present(item, "arguments").as_ref().and_then(parsed_arguments) != s::present(&block, "input")
                {
                    return Ok(false);
                }
                position += 1;
            }
            _ => return Ok(false),
        }
    }
    position == output.len()

    })
}


fn native_chat_matches(native: &SourceJson, output: &[SourceJson], names: &ToolMap) -> Result<bool, gateway_core::error::GatewayError> {
    Ok({
    let text = s::present(native, "content").and_then(|observed| s::string(&observed)).unwrap_or_else(empty_string);
    let mut tools = vec![];
    let Some(thinking) = chat_readable_reasoning(native) else {
        return Ok(false);
    };
    for call in s::present(native, "tool_calls").and_then(|observed| source::array(&observed))
        .into_iter()
        .flatten()
    {
        let Some(function) = s::present(&call, "function") else { return Ok(false); };
        let Some(wire) = s::present(&function, "name").and_then(|observed| s::string(&observed)) else {
            return Ok(false);
        };
        let Some(wire) = wire.try_utf8().ok() else {
            return Ok(false);
        };
        let identity = gateway_provider_tool_grammar::tools::original(&wire, names);
        let Some(arguments) = s::present(&function, "arguments").as_ref().and_then(parsed_arguments) else {
            return Ok(false);
        };
        let Some(id) = s::present(&call, "id").and_then(|observed| s::string(&observed)) else {
            return Ok(false);
        };
        tools.push((id, identity.name, arguments));
    }
    let mut actual_text = JsString16::from_units(vec![]);
    let mut actual_thinking = JsString16::from_units(vec![]);
    let mut actual_tools = vec![];
    for item in output {
        match s::present(item, "type").and_then(|observed| s::control(&observed)).as_deref().unwrap_or("") {
            "message" => {
                for part in s::present(item, "content").and_then(|observed| source::array(&observed))
                    .into_iter()
                    .flatten()
                {
                    if let Some(text) = s::present(&part, "text").and_then(|observed| s::string(&observed)) {
                        actual_text = s::concat(&[&actual_text, &text]);
                    }
                    if let Some(refusal) = s::present(&part, "refusal").and_then(|observed| s::string(&observed)) {
                        actual_text = s::concat(&[&actual_text, &refusal]);
                    }
                }
            }
            "reasoning" => {
                for part in s::present(item, "summary").and_then(|observed| source::array(&observed))
                    .into_iter()
                    .flatten()
                {
                    if let Some(text) = s::present(&part, "text").and_then(|observed| s::string(&observed)) {
                        actual_thinking = s::concat(&[&actual_thinking, &text]);
                    }
                }
            }
            "function_call" => {
                let Some(id) = s::present(item, "call_id").and_then(|observed| s::string(&observed)) else {
                    return Ok(false);
                };
                let Some(name) = s::present(item, "name").and_then(|observed| s::string(&observed)) else {
                    return Ok(false);
                };
                let Some(arguments) = s::present(item, "arguments").as_ref().and_then(parsed_arguments) else {
                    return Ok(false);
                };
                actual_tools.push((id, name, arguments));
            }
            "custom_tool_call" => {
                let Some(id) = s::present(item, "call_id").and_then(|observed| s::string(&observed)) else {
                    return Ok(false);
                };
                let Some(name) = s::present(item, "name").and_then(|observed| s::string(&observed)) else {
                    return Ok(false);
                };
                let Some(input) = s::present(item, "input").and_then(|observed| s::string(&observed)) else {
                    return Ok(false);
                };
                actual_tools.push((id, name, s::object(vec![("input", s::quoted(&input)?)])?));
            }
            _ => return Ok(false),
        }
    }
    text == actual_text
        && (actual_thinking.as_units().is_empty() || thinking == actual_thinking)
        && tools == actual_tools

    })
}


impl ReplayStore {
    fn prune(&mut self, now: i64) {
        self.rows.retain(|row| row.expires > now);
    }
    pub fn remember(
        &mut self,
        context: ReplayContext,
        output: &[SourceJson],
        native: SourceJson,
        transport: ReplayTransport,
        now_ms: i64,
    ) -> Result<bool, gateway_core::error::GatewayError> {
    Ok({
        self.remember_with_tools(context, output, native, transport, &ToolMap::new(), now_ms)?
    
    })
}
    pub fn remember_with_tools(
        &mut self,
        context: ReplayContext,
        output: &[SourceJson],
        native: SourceJson,
        transport: ReplayTransport,
        names: &ToolMap,
        now_ms: i64,
    ) -> Result<bool, gateway_core::error::GatewayError> {
    Ok({
        if !context_valid(&context)
            || output.is_empty()
            || output.len() > 256
            || !native_valid(&native, transport)
            || !match transport {
                ReplayTransport::Messages => native_messages_match(&native, output)?,
                ReplayTransport::Chat => native_chat_matches(&native, output, names)?,
            }
        {
            return Ok(false);
        }
        let ids = output
            .iter()
            .map(|v| {
                s::present(v, "id").and_then(|observed| s::string(&observed))
                    .filter(|s| !s.as_units().is_empty())
                    .and_then(|s| s.try_utf8().ok())
            })
            .collect::<Option<Vec<_>>>();
        let Some(ids) = ids else { return Ok(false )};
        if ids.iter().collect::<BTreeSet<_>>().len() != ids.len() {
            return Ok(false);
        }
        let mut fingerprints = Vec::new();
        for item in output {
            let Some(value) = fingerprint(item)? else { return Ok(false); };
            fingerprints.push(value);
        }
        self.prune(now_ms);
        if self
            .rows
            .iter()
            .any(|r| r.context == context && r.ids.iter().any(|id| ids.contains(id)))
        {
            return Ok(false);
        }
        let bytes =
            native.as_str().len() + fingerprints.iter().map(|v| v.as_str().len()).sum::<usize>();
        if bytes > self.max_entry_bytes.min(1024 * 1024)
            || bytes > self.max_bytes.min(8 * 1024 * 1024)
        {
            return Ok(false);
        }
        let limit = self.max_entries.min(64);
        if limit == 0 {
            return Ok(false);
        }
        while self.rows.len() >= limit
            || self.rows.iter().map(|r| r.bytes).sum::<usize>() + bytes
                > self.max_bytes.min(8 * 1024 * 1024)
        {
            if self.rows.pop_front().is_none() {
                return Ok(false);
            }
        }
        self.rows.push_back(Receipt {
            context,
            ids,
            fingerprints,
            native,
            transport,
            expires: now_ms.saturating_add(self.ttl_ms.clamp(1, 1_800_000)),
            bytes,
        });
        true
    
    })
}
    pub fn restore(
        &mut self,
        context: &ReplayContext,
        input: &[SourceJson],
        transport: ReplayTransport,
        now_ms: i64,
    ) -> Result<Option<Vec<ReplayEntry>>, gateway_core::error::GatewayError> {
    Ok({
        if !context_valid(context) {
            return Ok(None);
        }
        self.prune(now_ms);
        let mut result = vec![];
        let mut used = BTreeSet::new();
        let mut i = 0;
        while i < input.len() {
            let item = &input[i];
            represented!(source::entries(item));
            let id = s::present(item, "id").and_then(|observed| s::control(&observed)).unwrap_or_default();
            let found = self
                .rows
                .iter()
                .enumerate()
                .find(|(_, r)| r.context == *context && r.ids.iter().any(|s| s == &id));
            if let Some((index, row)) = found {
                if row.transport != transport
                    || row.ids.first().map(String::as_str) != Some(&id)
                    || !used.insert(index)
                    || i + row.ids.len() > input.len()
                {
                    return Ok(None);
                }
                for (offset, expected) in row.fingerprints.iter().enumerate() {
                    if s::present(&input[i + offset], "id").and_then(|observed| s::control(&observed)).as_deref()
                        != Some(&row.ids[offset])
                        || fingerprint(&input[i + offset])?.as_ref() != Some(expected)
                    {
                        return Ok(None);
                    }
                }
                result.push(ReplayEntry::NativeAssistant {
                    transport,
                    payload: row.native.clone(),
                });
                i += row.ids.len();
            } else {
                if s::present(item, "type").is_some_and(|observed| s::is(&observed, "reasoning"))
                    || id.starts_with("msg_resp_")
                    || id.starts_with("rs_resp_")
                    || id.starts_with("fc_resp_")
                    || id.starts_with("ctc_resp_")
                {
                    return Ok(None);
                }
                result.push(ReplayEntry::Original(item.clone()));
                i += 1;
            }
        }
        Some(result)
    
    })
}
    /// Retains the assistant envelope emitted by `DecodedResponse::native_replay`.
    pub fn remember_decoded_native(
        &mut self,
        context: ReplayContext,
        output: &[SourceJson],
        native: SourceJson,
        transport: ReplayTransport,
        names: &ToolMap,
        now_ms: i64,
    ) -> Result<bool, gateway_core::error::GatewayError> {
    Ok({
        let payload = match transport {
            ReplayTransport::Messages => {
                if !s::present(&native, "role").is_some_and(|observed| s::is(&observed, "assistant")) {
                    return Ok(false);
                }
                match s::present(&native, "content") { Some(value) => value, None => return Ok(false )}
            }
            ReplayTransport::Chat => native,
        };
        self.remember_with_tools(context, output, payload, transport, names, now_ms)?
    
    })
}
    /// Restores a complete provider history into the existing PrepareOptions seam.
    ///
    /// The helper never mutates the raw request or its decode echo. It fails
    /// closed when an input item cannot be represented by the selected transport.
    pub fn restore_prepare_options(
        &mut self,
        context: &ReplayContext,
        raw: &SourceJson,
        options: &mut PrepareOptions,
        transport: ReplayTransport,
        now_ms: i64,
    ) -> Result<bool, gateway_core::error::GatewayError> {
    Ok({
        let Some(input) = s::present(raw, "input").and_then(|observed| source::array(&observed)) else {
            return Ok(false);
        };
        let Some(entries) = self.restore(context, &input, transport, now_ms)? else {
            return Ok(false);
        };
        let Some(messages) = (match transport { ReplayTransport::Messages => gateway_provider_claude_body::source_anthropic::replay_messages(raw, &entries)?, ReplayTransport::Chat => gateway_provider_chat::source_anthropic::replay_messages(raw, &entries)? })
        else {
            return Ok(false);
        };
        options.restored_native_messages = Some(messages);
        true
    
    })
}
}



impl std::fmt::Debug for ReplayStore {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ReplayStore")
            .field("receipt_count", &self.rows.len())
            .finish_non_exhaustive()
    }
}
