#pragma once
// Port of src/harmony/reliability/context.ts.
/**
 * Transcript compaction and structured context (plan m05).
 *
 * The canonical harness keeps the authoritative conversation in full and
 * compacts the MODEL-FACING view per context budget tier before every
 * request.  Three deterministic policies are defined for short / medium /
 * large contexts (larger budgets → less aggressive compaction):
 *
 * - **never dropped** (contract-critical): failed calls, `editor.apply_patch`
 *   pairs, `task.update_plan` pairs, `shell.exec` pairs (command evidence and
 *   verification), guard feedback, final answers, user/system turns;
 * - **droppable** when old or stale: successful `filesystem.read` pairs
 *   beyond `readsPerPath` per path, successful find/search/browser pairs
 *   beyond the recent window, and private analysis outside the window.
 *
 * Dropping is always WHOLE pairs (assistant call + tool result together) and
 * never changes content, which keeps the structured-state contract invariant:
 * the last read per path and every patch/plan/command pair survive, so the
 * verification replay produces identical state from the compacted view.
 *
 * Tool results are stored in the conversation as one deterministic JSON line
 * ({@link serializeToolResultContent}) so the compaction layer can classify
 * pairs without side tables; the same parse is used by the replay derivation.
 *
 * Token accounting is a deterministic surrogate (`estimateTokens`), never a
 * live usage report: it exists so fake-transport runs and the comparison
 * evidence are reproducible offline.
 */
