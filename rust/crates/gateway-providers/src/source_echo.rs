//! Source-bearing envelope composition; scalar upstream fragments are lifted, not request text.
use crate::{source as s, types::DecodePlan};
use gateway_core::{source_json::SourceJson, source_serialization as json};

pub(crate) fn envelope(plan: &DecodePlan, status: &str) -> SourceJson {
    let instructions = s::get(&plan.request, "instructions");
    let choice = s::present(&plan.request, "tool_choice").unwrap_or_else(|| s::text("auto"));
    let tools = s::present(&plan.request, "tools")
        .filter(|value| json::array(value).is_some())
        .unwrap_or_else(|| s::array(&[]));
    let parallel = s::boolean(&s::get(&plan.request, "parallel_tool_calls")).unwrap_or(true);
    s::object(vec![
        ("id", s::text(&plan.response_id)),
        ("object", s::text("response")),
        ("created_at", s::generated(&plan.created_at)),
        ("status", s::text(status)),
        ("model", s::quoted(plan.model.as_js_string())),
        ("output", s::array(&[])),
        ("error", s::null()),
        ("incomplete_details", s::null()),
        ("instructions", instructions),
        ("metadata", s::object(vec![])),
        ("parallel_tool_calls", s::generated(&parallel)),
        ("tool_choice", choice),
        ("tools", tools),
        ("temperature", s::null()),
        ("top_p", s::null()),
        ("max_output_tokens", s::null()),
        ("previous_response_id", s::null()),
        ("reasoning", s::null()),
        ("store", s::generated(&false)),
        ("truncation", s::text("disabled")),
        ("usage", s::null()),
    ])
}

pub(crate) fn response_event(kind: &str, response: SourceJson) -> crate::types::ProviderEvent {
    crate::types::ProviderEvent::responses_source(s::object(vec![
        ("type", s::text(kind)),
        ("response", response),
    ]))
}

pub(crate) fn item_event(
    kind: &str,
    index: usize,
    item: SourceJson,
) -> crate::types::ProviderEvent {
    crate::types::ProviderEvent::responses_source(s::object(vec![
        ("type", s::text(kind)),
        ("output_index", s::generated(&index)),
        ("item", item),
    ]))
}
