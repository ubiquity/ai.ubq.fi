#pragma once
// Port of lib/jev_compaction/compact.ts.
//
// Ported from 0x4007/fast-jev-compaction (MIT), commit
// c1eab5fdbd6bde7d67f2da070116496558836884, `src/compact.ts`. Mechanical
// changes: Deno `.ts` specifiers, repository formatting, non-consumed exports
// made module-private, and the unused `reductionRatio` helper dropped. The
// selection rules, batching, truncation semantics and decision mapping are
// unchanged. See ./LICENSE and ./README.md.
#include "uos/lib/jev_compaction/types.hpp"
#include "uos/rt/async.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uos::lib::jev_compaction::compact {
  // The bounded head plus notice a `drop_result` decision leaves behind.
  // Exported so renderers outside `applyDecisions` (the Codex adapter) reuse the
  // exact same truncation semantics instead of duplicating them.
  std::string truncatedResultText(std::string_view text, bool isError, double headChars);

  // One element of planWindows' result: messages [start, end).
  struct Window {
    std::int64_t start = 0;
    std::int64_t end = 0;
  };

  // Splits messages into windows of at most `maxWindowChars`, never splitting a
  // tool call from its result pair. Callers slice windows out of the transcript
  // for local state fitting and decisions.
  std::vector<Window> planWindows(const std::vector<types::Message>& messages, const std::vector<types::ToolCall>& calls, double maxWindowChars);

  // Compacts a transcript by asking Jev, for every tool call outside the pinned
  // first and newest messages, whether the call and whether its result must
  // stay. The whole history (results omitted, fitted into `maxStateTokens`) is
  // sent as state with every batch of questions. Throws when Jev fails or the
  // history cannot be fitted; the caller decides whether to fall back.
  [[nodiscard]] rt::Task<types::CompactResult> compact(std::vector<types::Message> messages, std::shared_ptr<types::JevAsker> asker,
                                                       types::CompactOptions options = {});
}
