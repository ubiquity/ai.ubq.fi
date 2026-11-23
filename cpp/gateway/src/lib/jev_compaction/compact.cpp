// Port of lib/jev_compaction/compact.ts.
#include "uos/lib/jev_compaction/compact.hpp"

#include "uos/lib/jev_compaction/request.hpp"
#include "uos/lib/jev_compaction/state.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace uos::lib::jev_compaction::compact {
  using request::noulAnswer;
  using state::collectToolCalls;
  using state::estimateTokens;
  using state::fitState;
  using types::CallAction;
  using types::CallAnswer;
  using types::CallDecision;
  using types::CompactOptions;
  using types::CompactResult;
  using types::FittedState;
  using types::JevAsker;
  using types::JevQuestions;
  using types::JevResponse;
  using types::JevState;
  using types::Message;
  using types::NoulQuestion;
  using types::ResolvedCompactOptions;
  using types::ToolCall;
  using types::ToolResult;
  using types::ToolUse;

  namespace {
    using AnswerMap = rt::OrderedMap<std::string, CallAnswer>;

    ResolvedCompactOptions defaultOptions() {
      ResolvedCompactOptions options;
      options.goal = "";
      options.keepThreshold = 0.5;
      options.preserveRecentMessages = 6;
      options.maxStateTokens = 25000;
      options.maxRequestTokens = 30000;
      options.truncateHeadChars = 300;
      return options;
    }
    const ResolvedCompactOptions DEFAULT_OPTIONS = defaultOptions();

    // Tokens the request envelope (`model`, key names) adds around state and questions.
    constexpr double REQUEST_OVERHEAD_TOKENS = 20;

    // Windowed decisions: when the global state fit has to collapse or drop
    // context and the transcript carries enough calls to matter, calls are decided
    // per message window with a local state instead of one degraded global state.
    constexpr std::size_t WINDOW_MIN_CANDIDATES = 40;
    constexpr double WINDOW_TARGET_CHARS = 80000;
    constexpr std::size_t WINDOW_CONCURRENCY = 3;

    double finite(const std::optional<double>& value, double fallback) {
      return value && rt::Number::isFinite(*value) ? *value : fallback;
    }

    ResolvedCompactOptions resolveOptions(const CompactOptions& options) {
      ResolvedCompactOptions resolved;
      resolved.goal = options.goal.value_or(DEFAULT_OPTIONS.goal);
      resolved.keepThreshold = finite(options.keepThreshold, DEFAULT_OPTIONS.keepThreshold);
      resolved.preserveRecentMessages = rt::Math::max(0, rt::Math::floor(finite(options.preserveRecentMessages, DEFAULT_OPTIONS.preserveRecentMessages)));
      resolved.maxStateTokens = rt::Math::max(1, finite(options.maxStateTokens, DEFAULT_OPTIONS.maxStateTokens));
      resolved.maxRequestTokens = rt::Math::max(1, finite(options.maxRequestTokens, DEFAULT_OPTIONS.maxRequestTokens));
      resolved.truncateHeadChars = rt::Math::max(0, rt::Math::floor(finite(options.truncateHeadChars, DEFAULT_OPTIONS.truncateHeadChars)));
      return resolved;
    }

    // The two `noul` questions asked about one call: keep the call, keep its result.
    JevQuestions questionsFor(const ToolCall& call) {
      JevQuestions questions;
      NoulQuestion keepCall;
      keepCall.instructions = "Tool call " + call.id + " (" + call.tool +
                              ") should stay in the history: knowing this call was made, with its input, still matters for what the assistant does next";
      questions.set("call_" + call.id, std::move(keepCall));
      NoulQuestion keepResult;
      keepResult.instructions = "The full output of tool call " + call.id + " (" + call.tool + ", " + std::to_string(call.resultChars) +
                                " chars) should stay in the history verbatim: the assistant still needs its contents and re-running the tool would not do";
      questions.set("result_" + call.id, std::move(keepResult));
      return questions;
    }

    // Splits the candidate calls into batches whose questions, together with the
    // (always complete) state, fit one request.
    std::vector<std::vector<ToolCall>> batchCalls(const std::vector<ToolCall>& calls, double stateTokens, const ResolvedCompactOptions& options) {
      const double budget = options.maxRequestTokens - stateTokens - REQUEST_OVERHEAD_TOKENS;
      std::vector<std::vector<ToolCall>> batches;
      std::vector<ToolCall> current;
      double currentTokens = 0;
      for (const ToolCall& call : calls) {
        const double tokens = estimateTokens(rt::jsonStringify(types::toJson(questionsFor(call))));
        if (!current.empty() && currentTokens + tokens > budget) {
          batches.push_back(std::move(current));
          current = {};
          currentTokens = 0;
        }
        if (current.empty() && tokens > budget) {
          throw rt::JsError("state leaves no room for questions (~" + rt::numberToString(stateTokens) + " of " +
                            rt::numberToString(options.maxRequestTokens) + " tokens)");
        }
        current.push_back(call);
        currentTokens += tokens;
      }
      if (!current.empty()) batches.push_back(std::move(current));
      return batches;
    }

    CallDecision decideCall(const ToolCall& call, const CallAnswer& answer, const ResolvedCompactOptions& options) {
      CallDecision decision{call.id, call.tool, answer.keepCall, answer.keepResult, "", ""};
      if (call.pinned) {
        decision.action = "keep";
        decision.reason = "pinned";
      } else if (answer.keepResult >= options.keepThreshold) {
        decision.action = "keep";
        decision.reason = "kept";
      } else if (answer.keepCall >= options.keepThreshold) {
        decision.action = "drop_result";
        decision.reason = "result_dropped";
      } else {
        decision.action = "drop_call";
        decision.reason = "call_dropped";
      }
      return decision;
    }

    rt::Task<AnswerMap> askBatch(std::shared_ptr<JevAsker> asker, JevState state, std::vector<ToolCall> batch) {
      JevQuestions questions;
      for (const ToolCall& call : batch) {
        for (const auto& [name, question] : questionsFor(call)) questions.set(name, question);
      }
      rt::Promise<JevResponse> asking(asker->ask(std::move(state), std::move(questions)));
      const JevResponse response = co_await asking.wait();
      const json::value* answers = rt::get(response, "answers");
      AnswerMap answered;
      for (const ToolCall& call : batch) {
        const double keepCall = noulAnswer(answers, "call_" + call.id);
        const double keepResult = noulAnswer(answers, "result_" + call.id);
        answered.set(call.id, CallAnswer{keepCall, keepResult});
      }
      co_return answered;
    }
  }

  std::string truncatedResultText(std::string_view text, bool isError, double headChars) {
    const auto length = static_cast<double>(rt::length(text));
    if (length <= headChars + 120) return std::string(text);
    const std::string head = headChars > 0 ? rt::slice(text, 0, headChars) + "\n" : "";
    return head + "[fast-jev-compaction truncated " + rt::numberToString(length - headChars) + " chars of this tool result" + (isError ? " (error)" : "") +
           "; re-run the tool if needed]";
  }

  namespace {
    // Rebuilds the conversation from the decisions. A dropped call disappears
    // together with its result; a dropped result keeps a bounded head and note.
    // Messages that lose all their content are removed; untouched messages are
    // returned as copies of the messages they came in as.
    std::vector<Message> applyDecisions(const std::vector<Message>& messages, const std::vector<CallDecision>& decisions, const std::vector<ToolCall>& calls,
                                        double headChars) {
      std::unordered_map<std::string, const ToolCall*> byId;
      for (const ToolCall& call : calls) byId.insert_or_assign(call.id, &call);
      std::unordered_map<std::string, CallAction> actions;
      for (const CallDecision& decision : decisions) {
        const auto call = byId.find(decision.id);
        if (call != byId.end() && decision.action != "keep") actions.insert_or_assign(call->second->tool_use_id, decision.action);
      }
      const auto actionOf = [&](const std::string& id) -> const CallAction* {
        const auto found = actions.find(id);
        return found == actions.end() ? nullptr : &found->second;
      };
      const auto is = [&](const std::string& id, std::string_view action) {
        const CallAction* found = actionOf(id);
        return found != nullptr && *found == action;
      };
      const std::vector<ToolResult> noResults;
      std::vector<Message> kept;
      for (const Message& message : messages) {
        const std::vector<ToolResult>& results = message.toolResults ? *message.toolResults : noResults;
        const bool touched = std::any_of(message.toolUses.begin(), message.toolUses.end(), [&](const ToolUse& tool) { return actionOf(tool.tool_use_id) != nullptr; }) ||
                             std::any_of(results.begin(), results.end(), [&](const ToolResult& result) { return actionOf(result.tool_use_id) != nullptr; });
        if (!touched) {
          kept.push_back(message);
          continue;
        }
        // Each mapped element and whether it is the input object itself (`===`).
        std::vector<std::pair<ToolUse, bool>> toolUses;
        for (const ToolUse& tool : message.toolUses) {
          if (is(tool.tool_use_id, "drop_call")) continue;
          if (!is(tool.tool_use_id, "drop_result")) {
            toolUses.emplace_back(tool, true);
            continue;
          }
          const std::string text = truncatedResultText(tool.text.value_or(""), tool.isError.value_or(false), headChars);
          if (tool.text.value_or("") == text) {
            toolUses.emplace_back(tool, true);
            continue;
          }
          ToolUse copy{tool.tool_use_id, tool.tool, tool.input, text, std::nullopt};
          if (tool.isError.value_or(false)) copy.isError = true;
          toolUses.emplace_back(std::move(copy), false);
        }
        std::vector<std::pair<ToolResult, bool>> toolResults;
        for (const ToolResult& result : results) {
          if (is(result.tool_use_id, "drop_call")) continue;
          if (!is(result.tool_use_id, "drop_result")) {
            toolResults.emplace_back(result, true);
            continue;
          }
          const std::string text = truncatedResultText(result.text, result.isError.value_or(false), headChars);
          if (text == result.text) toolResults.emplace_back(result, true);
          else toolResults.emplace_back(ToolResult{result.tool_use_id, text, result.isError}, false);
        }
        const bool dropsCall = std::any_of(message.toolUses.begin(), message.toolUses.end(), [&](const ToolUse& tool) { return is(tool.tool_use_id, "drop_call"); }) ||
                               std::any_of(results.begin(), results.end(), [&](const ToolResult& result) { return is(result.tool_use_id, "drop_call"); });
        const bool sameUses = std::all_of(toolUses.begin(), toolUses.end(), [](const auto& entry) { return entry.second; });
        const bool sameResults = std::all_of(toolResults.begin(), toolResults.end(), [](const auto& entry) { return entry.second; });
        if (!dropsCall && sameUses && sameResults) {
          kept.push_back(message);
          continue;
        }
        if (rt::trim(message.text).empty() && toolUses.empty() && toolResults.empty()) {
          continue;
        }
        Message rebuilt{message.role, message.text, {}, std::nullopt};
        for (auto& [tool, same] : toolUses) rebuilt.toolUses.push_back(std::move(tool));
        if (!toolResults.empty()) {
          rebuilt.toolResults.emplace();
          for (auto& [result, same] : toolResults) rebuilt.toolResults->push_back(std::move(result));
        }
        kept.push_back(std::move(rebuilt));
      }
      return kept;
    }

    // Characters of text, tool input and tool output a message holds.
    std::int64_t messageChars(const Message& message) {
      std::int64_t total = rt::length(message.text);
      for (const ToolUse& tool : message.toolUses) {
        try {
          total += rt::length(rt::jsonStringify(json::value(tool.input)));
        } catch (const rt::JsError&) {
          total += 20;
        }
      }
      if (message.toolResults) {
        for (const ToolResult& result : *message.toolResults) total += rt::length(result.text);
      }
      return total;
    }

    std::int64_t count(const std::vector<CallDecision>& decisions, std::string_view reason) {
      return static_cast<std::int64_t>(std::count_if(decisions.begin(), decisions.end(), [&](const CallDecision& decision) { return decision.reason == reason; }));
    }

    bool isDegradedStage(std::string_view stage) { return stage != "full" && !rt::startsWith(stage, "inputs<="); }
  }

  std::vector<Window> planWindows(const std::vector<Message>& messages, const std::vector<ToolCall>& calls, double maxWindowChars) {
    std::unordered_map<std::int64_t, std::int64_t> requiredEnd;
    for (const ToolCall& call : calls) {
      const std::int64_t lo = std::min(call.callIndex, call.resultIndex);
      const std::int64_t hi = std::max(call.callIndex, call.resultIndex);
      const auto existing = requiredEnd.find(lo);
      if (existing == requiredEnd.end() || existing->second < hi) requiredEnd.insert_or_assign(lo, hi);
    }
    std::vector<Window> windows;
    const auto total = static_cast<std::int64_t>(messages.size());
    std::int64_t start = 0;
    std::int64_t chars = 0;
    for (std::int64_t index = 0; index < total; index += 1) {
      const std::int64_t size = messageChars(messages[static_cast<std::size_t>(index)]);
      if (index > start && static_cast<double>(chars + size) > maxWindowChars) {
        std::int64_t end = index;
        for (std::int64_t cursor = start; cursor < end; cursor += 1) {
          const auto required = requiredEnd.find(cursor);
          if (required != requiredEnd.end()) end = std::max(end, required->second + 1);
        }
        windows.push_back(Window{start, end});
        start = end;
        chars = 0;
        index = std::max(index, end - 1);
        continue;
      }
      chars += size;
    }
    if (start < total) windows.push_back(Window{start, total});
    return windows;
  }

  namespace {
    struct WindowTask {
      std::vector<Message> messages;
      std::vector<ToolCall> calls;
      std::vector<ToolCall> candidates;
    };

    // What askWindowed resolves with.
    struct Windowed {
      AnswerMap answers;
      std::int64_t windows = 0;
      std::vector<std::string> stages;
      double maxStateTokens = 0;
      std::int64_t requests = 0;
    };

    // The variables the workers share. Workers still running after another one
    // failed keep them alive, as their JS closures do.
    struct WindowWork {
      std::shared_ptr<JevAsker> asker;
      ResolvedCompactOptions options;
      std::vector<WindowTask> tasks;
      AnswerMap answers;
      std::vector<std::string> stages;
      double maxStateTokens = 0;
      std::int64_t requests = 0;
      std::size_t cursor = 0;
    };

    rt::Task<void> worker(std::shared_ptr<WindowWork> work) {
      while (true) {
        const std::size_t index = work->cursor;
        work->cursor += 1;
        if (index >= work->tasks.size()) co_return;
        const WindowTask& task = work->tasks[index];
        ResolvedCompactOptions localOptions;
        localOptions.maxStateTokens = work->options.maxStateTokens;
        localOptions.preserveRecentMessages = 0;
        localOptions.goal = work->options.goal;
        const FittedState local = fitState(task.messages, task.calls, localOptions);
        work->stages[index] = local.stage;
        work->maxStateTokens = rt::Math::max(work->maxStateTokens, local.tokens);
        const std::vector<std::vector<ToolCall>> batches = batchCalls(task.candidates, local.tokens, work->options);
        const JevState localState = types::toJson(local.state);
        for (const std::vector<ToolCall>& batch : batches) {
          rt::Promise<AnswerMap> asking(askBatch(work->asker, localState, batch));
          const AnswerMap answered = co_await asking.wait();
          for (const auto& [id, answer] : answered) work->answers.set(id, answer);
          work->requests += 1;
        }
      }
    }

    rt::Task<Windowed> askWindowed(std::shared_ptr<JevAsker> asker, std::vector<Message> messages, std::vector<ToolCall> calls, std::vector<ToolCall> candidates,
                                   ResolvedCompactOptions options, std::vector<Window> plan) {
      std::unordered_set<std::string> candidateIds;
      for (const ToolCall& call : candidates) candidateIds.insert(call.id);
      auto work = std::make_shared<WindowWork>();
      work->asker = std::move(asker);
      work->options = std::move(options);
      const auto total = static_cast<std::int64_t>(messages.size());
      for (const Window& window : plan) {
        WindowTask task;
        for (const ToolCall& call : calls) {
          if (call.callIndex >= window.start && call.callIndex < window.end && call.resultIndex < window.end) task.calls.push_back(call);
        }
        const std::int64_t from = std::clamp<std::int64_t>(window.start, 0, total);
        const std::int64_t to = std::clamp<std::int64_t>(window.end, from, total);
        task.messages.assign(messages.begin() + from, messages.begin() + to);
        for (const ToolCall& call : task.calls) {
          if (candidateIds.count(call.id) != 0) task.candidates.push_back(call);
        }
        if (!task.candidates.empty()) work->tasks.push_back(std::move(task));
      }
      work->stages.resize(work->tasks.size());
      const std::size_t workerCount = std::max<std::size_t>(1, std::min(WINDOW_CONCURRENCY, work->tasks.size()));
      std::vector<rt::Promise<void>> workers;
      for (std::size_t started = 0; started < workerCount; ++started) workers.push_back(rt::Promise<void>(worker(work)));
      co_await rt::all(std::move(workers)).wait();
      Windowed windowed;
      windowed.answers = work->answers;
      windowed.windows = static_cast<std::int64_t>(work->tasks.size());
      windowed.stages = work->stages;
      windowed.maxStateTokens = work->maxStateTokens;
      windowed.requests = work->requests;
      co_return windowed;
    }

    std::string join(const std::vector<std::string>& parts, std::string_view separator) {
      std::string out;
      for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) out += separator;
        out += parts[index];
      }
      return out;
    }
  }

  rt::Task<CompactResult> compact(std::vector<Message> messages, std::shared_ptr<JevAsker> asker, CompactOptions options) {
    const double started = rt::dateNow();
    const ResolvedCompactOptions resolved = resolveOptions(options);
    const std::vector<ToolCall> calls = collectToolCalls(messages, resolved.preserveRecentMessages);
    std::vector<ToolCall> candidates;
    for (const ToolCall& call : calls) {
      if (!call.pinned) candidates.push_back(call);
    }
    std::int64_t charsBefore = 0;
    for (const Message& message : messages) charsBefore += messageChars(message);

    double fittedTokens = 0;
    std::string fittedStage;
    std::vector<std::vector<ToolCall>> batches;
    std::int64_t windows = 0;
    AnswerMap answers;
    if (!candidates.empty()) {
      const FittedState state = fitState(messages, calls, resolved);
      const std::vector<Window> plan = planWindows(messages, calls, WINDOW_TARGET_CHARS);
      if (candidates.size() >= WINDOW_MIN_CANDIDATES && isDegradedStage(state.stage) && plan.size() > 1) {
        rt::Promise<Windowed> asking(askWindowed(asker, messages, calls, candidates, resolved, plan));
        const Windowed windowed = co_await asking.wait();
        fittedTokens = windowed.maxStateTokens;
        fittedStage = rt::slice("windowed x" + std::to_string(windowed.windows) + " [" + join(windowed.stages, ", ") + "]", 0, 200);
        batches.assign(static_cast<std::size_t>(windowed.requests), {});
        windows = windowed.windows;
        for (const auto& [id, answer] : windowed.answers) answers.set(id, answer);
      } else {
        fittedTokens = state.tokens;
        fittedStage = state.stage;
        batches = batchCalls(candidates, state.tokens, resolved);
        const JevState shared = types::toJson(state.state);
        std::vector<rt::Promise<AnswerMap>> asking;
        for (const std::vector<ToolCall>& batch : batches) asking.push_back(rt::Promise<AnswerMap>(askBatch(asker, shared, batch)));
        const std::vector<AnswerMap> answered = co_await rt::all(std::move(asking)).wait();
        for (const AnswerMap& map : answered) {
          for (const auto& [id, answer] : map) answers.set(id, answer);
        }
      }
    }

    std::vector<CallDecision> decisions;
    for (const ToolCall& call : calls) {
      const CallAnswer* found = answers.get(call.id);
      const CallAnswer answer = found != nullptr ? *found : CallAnswer{1, 1};
      decisions.push_back(decideCall(call, answer, resolved));
    }
    std::vector<Message> kept = applyDecisions(messages, decisions, calls, resolved.truncateHeadChars);
    CompactResult result;
    result.stats.messagesBefore = static_cast<std::int64_t>(messages.size());
    result.stats.messagesAfter = static_cast<std::int64_t>(kept.size());
    result.stats.charsBefore = charsBefore;
    std::int64_t charsAfter = 0;
    for (const Message& message : kept) charsAfter += messageChars(message);
    result.stats.charsAfter = charsAfter;
    result.stats.calls = static_cast<std::int64_t>(calls.size());
    result.stats.kept = count(decisions, "kept");
    result.stats.resultsDropped = count(decisions, "result_dropped");
    result.stats.callsDropped = count(decisions, "call_dropped");
    result.stats.pinned = count(decisions, "pinned");
    result.stats.stateTokens = fittedTokens;
    result.stats.stateStage = fittedStage;
    result.stats.windows = windows;
    result.stats.requests = static_cast<std::int64_t>(batches.size());
    result.stats.ms = rt::dateNow() - started;
    result.messages = std::move(kept);
    result.decisions = std::move(decisions);
    co_return result;
  }
}
