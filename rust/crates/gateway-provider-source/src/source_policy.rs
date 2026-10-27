use crate::source as s;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;

fn nonempty(value: &SourceJson) -> bool {
    s::string(value).is_some_and(|text| !text.as_units().is_empty())
}


pub fn response_has_answer(value: &SourceJson) -> bool {
    s::present(value, "output").and_then(|observed| json::array(&observed)).is_some_and(|items| items.iter().any(item_has_answer))
}


pub fn item_has_answer(item: &SourceJson) -> bool {
    match s::present(item, "type").and_then(|observed| s::control(&observed)).as_deref().unwrap_or("") {
        "reasoning" => false,
        "function_call" | "custom_tool_call" => {
            s::present(item, "call_id").is_some_and(|observed| nonempty(&observed))
                && s::present(item, "name").is_some_and(|observed| nonempty(&observed))
                && (s::present(item, "arguments").and_then(|observed| s::string(&observed)).is_some()
                    || s::present(item, "input").and_then(|observed| s::string(&observed)).is_some())
        }
        "code_interpreter_call"
        | "computer_call"
        | "file_search_call"
        | "image_generation_call"
        | "mcp_call"
        | "web_search_call" => matches!(
            s::present(item, "status").and_then(|observed| s::control(&observed)).as_deref(),
            Some("in_progress" | "completed" | "failed")
        ),
        _ => s::present(item, "content").and_then(|observed| json::array(&observed)).is_some_and(|parts| {
            parts
                .iter()
                .any(|part| s::present(part, "text").is_some_and(|observed| nonempty(&observed)) || s::present(part, "refusal").is_some_and(|observed| nonempty(&observed)))
        }),
    }
}


pub fn semantic_event(event: &SourceJson) -> bool {
    let kind = s::present(event, "type").and_then(|observed| s::control(&observed)).unwrap_or_default();
    if [
        "response.output_text.delta",
        "response.output_text.done",
        "response.refusal.delta",
        "response.refusal.done",
    ]
    .contains(&kind.as_str())
    {
        return s::present(event, "delta").is_some_and(|observed| nonempty(&observed))
            || s::present(event, "text").is_some_and(|observed| nonempty(&observed))
            || s::present(event, "refusal").is_some_and(|observed| nonempty(&observed));
    }
    if kind == "response.image_generation_call.partial_image" {
        return s::present(event, "partial_image_b64").is_some_and(|observed| nonempty(&observed));
    }
    if ["response.output_item.added", "response.output_item.done"].contains(&kind.as_str()) {
        return s::present(event, "item").is_some_and(|observed| item_has_answer(&observed));
    }
    if kind == "response.completed" || kind == "response.output" {
        return s::present(event, "response").is_some_and(|observed| response_has_answer(&observed)) || response_has_answer(event);
    }
    [
        "code_interpreter_call",
        "computer_call",
        "file_search_call",
        "image_generation_call",
        "mcp_call",
        "web_search_call",
    ]
    .iter()
    .any(|tool| {
        ["completed", "in_progress", "failed"]
            .iter()
            .any(|status| kind == format!("response.{tool}.{status}"))
    })
}


pub fn reasoning_progress(event: &SourceJson) -> bool {
    s::present(event, "type").and_then(|observed| s::control(&observed)).is_some_and(|kind| kind.starts_with("response.reasoning"))
        && (s::present(event, "delta").is_some_and(|observed| nonempty(&observed))
            || s::present(event, "text").is_some_and(|observed| nonempty(&observed))
            || s::present_path(event, &["part", "text"]).is_some_and(|observed| nonempty(&observed)))
}
