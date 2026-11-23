#pragma once
// Port of lib/jev_compaction/types.ts.
//
// Ported from 0x4007/fast-jev-compaction (MIT), commit
// c1eab5fdbd6bde7d67f2da070116496558836884, `src/types.ts`. Mechanical changes:
// Deno `.ts` specifiers, repository formatting (type aliases, not interfaces),
// and only the types this gateway's subset consumes are kept. See ./LICENSE and
// ./README.md.
//
// TS object types the library constructs and reads are structs (same field names
// and order); `Record<string, unknown>` members and the wire views (`JevResponse`,
// `JevAnswer`) are JSON. String literal unions that are only compared and printed
// (`Role`, `CallAction`, decision reasons) are std::string.
#include "uos/rt/async.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace uos::lib::jev_compaction::types {
  // "user" | "assistant"
  using Role = std::string;

  // A tool_use block of an assistant message. `text` and `isError` mirror the
  // outcome once the transcript holds it (Codex attaches them).
  struct ToolUse {
    std::string tool_use_id;
    std::string tool;
    json::object input;
    std::optional<std::string> text;
    std::optional<bool> isError;
  };

  // A tool_result block of a user message.
  struct ToolResult {
    std::string tool_use_id;
    std::string text;
    std::optional<bool> isError;
  };

  // One transcript message, the shape the selection library decides over.
  struct Message {
    Role role;
    std::string text;
    std::vector<ToolUse> toolUses;
    std::optional<std::vector<ToolResult>> toolResults;
  };

  // A tool call paired with its result by `tool_use_id`.
  struct ToolCall {
    // Short id used in the Jev state and question names (`t1`, `t2`, ...).
    std::string id;
    std::string tool_use_id;
    std::string tool;
    json::object input;
    // Index of the message holding the tool_use block.
    std::int64_t callIndex = 0;
    // Index of the message holding the tool_result block.
    std::int64_t resultIndex = 0;
    std::int64_t resultChars = 0;
    bool isError = false;
    // In the first or the newest preserved messages; never a candidate.
    bool pinned = false;
  };

  struct CallAnswer {
    // Jev's probability that the call itself still matters.
    double keepCall = 0;
    // Jev's probability that the full result still needs to stay verbatim.
    double keepResult = 0;
  };

  // "keep" | "drop_result" | "drop_call"
  using CallAction = std::string;

  // `CallAnswer & { id; tool; action; reason }`, members in the order the library
  // builds the object (`{ id, tool, ...answer, action, reason }`).
  struct CallDecision {
    std::string id;
    std::string tool;
    double keepCall = 0;
    double keepResult = 0;
    CallAction action;
    // "pinned" | "kept" | "result_dropped" | "call_dropped"
    std::string reason;
  };

  struct HistoryToolCall {
    std::string id;
    std::string tool;
    std::string input;
    std::string result;
  };

  struct HistoryEntry {
    std::int64_t i = 0;
    Role role;
    std::string text;
    // Structured per call, or one compact line per call once the state has to shrink.
    std::optional<std::variant<std::vector<HistoryToolCall>, std::vector<std::string>>> tool_calls;
  };

  // The state sent with every Jev request: the whole history, results omitted.
  struct CompactionState {
    std::string context;
    std::string goal;
    std::vector<HistoryEntry> history;
  };

  struct FittedState {
    CompactionState state;
    double tokens = 0;
    // Which fitting stage produced the state, for diagnostics.
    std::string stage;
  };

  struct CompactOptions {
    // Ongoing task description; defaults to the last few user prompts.
    std::optional<std::string> goal;
    // Minimum keep probability for a call or result to stay. Default 0.5.
    std::optional<double> keepThreshold;
    // Newest messages never touched (the first message is always kept). Default 6.
    std::optional<double> preserveRecentMessages;
    // Estimated token ceiling for the state. Default 25000.
    std::optional<double> maxStateTokens;
    // Estimated token ceiling for state plus one batch of questions. Default 30000.
    std::optional<double> maxRequestTokens;
    // Characters of a dropped tool result to retain. Default 300.
    std::optional<double> truncateHeadChars;
  };

  // Also the parameter type of the `Pick<ResolvedCompactOptions, ...>` parameters
  // (fitState, batchCalls, decideCall): members a callee does not pick are unused.
  struct ResolvedCompactOptions {
    std::string goal;
    double keepThreshold = 0;
    double preserveRecentMessages = 0;
    double maxStateTokens = 0;
    double maxRequestTokens = 0;
    double truncateHeadChars = 0;
  };

  struct CompactResult {
    // `CompactResult["stats"]`
    struct Stats {
      std::int64_t messagesBefore = 0;
      std::int64_t messagesAfter = 0;
      std::int64_t charsBefore = 0;
      std::int64_t charsAfter = 0;
      std::int64_t calls = 0;
      std::int64_t kept = 0;
      std::int64_t resultsDropped = 0;
      std::int64_t callsDropped = 0;
      std::int64_t pinned = 0;
      double stateTokens = 0;
      // Which fitting stage the state needed, "" when no request was made.
      std::string stateStage;
      // Number of decision windows used; 0 when the global state served every call.
      std::int64_t windows = 0;
      std::int64_t requests = 0;
      double ms = 0;
    };

    // The compacted transcript; untouched messages are copies of the input messages
    // (the TS returns the same objects).
    std::vector<Message> messages;
    std::vector<CallDecision> decisions;
    Stats stats;
  };

  // The `state` of a Jev request: a string or any JSON-serialisable object.
  using JevState = json::value;

  struct NoulQuestion {
    std::string type = "noul";
    std::string instructions;
  };

  // `Record<string, NoulQuestion>`: iterates in insertion order (JS order differs
  // only for array-index names, which question names never are).
  using JevQuestions = rt::OrderedMap<std::string, NoulQuestion>;

  struct NoulAnswer {
    // "noul"
    std::optional<std::string> type;
    double noul = 0;
  };

  // `NoulAnswer | Record<string, unknown>`: an answer as it arrived on the wire.
  using JevAnswer = json::value;

  // `{ model?: string; answers: Record<string, JevAnswer>; usage?: { input_tokens?,
  // output_tokens? }; [key: string]: unknown }`: the response object as parsed.
  using JevResponse = json::object;

  // Anything that can answer Jev questions: `JevClient`, or a host-provided adapter.
  class JevAsker {
  public:
    virtual ~JevAsker() = default;
    [[nodiscard]] virtual rt::Task<JevResponse> ask(JevState state, JevQuestions questions) = 0;
  };

  // `JSON.parse(JSON.stringify(x))` of these objects: their members in the order the
  // TS builds them, an absent optional member left out.
  json::value toJson(const HistoryToolCall& call);
  json::value toJson(const HistoryEntry& entry);
  json::value toJson(const CompactionState& state);
  json::value toJson(const NoulQuestion& question);
  json::value toJson(const JevQuestions& questions);
}
