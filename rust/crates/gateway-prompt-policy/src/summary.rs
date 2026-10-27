//! Exact adopted-summary classification selects a field exemption, never authority.
use crate::payload::PER_MESSAGE_LIMIT;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as json,
};
use serde::Serialize;

pub const VERSION: &str = "codex-compaction-summary/v1";
pub const PREFIX: &str = "Another language model started to solve this problem and produced a summary of its thinking process. You also have access to the state of the tools that were used by that language model. Use this to build on the work that has already been done and avoid duplicating work. Here is the summary produced by the other language model, use the information in this summary to assist with your own analysis:";
pub const PREFIX_BYTES: usize = 399;
pub const PREFIX_SHA256: &str = "e9b088e794a6bb9082ac053fcc760bd818d7e720ee4bcdc72c6e480de7b7cb0e";

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SummaryRefusal {
    pub path: String,
    pub message: String,
}

fn refusal(path: &str, detail: &str) -> SummaryRefusal {
    SummaryRefusal {
        path: path.into(),
        message: format!(
            "{detail}; {VERSION} exempts at most 1 exact user input_text envelope beginning with the {PREFIX_BYTES}-byte Codex summary prefix and a newline. The summary was not reduced or forwarded."
        ),
    }
}

fn begins(value: &JsString16) -> bool {
    value
        .as_units()
        .starts_with(&PREFIX.encode_utf16().collect::<Vec<_>>())
}

fn text(value: &SourceJson, name: &str) -> Option<JsString16> {
    json::member(value, name).and_then(|value| json::string(&value))
}

fn equals(value: Option<JsString16>, expected: &str) -> bool {
    value.is_some_and(|value| value.as_units().iter().copied().eq(expected.encode_utf16()))
}

fn exact(item: &SourceJson, parts: &[SourceJson]) -> bool {
    let Some(part) = parts.first().filter(|_| parts.len() == 1) else {
        return false;
    };
    let Some(content) = text(part, "text") else {
        return false;
    };
    let prefix = format!("{PREFIX}\n").encode_utf16().collect::<Vec<_>>();
    let message_type = text(item, "type").unwrap_or_else(|| JsString16::from_utf8("message"));
    equals(Some(message_type), "message")
        && equals(text(item, "role"), "user")
        && equals(text(part, "type"), "input_text")
        && json::entries(part).is_some()
        && content.as_units().starts_with(&prefix)
        && content.as_units().len() > prefix.len()
}

fn inspect_item(item: &SourceJson, index: usize) -> Result<bool, SummaryRefusal> {
    let path = format!("input[{index}]");
    if let Some(value) = json::string(item) {
        return if begins(&value) {
            Err(refusal(&path, "is a string item carrying a summary prefix"))
        } else {
            Ok(false)
        };
    }
    let Some(content) = json::member(item, "content") else {
        return Ok(false);
    };
    let parts = json::array(&content).unwrap_or_default();
    let texts = if let Some(value) = json::string(&content) {
        vec![value]
    } else {
        parts.iter().filter_map(|part| text(part, "text")).collect()
    };
    if !texts.iter().any(begins) {
        return Ok(false);
    }
    if json::array(&content).is_none() || !exact(item, &parts) {
        return Err(refusal(
            &path,
            "carries a summary prefix outside the exact one-part user input_text envelope",
        ));
    }
    Ok(true)
}

pub fn inspect_input(input: &SourceJson) -> Result<Option<usize>, SummaryRefusal> {
    if let Some(value) = json::string(input) {
        return if begins(&value) {
            Err(refusal("input", "is a string carrying a summary prefix"))
        } else {
            Ok(None)
        };
    }
    let mut found = None;
    for (index, item) in json::array(input).unwrap_or_default().iter().enumerate() {
        if !inspect_item(item, index)? {
            continue;
        }
        if found.is_some() {
            return Err(refusal(
                &format!("input[{index}]"),
                "adds a second summary envelope",
            ));
        }
        found = Some(index);
    }
    Ok(found)
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SummaryExemption {
    pub path: String,
    pub kind: &'static str,
    pub summary_policy: &'static str,
    pub prefix_sha256: &'static str,
    pub bytes: usize,
    pub content_parts: u8,
    pub per_message_limit: usize,
}

pub fn exemption(path: &str, bytes: usize) -> SummaryExemption {
    SummaryExemption {
        path: path.into(),
        kind: "compaction_summary",
        summary_policy: VERSION,
        prefix_sha256: PREFIX_SHA256,
        bytes,
        content_parts: 1,
        per_message_limit: PER_MESSAGE_LIMIT,
    }
}
