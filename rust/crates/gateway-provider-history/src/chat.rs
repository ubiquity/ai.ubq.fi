use gateway_core::js_string::JsString16;
use gateway_provider_protocol::types::Elision;
use gateway_provider_source::source as s;
use gateway_prompt_policy::payload::{self, PayloadKind};




pub const CONTINUATION: &str = "When tools are available, a progress update does not complete a requested action. If required work remains and you can perform it, continue with the next appropriate tool call instead of ending with a status message. Provide a final answer when the requested work is complete, or when you need user input or are blocked. Never claim a tool action has been done unless its result is in the conversation.";

pub fn oversized(path: &str, bytes: usize, call_id: Option<&JsString16>) -> gateway_local_refusal::source_error::SourceRequestError {
    refusal(payload::refusal(path, bytes, call_id))
}

fn refusal(error: payload::PayloadRefusal) -> gateway_local_refusal::source_error::SourceRequestError {
    gateway_local_refusal::source_error::SourceRequestError::new(400, error.message, "invalid_request_error", Some("context_length_exceeded"), Some(Some(JsString16::from_utf8(&error.path))))
}

fn retain(value: payload::Forwarded, kind: &str, elisions: &mut Vec<Elision>) -> JsString16 {
    if let Some(record) = value.elision {
        elisions.push(Elision { path: record.path, call_id: record.call_id, kind: kind.into(), original_bytes: record.original_bytes, forwarded_bytes: record.forwarded_bytes, omitted_bytes: record.omitted_bytes });
    }
    value.content
}

pub fn bound_source(text: &JsString16, path: &str, call_id: Option<&JsString16>, kind: &str, reject: bool, elisions: &mut Vec<Elision>) -> s::Result<JsString16> {
    bound_source_limit(text, path, call_id, kind, reject, elisions, payload::PER_MESSAGE_LIMIT)
}

pub fn bound_source_limit(text: &JsString16, path: &str, call_id: Option<&JsString16>, kind: &str, reject: bool, elisions: &mut Vec<Elision>, limit: usize) -> s::Result<JsString16> {
    let category = match kind { "tool_output" => PayloadKind::ToolOutput, "agent_message" => PayloadKind::AgentMessage, "reasoning" => PayloadKind::Reasoning, _ => PayloadKind::Message };
    let value = payload::bound(text, path, call_id, category, reject, limit).map_err(refusal)?;
    Ok(retain(value, kind, elisions))
}

pub fn bound_arguments(text: &JsString16, path: &str, call_id: &JsString16, reject: bool, elisions: &mut Vec<Elision>) -> s::Result<JsString16> {
    let value = payload::bound_tool_arguments(text, path, call_id, reject, payload::PER_MESSAGE_LIMIT).map_err(refusal)?;
    Ok(retain(value, "tool_arguments", elisions))
}

#[cfg(test)]
mod unicode_tests;
