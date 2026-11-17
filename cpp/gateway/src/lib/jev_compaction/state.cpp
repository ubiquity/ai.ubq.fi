// Port of lib/jev_compaction/state.ts.
#include "uos/lib/jev_compaction/state.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace uos::lib::jev_compaction::state {
  using types::CompactionState;
  using types::FittedState;
  using types::HistoryEntry;
  using types::HistoryToolCall;
  using types::Message;
  using types::ResolvedCompactOptions;
  using types::ToolCall;
  using types::ToolResult;

  namespace {
    // Successive caps on the serialised tool input included per call.
    constexpr std::array<double, 3> INPUT_CHARS = {1000, 200, 60};
    constexpr double TEXT_HEAD = 400;
    constexpr double TEXT_TAIL = 150;

    const rt::Regex& TOKEN_PIECES() {
      static const rt::Regex regex("[A-Za-z]+|\\d+|[^\\sA-Za-z\\d]", "g");
      return regex;
    }

    const rt::Regex& WHITESPACE_RUNS() {
      static const rt::Regex regex("\\s+", "g");
      return regex;
    }

    std::string join(const std::vector<std::string>& parts, std::string_view separator) {
      std::string out;
      for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) out += separator;
        out += parts[index];
      }
      return out;
    }

    std::string truncate(std::string_view text, double limit) {
      return static_cast<double>(rt::length(text)) <= limit ? std::string(text) : rt::slice(text, 0, rt::Math::max(0, limit - 1)) + "…";
    }

    std::string abridge(std::string_view text, double head, double tail) {
      const auto length = static_cast<double>(rt::length(text));
      if (length <= head + tail + 40) return std::string(text);
      const double omitted = length - head - tail;
      return rt::slice(text, 0, head) + "\n[… " + rt::numberToString(omitted) + " chars omitted …]\n" + rt::slice(text, -tail);
    }

    bool isPinned(std::int64_t index, std::size_t total, double preserveRecentMessages) {
      return index == 0 || static_cast<double>(index) >= static_cast<double>(total) - preserveRecentMessages;
    }

    std::string inputText(const json::object& input, double limit) {
      std::string json;
      try {
        json = rt::jsonStringify(json::value(input));
      } catch (const rt::JsError&) {
        json = "[unserializable input]";
      }
      return truncate(json, limit);
    }

    std::string resultNote(const ToolCall& call) {
      return std::string(call.isError ? "error" : "ok") + ", " + std::to_string(call.resultChars) + " chars (omitted)";
    }

    // One call as a single line, for when the structured form is too costly.
    std::string compactCall(const ToolCall& call) {
      std::vector<std::string> pairs;
      for (const json::key_value_pair* member : rt::objectEntries(call.input)) {
        const std::string key(member->key());
        const json::value& value = member->value();
        std::string text;
        if (value.is_string()) {
          text = std::string(value.as_string());
        } else {
          json::object single;
          rt::objectDefine(single, key, value);
          text = inputText(single, 200);
        }
        pairs.push_back(key + "=" + rt::replace(text, WHITESPACE_RUNS(), " "));
      }
      const std::string input = join(pairs, " ");
      return call.id + " " + call.tool + " " + truncate(input, INPUT_CHARS[2]) + " → " + (call.isError ? "error" : "ok") + " " +
             std::to_string(call.resultChars) + "ch";
    }

    bool hasCallLines(const HistoryEntry& entry) {
      if (!entry.tool_calls) return false;
      const auto* lines = std::get_if<std::vector<std::string>>(&*entry.tool_calls);
      return lines != nullptr && !lines->empty();
    }

    // Folds runs of adjacent call-only entries into one entry each, so the
    // per-entry envelope is paid once per run; the call lines keep their ids.
    std::vector<HistoryEntry> mergeCallRuns(const std::vector<HistoryEntry>& history, const std::function<bool(const HistoryEntry&)>& pinned) {
      std::vector<HistoryEntry> merged;
      for (const HistoryEntry& entry : history) {
        HistoryEntry* previous = merged.empty() ? nullptr : &merged.back();
        const auto foldable = [&](const HistoryEntry& e) { return !pinned(e) && rt::length(e.text) == 0 && hasCallLines(e); };
        if (previous && foldable(*previous) && foldable(entry) && previous->role == entry.role) {
          std::vector<std::string> lines = std::get<std::vector<std::string>>(*previous->tool_calls);
          for (const std::string& line : std::get<std::vector<std::string>>(*entry.tool_calls)) lines.push_back(line);
          previous->tool_calls = std::move(lines);
          continue;
        }
        merged.push_back(entry);
      }
      return merged;
    }

    std::unordered_map<std::int64_t, std::vector<ToolCall>> callsByMessage(const std::vector<ToolCall>& calls) {
      std::unordered_map<std::int64_t, std::vector<ToolCall>> byMessage;
      for (const ToolCall& call : calls) byMessage[call.callIndex].push_back(call);
      return byMessage;
    }

    std::vector<HistoryEntry> historyEntries(const std::vector<Message>& messages, const std::vector<ToolCall>& calls, double inputChars) {
      const auto byMessage = callsByMessage(calls);
      std::vector<HistoryEntry> entries;
      for (std::size_t index = 0; index < messages.size(); ++index) {
        const Message& message = messages[index];
        const auto i = static_cast<std::int64_t>(index);
        std::vector<HistoryToolCall> toolCalls;
        if (const auto found = byMessage.find(i); found != byMessage.end()) {
          for (const ToolCall& call : found->second) toolCalls.push_back(HistoryToolCall{call.id, call.tool, inputText(call.input, inputChars), resultNote(call)});
        }
        if (rt::trim(message.text).empty() && toolCalls.empty()) continue;
        HistoryEntry entry{i, message.role, message.text, std::nullopt};
        if (!toolCalls.empty()) entry.tool_calls = std::move(toolCalls);
        entries.push_back(std::move(entry));
      }
      return entries;
    }

    // The last three user prompts, as the default `goal`.
    std::string goalFromMessages(const std::vector<Message>& messages) {
      std::vector<const Message*> prompts;
      for (const Message& message : messages) {
        const bool noResults = !message.toolResults || message.toolResults->empty();
        if (message.role == "user" && !rt::trim(message.text).empty() && noResults) prompts.push_back(&message);
      }
      const std::size_t first = prompts.size() > 3 ? prompts.size() - 3 : 0;
      std::vector<std::string> goals;
      for (std::size_t index = first; index < prompts.size(); ++index) goals.push_back(truncate(prompts[index]->text, 500));
      return join(goals, "\n");
    }
  }

  double estimateTokens(std::string_view text) {
    double tokens = 0;
    for (const rt::RegexMatch& match : rt::matchAll(text, TOKEN_PIECES())) {
      const auto first = static_cast<unsigned char>(match.str().front());
      const auto length = static_cast<double>(match.endIndex() - match.index);
      if (first >= 48 && first <= 57) tokens += length / 2;
      else if ((first >= 65 && first <= 90) || (first >= 97 && first <= 122)) {
        tokens += 1 + rt::Math::floor((length - 1) / 6);
      } else tokens += 0.9;
    }
    return rt::Math::ceil(tokens);
  }

  std::vector<ToolCall> collectToolCalls(const std::vector<Message>& messages, double preserveRecentMessages) {
    struct Found {
      std::int64_t index;
      const ToolResult* result;
    };
    std::unordered_map<std::string, Found> results;
    for (std::size_t index = 0; index < messages.size(); ++index) {
      if (!messages[index].toolResults) continue;
      for (const ToolResult& result : *messages[index].toolResults) results.insert_or_assign(result.tool_use_id, Found{static_cast<std::int64_t>(index), &result});
    }
    std::vector<ToolCall> calls;
    for (std::size_t callIndex = 0; callIndex < messages.size(); ++callIndex) {
      for (const types::ToolUse& tool : messages[callIndex].toolUses) {
        const auto found = results.find(tool.tool_use_id);
        if (found == results.end()) continue;
        const auto index = static_cast<std::int64_t>(callIndex);
        ToolCall call;
        call.id = "t" + std::to_string(calls.size() + 1);
        call.tool_use_id = tool.tool_use_id;
        call.tool = tool.tool;
        call.input = tool.input;
        call.callIndex = index;
        call.resultIndex = found->second.index;
        call.resultChars = rt::length(found->second.result->text);
        call.isError = found->second.result->isError.value_or(false);
        call.pinned = isPinned(index, messages.size(), preserveRecentMessages) || isPinned(found->second.index, messages.size(), preserveRecentMessages);
        calls.push_back(std::move(call));
      }
    }
    return calls;
  }

  FittedState fitState(const std::vector<Message>& messages, const std::vector<ToolCall>& calls, const ResolvedCompactOptions& options) {
    const std::string goal = rt::truthy(std::string_view(options.goal)) ? options.goal : goalFromMessages(messages);
    const auto stateOf = [&](std::vector<HistoryEntry> history) -> CompactionState { return CompactionState{std::string(STATE_CONTEXT), goal, std::move(history)}; };
    const auto entryTokens = [](const HistoryEntry& entry) -> double { return estimateTokens(rt::jsonStringify(types::toJson(entry))) + 1; };
    const double baseTokens = estimateTokens(rt::jsonStringify(types::toJson(stateOf({}))));
    const auto fitted = [&](std::vector<HistoryEntry> history, double tokens, std::string stage) -> FittedState {
      return FittedState{stateOf(std::move(history)), tokens, std::move(stage)};
    };

    std::vector<HistoryEntry> history;
    std::vector<double> perEntry;
    double tokens = 0;
    const auto sumOf = [](const std::vector<double>& values) {
      double sum = 0;
      for (const double n : values) sum = sum + n;
      return sum;
    };
    const auto rebuild = [&](double inputChars) {
      history = historyEntries(messages, calls, inputChars);
      perEntry.clear();
      for (const HistoryEntry& entry : history) perEntry.push_back(entryTokens(entry));
      tokens = baseTokens + sumOf(perEntry);
    };
    const auto fits = [&]() { return tokens <= options.maxStateTokens; };
    const auto shrink = [&](std::size_t index, const std::function<void(HistoryEntry&)>& change) {
      if (index >= history.size()) return;
      HistoryEntry& entry = history[index];
      change(entry);
      const double now = entryTokens(entry);
      const double previousTokens = index < perEntry.size() ? perEntry[index] : 0;
      tokens += now - previousTokens;
      perEntry[index] = now;
    };

    rebuild(INPUT_CHARS[0]);
    if (fits()) return fitted(history, tokens, "full");

    for (std::size_t stage = 1; stage < INPUT_CHARS.size(); ++stage) {
      const double limit = INPUT_CHARS[stage];
      rebuild(limit);
      if (fits()) return fitted(history, tokens, "inputs<=" + rt::numberToString(limit));
    }

    const auto pinned = [&](const HistoryEntry& entry) { return isPinned(entry.i, messages.size(), options.preserveRecentMessages); };
    std::vector<std::size_t> unpinnedFirst;
    std::vector<std::size_t> pinnedLast;
    for (std::size_t index = 0; index < history.size(); ++index) {
      if (pinned(history[index])) pinnedLast.push_back(index);
      else unpinnedFirst.push_back(index);
    }
    std::vector<std::size_t> order = unpinnedFirst;
    order.insert(order.end(), pinnedLast.begin(), pinnedLast.end());

    for (const std::size_t index : order) {
      if (index >= history.size()) continue;
      if (static_cast<double>(rt::length(history[index].text)) <= TEXT_HEAD + TEXT_TAIL + 40) continue;
      shrink(index, [](HistoryEntry& e) { e.text = abridge(e.text, TEXT_HEAD, TEXT_TAIL); });
      if (fits()) return fitted(history, tokens, "texts abridged");
    }

    for (const std::size_t index : order) {
      if (index >= history.size()) continue;
      const HistoryEntry& entry = history[index];
      if (pinned(entry) || rt::length(entry.text) == 0) continue;
      const auto messageIndex = static_cast<std::size_t>(entry.i);
      const std::int64_t original = messageIndex < messages.size() ? rt::length(messages[messageIndex].text) : rt::length(entry.text);
      shrink(index, [original](HistoryEntry& e) { e.text = "[… " + std::to_string(original) + " chars omitted …]"; });
      if (fits()) return fitted(history, tokens, "old messages collapsed");
    }

    const auto byMessage = callsByMessage(calls);
    for (const std::size_t index : order) {
      if (index >= history.size()) continue;
      const HistoryEntry& entry = history[index];
      const auto own = byMessage.find(entry.i);
      if (pinned(entry) || own == byMessage.end()) continue;
      const std::vector<ToolCall>& ownCalls = own->second;
      shrink(index, [&ownCalls](HistoryEntry& e) {
        std::vector<std::string> lines;
        for (const ToolCall& call : ownCalls) lines.push_back(compactCall(call));
        e.tool_calls = std::move(lines);
      });
      if (fits()) return fitted(history, tokens, "old calls compacted");
    }

    rt::OrderedSet<std::size_t> left;
    const auto remaining = [&]() {
      std::vector<HistoryEntry> kept;
      for (std::size_t index = 0; index < history.size(); ++index) {
        if (!left.has(index)) kept.push_back(history[index]);
      }
      return kept;
    };
    for (const std::size_t index : order) {
      if (index >= history.size()) continue;
      const HistoryEntry& entry = history[index];
      if (pinned(entry) || entry.tool_calls) continue;
      left.add(index);
      const double removedTokens = index < perEntry.size() ? perEntry[index] : 0;
      tokens -= removedTokens;
      if (fits()) {
        return fitted(remaining(), tokens, "old messages left out");
      }
    }

    history = mergeCallRuns(remaining(), pinned);
    perEntry.clear();
    for (const HistoryEntry& entry : history) perEntry.push_back(entryTokens(entry));
    tokens = baseTokens + sumOf(perEntry);
    if (fits()) return fitted(history, tokens, "old calls merged");

    // Last resort for very long sessions: drop the oldest merged call runs while
    // keeping pinned entries. The per-call questions carry each candidate on
    // their own, so old calls can still be decided without their collapsed
    // context, and the state stays inside the cap at any session length.
    for (std::int64_t index = 0; index < static_cast<std::int64_t>(history.size()); index += 1) {
      const auto position = static_cast<std::size_t>(index);
      if (pinned(history[position])) continue;
      tokens -= position < perEntry.size() ? perEntry[position] : 0;
      history.erase(history.begin() + index);
      if (position < perEntry.size()) perEntry.erase(perEntry.begin() + index);
      index -= 1;
      if (fits()) return fitted(history, tokens, "old call runs left out");
    }

    throw rt::JsError("history too large for Jev (~" + rt::numberToString(tokens) + " tokens after truncation, limit " +
                      rt::numberToString(options.maxStateTokens) + ")");
  }
}
