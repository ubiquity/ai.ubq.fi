//! Native Codex aggregate context recovery: a streamed native turn's local aggregate overflow is
//! `response.created` then one `response.failed`; turn metadata selects encoding only and grants no
//! authority. The failure is nonsemantic, dispatches nothing and is never quota or capacity proof.
use crate::admission::is_aggregate_context_overflow;
use gateway_core::{error::GatewayError, source_json::SourceJson, source_serialization as json};
use serde_json::json;

pub const FAILURE_KIND: &str = "aggregate_context_overflow";
pub const TERMINAL_TYPE: &str = "response.failed";
pub const CONTENT_TYPE: &str = "text/event-stream";
/// Source `isCodexNativeRequest`: a parsed metadata object with three nonempty JS-trimmed ids.
pub fn is_codex_native_turn(header: Option<&str>) -> bool {
    let Some(metadata) = header
        .filter(|value| !value.is_empty())
        .and_then(|value| SourceJson::parse(value).ok())
        .filter(|value| json::entries(value).is_some())
    else {
        return false;
    };
    ["session_id", "thread_id", "turn_id"].iter().all(|key| {
        json::member(&metadata, key)
            .and_then(|value| json::string(&value))
            .is_some_and(|value| {
                char::decode_utf16(value.as_units().iter().copied())
                    .any(|unit| !unit.is_ok_and(gateway_schema::util::js_whitespace))
            })
    })
}
/// True only for the native streamed request whose local failure is the aggregate overflow.
pub fn selects_native_encoding(
    stream: Option<&SourceJson>,
    turn_metadata: Option<&str>,
    error: &GatewayError,
) -> bool {
    stream.is_some_and(|value| value.as_str() == "true")
        && is_aggregate_context_overflow(error)
        && is_codex_native_turn(turn_metadata)
}
/// The two SSE frames for one local aggregate failure under a gateway-minted response id.
pub fn encode_native_context_failure(id: &str, created_at: i64, error: &GatewayError) -> String {
    let fields = &error.envelope.error;
    failure_frames(
        id,
        created_at,
        json!({
            "code": fields.code,
            "message": fields.message,
            "param": fields.param.clone().flatten(),
        }),
    )
}
/// Source `response.created` then one `response.failed` carrying `error`, with no usage or output.
pub fn failure_frames(id: &str, created_at: i64, error: serde_json::Value) -> String {
    let response = json!({"id":id,"object":"response","created_at":created_at,"status":"in_progress","error":null,"output":[],"usage":null});
    let mut failed = response.clone();
    failed["status"] = json!("failed");
    failed["error"] = error;
    [
        json!({"type":"response.created","sequence_number":0,"response":response}),
        json!({"type":TERMINAL_TYPE,"sequence_number":1,"response":failed}),
    ]
    .iter()
    .map(|event| {
        format!(
            "event: {}\ndata: {event}\n\n",
            event["type"].as_str().unwrap_or_default()
        )
    })
    .collect()
}
/// Selects and encodes in one step; `None` keeps the existing HTTP 400 answer.
pub fn native_context_failure(
    stream: Option<&SourceJson>,
    turn_metadata: Option<&str>,
    error: &GatewayError,
    id: &str,
    created_at: i64,
) -> Option<String> {
    selects_native_encoding(stream, turn_metadata, error)
        .then(|| encode_native_context_failure(id, created_at, error))
}
