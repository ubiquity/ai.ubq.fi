use gateway_provider_source::source as s;
use gateway_provider_protocol::types::DecodePlan;
use gateway_provider_events::event::ProviderEvent;
use gateway_provider_source::extras::malformed;
use gateway_core::error::GatewayError;
use gateway_core::js_string::JsString16;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;
use serde_json::Value;
use std::collections::BTreeMap;
use gateway_provider_source::source_collector::usage;

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
    text: crate::parts::Parts,
    refusal: crate::parts::Parts,
    calls: BTreeMap<usize, Call>,
    pub usage: Option<Value>,
}


fn string(value: Option<&SourceJson>, message: &str) -> Result<JsString16, GatewayError> {
    value.and_then(s::string).ok_or_else(|| malformed(message))
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


impl ChatCollector {
    pub fn new(plan: DecodePlan) -> Self {
        Self {
            plan,
            started: false,
            finished: false,
            text: crate::parts::Parts::default(),
            refusal: crate::parts::Parts::default(),
            calls: BTreeMap::new(),
            usage: None,
        }
    }
    fn chunk(&self, delta: SourceJson, finish: SourceJson) -> Result<ProviderEvent, gateway_core::error::GatewayError> {
    Ok(ProviderEvent::chat_source(s::object(vec![
            ("id", s::text(&self.plan.response_id)?),
            ("object", s::text("chat.completion.chunk")?),
            ("created", s::scalar_json(&serde_json::json!(self.plan.created_at))?),
            ("model", s::quoted(self.plan.model.as_js_string())?),
            (
                "choices",
                s::array(&[s::object(vec![
                    ("index", s::scalar_json(&serde_json::json!(0))?),
                    ("delta", delta),
                    ("finish_reason", finish),
                ])?])?,
            ),
        ])?))
}
    fn text_chunk(&self, text: &JsString16, refusal: bool) -> Result<ProviderEvent, gateway_core::error::GatewayError> {
    self.chunk(
            s::object(vec![(
                if refusal { "refusal" } else { "content" },
                s::quoted(text)?,
            )])?,
            s::null()?,
        )
}
    fn call_chunk(
        &self,
        index: usize,
        id: Option<&JsString16>,
        name: Option<&JsString16>,
        args: &JsString16,
    ) -> Result<ProviderEvent, gateway_core::error::GatewayError> {
        let mut function = s::object(vec![("arguments", s::quoted(args)?)])?;
        if let Some(name) = name {
            s::set(&mut function, "name", s::quoted(name)?)?;
        }
        let mut call = s::object(vec![
            ("index", s::scalar_json(&serde_json::json!(index))?),
            ("function", function),
        ])?;
        if let Some(id) = id {
            s::set(&mut call, "id", s::quoted(id)?)?;
            s::set(&mut call, "type", s::text("function")?)?;
        }
    self.chunk(
            s::object(vec![("tool_calls", s::array(&[call])?)])?,
            s::null()?,
        )
}
    fn reconcile(
        &mut self,
        index: usize,
        item: Option<&SourceJson>,
    ) -> Result<Vec<ProviderEvent>, GatewayError> {
        let mut result = vec![];
        let Some(item) = item else { return Ok(result); };
        if s::present(item, "type").is_some_and(|observed| s::is(&observed, "message")) {
            for (content_index, part) in s::present(item, "content").and_then(|observed| json::array(&observed))
                .unwrap_or_default()
                .iter()
                .enumerate()
            {
                let refusal = s::present(part, "type").is_some_and(|observed| s::is(&observed, "refusal"));
                if !refusal && !s::present(part, "type").is_some_and(|observed| s::is(&observed, "output_text")) {
                    continue;
                }
                let full = string(s::present(part, if refusal { "refusal" } else { "text" }).as_ref(), "Completed output content is not string.")?;
                let map = if refusal {
                    &mut self.refusal
                } else {
                    &mut self.text
                };
                let delta = map.completed(index, content_index, item, &full)?;
                if !delta.as_units().is_empty() {
                    result.push(self.text_chunk(&delta, refusal)?);
                }
            }
        } else if s::present(item, "type").is_some_and(|observed| s::is(&observed, "function_call")) {
            let id = s::present(item, "call_id").and_then(|observed| s::nonempty(&observed))
                .ok_or_else(|| malformed("Completed function call has no call_id."))?;
            let name = s::present(item, "name").and_then(|observed| s::nonempty(&observed))
                .ok_or_else(|| malformed("Completed function call has no name."))?;
            let args = string(s::present(item, "arguments").as_ref(), "Completed function arguments are not string.")?;
            let next = self.calls.len();
            let added = !self.calls.contains_key(&index);
            let call = self.calls.entry(index).or_insert_with(|| Call {
                index: next,
                ..Default::default()
            });
            if (!call.id.as_units().is_empty() && call.id != id)
                || (!call.name.as_units().is_empty() && call.name != name)
            {
                return Err(malformed(
                    "Completed function identity disagrees with streamed metadata.",
                ));
            }
            let delta = suffix(
                &args,
                &call.args,
                "Completed function arguments disagree with streamed prefix.",
            )?;
            call.id = id.clone();
            call.name = name.clone();
            call.args = args.clone();
            let slot = call.index;
            if added {
                result.push(self.call_chunk(slot, Some(&id), Some(&name), &args)?);
            } else if !delta.as_units().is_empty() {
                result.push(self.call_chunk(slot, None, None, &delta)?);
            }
        }
        Ok(result)
    }
    fn complete(&mut self, response: Option<&SourceJson>) -> Result<Vec<ProviderEvent>, GatewayError> {
        let mut result = vec![];
                for (index, item) in response.and_then(|response| s::present(response, "output")).and_then(|observed| json::array(&observed))
                    .unwrap_or_default()
                    .iter()
                    .enumerate()
                {
                    result.extend(self.reconcile(index, Some(item))?);
                }
                if self.text.is_empty()
                    && self.refusal.is_empty()
                    && self.calls.is_empty()
                {
                    return Err(GatewayError::new(
                        502,
                        "Upstream response completed with no translated semantic output.",
                        Some("empty_upstream_completion"),
                    )
                    .with_type("server_error"));
                }
                self.usage = response.and_then(|response| s::present(response, "usage")).as_ref().and_then(usage);
                result.push(self.chunk(
                    s::object(vec![])?,
                    s::text(if self.calls.is_empty() {
                        "stop"
                    } else {
                        "tool_calls"
                    })?,
                )?);
                if s::present_path(&self.plan.request, &["stream_options", "include_usage"]).and_then(|observed| s::boolean(&observed)) == Some(true)
                {
                    let usage = self
                        .usage
                        .as_ref()
                        .and_then(gateway_provider_source::usage::responses_to_chat);
                    result.push(ProviderEvent::chat_source(s::object(vec![
                        ("id", s::text(&self.plan.response_id)?),
                        ("object", s::text("chat.completion.chunk")?),
                        ("created", s::scalar_json(&serde_json::json!(self.plan.created_at))?),
                        ("model", s::quoted(self.plan.model.as_js_string())?),
                        ("choices", s::array(&[])?),
                        ("usage", match &usage { Some(value) => s::scalar_json(value)?, None => s::null()? }),
                    ])?));
                }
                result.push(ProviderEvent::chat(serde_json::json!("[DONE]"))?);
                self.finished = true;
        Ok(result)
    }
    pub fn push(&mut self, event: &SourceJson) -> Result<Vec<ProviderEvent>, GatewayError> {
        if self.finished {
            return Ok(vec![]);
        }
        let mut result = vec![];
        let kind = s::present(event, "type").and_then(|observed| s::control(&observed)).unwrap_or_default();
        if kind == "response.created" {
            if let Some(id) = s::present_path(event, &["response", "id"]).and_then(|observed| s::control(&observed)) {
                self.plan.response_id = id;
            }
            if let Some(created) = s::present_path(event, &["response", "created_at"]).and_then(|observed| json::number(&observed)) {
                self.plan.created_at = created as i64;
            }
        }
        if !self.started {
            result.push(self.chunk(
                s::scalar_json(&serde_json::json!({"role":"assistant","content":""}))?,
                s::null()?,
            )?);
            self.started = true;
        }
        let index = s::present(event, "output_index").as_ref().map_or(0, number);
        match kind.as_str() {
            "response.output_text.delta" | "response.refusal.delta" => {
                let text = string(s::present(event, "delta").as_ref(), "Output delta is not string.")?;
                let refusal = kind == "response.refusal.delta";
                let map = if refusal {
                    &mut self.refusal
                } else {
                    &mut self.text
                };
                map.delta(event, &text)?;
                if !text.as_units().is_empty() {
                    result.push(self.text_chunk(&text, refusal)?);
                }
            }
            "response.output_text.done" | "response.refusal.done" => {
                let refusal = kind == "response.refusal.done";
                let full = string(s::present(event, if refusal { "refusal" } else { "text" }).as_ref(), "Completed output is not string.")?;
                let map = if refusal {
                    &mut self.refusal
                } else {
                    &mut self.text
                };
                let delta = map.done(event, &full)?;
                if !delta.as_units().is_empty() {
                    result.push(self.text_chunk(&delta, refusal)?);
                }
            }
            "response.output_item.added" => {
                let Some(item) = s::present(event, "item") else { return Ok(result); };
                if s::present(&item, "type").is_some_and(|observed| s::is(&observed, "function_call")) {
                    let call = Call {
                        id: s::present(&item, "call_id").and_then(|observed| s::string(&observed)).unwrap_or_default(),
                        name: s::present(&item, "name").and_then(|observed| s::string(&observed)).unwrap_or_default(),
                        args: s::present(&item, "arguments").and_then(|observed| s::string(&observed)).unwrap_or_default(),
                        index: self.calls.len(),
                    };
                    result.push(self.call_chunk(
                        call.index,
                        Some(&call.id),
                        Some(&call.name),
                        &call.args,
                    )?);
                    self.calls.insert(index, call);
                }
            }
            "response.function_call_arguments.delta" => {
                let delta = string(s::present(event, "delta").as_ref(), "Tool argument delta is not string.")?;
                let next = self.calls.len();
                let call = self.calls.entry(index).or_insert_with(|| Call {
                    index: next,
                    ..Default::default()
                });
                call.args = s::concat(&[&call.args, &delta]);
                let slot = call.index;
                result.push(self.call_chunk(slot, None, None, &delta)?);
            }
            "response.function_call_arguments.done" => {
                let full = string(s::present(event, "arguments").as_ref(), "Tool arguments are not string.")?;
                let next = self.calls.len();
                let call = self.calls.entry(index).or_insert_with(|| Call {
                    index: next,
                    ..Default::default()
                });
                let delta = suffix(
                    &full,
                    &call.args,
                    "Tool arguments disagree with streamed prefix.",
                )?;
                call.args = full;
                let slot = call.index;
                if !delta.as_units().is_empty() {
                    result.push(self.call_chunk(slot, None, None, &delta)?);
                }
            }
            "response.output_item.done" => {
                result.extend(self.reconcile(index, s::present(event, "item").as_ref())?)
            }
            "response.output" => {
                for (index, item) in s::present(event, "output").and_then(|observed| json::array(&observed))
                    .unwrap_or_default()
                    .iter()
                    .enumerate()
                {
                    result.extend(self.reconcile(index, Some(item))?);
                }
            }
            "response.completed" => {
                result.extend(self.complete(s::present(event, "response").as_ref())?);
            }
            "response.failed" | "response.incomplete" | "error" => {
                result.push(ProviderEvent::chat(serde_json::json!({"error":{"message":"Upstream stream ended without response.completed.","type":"server_error","code":"upstream_stream_error","param":null}}))?);
                self.finished = true;
            }
            _ => (),
        }
        Ok(result)
    }
}
