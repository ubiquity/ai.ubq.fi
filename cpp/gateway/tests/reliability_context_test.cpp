// Port of tests/reliability-context.test.ts.
// Links: tests/reliability_context_test.cpp src/harmony/reliability/context.cpp src/harmony/reliability/surfaces.cpp
//        src/harmony/conversation.cpp src/harmony/tools/schemas.cpp src/harmony/types.cpp src/harmony/reliability/state.cpp
//        src/harmony/reliability/verify.cpp src/harmony/reliability/loops.cpp src/harmony/reliability/hash.cpp
#include "uos/harmony/reliability/context.hpp"

#include "uos/harmony/conversation.hpp"
#include "uos/harmony/reliability/state.hpp"
#include "uos/harmony/reliability/surfaces.hpp"
#include "uos/harmony/reliability/verify.hpp"
#include "uos/harmony/types.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::harmony::reliability::context;
  using uos::harmony::conversation::Conversation;
  using uos::harmony::conversation::createConversation;
  using uos::harmony::types::AssistantTurn;
  using uos::harmony::types::ConversationTurn;
  using uos::harmony::types::ToolResultTurn;
  namespace state = uos::harmony::reliability::state;
  namespace surfaces = uos::harmony::reliability::surfaces;
  namespace verify = uos::harmony::reliability::verify;
  namespace loops = uos::harmony::reliability::loops;

  // Module state shared by every case, as in the TS file (ids keep counting across cases).
  int pairCounter = 0;

  std::vector<ConversationTurn> pairTurns(const std::string& tool, const json::object& args, const ParsedToolResult& result) {
    const std::string id = "ctx-" + std::to_string(++pairCounter);
    return {
      AssistantTurn{
        .content = std::nullopt,
        .analysis = {},
        .toolCalls = {{.id = id, .name = tool, .arguments = rt::jsonStringify(json::value(args))}},
        .finishReason = "tool_calls",
      },
      ToolResultTurn{.toolCallId = id, .name = tool, .content = serializeToolResultContent(result)},
    };
  }

  ParsedToolResult okResult(std::string output) { return ParsedToolResult{.ok = true, .output = std::move(output)}; }

  void push(std::vector<ConversationTurn>& turns, std::vector<ConversationTurn> more) { turns.insert(turns.end(), more.begin(), more.end()); }

  Conversation scaffold() {
    std::vector<ConversationTurn> turns{uos::harmony::types::ConversationTurnUser{.content = "Fix all files."}};
    const auto big = [](const std::string& path) { return rt::repeat(path + "=1\n", 120); };  // ~720 chars per read
    push(turns, pairTurns("task.update_plan", json::object{{"plan", json::array{"read", "edit", "verify"}}}, okResult("plan updated (3 items)")));
    push(turns, pairTurns("editor.apply_patch", json::object{{"path", "a.txt"}, {"old", "a=0"}, {"new", "a=1"}}, okResult("patched a.txt")));
    for (int i = 0; i < 6; i++) {
      push(turns, pairTurns("filesystem.read", json::object{{"path", "a.txt"}}, okResult(big("a"))));
    }
    push(turns, pairTurns("editor.apply_patch", json::object{{"path", "b.txt"}, {"old", "b=0"}, {"new", "b=1"}}, okResult("patched b.txt")));
    push(turns, pairTurns("shell.exec", json::object{{"command", "sh tests/run.sh"}}, okResult("ok")));
    push(turns, pairTurns("filesystem.find", json::object{{"path", "."}, {"pattern", "*.txt"}}, okResult("a.txt\nb.txt")));
    push(turns, pairTurns("filesystem.search", json::object{{"path", "."}, {"query", "a=1"}}, okResult("a.txt:1:a=1")));
    push(turns, pairTurns("editor.apply_patch", json::object{{"path", "c.txt"}, {"old", "c=0"}, {"new", "c=1"}}, okResult("patched c.txt")));
    for (int i = 0; i < 3; i++) {
      push(turns, pairTurns("filesystem.read", json::object{{"path", "c.txt"}}, okResult(big("c"))));
    }
    push(turns, pairTurns("shell.exec", json::object{{"command", "grep -c c=1 c.txt"}},
                          ParsedToolResult{.ok = false, .error = "grep: no match", .error_code = "exec_failed"}));
    return createConversation(turns);
  }

  verify::VerificationPolicy policy() {
    verify::VerificationPolicy value = verify::DEFAULT_VERIFICATION_POLICY;
    value.verificationCommand = "sh tests/run.sh";
    return value;
  }

  state::StructuredTaskState stateOf(const Conversation& conversation) {
    const std::vector<ConversationTurn>& turns = conversation.turns;
    std::vector<state::ToolObservation> observations;
    double seq = 0;
    for (std::size_t i = 0; i < turns.size(); i++) {
      const AssistantTurn* turn = std::get_if<AssistantTurn>(&turns[i]);
      if (!turn || turn->toolCalls.size() != 1) continue;
      const uos::harmony::types::ToolCall& call = turn->toolCalls[0];
      const ConversationTurn* next = rt::at(turns, static_cast<double>(i + 1));
      if (next == nullptr) continue;
      const ToolResultTurn* result = std::get_if<ToolResultTurn>(next);
      if (!result || result->toolCallId != call.id) continue;
      const std::optional<ParsedToolResult> parsed = parseToolResultContent(result->content);
      seq += 1;
      observations.push_back(state::ToolObservation{
        .seq = seq,
        .tool = call.name,
        .args = rt::jsonParse(call.arguments).as_object(),
        .valid = parsed.has_value(),
        .result = parsed ? loops::ResultLike{.ok = parsed->ok, .error_code = parsed->error_code, .error = parsed->error, .output = parsed->output}
                         : loops::ResultLike{.ok = false, .error_code = "internal", .error = "unparseable"},
      });
    }
    return state::deriveStateWithMeta(state::ReliabilityRun{.observations = observations, .finals = {}, .modelCalls = 0}, state::replayMeta(observations, policy()));
  }

  std::vector<std::string> kindsOf(const CompactionResult& result) {
    std::vector<std::string> kinds;
    for (const CompactionDrop& drop : result.drops) kinds.push_back(drop.kind);
    return kinds;
  }

  std::size_t countKind(const CompactionResult& result, std::string_view kind) {
    return static_cast<std::size_t>(std::count_if(result.drops.begin(), result.drops.end(), [kind](const CompactionDrop& drop) { return drop.kind == kind; }));
  }

  bool matches(const std::string& text, const char* pattern) { return rt::Regex(pattern, "").test(text); }
}

