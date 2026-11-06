//! Source `nativeFailureAfterEarlyHeaders` (e9b1b90): once admitted native SSE headers are
//! committed, a selected late non-2xx, non-SSE answer becomes `response.created` then one
//! `response.failed` with only its original error code and message. The original status, safe
//! headers and bounded original error stay internal evidence; no usage or wire status is invented.
use crate::native_failure::failure_frames;
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as json,
};
use gateway_schema::util::js_whitespace;
use serde::Serialize;
use serde_json::json;
use std::collections::BTreeMap;

/// Source `SENTINEL_UPSTREAM_MAX_HEADER_VALUE_CHARS`, counted in UTF-16 units.
pub const MAX_HEADER_VALUE_UNITS: usize = 512;
const EVENT_STREAM: &str = "text/event-stream";

/// Source `NativeHttpFailure`: terminal-log evidence only, never part of the client wire.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct NativeHttpFailure {
    pub status: u16,
    pub headers: BTreeMap<String, String>,
    pub headers_truncated: bool,
    /// The original error when the bounded read completed; otherwise `null`.
    pub error: SourceJson,
    pub body_complete: bool,
}
/// The two client frames, the source failure kind and the internal evidence.
#[derive(Debug)]
pub struct LateFailure {
    pub body: String,
    pub code: String,
    pub evidence: NativeHttpFailure,
}
/// One selected answer as the transport read it; `complete` is false for a bounded or failed read.
pub struct SelectedAnswer<'a> {
    pub status: u16,
    pub headers: &'a BTreeMap<String, String>,
    pub body: &'a [u8],
    pub complete: bool,
}

/// Source `/^text\/event-stream(?:\s*;|$)/i` over the answer's content type.
pub fn is_event_stream(content_type: Option<&str>) -> bool {
    let Some(rest) = content_type.and_then(|value| {
        value
            .get(..EVENT_STREAM.len())
            .filter(|head| head.eq_ignore_ascii_case(EVENT_STREAM))
            .map(|_| &value[EVENT_STREAM.len()..])
    }) else {
        return false;
    };
    rest.is_empty() || rest.trim_start_matches(js_whitespace).starts_with(';')
}
/// Only a non-2xx answer that is not already SSE changes encoding after early headers.
pub fn reencodes(status: u16, content_type: Option<&str>) -> bool {
    !(200..300).contains(&status) && !is_event_stream(content_type)
}
/// Source `safeFailureHeaders`: allowlisted names only, each value cut to 512 UTF-16 units.
pub fn safe_headers(
    headers: &BTreeMap<String, String>,
    names: &[&str],
) -> (BTreeMap<String, String>, bool) {
    let mut kept = BTreeMap::new();
    let mut truncated = false;
    for name in names {
        let Some((_, value)) = headers
            .iter()
            .find(|(key, _)| key.eq_ignore_ascii_case(name))
        else {
            continue;
        };
        let mut units = 0;
        let prefix = value
            .chars()
            .take_while(|ch| {
                units += ch.len_utf16();
                units <= MAX_HEADER_VALUE_UNITS
            })
            .collect::<String>();
        truncated |= prefix.len() < value.len();
        kept.insert((*name).to_owned(), prefix);
    }
    (kept, truncated)
}
/// Source body handling: a complete JSON `error` record is kept; any other complete text becomes
/// `{message}`; an incomplete read claims no original error and uses the HTTP fallback message.
pub fn late_failure(
    answer: &SelectedAnswer<'_>,
    safe_names: &[&str],
    id: &str,
    created_at: i64,
) -> Result<LateFailure, GatewayError> {
    let fallback = format!("Upstream request failed with HTTP {}.", answer.status);
    let text = if answer.complete {
        let decoded = String::from_utf8_lossy(answer.body);
        decoded
            .strip_prefix('\u{feff}')
            .unwrap_or(&decoded)
            .to_owned()
    } else {
        String::new()
    };
    let original = match SourceJson::parse(&text)
        .ok()
        .filter(is_record)
        .and_then(|payload| json::member(&payload, "error"))
        .filter(is_record)
    {
        Some(error) => error,
        None => {
            let message = if text.is_empty() { &fallback } else { &text };
            json::object(vec![(
                "message",
                json::quoted(&JsString16::from_utf8(message))?,
            )])?
        }
    };
    let field = |name: &str| {
        json::member(&original, name)
            .and_then(|value| json::string(&value))
            .map(|value| String::from_utf16_lossy(value.as_units()))
    };
    let code = field("code").unwrap_or_else(|| "server_error".into());
    let message = field("message").unwrap_or(fallback);
    let (headers, headers_truncated) = safe_headers(answer.headers, safe_names);
    Ok(LateFailure {
        body: failure_frames(id, created_at, json!({"code": code, "message": message})),
        evidence: NativeHttpFailure {
            status: answer.status,
            headers,
            headers_truncated,
            error: if answer.complete {
                json::canonical(&original)?
            } else {
                SourceJson::null()
            },
            body_complete: answer.complete,
        },
        code,
    })
}
/// Source `isRecord`: any non-null object, arrays included.
fn is_record(value: &SourceJson) -> bool {
    value.object_entries().is_ok() || value.array_items().is_ok()
}
