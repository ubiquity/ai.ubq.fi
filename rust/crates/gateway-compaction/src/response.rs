//! Actual local response orchestration over the caller-owned finite host.
use crate::{engine, hybrid};
use gateway_compaction_history::{state, transcript};
use gateway_compaction_render::{
    hybrid::CHUNK_CHARS,
    response::{fit, payload, structural},
};
use gateway_compaction_source::{
    model::Options,
    text::{self, Text},
    types::{
        CompactionFailure, CompactionHost, CompactionOutcome, FailureKind, RESIDUE_TIMEOUT_MS,
        ResponseOptions,
    },
};
use gateway_core::source_json::SourceJson;
use gateway_prompt_policy::tokens::estimate_tokens;
use std::collections::{BTreeMap, BTreeSet};
fn failed(kind: FailureKind, detail: &'static str) -> CompactionFailure {
    CompactionFailure::new(kind, detail)
}
pub(super) async fn build<H: CompactionHost>(
    body: &SourceJson,
    host: &H,
    options: ResponseOptions,
) -> Result<CompactionOutcome, CompactionFailure> {
    engine::check(host)?;
    let input = text::member(body, "input")
        .ok_or_else(|| failed(FailureKind::MissingInput, "missing-input"))?;
    let transcript = transcript::parse_input(&input)?
        .ok_or_else(|| failed(FailureKind::MissingInput, "missing-input"))?;
    if transcript.entries.is_empty() {
        return Err(failed(FailureKind::EmptyTranscript, "empty-transcript"));
    }
    let calls = state::collect_calls(&transcript.messages, 6);
    let candidates = calls.iter().filter(|c| !c.pinned).count();
    if candidates == 0 {
        return Err(failed(FailureKind::NoCandidates, "no-candidates"));
    }
    let result = engine::compact(&transcript.messages, host, &Options::default()).await?;
    let decided = result
        .decisions
        .iter()
        .map(|d| &d.id)
        .collect::<BTreeSet<_>>();
    if calls.iter().any(|c| !c.pinned && !decided.contains(&c.id)) {
        return Err(failed(FailureKind::JevFailed, "missing-decision"));
    }
    let input_tokens = estimate_tokens(Text::from(text::canonical(&input)?.as_str()).as_units());
    let fitted = fit(&transcript, &calls, &result, input_tokens)?;
    let (mut summary, fitted_drops, fitted_stats, floor_bound) = fitted.into_parts();
    let mut hybrid_applied = false;
    let mut hybrid_ms = None;
    if options.hybrid {
        let started = engine::now(host)?;
        if let Some(rewritten) = hybrid::run(&summary, host, CHUNK_CHARS, RESIDUE_TIMEOUT_MS).await
            && !rewritten.blank()
            && rewritten.len() < summary.len()
        {
            summary = rewritten;
            hybrid_applied = true;
        }
        hybrid_ms = Some(engine::now(host)? - started);
    }
    engine::check(host)?;
    let mut stats = fitted_stats;
    stats.chars_after = summary.len();
    if stats.results_dropped + stats.calls_dropped == 0 || stats.chars_after >= stats.chars_before {
        return Err(failed(FailureKind::NoReduction, "no-reduction"));
    }
    let suffix = host
        .random_uuid()
        .map_err(|_| failed(FailureKind::RenderFailed, "random-unavailable"))?
        .replace('-', "")
        .chars()
        .take(16)
        .collect::<String>();
    let wire = payload(&summary, options.stream, &suffix)?;
    let headers = BTreeMap::from([(
        "x-jev-compaction".into(),
        structural(&stats, fitted_drops, floor_bound, hybrid_applied),
    )]);
    Ok(CompactionOutcome {
        status: 200,
        content_type: if options.stream {
            "text/event-stream"
        } else {
            "application/json"
        },
        headers,
        body: wire,
        summary: summary.core(),
        stats,
        candidates,
        items: transcript.entries.len(),
        fitted_drops,
        floor_bound,
        hybrid: hybrid_applied,
        hybrid_ms,
        input_estimate_tokens: input_tokens,
    })
}
