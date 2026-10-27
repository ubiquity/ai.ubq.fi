//! Pure source state helpers.
use crate::{
    text::Text,
    types::{CompactionFailure, FailureKind},
};
use gateway_core::source_json::SourceJson;
use serde::Serialize;
pub const CONTEXT: &str = "A coding assistant conversation is being compacted to free context. `history` is the whole conversation so far, oldest first; tool outputs are replaced by a short `result` note and long texts may be abridged. Each question asks whether one tool call, or the full output of that call, still needs to stay in the history verbatim. Whatever is not kept is deleted permanently, but the assistant can always re-run a tool or re-read a file.";
pub fn json<T: Serialize + ?Sized>(value: &T) -> Result<SourceJson, CompactionFailure> {
    SourceJson::from_serializable(value)
        .map_err(|_| CompactionFailure::new(FailureKind::JevFailed, "state serialization failed"))
}
pub fn truncate(text: &Text, limit: usize) -> Text {
    if text.len() <= limit {
        text.clone()
    } else {
        text!(text.slice(0, limit.saturating_sub(1)), "…")
    }
}
pub fn abridge(value: &Text, head: usize, tail: usize) -> Text {
    if value.len() <= head + tail + 40 {
        return value.clone();
    }
    text!(
        value.slice(0, head),
        "\n[… ",
        value.len() - head - tail,
        " chars omitted …]\n",
        value.slice(value.len().saturating_sub(tail), value.len())
    )
}
pub fn pinned(i: usize, total: usize, recent: usize) -> bool {
    i == 0 || i >= total.saturating_sub(recent)
}
