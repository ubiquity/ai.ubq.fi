use gateway_core::{error::GatewayError, js_string::JsString16, source_json::SourceJson, source_serialization};
use gateway_provider_source::source as s;
use serde::{Deserialize, Serialize};
use serde_json::Value;



#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct ProviderEvent {
    pub event: Option<String>,
    pub data: SourceJson,
    #[serde(skip)]
    original_observation: Option<SourceJson>,
}



impl ProviderEvent {
    pub fn responses(data: Value) -> Result<Self, gateway_core::error::GatewayError> {
    Ok({
        Self {
            event: data.get("type").and_then(Value::as_str).map(str::to_owned),
            data: s::scalar_json(&data)?,
            original_observation: None,
        }
    
    })
}
    pub fn chat(data: Value) -> Result<Self, gateway_core::error::GatewayError> {
    Ok({
        Self {
            event: None,
            data: s::scalar_json(&data)?,
            original_observation: None,
        }
    
    })
}
    pub fn responses_source(data: SourceJson) -> Self {
        let event = source_serialization::member(&data, "type")
            .and_then(|value| source_serialization::string(&value))
            .and_then(|value| value.try_utf8().ok());
        Self { event, data, original_observation: None }
    }
    pub fn chat_source(data: SourceJson) -> Self {
        Self { event: None, data, original_observation: None }
    }
    pub fn field(&self, name: &str) -> Option<SourceJson> {
        source_serialization::member(&self.data, name)
    }
    pub fn type_name(&self) -> Option<String> {
        self.field("type")
            .and_then(|value| source_serialization::string(&value))
            .and_then(|value| value.try_utf8().ok())
    }
    pub fn is_done(&self) -> bool {
        s::is(&self.data, "[DONE]")
    }
    pub fn semantic_event(&self) -> bool {
        gateway_provider_source::source_policy::semantic_event(self.observation())
    }
    pub fn reasoning_progress(&self) -> bool {
        gateway_provider_source::source_policy::reasoning_progress(self.observation())
    }
    /// Only the isolated numeric/accounting fragment crosses the scalar decoder boundary.
    pub fn observed_usage(&self) -> Option<Value> {
        source_serialization::member(self.observation(), "usage")
            .as_ref()
            .and_then(gateway_provider_source::source_collector::usage)
            .or_else(|| {
                source_serialization::member(self.observation(), "response").and_then(|response| {
                    s::present(&response, "usage").as_ref().and_then(gateway_provider_source::source_collector::usage)
                })
            })
    }
    pub fn terminal(&self) -> Option<gateway_core::lifecycle::SemanticTerminal> {
        use gateway_core::lifecycle::SemanticTerminal;
        let observation = Self::responses_source(self.observation().clone());
        match observation.type_name().as_deref() {
            Some("response.completed") => Some(SemanticTerminal::Completed),
            Some("response.incomplete") => Some(SemanticTerminal::Incomplete),
            Some("response.failed") => Some(SemanticTerminal::Failed),
            Some("error") => Some(SemanticTerminal::Error),
            _ if observation.field("error").is_some() => Some(SemanticTerminal::Error),
            _ => observation
                .field("choices")
                .and_then(|value| source_serialization::array(&value))
                .into_iter()
                .flatten()
                .find_map(|choice| {
                    match s::present(&choice, "finish_reason").and_then(|observed| s::control(&observed))
                        .as_deref()
                    {
                        Some("stop" | "tool_calls" | "function_call") => {
                            Some(SemanticTerminal::Completed)
                        }
                        Some("length" | "content_filter") => Some(SemanticTerminal::Incomplete),
                        _ => None,
                    }
                }),
        }
    }
    pub fn encode_sse(&self) -> String {
        if self.is_done() {
            return "data: [DONE]\n\n".into();
        }
        let event = self
            .event
            .as_ref()
            .map(|s| format!("event: {s}\n"))
            .unwrap_or_default();
        format!("{event}data: {}\n\n", self.data.as_str())
    }
}
impl ProviderEvent {
    /// Original upstream data remains the lifecycle/accounting observation after wire normalization.
    pub fn observation(&self) -> &SourceJson { self.original_observation.as_ref().unwrap_or(&self.data) }
    pub fn codex_context_failure(&self, response_id: &JsString16, template: &SourceJson, visible: &[SourceJson]) -> Result<Self, GatewayError> {
        if self.type_name().as_deref() != Some("error") || response_id.as_units().is_empty() { return Ok(self.clone()); }
        let error = match s::present(&self.data, "error").filter(|value| source_serialization::entries(value).is_some()) {
            Some(error) => error,
            None => {
                let mut fields = self.data.object_entries()?;
                fields.retain(|(key, _)| !["type", "sequence_number"].iter().any(|name| key.as_units().iter().copied().eq(name.encode_utf16())));
                s::object_keys(fields)?
            }
        };
        if !s::present(&error, "code").is_some_and(|value| s::is(&value, "context_length_exceeded")) { return Ok(self.clone()); }
        let mut output = s::present(template, "output").and_then(|value| source_serialization::array(&value)).unwrap_or_default();
        for item in visible {
            let id = s::present(item, "id").and_then(|value| s::string(&value));
            let index = id.as_ref().filter(|id| !id.as_units().is_empty()).and_then(|id| output.iter().position(|old| s::present(old, "id").and_then(|value| s::string(&value)).as_ref() == Some(id)));
            if let Some(index) = index { output[index] = item.clone(); } else { output.push(item.clone()); }
        }
        let mut response = template.clone();
        s::set(&mut response, "id", s::quoted(response_id)?)?;
        let object = match s::present(template, "object").filter(|value| s::string(value).is_some()) { Some(object) => object, None => s::text("response")? };
        s::set(&mut response, "object", object)?;
        s::set(&mut response, "status", s::text("failed")?)?;
        s::set(&mut response, "error", error)?;
        s::set(&mut response, "output", s::array(&output)?)?;
        let mut fields = vec![("type", s::text("response.failed")?)];
        if let Some(sequence) = s::present(&self.data, "sequence_number") { fields.push(("sequence_number", sequence)); }
        fields.push(("response", response));
        let mut normalized = Self::responses_source(s::object(fields)?);
        normalized.original_observation = Some(self.observation().clone());
        Ok(normalized)
    }
}