TEST("context: token estimation is deterministic and monotone") {
  EXPECT_EQ(estimateTokens(""), 1.0);
  EXPECT_EQ(estimateTokens("a"), 1.0);
  EXPECT_EQ(estimateTokens(rt::repeat("a", 4000)), 1000.0);
  EXPECT(estimateConversationTokens(scaffold()) > 0);
  const double first = estimateConversationTokens(scaffold());
  const double second = estimateConversationTokens(scaffold());
  EXPECT_EQ(first, second);
}

TEST("context: tool-result serialization round-trips") {
  const std::string line = serializeToolResultContent(ParsedToolResult{.ok = false, .output = nullptr, .error = "dup", .error_code = "duplicate_call"});
  const std::optional<ParsedToolResult> parsed = parseToolResultContent(line);
  ASSERT(parsed.has_value());
  EXPECT_EQ(parsed->ok, false);
  EXPECT(parsed->error_code == "duplicate_call");
  EXPECT(parsed->error == "dup");
  EXPECT(!parseToolResultContent("not json").has_value());
}

TEST("context: short tier drops stale reads and old explore pairs") {
  const Conversation conversation = scaffold();
  const CompactionResult result = compactTranscript(conversation, CompactOptions{.budget = ContextBudgetKind::short_});
  const std::vector<std::string> kinds = kindsOf(result);
  EXPECT(rt::includes(kinds, std::string("stale_read")));
  EXPECT(rt::includes(kinds, std::string("old_explore")));
  EXPECT(result.droppedTurnCount > 0);
  const std::vector<ConversationPair> pairs = extractPairs(result.conversation);
  EXPECT(std::any_of(pairs.begin(), pairs.end(), [](const ConversationPair& pair) { return pair.tool == "shell.exec" && pair.parsed && pair.parsed->ok == false; }));
  EXPECT(std::count_if(pairs.begin(), pairs.end(), [](const ConversationPair& pair) { return pair.tool == "editor.apply_patch"; }) == 3);
  EXPECT(result.estimatedTokens < estimateConversationTokens(conversation));
}

