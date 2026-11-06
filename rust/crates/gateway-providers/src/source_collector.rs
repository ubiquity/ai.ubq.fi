//! Source-bearing event-to-Chat observation; upstream parsing remains in sse.rs.
use crate::{source as s, types::*};
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as json,
};
use serde_json::Value;
use std::collections::BTreeMap;
#[derive(Clone, Default)]
struct Call {
    id: JsString16,
    name: JsString16,
    args: JsString16,
    index: usize,
}
#[derive(Clone)]
pub struct ChatCollector {
    plan: DecodePlan,
    started: bool,
    finished: bool,
    /// Text then refusal parts, indexed by `usize::from(refusal)`.
    parts: [Parts; 2],
    calls: BTreeMap<usize, Call>,
    pub usage: Option<Value>,
}
/// Source-ordered part text keyed `output:<index>:<part>` or `item:<id>:<part>`, as the source `Map` is.
type Parts = Vec<(String, JsString16)>;
/// Storage key, the item-id alias a positional part also writes (terminal events may omit
/// output_index), and whether the part is positional.
struct Keys(String, Option<String>, bool);
fn position(value: &SourceJson) -> Option<String> {
    let number = json::number(value).map(|n| n.to_string());
    number.or_else(|| s::control(value))
}
fn keys(output: Option<String>, item: Option<JsString16>, part: String) -> Keys {
    let item = item.filter(|id| !s::trim(id).as_units().is_empty());
    let item = item.map(|id| format!("item:{:?}:{part}", s::trim(&id).as_units()));
    match output {
        Some(output) => Keys(format!("output:{output}:{part}"), item, true),
        None => Keys(item.unwrap_or(format!("output:0:{part}")), None, false),
    }
}
fn read(parts: &Parts, key: &str) -> JsString16 {
    let found = parts.iter().find(|(k, _)| k == key);
    found.map(|(_, v)| v.clone()).unwrap_or_default()
}
fn write(parts: &mut Parts, keys: &Keys, value: &JsString16) {
    for key in std::iter::once(&keys.0).chain(&keys.1) {
        match parts.iter_mut().find(|(k, _)| k == key) {
            Some(entry) => entry.1 = value.clone(),
            None => parts.push((key.clone(), value.clone())),
        }
    }
}
/// An id-less terminal part repeating its position, the sole item part, or all positional parts.
fn repeated(parts: &Parts, keys: &Keys, done: &JsString16) -> Option<JsString16> {
    let hit = |e: &&(String, JsString16)| e.1.as_units().starts_with(done.as_units());
    if keys.0.starts_with("item:") || keys.1.is_some() || done.as_units().is_empty() {
        return None;
    }
    let at = parts.iter().find(|(k, _)| keys.2 && *k == keys.0);
    let (items, outputs): (Vec<_>, Vec<_>) = parts.iter().partition(|e| e.0.starts_with("item:"));
    let single = items.first().copied().filter(|_| items.len() == 1);
    if let Some(e) = at.filter(hit).or(single.filter(hit)) {
        return Some(e.1.clone());
    }
    let all = if outputs.is_empty() { items } else { outputs };
    let joined = s::concat(&all.iter().map(|e| &e.1).collect::<Vec<_>>());
    (all.len() >= 2 && joined == *done).then_some(joined)
}
fn string(value: &SourceJson, message: &str) -> Result<JsString16, GatewayError> {
    s::string(value).ok_or_else(|| malformed(message))
}
fn suffix(full: &JsString16, old: &JsString16, message: &str) -> Result<JsString16, GatewayError> {
    full.as_units()
        .strip_prefix(old.as_units())
        .map(|units| JsString16::from_units(units.to_vec()))
        .ok_or_else(|| malformed(message))
}
fn number(value: &SourceJson) -> usize {
    json::number(value).unwrap_or(0.0) as usize
}
pub(crate) fn usage(value: &SourceJson) -> Option<Value> {
    (!s::is_null(value))
        .then(|| value.deserialize().ok())
        .flatten()
}
/// Settled usage: a Chat body or chunk projects through source `extractChatUsageTokens`.
pub(crate) fn accounting_usage(chat: bool, body: &SourceJson) -> Option<Value> {
    let usage = usage(&s::get(body, "usage"));
    let fields = gateway_core::source_usage::chat_usage_fields;
    usage.and_then(|usage| if chat { fields(&usage) } else { Some(usage) })
}
impl ChatCollector {
    pub fn new(plan: DecodePlan) -> Self {
        Self {
            plan,
            started: false,
            finished: false,
            parts: [Vec::new(), Vec::new()],
            calls: BTreeMap::new(),
            usage: None,
        }
    }
    fn chunk(&self, delta: SourceJson, finish: SourceJson) -> ProviderEvent {
        ProviderEvent::chat_source(s::object(vec![
            ("id", s::text(&self.plan.response_id)),
            ("object", s::text("chat.completion.chunk")),
            ("created", s::generated(&self.plan.created_at)),
            ("model", s::quoted(self.plan.model.as_js_string())),
            (
                "choices",
                s::array(&[s::object(vec![
                    ("index", s::generated(&0)),
                    ("delta", delta),
                    ("finish_reason", finish),
                ])]),
            ),
        ]))
    }
    fn text_chunk(&self, text: &JsString16, refusal: bool) -> ProviderEvent {
        let field = if refusal { "refusal" } else { "content" };
        self.chunk(s::object(vec![(field, s::quoted(text))]), s::null())
    }
    fn call_chunk(
        &self,
        index: usize,
        id: Option<&JsString16>,
        name: Option<&JsString16>,
        args: &JsString16,
    ) -> ProviderEvent {
        let mut function = s::object(vec![("arguments", s::quoted(args))]);
        if let Some(name) = name {
            s::set(&mut function, "name", s::quoted(name));
        }
        let mut call = s::object(vec![
            ("index", s::generated(&index)),
            ("function", function),
        ]);
        if let Some(id) = id {
            s::set(&mut call, "id", s::quoted(id));
            s::set(&mut call, "type", s::text("function"));
        }
        let delta = s::object(vec![("tool_calls", s::array(&[call]))]);
        self.chunk(delta, s::null())
    }
    fn call(&mut self, index: usize) -> &mut Call {
        let next = self.calls.len();
        let fresh = || Call {
            index: next,
            ..Default::default()
        };
        self.calls.entry(index).or_insert_with(fresh)
    }
    fn complete(
        &mut self,
        refusal: bool,
        keys: &Keys,
        full: &JsString16,
        terminal: bool,
    ) -> Result<Option<ProviderEvent>, GatewayError> {
        let parts = &mut self.parts[usize::from(refusal)];
        if let Some(repeat) = terminal.then(|| repeated(parts, keys, full)).flatten() {
            write(parts, keys, &repeat);
            return Ok(None);
        }
        let previous = read(parts, &keys.0);
        let message = "Completed output disagrees with streamed prefix.";
        let delta = suffix(full, &previous, message)?;
        write(parts, keys, full);
        Ok((!delta.as_units().is_empty()).then(|| self.text_chunk(&delta, refusal)))
    }
    fn reconcile(
        &mut self,
        index: usize,
        output: Option<String>,
        item_id: Option<JsString16>,
        item: &SourceJson,
    ) -> Result<Vec<ProviderEvent>, GatewayError> {
        let mut result = vec![];
        if s::is(&s::get(item, "type"), "message") {
            let id = s::string(&s::get(item, "id")).or(item_id);
            for (content_index, part) in json::array(&s::get(item, "content"))
                .unwrap_or_default()
                .iter()
                .enumerate()
            {
                let refusal = s::is(&s::get(part, "type"), "refusal");
                if !refusal && !s::is(&s::get(part, "type"), "output_text") {
                    continue;
                }
                let full = string(
                    &s::get(part, if refusal { "refusal" } else { "text" }),
                    "Completed output content is not string.",
                )?;
                let keys = keys(output.clone(), id.clone(), content_index.to_string());
                result.extend(self.complete(refusal, &keys, &full, true)?);
            }
        } else if s::is(&s::get(item, "type"), "function_call") {
            let id = s::nonempty(&s::get(item, "call_id"))
                .ok_or_else(|| malformed("Completed function call has no call_id."))?;
            let name = s::nonempty(&s::get(item, "name"))
                .ok_or_else(|| malformed("Completed function call has no name."))?;
            let args = s::get(item, "arguments");
            let args = string(&args, "Completed function arguments are not string.")?;
            let added = !self.calls.contains_key(&index);
            let call = self.call(index);
            if (!call.id.as_units().is_empty() && call.id != id)
                || (!call.name.as_units().is_empty() && call.name != name)
            {
                return Err(malformed(
                    "Completed function identity disagrees with streamed metadata.",
                ));
            }
            let message = "Completed function arguments disagree with streamed prefix.";
            let delta = suffix(&args, &call.args, message)?;
            call.id = id.clone();
            call.name = name.clone();
            call.args = args.clone();
            let slot = call.index;
            if added {
                result.push(self.call_chunk(slot, Some(&id), Some(&name), &args));
            } else if !delta.as_units().is_empty() {
                result.push(self.call_chunk(slot, None, None, &delta));
            }
        }
        Ok(result)
    }
    /// The terminal Chat chunks once every response output part has been reconciled.
    fn complete_response(
        &mut self,
        response: &SourceJson,
        result: &mut Vec<ProviderEvent>,
    ) -> Result<(), GatewayError> {
        let (mut parts, calls) = (self.parts.iter().flatten(), !self.calls.is_empty());
        if parts.all(|(_, text)| text.as_units().is_empty()) && !calls {
            return Err(GatewayError::new(
                502,
                "Upstream response completed with no translated semantic output.",
                Some("empty_upstream_completion"),
            )
            .with_type("server_error"));
        }
        self.usage = usage(&s::get(response, "usage"));
        let finish = if calls { "tool_calls" } else { "stop" };
        result.push(self.chunk(s::object(vec![]), s::text(finish)));
        let include = ["stream_options", "include_usage"];
        if s::boolean(&s::path(&self.plan.request, &include)) == Some(true) {
            let usage = self.usage.as_ref();
            let usage = usage.and_then(crate::usage::responses_to_chat);
            result.push(ProviderEvent::chat_source(s::object(vec![
                ("id", s::text(&self.plan.response_id)),
                ("object", s::text("chat.completion.chunk")),
                ("created", s::generated(&self.plan.created_at)),
                ("model", s::quoted(self.plan.model.as_js_string())),
                ("choices", s::array(&[])),
                ("usage", s::generated(&usage)),
            ])));
        }
        result.push(ProviderEvent::chat(serde_json::json!("[DONE]")));
        self.finished = true;
        Ok(())
    }
    pub fn push(&mut self, event: &SourceJson) -> Result<Vec<ProviderEvent>, GatewayError> {
        if self.finished {
            return Ok(vec![]);
        }
        let mut result = vec![];
        let kind = s::control(&s::get(event, "type")).unwrap_or_default();
        if kind == "response.created" {
            if let Some(id) = s::control(&s::path(event, &["response", "id"])) {
                self.plan.response_id = id;
            }
            if let Some(created) = json::number(&s::path(event, &["response", "created_at"])) {
                self.plan.created_at = created as i64;
            }
        }
        if !self.started {
            let role = s::generated(&serde_json::json!({"role":"assistant","content":""}));
            result.push(self.chunk(role, s::null()));
            self.started = true;
        }
        let index = number(&s::get(event, "output_index"));
        let output = position(&s::get(event, "output_index"));
        let item_id = s::string(&s::get(event, "item_id"));
        let content = position(&s::get(event, "content_index")).unwrap_or_else(|| "0".into());
        let part_keys = keys(output.clone(), item_id.clone(), content);
        match kind.as_str() {
            "response.output_text.delta" | "response.refusal.delta" => {
                let text = string(&s::get(event, "delta"), "Output delta is not string.")?;
                let refusal = kind == "response.refusal.delta";
                let parts = &mut self.parts[usize::from(refusal)];
                let previous = read(parts, &part_keys.0);
                write(parts, &part_keys, &s::concat(&[&previous, &text]));
                if !text.as_units().is_empty() {
                    result.push(self.text_chunk(&text, refusal));
                }
            }
            "response.output_text.done" | "response.refusal.done" => {
                let refusal = kind == "response.refusal.done";
                let full = string(
                    &s::get(event, if refusal { "refusal" } else { "text" }),
                    "Completed output is not string.",
                )?;
                result.extend(self.complete(refusal, &part_keys, &full, false)?);
            }
            "response.output_item.added" => {
                let item = s::get(event, "item");
                if s::is(&s::get(&item, "type"), "function_call") {
                    let call = Call {
                        id: s::string(&s::get(&item, "call_id")).unwrap_or_default(),
                        name: s::string(&s::get(&item, "name")).unwrap_or_default(),
                        args: s::string(&s::get(&item, "arguments")).unwrap_or_default(),
                        index: self.calls.len(),
                    };
                    let (id, name) = (Some(&call.id), Some(&call.name));
                    result.push(self.call_chunk(call.index, id, name, &call.args));
                    self.calls.insert(index, call);
                }
            }
            "response.function_call_arguments.delta" => {
                let delta = s::get(event, "delta");
                let delta = string(&delta, "Tool argument delta is not string.")?;
                let call = self.call(index);
                call.args = s::concat(&[&call.args, &delta]);
                let slot = call.index;
                result.push(self.call_chunk(slot, None, None, &delta));
            }
            "response.function_call_arguments.done" => {
                let full = s::get(event, "arguments");
                let full = string(&full, "Tool arguments are not string.")?;
                let call = self.call(index);
                let message = "Tool arguments disagree with streamed prefix.";
                let delta = suffix(&full, &call.args, message)?;
                call.args = full;
                let slot = call.index;
                if !delta.as_units().is_empty() {
                    result.push(self.call_chunk(slot, None, None, &delta));
                }
            }
            "response.output_item.done" => {
                result.extend(self.reconcile(index, output, item_id, &s::get(event, "item"))?)
            }
            "response.output" | "response.completed" => {
                let response = s::get(event, "response");
                let terminal = kind == "response.completed";
                let source = if terminal { &response } else { event };
                for (index, item) in json::array(&s::get(source, "output"))
                    .unwrap_or_default()
                    .iter()
                    .enumerate()
                {
                    let output = Some(index.to_string());
                    result.extend(self.reconcile(index, output, item_id.clone(), item)?);
                }
                if terminal {
                    self.complete_response(&response, &mut result)?;
                }
            }
            "response.failed" | "response.incomplete" | "error" => {
                result.push(ProviderEvent::chat(serde_json::json!({"error":{"message":"Upstream stream ended without response.completed.","type":"server_error","code":"upstream_stream_error","param":null}})));
                self.finished = true;
            }
            _ => (),
        }
        Ok(result)
    }
}
