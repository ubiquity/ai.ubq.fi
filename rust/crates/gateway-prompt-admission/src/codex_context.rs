//! Codex subscription stream compatibility: installed Codex ignores a generic terminal `error`
//! carrying `context_length_exceeded`, while `response.failed.response.error.code` marks the
//! context full. Only the client-visible frame changes; provider capture, lifecycle, settlement
//! and fallback keep observing the original upstream error.
use serde_json::{Map, Value, json};
use std::collections::BTreeMap;

const HOSTED_TOOLS: &str = "code_interpreter_call computer_call file_search_call image_generation_call mcp_call web_search_call";
#[derive(Clone, Debug, Default, PartialEq)]
struct Recovered {
    id: String,
    text: String,
    completed: bool,
    refusal: bool,
}
/// Visible response identity, template and observed output, as the source owned stream keeps.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CodexContextCompat {
    response_id: Option<String>,
    template: Map<String, Value>,
    items: Vec<Value>,
    item_ids: BTreeMap<String, usize>,
    texts: Vec<Recovered>,
}
fn trimmed(value: &Value) -> Option<String> {
    value
        .as_str()
        .map(|text| text.trim().to_owned())
        .filter(|text| !text.is_empty())
}
fn item_id(event: &Value) -> Option<String> {
    trimmed(&event["item_id"]).or_else(|| trimmed(&event["item"]["id"]))
}
/// Source `hostedToolLifecycle`: `response.<type>.<lifecycle>` for one hosted tool type.
fn hosted(kind: &str) -> Option<(&'static str, &'static str)> {
    let rest = kind.strip_prefix("response.")?;
    HOSTED_TOOLS.split(' ').find_map(|tool| {
        let lifecycle = rest.strip_prefix(tool)?.strip_prefix('.')?;
        ["completed", "in_progress", "failed"]
            .into_iter()
            .find(|name| *name == lifecycle)
            .map(|lifecycle| (tool, lifecycle))
    })
}
impl CodexContextCompat {
    fn remember(&mut self, item: Value) {
        let id = trimmed(&item["id"]);
        match id.as_ref().and_then(|id| self.item_ids.get(id)) {
            Some(index) => self.items[*index] = item,
            None => {
                if let Some(id) = id {
                    self.item_ids.insert(id, self.items.len());
                }
                self.items.push(item);
            }
        }
    }
    fn remember_text(&mut self, event: &Value, kind: &str) {
        let field = match kind {
            "response.output_text.delta" | "response.refusal.delta" => "delta",
            "response.output_text.done" => "text",
            "response.refusal.done" => "refusal",
            _ => return,
        };
        let Some(text) = event[field].as_str().filter(|text| !text.is_empty()) else {
            return;
        };
        let refusal = kind.starts_with("response.refusal.");
        let id = item_id(event).unwrap_or_else(|| {
            let index = event["output_index"].as_str().map(str::to_owned);
            format!(
                "msg_recovered_{}",
                index.unwrap_or_else(|| self.texts.len().to_string())
            )
        });
        let position = self.texts.iter().position(|current| current.id == id);
        let previous = position.map(|index| self.texts[index].clone());
        let done = kind.ends_with(".done");
        if done
            && previous
                .as_ref()
                .is_some_and(|current| !text.starts_with(&current.text))
        {
            return;
        }
        let prefix = previous.as_ref().filter(|_| !done).map(|c| c.text.as_str());
        let value = Recovered {
            id,
            text: format!("{}{text}", prefix.unwrap_or_default()),
            completed: done || previous.is_some_and(|current| current.completed),
            refusal,
        };
        match position {
            Some(index) => self.texts[index] = value,
            None => self.texts.push(value),
        }
    }
    /// Source `observeVisibleEvent` for one client-visible Responses event.
    pub fn observe(&mut self, event: &Value) {
        let kind = event["type"].as_str().unwrap_or_default().to_owned();
        let candidate =
            trimmed(&event["response_id"]).or_else(|| trimmed(&event["response"]["id"]));
        if self.response_id.is_none() {
            self.response_id = candidate;
        }
        if let Some(response) = event["response"].as_object() {
            for (key, value) in response {
                self.template.insert(key.clone(), value.clone());
            }
        }
        self.remember_text(event, &kind);
        if kind == "response.output" {
            let output = match &event["output"] {
                Value::Null => &event["response"]["output"],
                output => output,
            };
            for item in output.as_array().into_iter().flatten() {
                if item.is_object() {
                    self.remember(item.clone());
                }
            }
        }
        if let ("response.output_item.added" | "response.output_item.done", Some(item)) =
            (kind.as_str(), event["item"].as_object())
        {
            let mut item = item.clone();
            if kind == "response.output_item.added" {
                item.insert("status".into(), json!("incomplete"));
            }
            self.remember(Value::Object(item));
        } else if let Some((tool, lifecycle)) = hosted(&kind) {
            let id = item_id(event);
            let mut item = id
                .as_ref()
                .and_then(|id| self.item_ids.get(id))
                .and_then(|index| self.items[*index].as_object().cloned())
                .unwrap_or_default();
            let fallback = format!("tool_recovered_{}", self.items.len());
            item.insert("id".into(), json!(id.unwrap_or(fallback)));
            item.insert("type".into(), json!(tool));
            item.insert("status".into(), json!(lifecycle));
            self.remember(Value::Object(item));
        }
    }
    /// Source `failureOutput`: completed output items, then recovered text messages.
    fn output(&self) -> Vec<Value> {
        let mut output = self.items.clone();
        let mut ids = self.item_ids.clone();
        for recovered in &self.texts {
            let index = ids.get(&recovered.id).copied();
            let existing = index.map(|index| &output[index]);
            if existing.is_some_and(|item| {
                item["content"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .any(|part| {
                        matches!(part["type"].as_str(), Some("output_text" | "text"))
                            && part["text"].as_str().is_some_and(|text| !text.is_empty())
                    })
            }) {
                continue;
            }
            let content = if recovered.refusal {
                json!([{"type":"refusal","refusal":recovered.text}])
            } else {
                json!([{"type":"output_text","text":recovered.text,"annotations":[]}])
            };
            let status = if recovered.completed {
                "completed"
            } else {
                "incomplete"
            };
            let message = json!({"id":recovered.id,"type":"message","status":status,"role":"assistant","content":content});
            match index {
                Some(index) => {
                    let mut merged = output[index].as_object().cloned().unwrap_or_default();
                    merged.extend(message.as_object().cloned().unwrap_or_default());
                    output[index] = Value::Object(merged);
                }
                None => {
                    ids.insert(recovered.id.clone(), output.len());
                    output.push(message);
                }
            }
        }
        output
    }
    /// Source `codexContextFailureEvent`: a terminal `error` with exact code
    /// `context_length_exceeded` and a tracked response id becomes one `response.failed`.
    pub fn failure_event(&self, event: &Value) -> Option<Value> {
        let id = self.response_id.as_ref()?;
        if event["type"] != "error" {
            return None;
        }
        let error = match event.get("error").and_then(Value::as_object) {
            Some(nested) => nested.clone(),
            None => event
                .as_object()?
                .iter()
                .filter(|(key, _)| !matches!(key.as_str(), "type" | "sequence_number"))
                .map(|(key, value)| (key.clone(), value.clone()))
                .collect(),
        };
        if error.get("code").and_then(Value::as_str) != Some("context_length_exceeded") {
            return None;
        }
        let mut output = (self.template.get("output"))
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        for item in self.output() {
            let position = item["id"]
                .as_str()
                .filter(|id| !id.is_empty())
                .and_then(|id| output.iter().position(|existing| existing["id"] == id));
            match position {
                Some(index) => output[index] = item,
                None => output.push(item),
            }
        }
        let mut response = self.template.clone();
        let object = response
            .get("object")
            .and_then(Value::as_str)
            .unwrap_or("response")
            .to_owned();
        response.insert("id".into(), json!(id));
        response.insert("object".into(), json!(object));
        response.insert("status".into(), json!("failed"));
        response.insert("error".into(), Value::Object(error));
        response.insert("output".into(), Value::Array(output));
        let mut failed = Map::new();
        failed.insert("type".into(), json!("response.failed"));
        if let Some(sequence) = event.get("sequence_number") {
            failed.insert("sequence_number".into(), sequence.clone());
        }
        failed.insert("response".into(), Value::Object(response));
        Some(Value::Object(failed))
    }
    /// Observes one visible event and returns its client-visible replacement, when any.
    pub fn visible(&mut self, event: &Value) -> Option<Value> {
        self.observe(event);
        self.failure_event(event)
    }
}