TEST("context: compaction preserves the structured state contract at every budget") {
  const Conversation conversation = scaffold();
  const state::StructuredTaskState full = stateOf(conversation);
  const std::string fullContract = state::stateContract(full);
  EXPECT_EQ(full.pendingVerification.size(), std::size_t{0});
  for (ContextBudgetKind kind : {ContextBudgetKind::short_, ContextBudgetKind::medium, ContextBudgetKind::large}) {
    const CompactionResult compacted = compactTranscript(conversation, CompactOptions{.budget = kind});
    const std::string compactedContract = state::stateContract(stateOf(compacted.conversation));
    EXPECT_EQ(compactedContract, fullContract);
  }
}

TEST("context: large keeps more reads per path than short") {
  const Conversation conversation = scaffold();
  const std::size_t shortReads = countKind(compactTranscript(conversation, CompactOptions{.budget = ContextBudgetKind::short_}), "stale_read");
  const std::size_t largeReads = countKind(compactTranscript(conversation, CompactOptions{.budget = ContextBudgetKind::large}), "stale_read");
  EXPECT(shortReads > largeReads);
}

TEST("context: budgets are ordered and per-tier policies are stable") {
  EXPECT(COMPACTION_POLICIES.short_.targetTokens < COMPACTION_POLICIES.medium.targetTokens);
  EXPECT(COMPACTION_POLICIES.medium.targetTokens < COMPACTION_POLICIES.large.targetTokens);
  EXPECT_EQ(COMPACTION_POLICIES.short_.readsPerPath, 1.0);
  EXPECT_EQ(COMPACTION_POLICIES.large.readsPerPath, 2.0);
}

TEST("context: structured context is cheaper than a full transcript replay") {
  const Conversation conversation = scaffold();
  const state::StructuredTaskState taskState = stateOf(conversation);
  const double full = estimateConversationTokens(conversation);
  const std::pair<ContextBudgetKind, double> tailTurnsByKind[]{{ContextBudgetKind::short_, 2}, {ContextBudgetKind::medium, 4}, {ContextBudgetKind::large, 8}};
  for (const auto& [kind, tailTurns] : tailTurnsByKind) {
    const std::string structured = renderStructuredContext(taskState, conversation, StructuredContextOptions{.maxTailTurns = tailTurns});
    const double structuredTokens = estimateTokens(structured);
    EXPECT(structuredTokens < full);
    EXPECT(matches(structured, "\\[structured task state\\]"));
    EXPECT(matches(structured, "phase: "));
    (void)kind;
  }
}

TEST("context: the broad experimental surface is larger than the compact one") {
  const surfaces::ToolSurface compact = surfaces::compactToolSurface();
  const surfaces::ToolSurface broad = surfaces::broadToolSurface();
  EXPECT_EQ(compact.definitions.size(), std::size_t{9});
  EXPECT_EQ(broad.definitions.size(), std::size_t{13});
  const double compactTokens = surfaces::surfaceTokenCost(compact);
  const double broadTokens = surfaces::surfaceTokenCost(broad);
  EXPECT(broadTokens > compactTokens);
  EXPECT(std::any_of(broad.definitions.begin(), broad.definitions.end(), [](const uos::harmony::types::ToolDefinition& tool) { return tool.name == "filesystem.write"; }));
}