#include "uos/harmony/conversation.hpp"
#include "uos/harmony/reliability/state.hpp"
#include "uos/harmony/types.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/prelude.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::harmony::reliability::context {
  // ---------------------------------------------------------------------------
  // Deterministic token estimation (surrogate; not a live usage report)
  // ---------------------------------------------------------------------------

  double estimateTokens(std::string_view text);

  // `value: unknown` (every caller passes JSON data).
  double estimateJsonTokens(const json::value& value);

  /** Deterministic model-facing message payload of a conversation. */
  std::vector<json::object> wireMessages(const conversation::Conversation& conversation);

  /** Estimated tokens of a full wire transcript (messages only). */
  double estimateConversationTokens(const conversation::Conversation& conversation);

  /** Estimated tokens of one request body (messages + tools + overhead). */
  double estimateRequestTokens(const json::object& body);

  // ---------------------------------------------------------------------------
  // Machine-readable tool-result content (conversation storage format)
  // ---------------------------------------------------------------------------

  struct ParsedToolResult {
    bool ok;
    rt::Nullable<std::string> output{};
    rt::Nullable<std::string> error{};
    rt::Nullable<std::string> error_code{};
    friend bool operator==(const ParsedToolResult&, const ParsedToolResult&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const ParsedToolResult& value);
  ParsedToolResult tag_invoke(const json::value_to_tag<ParsedToolResult>&, const json::value& value);

  /** Serializes one result envelope as the deterministic conversation line. */
  std::string serializeToolResultContent(const ParsedToolResult& result);

  /** Parses a stored conversation tool-result line, or null when not canonical. */
  std::optional<ParsedToolResult> parseToolResultContent(std::string_view content);

  // ---------------------------------------------------------------------------
  // Budget tiers and policies
  // ---------------------------------------------------------------------------

  // "short" | "medium" | "large": the key of COMPACTION_POLICIES, switched over exhaustively.
  enum class ContextBudgetKind { short_, medium, large };
  std::string_view toString(ContextBudgetKind kind);
  std::optional<ContextBudgetKind> parseContextBudgetKind(std::string_view text);

  struct CompactionPolicy {
    ContextBudgetKind kind;
    /** Deterministic token budget the tier targets. */
    double targetTokens;
    /** Successful read pairs kept per path (older reads are stale). */
    double readsPerPath;
    /** Recent window of pairs never aged out (reads/explore). */
    double keepRecentPairs;
    /** Drop successful find/search/browser pairs outside the recent window. */
    bool dropOldExplore;
    /** Analysis outside the last N turns is dropped locally. */
    double analysisWindow;
    friend bool operator==(const CompactionPolicy&, const CompactionPolicy&) = default;
  };

  // Readonly<Record<ContextBudgetKind, CompactionPolicy>> (`short` is a C++ keyword).
  struct CompactionPolicies {
    CompactionPolicy short_;
    CompactionPolicy medium;
    CompactionPolicy large;
  };

  inline constexpr CompactionPolicies COMPACTION_POLICIES{
    .short_ =
      {
        .kind = ContextBudgetKind::short_,
        .targetTokens = 4'000,
        .readsPerPath = 1,
        .keepRecentPairs = 4,
        .dropOldExplore = true,
        .analysisWindow = 6,
      },
    .medium =
      {
        .kind = ContextBudgetKind::medium,
        .targetTokens = 8'000,
        .readsPerPath = 1,
        .keepRecentPairs = 8,
        .dropOldExplore = true,
        .analysisWindow = 10,
      },
    .large =
      {
        .kind = ContextBudgetKind::large,
        .targetTokens = 16'000,
        .readsPerPath = 2,
        .keepRecentPairs = rt::Number::MAX_SAFE_INTEGER,
        .dropOldExplore = false,
        .analysisWindow = 48,
      },
  };

  /** Reads the tier for a budget argument (kind or explicit token count). */
  CompactionPolicy policyForBudget(ContextBudgetKind budget);

  // ---------------------------------------------------------------------------
  // Pair extraction
  // ---------------------------------------------------------------------------

  // Module-private `ConversationPair`, the element type of extractPairs.
  struct ConversationPair {
    std::int64_t assistantIndex;
    std::int64_t resultIndex;
    std::string callId;
    std::string tool;
    json::object args;
    std::optional<ParsedToolResult> parsed;
    friend bool operator==(const ConversationPair&, const ConversationPair&) = default;
  };

  /** Extracts (assistant tool_call, tool result) pairs from a conversation. */
  std::vector<ConversationPair> extractPairs(const conversation::Conversation& conversation);

  // "stale_read" | "old_explore" | "analysis"
  using CompactionDropKind = std::string;

  struct CompactionDrop {
    CompactionDropKind kind;
    std::string detail;
    friend bool operator==(const CompactionDrop&, const CompactionDrop&) = default;
  };

  struct CompactionResult {
    conversation::Conversation conversation;
    std::vector<CompactionDrop> drops;
    double estimatedTokens;
    bool metBudget;
    std::int64_t droppedTurnCount;
    friend bool operator==(const CompactionResult&, const CompactionResult&) = default;
  };

  struct CompactOptions {
    ContextBudgetKind budget;
    /** Kept in the compaction tests; harness defaults to the tier policy. */
    std::optional<CompactionPolicy> policy{};
  };

  /**
   * Compacts the model-facing view of one conversation for a budget tier.
   * The returned conversation preserves the contract-critical pairs; dropped
   * pairs are whole (assistant call + tool result) and never reordered.
   */
  CompactionResult compactTranscript(const conversation::Conversation& conversation, const CompactOptions& opts);

  // ---------------------------------------------------------------------------
  // Structured context rendering
  // ---------------------------------------------------------------------------

  struct StructuredContextOptions {
    /** Recent transcript turns appended after the state summary. */
    std::optional<double> maxTailTurns{};
  };

  /**
   * Renders the structured task context: the deterministic state summary plus
   * the recent transcript tail.  This is the model-facing replacement for a
   * full transcript replay at any budget tier.
   */
  // `opts = {}` is the overload without it.
  std::string renderStructuredContext(const state::StructuredTaskState& state, const conversation::Conversation& conversation);
  std::string renderStructuredContext(const state::StructuredTaskState& state, const conversation::Conversation& conversation, const StructuredContextOptions& opts);
}
