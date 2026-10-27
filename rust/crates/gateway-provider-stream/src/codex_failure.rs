//! Retains only public wire data needed for Codex's exact generic-context terminal.
use gateway_core::{error::GatewayError, js_string::JsString16, source_json::SourceJson, source_serialization as json};
use gateway_provider_source::{source as s, extras::malformed};
use gateway_provider_events::event::ProviderEvent;

#[derive(Clone)]
struct Text { id: JsString16, value: JsString16, completed: bool, refusal: bool }
#[derive(Clone, Default)]
pub(crate) struct CodexFailure { id: Option<JsString16>, template: Option<SourceJson>, items: Vec<SourceJson>, text: Vec<Text> }
fn string(value: &SourceJson, name: &str) -> Option<JsString16> { s::present(value, name).and_then(|value| s::string(&value)) }
fn trimmed(value: &SourceJson, name: &str) -> Option<JsString16> { string(value, name).map(|value| s::trim(&value)).filter(|value| !value.as_units().is_empty()) }
fn item_id(event: &SourceJson) -> Option<JsString16> { trimmed(event, "item_id").or_else(|| s::present(event, "item").and_then(|item| trimmed(&item, "id"))) }
fn merge(left: Option<&SourceJson>, right: &SourceJson) -> Result<SourceJson, GatewayError> {
    let mut fields = match left { Some(left) => left.object_entries()?, None => Vec::new() };
    for (key, value) in right.object_entries()? {
        if let Some((_, old)) = fields.iter_mut().find(|(old, _)| old == &key) { *old = value; }
        else { fields.push((key, value)); }
    }
    s::object_keys(fields)
}
impl CodexFailure {
    fn retain(&mut self, item: SourceJson) {
        let index = trimmed(&item, "id").and_then(|id| self.items.iter().position(|old| trimmed(old, "id").as_ref() == Some(&id)));
        if let Some(index) = index { self.items[index] = item; } else { self.items.push(item); }
    }
    fn remember_text(&mut self, event: &SourceJson, kind: &str) {
        let field = match kind { "response.output_text.delta" | "response.refusal.delta" => "delta", "response.output_text.done" => "text", "response.refusal.done" => "refusal", _ => return };
        let Some(value) = string(event, field).filter(|value| !value.as_units().is_empty()) else { return; };
        let id = item_id(event).unwrap_or_else(|| s::concat(&[&"msg_recovered_".into(), &string(event, "output_index").unwrap_or_else(|| self.text.len().to_string().into())]));
        let old = self.text.iter_mut().find(|old| old.id == id);
        let completed = kind.ends_with(".done");
        let refusal = kind.starts_with("response.refusal.");
        if let Some(old) = old {
            if completed {
                if value.as_units().starts_with(old.value.as_units()) { old.value = value; old.completed = true; old.refusal = refusal; }
            } else { old.value = s::concat(&[&old.value, &value]); old.refusal = refusal; }
        } else { self.text.push(Text { id, value, completed, refusal }); }
    }
    fn hosted(&self, event: &SourceJson, kind: &str) -> Result<Option<SourceJson>, GatewayError> {
        let Some((name, status)) = kind.strip_prefix("response.").and_then(|value| value.rsplit_once('.')) else { return Ok(None); };
        if !["code_interpreter_call", "computer_call", "file_search_call", "image_generation_call", "mcp_call", "web_search_call"].contains(&name) || !["completed", "in_progress", "failed"].contains(&status) { return Ok(None); }
        let id = item_id(event).unwrap_or_else(|| format!("tool_recovered_{}", self.items.len()).into());
        let mut item = match self.items.iter().find(|old| trimmed(old, "id").as_ref() == Some(&id)) { Some(old) => old.clone(), None => s::object(vec![])? };
        s::set(&mut item, "id", s::quoted(&id)?)?; s::set(&mut item, "type", s::text(name)?)?; s::set(&mut item, "status", s::text(status)?)?;
        Ok(Some(item))
    }
    pub(crate) fn observe(&mut self, event: &ProviderEvent) -> Result<(), GatewayError> {
        let kind = event.type_name().unwrap_or_default();
        let candidate = s::present(&event.data, "response").and_then(|response| trimmed(&response, "id")).or_else(|| trimmed(&event.data, "response_id"));
        if let Some(candidate) = candidate {
            if self.id.as_ref().is_some_and(|id| id != &candidate) { return Err(malformed("Upstream Responses stream changed response identifiers.")); }
            self.id = Some(candidate);
        }
        if let Some(response) = s::present(&event.data, "response").filter(|value| json::entries(value).is_some()) { self.template = Some(merge(self.template.as_ref(), &response)?); }
        self.remember_text(&event.data, &kind);
        if kind == "response.output" {
            let output = s::present(&event.data, "output").filter(|value| !s::is_null(value)).or_else(|| s::present_path(&event.data, &["response", "output"]));
            for item in output.as_ref().and_then(json::array).into_iter().flatten().filter(|item| json::entries(item).is_some()) { self.retain(item); }
        }
        if ["response.output_item.added", "response.output_item.done"].contains(&kind.as_str()) {
            if let Some(mut item) = s::present(&event.data, "item").filter(|value| json::entries(value).is_some()) {
                if kind.ends_with(".added") { s::set(&mut item, "status", s::text("incomplete")?)?; }
                self.retain(item);
            }
        } else if let Some(item) = self.hosted(&event.data, &kind)? { self.retain(item); }
        Ok(())
    }
    pub(crate) fn normalize(&self, event: &ProviderEvent) -> Result<ProviderEvent, GatewayError> {
        let Some(id) = &self.id else { return Ok(event.clone()); };
        if event.type_name().as_deref() != Some("error") { return Ok(event.clone()); }
        let mut output = self.items.clone();
        for text in &self.text {
            let index = output.iter().position(|item| trimmed(item, "id").as_ref() == Some(&text.id));
            if index.is_some_and(|index| s::present(&output[index], "content").and_then(|value| json::array(&value)).is_some_and(|parts| parts.iter().any(|part| s::present(part, "type").is_some_and(|kind| s::is(&kind, "output_text") || s::is(&kind, "text")) && string(part, "text").is_some_and(|text| !text.as_units().is_empty())))) { continue; }
            let part = if text.refusal { s::object(vec![("type", s::text("refusal")?), ("refusal", s::quoted(&text.value)?)])? }
                else { s::object(vec![("type", s::text("output_text")?), ("text", s::quoted(&text.value)?), ("annotations", s::array(&[])?)])? };
            let recovered = s::object(vec![("id", s::quoted(&text.id)?), ("type", s::text("message")?), ("status", s::text(if text.completed { "completed" } else { "incomplete" })?), ("role", s::text("assistant")?), ("content", s::array(&[part])?)])?;
            if let Some(index) = index { output[index] = merge(Some(&output[index]), &recovered)?; } else { output.push(recovered); }
        }
        let template = match &self.template { Some(value) => value.clone(), None => s::object(vec![])? };
        event.codex_context_failure(id, &template, &output)
    }
}
