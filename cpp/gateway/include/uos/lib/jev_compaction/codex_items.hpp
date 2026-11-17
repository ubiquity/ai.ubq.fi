#pragma once
// Port of lib/jev_compaction/codex_items.ts.
//
// Ported from 0x4007/fast-jev-compaction (MIT), commit
// c1eab5fdbd6bde7d67f2da070116496558836884, `codex/codex-items.ts`. Mechanical
// changes: Deno `.ts` specifiers importing this directory instead of `dist/`,
// repository formatting (type aliases, not interfaces; no nested ternaries), and
// non-consumed exports made module-private. The Codex item mapping, prompt
// exclusion, opaque-payload redaction and verbatim renderer are unchanged. See
// ./LICENSE and ./README.md.
//
// Pure mapping between Codex Responses API input items and the compaction
// library `Message` shape, plus the deterministic renderer that turns Jev's
// keep/drop decisions back into one text summary. No I/O, no Deno/Node APIs.
#include "uos/lib/jev_compaction/types.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::lib::jev_compaction::codex_items {
  // First line of every rendered summary; never the library's own prefix.
  inline constexpr std::string_view SUMMARY_MARKER = "# fast-jev-compaction memory summary";

  // Refuse to answer with a larger summary than this; never truncate pinned data.
  inline constexpr std::int64_t MAX_SUMMARY_CHARS = 400000;

  // The transcript entry kinds (module-private in the TS; CodexTranscript holds
  // them). The variant alternative is the `kind` discriminant.
  struct TextEntry {
    static constexpr std::string_view kind = "text";
    std::string itemType;
    // "user" | "assistant" | "developer" | "system"
    std::string role;
    std::string text;
  };

  struct CallEntry {
    static constexpr std::string_view kind = "call";
    std::string itemType;
    std::string callId;
    std::string tool;
    json::object input;
    std::string inputJson;
  };

  struct ResultEntry {
    static constexpr std::string_view kind = "result";
    std::string itemType;
    std::string callId;
    std::string text;
    bool isError = false;
  };

  using TranscriptEntry = std::variant<TextEntry, CallEntry, ResultEntry>;

  struct PromptExclusion {
    // Index in the original `input` array.
    std::int64_t index = 0;
    std::int64_t chars = 0;
    // "known-prompt" | "trailing-prompt-assumed"
    std::string reason;
  };

  struct CodexTranscript {
    // Original item order, injected compaction prompt removed.
    std::vector<TranscriptEntry> entries;
    // One library message per surviving entry, same order.
    std::vector<types::Message> messages;
    std::optional<PromptExclusion> exclusion;
    // Items rendered as an opaque placeholder instead of plaintext.
    std::int64_t opaqueItems = 0;
  };

  // Maps a request `input` array to library messages in original order. Returns
  // `null` when `input` is not an array of items (caller treats that as failure).
  // The pointer overload takes an absent member (nullptr is undefined).
  std::optional<CodexTranscript> parseCodexInput(const json::value& input);
  std::optional<CodexTranscript> parseCodexInput(const json::value* input);

  struct RenderOptions {
    std::vector<types::CallDecision> decisions;
    // Library short call id (`t1`, `t2`, ...) to the Codex `call_id`. Built with
    // `collectToolCalls(messages, preserveRecentMessages)`, the same public
    // pairing the library used, so decisions cannot drift out of alignment.
    rt::OrderedMap<std::string, std::string> callIds;
    double headChars = 0;
    std::optional<types::CompactResult::Stats> stats;
  };

  // Renders the surviving transcript; kept text is emitted verbatim.
  std::string renderSummary(const CodexTranscript& transcript, const RenderOptions& options);

  // `{ request_kind?: string; compaction?: { implementation?: string; [key]: unknown };
  // [key]: unknown }`: the parsed header object.
  using TurnMetadata = json::object;

  // Parses `x-codex-turn-metadata`; malformed or absent metadata is `null`.
  std::optional<TurnMetadata> parseTurnMetadata(const std::optional<std::string>& header);

  // Interception predicate: header metadata only, no prompt or body inspection.
  // An explicit compaction request with another implementation is *not* adopted.
  bool isResponsesCompaction(const std::optional<TurnMetadata>& metadata);
}
