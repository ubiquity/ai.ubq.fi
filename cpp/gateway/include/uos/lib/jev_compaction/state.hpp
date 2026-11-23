#pragma once
// Port of lib/jev_compaction/state.ts.
//
// Ported from 0x4007/fast-jev-compaction (MIT), commit
// c1eab5fdbd6bde7d67f2da070116496558836884, `src/state.ts`. Mechanical changes:
// Deno `.ts` specifiers, repository formatting, and non-consumed exports made
// module-private. The fitting stages and the token estimator are unchanged.
// See ./LICENSE and ./README.md.
#include "uos/lib/jev_compaction/types.hpp"
#include <string_view>
#include <vector>

namespace uos::lib::jev_compaction::state {
  inline constexpr std::string_view STATE_CONTEXT =
    "A coding assistant conversation is being compacted to free context. `history` is the whole conversation so far, oldest first; tool outputs are "
    "replaced by a short `result` note and long texts may be abridged. Each question asks whether one tool call, or the full output of that call, "
    "still needs to stay in the history verbatim. Whatever is not kept is deleted permanently, but the assistant can always re-run a tool or "
    "re-read a file.";

  // Estimates tokens without a tokenizer: a word costs one token per six
  // letters, a digit half a token, any other symbol nine tenths. Calibrated
  // against the usage Jev reports for real transcripts, where it lands 2-18%
  // above the true count; a plain characters-per-token ratio undercounts the
  // JSON-heavy states by up to 40%.
  double estimateTokens(std::string_view text);

  // Pairs every tool_use with its tool_result by `tool_use_id`. Calls without a
  // result are not candidates (there is nothing to drop yet).
  std::vector<types::ToolCall> collectToolCalls(const std::vector<types::Message>& messages, double preserveRecentMessages);

  // Builds the Jev state from the whole conversation and shrinks it in stages
  // until it fits `maxStateTokens`: tool inputs are truncated, then long texts
  // are abridged oldest-first (pinned messages last), then old messages collapse
  // to a one-line note, then old tool calls shrink to one line each, then old
  // messages that carry no call are left out, then runs of old call-only
  // messages are folded into one entry, and finally the oldest merged call runs
  // are dropped while pinned entries stay. Throws only when the pinned floor
  // alone exceeds the cap. Reads `maxStateTokens`, `preserveRecentMessages` and
  // `goal` of `options`.
  types::FittedState fitState(const std::vector<types::Message>& messages, const std::vector<types::ToolCall>& calls,
                              const types::ResolvedCompactOptions& options);
}
