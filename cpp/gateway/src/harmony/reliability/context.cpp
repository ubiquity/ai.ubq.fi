#include "uos/harmony/reliability/context.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace uos::harmony::reliability::context {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using types::AssistantTurn;
    using types::ConversationTurn;
    using types::ToolResultTurn;

    json::value nullableString(const rt::Nullable<std::string>& value) { return value.hasValue() ? json::value(*value) : json::value(nullptr); }

    rt::Nullable<std::string> stringOrNull(const json::object& value, std::string_view key) {
      const std::optional<std::string_view> text = rt::getString(value, key);
      if (text) return std::string(*text);
      return nullptr;
    }

    bool isGuardPair(const ConversationPair& pair) {
      return pair.parsed && (pair.parsed->error_code == "duplicate_call" || pair.parsed->error_code == "repeated_failure");
    }

    bool isErrorPair(const ConversationPair& pair) { return pair.parsed ? !pair.parsed->ok || isGuardPair(pair) : true; }

    json::object parseStoredArguments(std::string_view argumentsText) {
      try {
        json::value value = rt::jsonParse(argumentsText);
        if (value.is_object()) return std::move(value.get_object());
      } catch (const rt::JsError&) {
        // fall through
      }
      return {};
    }

    bool isSystemish(const ConversationTurn& turn) {
      return std::holds_alternative<types::ConversationTurnSystem>(turn) || std::holds_alternative<types::ConversationTurnDeveloper>(turn);
    }

    json::value messagesJson(const std::vector<json::object>& messages) {
      json::array out;
      for (const json::object& message : messages) out.push_back(message);
      return out;
    }
  }

  // ---------------------------------------------------------------------------
  // Deterministic token estimation (surrogate; not a live usage report)
  // ---------------------------------------------------------------------------

  double estimateTokens(std::string_view text) { return rt::Math::max(1.0, rt::Math::ceil(static_cast<double>(rt::length(text)) / 4)); }

  double estimateJsonTokens(const json::value& value) { return estimateTokens(rt::jsonStringify(value)); }

  std::vector<json::object> wireMessages(const conversation::Conversation& conversation) { return conversation::wireMessagesFromConversation(conversation); }

  double estimateConversationTokens(const conversation::Conversation& conversation) { return estimateJsonTokens(messagesJson(wireMessages(conversation))); }

  double estimateRequestTokens(const json::object& body) { return estimateJsonTokens(json::value(body)) + 4; }

  // ---------------------------------------------------------------------------
  // Machine-readable tool-result content (conversation storage format)
  // ---------------------------------------------------------------------------

  void tag_invoke(const json::value_from_tag&, json::value& out, const ParsedToolResult& value) {
    out = Writer().required("ok", value.ok).nullable("output", value.output).nullable("error", value.error).nullable("error_code", value.error_code).done();
  }

  ParsedToolResult tag_invoke(const json::value_to_tag<ParsedToolResult>&, const json::value& value) {
    const Reader in(value, "ParsedToolResult");
    return ParsedToolResult{
      .ok = in.required<bool>("ok"),
      .output = in.nullable<std::string>("output"),
      .error = in.nullable<std::string>("error"),
      .error_code = in.nullable<std::string>("error_code"),
    };
  }

  std::string serializeToolResultContent(const ParsedToolResult& result) {
    json::object out;
    out["ok"] = result.ok;
    out["output"] = nullableString(result.output);
    out["error"] = nullableString(result.error);
    out["error_code"] = nullableString(result.error_code);
    return rt::jsonStringify(json::value(std::move(out)));
  }

  std::optional<ParsedToolResult> parseToolResultContent(std::string_view content) {
    json::value value;
    try {
      value = rt::jsonParse(content);
    } catch (const rt::JsError&) {
      return std::nullopt;
    }
    if (!value.is_object()) return std::nullopt;
    const json::object& record = value.get_object();
    const std::optional<bool> ok = rt::getBool(record, "ok");
    if (!ok) return std::nullopt;
    return ParsedToolResult{
      .ok = *ok,
      .output = stringOrNull(record, "output"),
      .error = stringOrNull(record, "error"),
      .error_code = stringOrNull(record, "error_code"),
    };
  }

  // ---------------------------------------------------------------------------
  // Budget tiers and policies
  // ---------------------------------------------------------------------------

  std::string_view toString(ContextBudgetKind kind) {
    switch (kind) {
      case ContextBudgetKind::short_:
        return "short";
      case ContextBudgetKind::medium:
        return "medium";
      case ContextBudgetKind::large:
        return "large";
    }
    return "short";
  }

  std::optional<ContextBudgetKind> parseContextBudgetKind(std::string_view text) {
    if (text == "short") return ContextBudgetKind::short_;
    if (text == "medium") return ContextBudgetKind::medium;
    if (text == "large") return ContextBudgetKind::large;
    return std::nullopt;
  }

  CompactionPolicy policyForBudget(ContextBudgetKind budget) {
    switch (budget) {
      case ContextBudgetKind::short_:
        return COMPACTION_POLICIES.short_;
      case ContextBudgetKind::medium:
        return COMPACTION_POLICIES.medium;
      case ContextBudgetKind::large:
        return COMPACTION_POLICIES.large;
    }
    return COMPACTION_POLICIES.short_;
  }

  // ---------------------------------------------------------------------------
  // Pair extraction
  // ---------------------------------------------------------------------------

  std::vector<ConversationPair> extractPairs(const conversation::Conversation& conversation) {
    std::vector<ConversationPair> pairs;
    const std::vector<ConversationTurn>& turns = conversation.turns;
    for (std::size_t i = 0; i < turns.size(); i++) {
      const AssistantTurn* turn = std::get_if<AssistantTurn>(&turns[i]);
      if (!turn || turn->toolCalls.size() != 1) continue;
      const types::ToolCall& call = turn->toolCalls[0];
      // `.at()` keeps the out-of-bounds case honest: the last turn of a
      // conversation may be an assistant call whose result turn never arrived.
      const ConversationTurn* next = rt::at(turns, static_cast<double>(i + 1));
      if (next == nullptr) continue;
      const ToolResultTurn* result = std::get_if<ToolResultTurn>(next);
      if (!result || result->toolCallId != call.id) continue;
      pairs.push_back(ConversationPair{
        .assistantIndex = static_cast<std::int64_t>(i),
        .resultIndex = static_cast<std::int64_t>(i + 1),
        .callId = call.id,
        .tool = call.name,
        .args = parseStoredArguments(call.arguments),
        .parsed = parseToolResultContent(result->content),
      });
    }
    return pairs;
  }

  CompactionResult compactTranscript(const conversation::Conversation& conversation, const CompactOptions& opts) {
    const CompactionPolicy policy = opts.policy ? *opts.policy : policyForBudget(opts.budget);
    const std::vector<ConversationPair> pairs = extractPairs(conversation);
    std::vector<CompactionDrop> drops;

    // --- Phase 1: decide pair drops (never errors/patches/plan/shell/guards).
    std::set<std::size_t> droppedPairIndexes;
    const double tailStart = rt::Math::max(0.0, static_cast<double>(pairs.size()) - policy.keepRecentPairs);
    rt::OrderedMap<std::string, std::vector<std::size_t>> lastReadIndexByPath;
    for (std::size_t index = 0; index < pairs.size(); index += 1) {
      const ConversationPair& pair = pairs[index];
      const std::optional<std::string_view> path = rt::getString(pair.args, "path");
      if (pair.tool == "filesystem.read" && path) {
        std::vector<std::size_t> list;
        if (const std::vector<std::size_t>* existing = lastReadIndexByPath.get(*path)) list = *existing;
        list.push_back(index);
        lastReadIndexByPath.set(std::string(*path), std::move(list));
      }
    }
    for (std::size_t index = 0; index < pairs.size(); index += 1) {
      const ConversationPair& pair = pairs[index];
      if (isErrorPair(pair)) continue;
      if (pair.tool == "editor.apply_patch" || pair.tool == "task.update_plan" || pair.tool == "shell.exec") continue;
      if (static_cast<double>(index) >= tailStart) continue;
      const std::optional<std::string_view> path = rt::getString(pair.args, "path");
      if (pair.tool == "filesystem.read" && path) {
        const std::vector<std::size_t>* found = lastReadIndexByPath.get(*path);
        const std::vector<std::size_t> indexes = found ? *found : std::vector<std::size_t>{};
        const double position = static_cast<double>(std::find(indexes.begin(), indexes.end(), index) - indexes.begin());
        const double rankFromEnd = static_cast<double>(indexes.size()) - 1 - position;
        if (rankFromEnd >= policy.readsPerPath) {
          droppedPairIndexes.insert(index);
          drops.push_back(CompactionDrop{.kind = "stale_read", .detail = pair.tool + " " + std::string(*path)});
        }
        continue;
      }
      if (policy.dropOldExplore) {
        droppedPairIndexes.insert(index);
        drops.push_back(CompactionDrop{.kind = "old_explore", .detail = pair.tool + " " + rt::jsonStringify(json::value(pair.args))});
      }
    }

    // --- Phase 2: rebuild turns (drop whole pairs) and apply analysis window.
    const std::vector<ConversationTurn>& turns = conversation.turns;
    std::vector<ConversationTurn> kept;
    std::int64_t droppedTurns = 0;
    for (std::size_t i = 0; i < turns.size(); i++) {
      const std::int64_t turnIndex = static_cast<std::int64_t>(i);
      bool inDroppedPair = false;
      for (std::size_t pi = 0; pi < pairs.size(); pi += 1) {
        if (droppedPairIndexes.contains(pi) && (pairs[pi].assistantIndex == turnIndex || pairs[pi].resultIndex == turnIndex)) {
          inDroppedPair = true;
          break;
        }
      }
      if (inDroppedPair) {
        droppedTurns += 1;
        continue;
      }
      const ConversationTurn& turn = turns[i];
      const AssistantTurn* assistant = std::get_if<AssistantTurn>(&turn);
      if (assistant && !assistant->analysis.empty() && static_cast<double>(i) < static_cast<double>(turns.size()) - policy.analysisWindow) {
        AssistantTurn withoutAnalysis = *assistant;
        withoutAnalysis.analysis = {};
        kept.push_back(std::move(withoutAnalysis));
        drops.push_back(CompactionDrop{.kind = "analysis", .detail = "assistant turn " + std::to_string(i)});
        continue;
      }
      kept.push_back(turn);
    }
    conversation::Conversation compacted{.turns = std::move(kept)};

    const double estimatedTokens = estimateConversationTokens(compacted);
    return CompactionResult{
      .conversation = std::move(compacted),
      .drops = std::move(drops),
      .estimatedTokens = estimatedTokens,
      .metBudget = estimatedTokens <= policy.targetTokens,
      .droppedTurnCount = droppedTurns,
    };
  }

  // ---------------------------------------------------------------------------
  // Structured context rendering
  // ---------------------------------------------------------------------------

  std::string renderStructuredContext(const state::StructuredTaskState& state, const conversation::Conversation& conversation) {
    return renderStructuredContext(state, conversation, StructuredContextOptions{});
  }

  std::string renderStructuredContext(const state::StructuredTaskState& state, const conversation::Conversation& conversation, const StructuredContextOptions& opts) {
    std::vector<ConversationTurn> tail;
    const double maxTailTurns = opts.maxTailTurns.value_or(6);
    for (std::int64_t i = static_cast<std::int64_t>(conversation.turns.size()) - 1; i >= 0 && static_cast<double>(tail.size()) < maxTailTurns; i--) {
      const ConversationTurn& turn = conversation.turns[static_cast<std::size_t>(i)];
      if (isSystemish(turn)) continue;
      tail.insert(tail.begin(), turn);
    }
    const conversation::Conversation tailConversation{.turns = std::move(tail)};
    const std::vector<std::string> body{
      "[structured task state]",
      state::renderStateSummary(state),
      "",
      "[recent transcript]",
      rt::jsonStringify(messagesJson(conversation::wireMessagesFromConversation(tailConversation))),
    };
    return rt::join(body, "\n");
  }
}
