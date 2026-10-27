//! Verbatim transcript projection and bounded result provenance.
use gateway_compaction_history::transcript::{Entry, Transcript};
use gateway_compaction_source::{
    model::{Action, Decision, ToolCall},
    text::{self, Text},
    types::CompactionStats,
};
use std::collections::BTreeMap;
pub const SUMMARY_MARKER: &str = "# fast-jev-compaction memory summary";
pub fn truncated_result(value: &Text, error: bool, head: usize) -> Text {
    if value.len() <= head.saturating_add(120) {
        return value.clone();
    }
    let prefix = if head > 0 {
        text!(value.slice(0, head), "\n")
    } else {
        Text::default()
    };
    text!(
        prefix,
        "[fast-jev-compaction truncated ",
        value.len() - head,
        " chars of this tool result",
        if error { " (error)" } else { "" },
        "; re-run the tool if needed]"
    )
}
fn action(action: Action) -> &'static str {
    match action {
        Action::Keep => "keep",
        Action::DropResult => "drop_result",
        Action::DropCall => "drop_call",
    }
}
fn truncated_input(value: &Text, head: usize) -> Text {
    if value.len() <= head.saturating_add(60) {
        value.clone()
    } else {
        text!(
            value.slice(0, head),
            "\n[fast-jev-compaction truncated ",
            value.len() - head,
            " chars of this tool input; the call is kept as provenance]"
        )
    }
}
pub fn render(
    transcript: &Transcript,
    calls: &[ToolCall],
    decisions: &[Decision],
    head: usize,
    stats: Option<&CompactionStats>,
) -> Text {
    let ids = calls
        .iter()
        .map(|call| (call.id.clone(), call.tool_use_id.clone()))
        .collect::<BTreeMap<_, _>>();
    let mut by = BTreeMap::new();
    for d in decisions {
        by.insert(ids.get(&d.id).unwrap_or(&d.id).clone(), d);
    }
    let mut out=vec![SUMMARY_MARKER.into(),"Tool calls and results below were classified by TypeSafe Jev (keep / truncate).".into(),"A dropped call keeps its provenance line and a bounded head of its result; nothing is erased".into(),"outright. Text kept verbatim is copied from the original transcript; this memory is text only, so".into(),"structured tool-call replay and opaque items (encrypted reasoning or compaction payloads) are not restored.".into(),Text::default()];
    for entry in &transcript.entries {
        match entry {
            Entry::Text { role, text, .. } => {
                out.extend([text!("[", role.label(), "]"), text.clone(), Text::default()])
            }
            Entry::Call {
                call_id,
                tool,
                input_json,
                ..
            } => {
                let d = by.get(call_id);
                let act = d.map_or(Action::Keep, |d| d.action);
                out.extend([
                    text!(
                        "[tool call ",
                        if call_id.is_empty() {
                            Text::from("(no call id)")
                        } else {
                            call_id.clone()
                        },
                        "] ",
                        if tool.is_empty() {
                            Text::from("(unnamed tool)")
                        } else {
                            tool.clone()
                        },
                        " — ",
                        d.map_or("no decision (kept)", |d| action(d.action))
                    ),
                    text!(
                        "input: ",
                        if act == Action::Keep {
                            input_json.clone()
                        } else {
                            truncated_input(input_json, head)
                        }
                    ),
                    Text::default(),
                ]);
            }
            Entry::Result {
                call_id,
                text: value,
                is_error,
                ..
            } => {
                if let Some(d) = by.get(call_id) {
                    if d.action == Action::Keep {
                        out.extend([
                            text!("[tool result ", call_id, " — kept verbatim]"),
                            value.clone(),
                            Text::default(),
                        ]);
                    } else {
                        out.extend([
                            text!(
                                "[tool result ",
                                call_id,
                                if d.action == Action::DropCall {
                                    " — truncated (call dropped), original was "
                                } else {
                                    " — truncated, original was "
                                },
                                value.len(),
                                " chars]"
                            ),
                            truncated_result(value, *is_error, head),
                            Text::default(),
                        ]);
                    }
                } else {
                    out.extend([
                        text!(
                            "[tool result ",
                            if call_id.is_empty() {
                                Text::from("(no call id)")
                            } else {
                                call_id.clone()
                            },
                            " — no matching call in this transcript]"
                        ),
                        value.clone(),
                        Text::default(),
                    ]);
                }
            }
        }
    }
    let line = if let Some(s) = stats {
        text!(
            "calls=",
            s.results_dropped + s.calls_dropped + s.kept + s.pinned,
            " kept=",
            s.kept,
            " results_dropped=",
            s.results_dropped,
            " calls_dropped=",
            s.calls_dropped,
            " pinned=",
            s.pinned,
            " chars_before=",
            s.chars_before,
            " chars_after=",
            s.chars_after,
            " requests=",
            s.requests,
            " stage=",
            if s.state_stage.is_empty() {
                "none"
            } else {
                &s.state_stage
            }
        )
    } else {
        "calls=0 kept=0 results_dropped=0 calls_dropped=0 pinned=0 chars_before=0 chars_after=0 requests=0 stage=none".into()
    };
    out.push(text!(
        "[fast-jev-compaction stats] items=",
        transcript.entries.len(),
        " excluded_prompt_chars=",
        transcript.exclusion.as_ref().map_or(0, |e| e.chars),
        " excluded_prompt_reason=",
        transcript.exclusion.as_ref().map_or("none", |e| e.reason),
        " opaque_items=",
        transcript.opaque_items,
        " ",
        line
    ));
    text::join(&out, "\n")
}
