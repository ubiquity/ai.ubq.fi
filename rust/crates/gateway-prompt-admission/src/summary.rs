//! `codex-compaction-summary/v1`: one exact user message with one `input_text` part beginning with
//! the 399-byte Codex `SUMMARY_PREFIX` and a newline is exempt from the per-field cut. The prefix is
//! request text, not authority: the complete prompt still has to pass aggregate admission.
use crate::{
    admission::PromptAdmission,
    forwarding::{FORWARD_LIMIT, FORWARD_VERSION},
};
use gateway_core::{js_string::JsString16, source_json::SourceJson, source_serialization as json};
use gateway_schema::source_request::error::SourceRequestError;
use serde::Serialize;

pub const SUMMARY_VERSION: &str = "codex-compaction-summary/v1";
pub const SUMMARY_PREFIX: &str = "Another language model started to solve this problem and produced a summary of its thinking process. You also have access to the state of the tools that were used by that language model. Use this to build on the work that has already been done and avoid duplicating work. Here is the summary produced by the other language model, use the information in this summary to assist with your own analysis:";
pub const SUMMARY_PREFIX_BYTES: usize = 399;
pub const SUMMARY_PREFIX_SHA256: &str =
    "e9b088e794a6bb9082ac053fcc760bd818d7e720ee4bcdc72c6e480de7b7cb0e";
pub const SUMMARY_MAX_PER_REQUEST: usize = 1;
pub const FORWARDING_EXEMPTION_LOG: &str = "[ai.ubq.fi] forwarding_exemption";
/// One exact stock local-compaction summary forwarded above the generic bound.
#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Exemption {
    pub path: String,
    pub kind: &'static str,
    pub summary_policy: &'static str,
    pub prefix_sha256: &'static str,
    pub bytes: usize,
    pub content_parts: usize,
    pub per_message_limit: usize,
}
impl Exemption {
    pub fn compaction_summary(path: String, bytes: usize) -> Self {
        Self {
            path,
            kind: "compaction_summary",
            summary_policy: SUMMARY_VERSION,
            prefix_sha256: SUMMARY_PREFIX_SHA256,
            bytes,
            content_parts: 1,
            per_message_limit: FORWARD_LIMIT,
        }
    }
}
fn starts(text: &JsString16, prefix: &[u16]) -> bool {
    text.as_units().starts_with(prefix)
}
fn failure(path: &str, detail: &str) -> SourceRequestError {
    SourceRequestError::new(
        400,
        JsString16::from_utf8(&format!(
            "input item {detail}; {SUMMARY_VERSION} exempts at most {SUMMARY_MAX_PER_REQUEST} exact user input_text envelope beginning with the {SUMMARY_PREFIX_BYTES}-byte Codex summary prefix and a newline. The summary was not reduced or forwarded."
        )),
        "invalid_request_error",
        Some("context_length_exceeded"),
        Some(Some(JsString16::from_utf8(path))),
    )
}
fn is(value: Option<SourceJson>, expected: &str) -> bool {
    value
        .and_then(|value| json::string(&value))
        .is_some_and(|value| value.as_units().iter().copied().eq(expected.encode_utf16()))
}
fn exact_envelope(item: &SourceJson, parts: Option<&[SourceJson]>, prefix: &[u16]) -> bool {
    let kind = json::member(item, "type").and_then(|value| json::string(&value));
    let Some([part]) = parts else {
        return false;
    };
    let mut head = prefix.to_vec();
    head.push(u16::from(b'\n'));
    kind.is_none_or(|kind| kind.as_units().iter().copied().eq("message".encode_utf16()))
        && is(json::member(item, "role"), "user")
        && json::entries(part).is_some()
        && is(json::member(part, "type"), "input_text")
        && json::member(part, "text")
            .and_then(|value| json::string(&value))
            .is_some_and(|text| starts(&text, &head) && text.as_units().len() > head.len())
}
fn inspect_item(
    item: &SourceJson,
    index: usize,
    prefix: &[u16],
) -> Result<bool, SourceRequestError> {
    let path = format!("input[{index}]");
    if let Some(text) = json::string(item) {
        if starts(&text, prefix) {
            return Err(failure(&path, "is a string item carrying a summary prefix"));
        }
        return Ok(false);
    }
    if json::entries(item).is_none() {
        return Ok(false);
    }
    let content = json::member(item, "content");
    let parts = content.as_ref().and_then(json::array);
    let carries = match content.as_ref().and_then(json::string) {
        Some(text) => starts(&text, prefix),
        None => parts.iter().flatten().any(|part| {
            json::entries(part).is_some()
                && json::member(part, "text")
                    .and_then(|value| json::string(&value))
                    .is_some_and(|text| starts(&text, prefix))
        }),
    };
    if !carries {
        return Ok(false);
    }
    if !exact_envelope(item, parts.as_deref(), prefix) {
        return Err(failure(
            &path,
            "carries a summary prefix outside the exact one-part user input_text envelope",
        ));
    }
    Ok(true)
}
/// Finds the one readable local-compaction summary Codex can adopt. A request with two summary
/// envelopes, or a prefix-bearing message that is not exactly one user input_text part, fails
/// closed with HTTP 400 `context_length_exceeded` before projection.
pub fn inspect_compaction_summary_input(
    input: &SourceJson,
) -> Result<Option<usize>, SourceRequestError> {
    let prefix = SUMMARY_PREFIX.encode_utf16().collect::<Vec<_>>();
    if let Some(text) = json::string(input) {
        if starts(&text, &prefix) {
            return Err(failure("input", "is a string carrying a summary prefix"));
        }
        return Ok(None);
    }
    let mut found = None;
    for (index, item) in json::array(input).into_iter().flatten().enumerate() {
        if !inspect_item(&item, index, &prefix)? {
            continue;
        }
        if found.is_some() {
            return Err(failure(
                &format!("input[{index}]"),
                "adds a second summary envelope",
            ));
        }
        found = Some(index);
    }
    Ok(found)
}
/// The operator record for one exact summary admitted whole above the generic bound. It is
/// distinct from `forwarding_elision` because no bytes were removed.
pub fn exemption_log_line(
    exemption: &Exemption,
    request_id: Option<&str>,
    model: Option<&str>,
    admission: Option<&PromptAdmission>,
) -> String {
    serde_json::json!({
        "policy": FORWARD_VERSION,
        "request_id": request_id,
        "model": model,
        "prompt_admission": admission,
        "path": exemption.path,
        "kind": exemption.kind,
        "summaryPolicy": exemption.summary_policy,
        "prefixSha256": exemption.prefix_sha256,
        "bytes": exemption.bytes,
        "contentParts": exemption.content_parts,
        "perMessageLimit": exemption.per_message_limit,
        "prefixBytes": SUMMARY_PREFIX_BYTES,
    })
    .to_string()
}
