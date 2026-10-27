use gateway_provider_source::source as s;
use gateway_provider_protocol::types::DecodePlan;
use gateway_provider_events::event::ProviderEvent;
use gateway_provider_source::extras::malformed;
use gateway_core::error::GatewayError;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;
use std::collections::BTreeMap;
use gateway_provider_protocol::source_response::completion;

pub fn collect(
    events: &[ProviderEvent],
    plan: &DecodePlan,
) -> Result<SourceJson, GatewayError> {
    let mut texts = vec![];
    let mut refusals = vec![];
    let mut calls: BTreeMap<usize, BufferedCall> = BTreeMap::new();
    let mut usage = None;
    let mut complete = false;
    for event in events {
        if event.is_done() {
            complete = true;
            continue;
        }
        if event.field("error").is_some() {
            return Err(malformed(
                "Upstream stream ended without response.completed.",
            ));
        }
        if let Some(value) = event.field("usage") {
            usage = Some(value);
        }
        for choice in s::present(&event.data, "choices").and_then(|observed| json::array(&observed)).unwrap_or_default() {
            let delta = s::present(&choice, "delta");
            if let Some(text) = delta.as_ref().and_then(|delta| s::present(delta, "content")).and_then(|observed| s::string(&observed)) {
                texts.push(text);
            }
            if let Some(text) = delta.as_ref().and_then(|delta| s::present(delta, "refusal")).and_then(|observed| s::string(&observed)) {
                refusals.push(text);
            }
            for call in delta.as_ref().and_then(|delta| s::present(delta, "tool_calls")).and_then(|observed| json::array(&observed)).unwrap_or_default() {
                let index = s::present(&call, "index").and_then(|observed| json::number(&observed)).unwrap_or(0.0) as usize;
                let row = match calls.entry(index) {
                    std::collections::btree_map::Entry::Vacant(slot) => slot.insert(BufferedCall::new()?),
                    std::collections::btree_map::Entry::Occupied(slot) => slot.into_mut(),
                };
                if let Some(id) = s::present(&call, "id") {
                    s::set(&mut row.wire, "id", id)?;
                }
                let mut function = row.function.clone();
                let source = s::present(&call, "function");
                if let Some(name) = source.as_ref().and_then(|source| s::present(source, "name")) {
                    s::set(&mut function, "name", name)?;
                }
                if let Some(args) = source.as_ref().and_then(|source| s::present(source, "arguments")).and_then(|observed| s::string(&observed)) {
                    let previous = s::present(&function, "arguments").and_then(|observed| s::string(&observed)).unwrap_or_default();
                    s::set(
                        &mut function,
                        "arguments",
                        s::quoted(&s::concat(&[&previous, &args]))?,
                    )?;
                }
                s::set(&mut row.wire, "function", function.clone())?;
                row.function = function;
            }
        }
    }
    if !complete {
        return Err(malformed("Upstream Chat stream ended without sentinel."));
    }
    let mut result = completion(
        plan,
        s::join(&texts, ""),
        s::join(&refusals, ""),
        calls.into_values().map(|row| row.wire).collect(),
    )?;
    if let Some(usage) = usage {
        s::set(&mut result, "usage", usage)?;
    }
    Ok(result)
}

struct BufferedCall { wire: SourceJson, function: SourceJson }
impl BufferedCall {
    fn new() -> Result<Self, GatewayError> {
        let function = s::scalar_json(&serde_json::json!({"name":"","arguments":""}))?;
        let wire = s::object(vec![("id", s::text("")?), ("type", s::text("function")?), ("function", function.clone())])?;
        Ok(Self { wire, function })
    }
}
