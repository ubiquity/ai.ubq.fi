//! The source v2 field bound and parseable tool-argument elision; no message removal.
use gateway_source::js_string::JsString16;
use serde::Serialize;

pub const VERSION: &str = "deepseek-forwarded-payload/v2";
pub const PER_MESSAGE_LIMIT: usize = 65_536;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PayloadKind {
    ToolOutput,
    ToolArguments,
    Image,
    AgentMessage,
    Message,
    Reasoning,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Elision {
    pub path: String,
    pub call_id: Option<JsString16>,
    pub kind: PayloadKind,
    pub original_bytes: usize,
    pub forwarded_bytes: usize,
    pub omitted_bytes: usize,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Forwarded {
    pub content: JsString16,
    pub elision: Option<Elision>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PayloadRefusal {
    pub path: String,
    pub message: JsString16,
}

/// TextEncoder replaces unpaired units for byte counting without modifying source text.
pub fn byte_len(value: &JsString16) -> usize {
    char::decode_utf16(value.as_units().iter().copied())
        .map(|unit| unit.unwrap_or(char::REPLACEMENT_CHARACTER).len_utf8())
        .sum()
}

pub fn refusal(path: &str, bytes: usize, call_id: Option<&JsString16>) -> PayloadRefusal {
    let mut units = format!("input item {path}")
        .encode_utf16()
        .collect::<Vec<_>>();
    if let Some(call_id) = call_id {
        units.extend(" (tool call ".encode_utf16());
        units.extend_from_slice(call_id.as_units());
        units.push(u16::from(b')'));
    }
    units.extend(format!(" carries {bytes} bytes; this route forwards at most {PER_MESSAGE_LIMIT} bytes per message under {VERSION}, and the request set truncation 'disabled', so the gateway will not reduce it. Reduce the payload or send truncation 'auto' to allow the declared reduction.").encode_utf16());
    PayloadRefusal {
        path: path.into(),
        message: JsString16::from_units(units),
    }
}

fn head(units: &[u16], length: usize) -> &[u16] {
    let mut end = length.min(units.len());
    if end < units.len() && end > 0 && (0xd800..=0xdbff).contains(&units[end - 1]) {
        end -= 1
    }
    &units[..end]
}

fn unit_bytes(units: &[u16]) -> usize {
    byte_len(&JsString16::from_units(units.to_vec()))
}

fn marker(omitted: usize) -> String {
    format!(
        "\n[gateway: {VERSION} elided {omitted} bytes omitted; the model did not receive the elided bytes; this route forwards at most {PER_MESSAGE_LIMIT} bytes per message because the provider counts forwarded payloads as text tokens]"
    )
}

pub fn bound(
    value: &JsString16,
    path: &str,
    call_id: Option<&JsString16>,
    kind: PayloadKind,
    reject: bool,
    limit: usize,
) -> Result<Forwarded, PayloadRefusal> {
    let original_bytes = byte_len(value);
    if original_bytes <= limit {
        return Ok(Forwarded {
            content: value.clone(),
            elision: None,
        });
    }
    if reject {
        return Err(refusal(path, original_bytes, call_id));
    }
    let mut prefix = head(value.as_units(), limit);
    while !prefix.is_empty() && unit_bytes(prefix) > limit {
        prefix = head(prefix, (prefix.len() as f64 * 0.9).floor() as usize)
    }
    loop {
        let prefix_bytes = unit_bytes(prefix);
        let omitted_bytes = original_bytes - prefix_bytes;
        let notice = marker(omitted_bytes);
        if prefix.is_empty() || prefix_bytes + notice.len() <= limit {
            let mut units = prefix.to_vec();
            units.extend(notice.encode_utf16());
            let content = JsString16::from_units(units);
            return Ok(Forwarded {
                elision: Some(Elision {
                    path: path.into(),
                    call_id: call_id.cloned(),
                    kind,
                    original_bytes,
                    forwarded_bytes: byte_len(&content),
                    omitted_bytes,
                }),
                content,
            });
        }
        prefix = head(prefix, (prefix.len() as f64 * 0.9).floor() as usize);
    }
}

#[derive(Serialize)]
struct ArgumentsEnvelope<'a> {
    gateway_elision: ArgumentsElision<'a>,
}
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ArgumentsElision<'a> {
    policy: &'a str,
    kind: &'a str,
    original_bytes: usize,
    omitted_bytes: usize,
    message: String,
}

/// Returns None exactly when the complete parseable replacement does not fit the field.
pub fn reduce_tool_arguments(
    value: &JsString16,
    path: &str,
    call_id: &JsString16,
    limit: usize,
) -> Option<Forwarded> {
    let original_bytes = byte_len(value);
    let message = format!(
        "[gateway: {VERSION} elided {original_bytes} bytes omitted; the model did not receive these tool-call arguments; this route forwards at most {limit} bytes per message]"
    );
    let envelope = ArgumentsEnvelope {
        gateway_elision: ArgumentsElision {
            policy: VERSION,
            kind: "tool_arguments",
            original_bytes,
            omitted_bytes: original_bytes,
            message,
        },
    };
    let encoded = serde_json::to_string(&envelope).ok()?;
    if encoded.len() > limit {
        return None;
    }
    let content = JsString16::from_utf8(&encoded);
    Some(Forwarded {
        elision: Some(Elision {
            path: path.into(),
            call_id: Some(call_id.clone()),
            kind: PayloadKind::ToolArguments,
            original_bytes,
            forwarded_bytes: byte_len(&content),
            omitted_bytes: original_bytes,
        }),
        content,
    })
}

pub fn bound_tool_arguments(
    value: &JsString16,
    path: &str,
    call_id: &JsString16,
    reject: bool,
    limit: usize,
) -> Result<Forwarded, PayloadRefusal> {
    let original_bytes = byte_len(value);
    if original_bytes <= limit {
        return Ok(Forwarded {
            content: value.clone(),
            elision: None,
        });
    }
    if reject {
        return Err(refusal(path, original_bytes, Some(call_id)));
    }
    reduce_tool_arguments(value, path, call_id, limit)
        .ok_or_else(|| refusal(path, original_bytes, Some(call_id)))
}

/// Images are omitted as a whole; a partial data URL is never an image result.
pub fn omitted_image(value: &JsString16, path: &str) -> Forwarded {
    let original_bytes = byte_len(value);
    let content = JsString16::from_utf8(&format!(
        "[gateway: {VERSION} image omitted ({original_bytes} bytes); the model did not receive this image; this route forwards at most {PER_MESSAGE_LIMIT} bytes per message because the provider counts forwarded payloads as text tokens]"
    ));
    Forwarded {
        elision: Some(Elision {
            path: path.into(),
            call_id: None,
            kind: PayloadKind::Image,
            original_bytes,
            forwarded_bytes: byte_len(&content),
            omitted_bytes: original_bytes,
        }),
        content,
    }
}
