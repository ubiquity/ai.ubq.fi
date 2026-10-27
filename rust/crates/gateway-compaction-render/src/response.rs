//! Opaque summary fitting and exact structural/wire projection.
use crate::transcript;
use gateway_compaction_history::transcript::Transcript;
use gateway_compaction_source::{
    model::{Action, CompactResult, Decision, Reason, ToolCall},
    text::{self, Text},
    types::{CompactionFailure, CompactionStats, FailureKind, SUMMARY_CHAR_CAP},
};
use gateway_prompt_policy::tokens::estimate_tokens;
use std::collections::BTreeSet;
fn failed(kind: FailureKind, detail: &'static str) -> CompactionFailure {
    CompactionFailure::new(kind, detail)
}
/// Only the fitting algorithm can construct this result; runtime consumers can decompose data.
/// ```compile_fail
/// use gateway_compaction_render::response::Fit;
/// use gateway_compaction_source::{text::Text, types::CompactionStats};
/// let _ = Fit { summary: Text::default(), drops: 0, stats: CompactionStats::default(), floor: false };
/// ```
pub struct Fit {
    summary: Text,
    drops: usize,
    stats: CompactionStats,
    floor: bool,
}
impl Fit {
    pub fn into_parts(self) -> (Text, usize, CompactionStats, bool) {
        (self.summary, self.drops, self.stats, self.floor)
    }
}
fn render(
    transcript: &Transcript,
    calls: &[ToolCall],
    result: &CompactResult,
    decisions: &[Decision],
) -> Result<Text, CompactionFailure> {
    let summary = transcript::render(transcript, calls, decisions, 300, Some(&result.stats));
    if summary.blank() {
        Err(failed(FailureKind::EmptySummary, "empty-summary"))
    } else {
        Ok(summary)
    }
}
pub fn max_floor_tokens(input: usize) -> usize {
    (input as f64 * (1.0 - 0.15)).floor() as usize
}
pub fn fit(
    transcript: &Transcript,
    calls: &[ToolCall],
    result: &CompactResult,
    input: usize,
) -> Result<Fit, CompactionFailure> {
    let accepts = |summary: &Text| {
        input > 0 && estimate_tokens(summary.as_units()) <= max_floor_tokens(input)
    };
    let summary = render(transcript, calls, result, &result.decisions)?;
    if summary.len() <= SUMMARY_CHAR_CAP {
        return Ok(Fit {
            summary,
            drops: 0,
            stats: result.stats.clone(),
            floor: false,
        });
    }
    let mut ids = result
        .decisions
        .iter()
        .filter(|d| d.action == Action::Keep && d.reason == Reason::Kept)
        .collect::<Vec<_>>();
    ids.sort_by(|a, b| {
        a.keep_result
            .partial_cmp(&b.keep_result)
            .unwrap_or_else(|| panic!("finite source answers"))
    });
    let ids = ids.iter().map(|d| d.id.clone()).collect::<Vec<_>>();
    let with_dropped = |count: usize| {
        let drop = ids[..count].iter().collect::<BTreeSet<_>>();
        let decisions = result
            .decisions
            .iter()
            .map(|d| {
                let mut next = d.clone();
                if drop.contains(&d.id) {
                    next.action = Action::DropResult;
                    next.reason = Reason::ResultDropped;
                }
                next
            })
            .collect::<Vec<_>>();
        render(transcript, calls, result, &decisions)
    };
    if ids.is_empty() {
        return if accepts(&summary) {
            Ok(Fit {
                summary,
                drops: 0,
                stats: result.stats.clone(),
                floor: true,
            })
        } else {
            Err(failed(FailureKind::SummaryTooLarge, "summary-too-large"))
        };
    }
    let floor = with_dropped(ids.len())?;
    let mut floor_bound = false;
    let (summary, count) = if floor.len() > SUMMARY_CHAR_CAP {
        if !accepts(&floor) {
            return Err(failed(FailureKind::SummaryTooLarge, "summary-too-large"));
        }
        floor_bound = true;
        (floor, ids.len())
    } else {
        let (mut lo, mut hi) = (0, ids.len());
        while hi - lo > 1 {
            let mid = (lo + hi) / 2;
            if with_dropped(mid)?.len() <= SUMMARY_CHAR_CAP {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        let value = with_dropped(hi)?;
        if value.len() > SUMMARY_CHAR_CAP {
            return Err(failed(FailureKind::SummaryTooLarge, "summary-too-large"));
        }
        (value, hi)
    };
    let ids = ids[..count].iter().collect::<BTreeSet<_>>();
    let dropped = calls
        .iter()
        .filter(|c| ids.contains(&c.id))
        .map(|c| c.result_chars)
        .sum::<usize>();
    let mut stats = result.stats.clone();
    stats.kept = stats.kept.saturating_sub(count);
    stats.results_dropped += count;
    stats.chars_after = stats.chars_after.saturating_sub(dropped);
    Ok(Fit {
        summary,
        drops: count,
        stats,
        floor: floor_bound,
    })
}
pub fn structural(stats: &CompactionStats, fit: usize, floor: bool, hybrid: bool) -> String {
    let mut out = format!(
        "summary; kept={}; results_dropped={}; calls_dropped={}; pinned={}; chars_before={}; chars_after={}",
        stats.kept,
        stats.results_dropped,
        stats.calls_dropped,
        stats.pinned,
        stats.chars_before,
        stats.chars_after
    );
    if fit > 0 {
        out.push_str(&format!("; fitted={fit}"));
    }
    if floor {
        out.push_str("; floor=1");
    }
    if hybrid {
        out.push_str("; hybrid=1");
    }
    out
}
pub fn payload(summary: &Text, stream: bool, suffix: &str) -> Result<Vec<u8>, CompactionFailure> {
    let item = text::object(vec![
        ("type", text::raw("\"message\"")?),
        ("role", text::raw("\"assistant\"")?),
        ("id", text::quoted(&text!("msg_jev_", suffix))?),
        (
            "content",
            text::json_array(&[text::object(vec![
                ("type", text::raw("\"output_text\"")?),
                ("text", text::quoted(summary)?),
            ])?])?,
        ),
    ])?;
    let id = text::quoted(&text!("resp_jev_", suffix))?;
    if stream {
        let done = text::object(vec![
            ("type", text::raw("\"response.output_item.done\"")?),
            ("item", item),
        ])?;
        let completed = text::object(vec![
            ("type", text::raw("\"response.completed\"")?),
            (
                "response",
                text::object(vec![("id", id), ("status", text::raw("\"completed\"")?)])?,
            ),
        ])?;
        Ok(format!(
            "event: response.output_item.done\ndata: {}\n\nevent: response.completed\ndata: {}\n\n",
            done.as_str(),
            completed.as_str()
        )
        .into_bytes())
    } else {
        Ok(text::object(vec![
            ("id", id),
            ("object", text::raw("\"response\"")?),
            ("status", text::raw("\"completed\"")?),
            ("output", text::json_array(&[item])?),
        ])?
        .into_bytes())
    }
}
