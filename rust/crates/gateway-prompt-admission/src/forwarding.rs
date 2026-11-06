//! `deepseek-forwarded-payload/v2`: the declared per-field byte bound and its reductions.
use gateway_core::{js_string::JsString16, source_json::SourceJson, source_serialization as json};
use gateway_schema::source_request::error::SourceRequestError;
use serde::{Deserialize, Serialize};

pub const FORWARD_LIMIT: usize = 65_536;
pub const FORWARD_VERSION: &str = "deepseek-forwarded-payload/v2";
pub const FORWARD_MODE: &str = "bytes";
/// One payload the declared policy reduced, as recorded in the operator log.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Elision {
    pub path: String,
    pub call_id: Option<JsString16>,
    pub kind: String,
    pub original_bytes: usize,
    pub forwarded_bytes: usize,
    pub omitted_bytes: usize,
}
/// Source utf16Head: preserve the exact code-unit prefix, dropping a cut high surrogate.
fn utf16_head(value: &[u16], length: usize) -> &[u16] {
    let mut end = length.min(value.len());
    if end < value.len() && end > 0 && (0xd800..=0xdbff).contains(&value[end - 1]) {
        end -= 1;
    }
    &value[..end]
}
/// TextEncoder counts unpaired units as U+FFFD without replacing the stored source string.
pub fn forwarded_bytes(value: &[u16]) -> usize {
    char::decode_utf16(value.iter().copied())
        .map(|value| value.unwrap_or(char::REPLACEMENT_CHARACTER).len_utf8())
        .sum()
}
/// The fail-closed answer when the request forbade reduction (`truncation: "disabled"`).
pub fn oversized(
    path: &str,
    original_bytes: usize,
    call_id: Option<&JsString16>,
) -> SourceRequestError {
    let mut units = format!("input item {path}")
        .encode_utf16()
        .collect::<Vec<_>>();
    if let Some(id) = call_id {
        units.extend(" (tool call ".encode_utf16());
        units.extend_from_slice(id.as_units());
        units.push(u16::from(b')'));
    }
    units.extend(format!(
        " carries {original_bytes} bytes; this route forwards at most {FORWARD_LIMIT} bytes per message under {FORWARD_VERSION}, and the request set truncation 'disabled', so the gateway will not reduce it. Reduce the payload or send truncation 'auto' to allow the declared reduction."
    ).encode_utf16());
    let message = JsString16::from_units(units);
    SourceRequestError::new(
        400,
        message,
        "invalid_request_error",
        Some("context_length_exceeded"),
        Some(Some(JsString16::from_utf8(path))),
    )
}
/// Source `boundedForwardedText` at the declared per-field limit.
pub fn bound_source(
    text: &JsString16,
    path: &str,
    call_id: Option<&JsString16>,
    kind: &str,
    reject: bool,
    elisions: &mut Vec<Elision>,
) -> Result<JsString16, SourceRequestError> {
    bound_source_limit(text, path, call_id, kind, reject, elisions, FORWARD_LIMIT)
}
/// Source `reduceForwardedPayload`: a deterministic byte prefix plus a marker naming the counts.
pub fn bound_source_limit(
    text: &JsString16,
    path: &str,
    call_id: Option<&JsString16>,
    kind: &str,
    reject: bool,
    elisions: &mut Vec<Elision>,
    limit: usize,
) -> Result<JsString16, SourceRequestError> {
    let original_bytes = forwarded_bytes(text.as_units());
    if original_bytes <= limit {
        return Ok(text.clone());
    }
    if reject {
        return Err(oversized(path, original_bytes, call_id));
    }
    // Source utf8Head starts with a code-unit budget, not a UTF8 byte offset.
    let mut head = utf16_head(text.as_units(), limit);
    while !head.is_empty() && forwarded_bytes(head) > limit {
        head = utf16_head(head, (head.len() as f64 * 0.9).floor() as usize);
    }
    loop {
        let head_bytes = forwarded_bytes(head);
        let omitted = original_bytes - head_bytes;
        let marker = format!(
            "\n[gateway: {FORWARD_VERSION} elided {omitted} bytes omitted; the model did not receive the elided bytes; this route forwards at most {FORWARD_LIMIT} bytes per message because the provider counts forwarded payloads as text tokens]"
        );
        if head.is_empty() || head_bytes + marker.len() <= limit {
            let mut units = head.to_vec();
            units.extend(marker.encode_utf16());
            elisions.push(Elision {
                path: path.into(),
                call_id: call_id.cloned(),
                kind: kind.into(),
                original_bytes,
                forwarded_bytes: head_bytes + marker.len(),
                omitted_bytes: omitted,
            });
            return Ok(JsString16::from_units(units));
        }
        head = utf16_head(head, (head.len() as f64 * 0.9).floor() as usize);
    }
}
/// Source `reduceToolArgumentsPayload`: oversized arguments become a parseable JSON elision.
/// Replayed Chat history therefore never carries invalid JSON in `function.arguments`.
pub fn bound_tool_arguments(
    value: &JsString16,
    path: &str,
    call_id: &JsString16,
    reject: bool,
    elisions: &mut Vec<Elision>,
) -> Result<JsString16, SourceRequestError> {
    let original_bytes = forwarded_bytes(value.as_units());
    if original_bytes <= FORWARD_LIMIT {
        return Ok(value.clone());
    }
    if reject {
        return Err(oversized(path, original_bytes, Some(call_id)));
    }
    let message = format!(
        "[gateway: {FORWARD_VERSION} elided {original_bytes} bytes omitted; the model did not receive these tool-call arguments; this route forwards at most {FORWARD_LIMIT} bytes per message]"
    );
    let content = serde_json::json!({"gateway_elision": {
        "policy": FORWARD_VERSION,
        "kind": "tool_arguments",
        "originalBytes": original_bytes,
        "omittedBytes": original_bytes,
        "message": message,
    }})
    .to_string();
    if content.len() > FORWARD_LIMIT {
        return Err(oversized(path, original_bytes, Some(call_id)));
    }
    elisions.push(Elision {
        path: path.into(),
        call_id: Some(call_id.clone()),
        kind: "tool_arguments".into(),
        original_bytes,
        forwarded_bytes: content.len(),
        omitted_bytes: original_bytes,
    });
    Ok(JsString16::from_utf8(&content))
}
/// Source `reductionForTruncation`: absent or `auto` reduces; only `disabled` rejects.
pub fn rejects_reduction(truncation: &SourceJson) -> Result<bool, SourceRequestError> {
    let text = json::string(truncation);
    let is = |name: &str| {
        text.as_ref()
            .is_some_and(|t| t.as_units().iter().copied().eq(name.encode_utf16()))
    };
    if truncation.as_str() == "null" || is("disabled") || is("auto") {
        return Ok(is("disabled"));
    }
    let kind = match truncation.as_str() {
        "true" | "false" => "boolean",
        _ if json::number(truncation).is_some() => "number",
        _ => "object",
    };
    let mut units = "truncation '".encode_utf16().collect::<Vec<_>>();
    units.extend_from_slice(
        text.unwrap_or_else(|| JsString16::from_utf8(kind))
            .as_units(),
    );
    units.extend("' is not supported; expected 'auto' or 'disabled'".encode_utf16());
    let param = Some(JsString16::from_utf8("truncation"));
    Err(SourceRequestError::invalid(
        JsString16::from_units(units),
        param,
    ))
}
/// An oversized data URL is never cut: the model receives a marker instead of the image.
pub fn omitted_image(url: &JsString16, path: &str, elisions: &mut Vec<Elision>) -> JsString16 {
    let original_bytes = forwarded_bytes(url.as_units());
    let marker = format!(
        "[gateway: {FORWARD_VERSION} image omitted ({original_bytes} bytes); the model did not receive this image; this route forwards at most {FORWARD_LIMIT} bytes per message because the provider counts forwarded payloads as text tokens]"
    );
    elisions.push(Elision {
        path: path.into(),
        call_id: None,
        kind: "image".into(),
        original_bytes,
        forwarded_bytes: marker.len(),
        omitted_bytes: original_bytes,
    });
    JsString16::from_utf8(&marker)
}
