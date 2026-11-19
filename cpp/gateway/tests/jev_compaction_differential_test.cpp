// Differential cases for lib/jev_compaction (request.ts and client.ts have no bun tests;
// state.ts, compact.ts and codex_items.ts are covered beyond the ported tests). The
// vectors were recorded by running the TypeScript modules under Bun 1.4.0.
#include "uos/lib/jev_compaction/client.hpp"
#include "uos/lib/jev_compaction/codex_items.hpp"
#include "uos/lib/jev_compaction/compact.hpp"
#include "uos/lib/jev_compaction/request.hpp"
#include "uos/lib/jev_compaction/state.hpp"
#include "uos/lib/jev_compaction/types.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace uos;
namespace jc = uos::lib::jev_compaction;
using jc::types::CallDecision;
using jc::types::CompactResult;
using jc::types::JevAsker;
using jc::types::JevQuestions;
using jc::types::JevResponse;
using jc::types::JevState;
using jc::types::Message;
using jc::types::ToolCall;
using jc::types::ToolResult;
using jc::types::ToolUse;

namespace {
  const char* const VECTOR_TEXT =
#include "jev_compaction_differential_vectors.inc"
    ;

  const json::object& vectors() {
    static const json::object parsed = rt::jsonParse(VECTOR_TEXT).as_object();
    return parsed;
  }
  const json::array& section(std::string_view name) { return *rt::getArray(vectors(), name); }
  const json::object& asObject(const json::value& value) { return value.as_object(); }
  std::string text(const json::value* value) { return std::string(*rt::getString(value)); }
  double number(const json::value* value) { return *rt::getNumber(value); }

  void expectJson(const json::value& actual, const json::value& expected, const std::string& label) {
    if (!rt::deepEqual(actual, expected)) {
      ::uos::rt::test::report(__FILE__, __LINE__, label + "\n      actual:   " + rt::jsonStringify(actual) + "\n      expected: " + rt::jsonStringify(expected), false);
    }
  }

  std::string sha256(std::string_view data) { return rt::crypto::createHash("sha256").update(data).digest("hex"); }

  // ---- JSON <-> library objects, in the key order the TS objects have

  ToolUse toolUseFromJson(const json::object& object) {
    ToolUse tool;
    tool.tool_use_id = text(rt::get(object, "tool_use_id"));
    tool.tool = text(rt::get(object, "tool"));
    tool.input = *rt::getObject(object, "input");
    if (const auto value = rt::getString(object, "text")) tool.text = std::string(*value);
    if (const auto value = rt::getBool(object, "isError")) tool.isError = *value;
    return tool;
  }

  ToolResult toolResultFromJson(const json::object& object) {
    ToolResult result;
    result.tool_use_id = text(rt::get(object, "tool_use_id"));
    result.text = text(rt::get(object, "text"));
    if (const auto value = rt::getBool(object, "isError")) result.isError = *value;
    return result;
  }

  Message messageFromJson(const json::value& value) {
    const json::object& object = value.as_object();
    Message message;
    message.role = text(rt::get(object, "role"));
    message.text = text(rt::get(object, "text"));
    for (const json::value& tool : *rt::getArray(object, "toolUses")) message.toolUses.push_back(toolUseFromJson(tool.as_object()));
    if (const json::array* results = rt::getArray(object, "toolResults")) {
      message.toolResults.emplace();
      for (const json::value& result : *results) message.toolResults->push_back(toolResultFromJson(result.as_object()));
    }
    return message;
  }

  std::vector<Message> messagesFromJson(const json::value& value) {
    std::vector<Message> messages;
    for (const json::value& message : value.as_array()) messages.push_back(messageFromJson(message));
    return messages;
  }

  json::value toJson(const ToolUse& tool) {
    json::object out;
    rt::objectDefine(out, "tool_use_id", json::value(tool.tool_use_id));
    rt::objectDefine(out, "tool", json::value(tool.tool));
    rt::objectDefine(out, "input", json::value(tool.input));
    if (tool.text) rt::objectDefine(out, "text", json::value(*tool.text));
    if (tool.isError) rt::objectDefine(out, "isError", json::value(*tool.isError));
    return out;
  }

  json::value toJson(const ToolResult& result) {
    json::object out;
    rt::objectDefine(out, "tool_use_id", json::value(result.tool_use_id));
    rt::objectDefine(out, "text", json::value(result.text));
    if (result.isError) rt::objectDefine(out, "isError", json::value(*result.isError));
    return out;
  }

  json::value toJson(const Message& message) {
    json::object out;
    rt::objectDefine(out, "role", json::value(message.role));
    rt::objectDefine(out, "text", json::value(message.text));
    json::array tools;
    for (const ToolUse& tool : message.toolUses) tools.push_back(toJson(tool));
    rt::objectDefine(out, "toolUses", json::value(std::move(tools)));
    if (message.toolResults) {
      json::array results;
      for (const ToolResult& result : *message.toolResults) results.push_back(toJson(result));
      rt::objectDefine(out, "toolResults", json::value(std::move(results)));
    }
    return out;
  }

  json::value toJson(const std::vector<Message>& messages) {
    json::array out;
    for (const Message& message : messages) out.push_back(toJson(message));
    return out;
  }

  json::value toJson(const ToolCall& call) {
    json::object out;
    rt::objectDefine(out, "id", json::value(call.id));
    rt::objectDefine(out, "tool_use_id", json::value(call.tool_use_id));
    rt::objectDefine(out, "tool", json::value(call.tool));
    rt::objectDefine(out, "input", json::value(call.input));
    rt::objectDefine(out, "callIndex", rt::makeNumber(call.callIndex));
    rt::objectDefine(out, "resultIndex", rt::makeNumber(call.resultIndex));
    rt::objectDefine(out, "resultChars", rt::makeNumber(call.resultChars));
    rt::objectDefine(out, "isError", json::value(call.isError));
    rt::objectDefine(out, "pinned", json::value(call.pinned));
    return out;
  }

  json::value toJson(const std::vector<ToolCall>& calls) {
    json::array out;
    for (const ToolCall& call : calls) out.push_back(toJson(call));
    return out;
  }

  json::value toJson(const CallDecision& decision) {
    json::object out;
    rt::objectDefine(out, "id", json::value(decision.id));
    rt::objectDefine(out, "tool", json::value(decision.tool));
    rt::objectDefine(out, "keepCall", rt::makeNumber(decision.keepCall));
    rt::objectDefine(out, "keepResult", rt::makeNumber(decision.keepResult));
    rt::objectDefine(out, "action", json::value(decision.action));
    rt::objectDefine(out, "reason", json::value(decision.reason));
    return out;
  }

  CallDecision decisionFromJson(const json::object& object) {
    CallDecision decision;
    decision.id = text(rt::get(object, "id"));
    decision.tool = text(rt::get(object, "tool"));
    decision.keepCall = number(rt::get(object, "keepCall"));
    decision.keepResult = number(rt::get(object, "keepResult"));
    decision.action = text(rt::get(object, "action"));
    decision.reason = text(rt::get(object, "reason"));
    return decision;
  }

  // The stats object without `ms` (wall-clock time is not recorded).
  json::value toJson(const CompactResult::Stats& stats) {
    json::object out;
    rt::objectDefine(out, "messagesBefore", rt::makeNumber(stats.messagesBefore));
    rt::objectDefine(out, "messagesAfter", rt::makeNumber(stats.messagesAfter));
    rt::objectDefine(out, "charsBefore", rt::makeNumber(stats.charsBefore));
    rt::objectDefine(out, "charsAfter", rt::makeNumber(stats.charsAfter));
    rt::objectDefine(out, "calls", rt::makeNumber(stats.calls));
    rt::objectDefine(out, "kept", rt::makeNumber(stats.kept));
    rt::objectDefine(out, "resultsDropped", rt::makeNumber(stats.resultsDropped));
    rt::objectDefine(out, "callsDropped", rt::makeNumber(stats.callsDropped));
    rt::objectDefine(out, "pinned", rt::makeNumber(stats.pinned));
    rt::objectDefine(out, "stateTokens", rt::makeNumber(stats.stateTokens));
    rt::objectDefine(out, "stateStage", json::value(stats.stateStage));
    rt::objectDefine(out, "windows", rt::makeNumber(stats.windows));
    rt::objectDefine(out, "requests", rt::makeNumber(stats.requests));
    return out;
  }

  CompactResult::Stats statsFromJson(const json::object& object) {
    CompactResult::Stats stats;
    stats.messagesBefore = static_cast<std::int64_t>(number(rt::get(object, "messagesBefore")));
    stats.messagesAfter = static_cast<std::int64_t>(number(rt::get(object, "messagesAfter")));
    stats.charsBefore = static_cast<std::int64_t>(number(rt::get(object, "charsBefore")));
    stats.charsAfter = static_cast<std::int64_t>(number(rt::get(object, "charsAfter")));
    stats.calls = static_cast<std::int64_t>(number(rt::get(object, "calls")));
    stats.kept = static_cast<std::int64_t>(number(rt::get(object, "kept")));
    stats.resultsDropped = static_cast<std::int64_t>(number(rt::get(object, "resultsDropped")));
    stats.callsDropped = static_cast<std::int64_t>(number(rt::get(object, "callsDropped")));
    stats.pinned = static_cast<std::int64_t>(number(rt::get(object, "pinned")));
    stats.stateTokens = number(rt::get(object, "stateTokens"));
    stats.stateStage = text(rt::get(object, "stateStage"));
    stats.windows = static_cast<std::int64_t>(number(rt::get(object, "windows")));
    stats.requests = static_cast<std::int64_t>(number(rt::get(object, "requests")));
    stats.ms = number(rt::get(object, "ms"));
    return stats;
  }

  json::value toJson(const jc::codex_items::CodexTranscript& transcript) {
    json::array entries;
    for (const jc::codex_items::TranscriptEntry& entry : transcript.entries) {
      json::object out;
      if (const auto* textEntry = std::get_if<jc::codex_items::TextEntry>(&entry)) {
        rt::objectDefine(out, "kind", json::value("text"));
        rt::objectDefine(out, "itemType", json::value(textEntry->itemType));
        rt::objectDefine(out, "role", json::value(textEntry->role));
        rt::objectDefine(out, "text", json::value(textEntry->text));
      } else if (const auto* callEntry = std::get_if<jc::codex_items::CallEntry>(&entry)) {
        rt::objectDefine(out, "kind", json::value("call"));
        rt::objectDefine(out, "itemType", json::value(callEntry->itemType));
        rt::objectDefine(out, "callId", json::value(callEntry->callId));
        rt::objectDefine(out, "tool", json::value(callEntry->tool));
        rt::objectDefine(out, "input", json::value(callEntry->input));
        rt::objectDefine(out, "inputJson", json::value(callEntry->inputJson));
      } else {
        const auto& resultEntry = std::get<jc::codex_items::ResultEntry>(entry);
        rt::objectDefine(out, "kind", json::value("result"));
        rt::objectDefine(out, "itemType", json::value(resultEntry.itemType));
        rt::objectDefine(out, "callId", json::value(resultEntry.callId));
        rt::objectDefine(out, "text", json::value(resultEntry.text));
        rt::objectDefine(out, "isError", json::value(resultEntry.isError));
      }
      entries.push_back(std::move(out));
    }
    json::object out;
    rt::objectDefine(out, "entries", json::value(std::move(entries)));
    rt::objectDefine(out, "messages", toJson(transcript.messages));
    if (transcript.exclusion) {
      json::object exclusion;
      rt::objectDefine(exclusion, "index", rt::makeNumber(transcript.exclusion->index));
      rt::objectDefine(exclusion, "chars", rt::makeNumber(transcript.exclusion->chars));
      rt::objectDefine(exclusion, "reason", json::value(transcript.exclusion->reason));
      rt::objectDefine(out, "exclusion", json::value(std::move(exclusion)));
    } else {
      rt::objectDefine(out, "exclusion", json::value(nullptr));
    }
    rt::objectDefine(out, "opaqueItems", rt::makeNumber(transcript.opaqueItems));
    return out;
  }

  json::value errorJson(const rt::JsError& error) {
    json::object inner;
    rt::objectDefine(inner, "name", json::value(error.name));
    rt::objectDefine(inner, "message", json::value(error.message));
    json::object out;
    rt::objectDefine(out, "error", json::value(std::move(inner)));
    return out;
  }

  // The verdict the recording asker of the generator answers for a call id.
  jc::types::CallAnswer hashVerdict(std::string_view id) {
    std::int64_t h = 0;
    for (const char ch : id) h = (h * 31 + static_cast<unsigned char>(ch)) % 1000;
    return jc::types::CallAnswer{static_cast<double>(h % 10) / 10, static_cast<double>((h >> 2) % 10) / 10};
  }

  // `makeAsker(log, mode)` of the generator.
  class RecordingAsker : public JevAsker {
  public:
    explicit RecordingAsker(std::string mode_) : mode(std::move(mode_)) {}
    json::array log;

    rt::Task<JevResponse> ask(JevState state, JevQuestions questions) override {
      json::array names;
      for (const auto& [name, question] : questions) names.push_back(json::value(name));
      const std::string stateJson = rt::jsonStringify(state);
      json::object entry;
      rt::objectDefine(entry, "stateSha256", json::value(sha256(stateJson)));
      rt::objectDefine(entry, "stateTokens", rt::makeNumber(jc::state::estimateTokens(stateJson)));
      rt::objectDefine(entry, "names", json::value(names));
      log.push_back(json::value(std::move(entry)));
      if (mode == "fail") throw rt::JsError("asker failure");
      json::object answers;
      for (const json::value& value : names) {
        const std::string name(value.as_string());
        if (mode == "partial" && name.starts_with("result_")) continue;
        const std::string id = name.starts_with("call_") ? name.substr(5) : name.substr(7);
        const jc::types::CallAnswer verdict = hashVerdict(id);
        json::object answer;
        rt::objectDefine(answer, "noul", rt::makeNumber(name.starts_with("call_") ? verdict.keepCall : verdict.keepResult));
        rt::objectSet(answers, name, json::value(std::move(answer)));
      }
      if (mode == "badnoul") {
        json::object answer;
        rt::objectDefine(answer, "noul", json::value("high"));
        rt::objectSet(answers, std::string(names.at(0).as_string()), json::value(std::move(answer)));
      }
      JevResponse response;
      if (mode == "noanswers") {
        rt::objectDefine(response, "model", json::value("x"));
        co_return response;
      }
      rt::objectDefine(response, "answers", json::value(std::move(answers)));
      co_return response;
    }

  private:
    std::string mode;
  };

  std::vector<Message> windowedTranscript(std::int64_t count, std::int64_t repeat, std::int64_t noteEvery) {
    std::vector<Message> messages;
    for (std::int64_t index = 0; index < count; index += 1) {
      const std::string id = "call_" + std::to_string(index);
      Message call;
      call.role = "assistant";
      call.text = index % noteEvery == 0 ? "thinking about it" : "";
      json::object input;
      rt::objectDefine(input, "cmd", json::value("command " + std::to_string(index)));
      call.toolUses.push_back(ToolUse{id, "exec_command", input, std::nullopt, std::nullopt});
      messages.push_back(std::move(call));
      Message result;
      result.role = "user";
      result.toolResults = std::vector<ToolResult>{ToolResult{id, rt::repeat("result " + std::to_string(index) + " ", static_cast<double>(repeat)), std::nullopt}};
      messages.push_back(std::move(result));
    }
    Message last;
    last.role = "user";
    last.text = "Final goal.";
    messages.push_back(std::move(last));
    return messages;
  }
}

TEST("estimateTokens matches the recorded estimates") {
  for (const json::value& entry : section("estimateTokens")) {
    const json::object& vector = asObject(entry);
    EXPECT_EQ(jc::state::estimateTokens(text(rt::get(vector, "text"))), number(rt::get(vector, "tokens")));
  }
}

TEST("collectToolCalls pairs calls and results like the TS") {
  for (const json::value& entry : section("collectToolCalls")) {
    const json::object& vector = asObject(entry);
    const std::vector<Message> messages = messagesFromJson(*rt::get(vector, "messages"));
    const double preserve = number(rt::get(vector, "preserve"));
    expectJson(toJson(jc::state::collectToolCalls(messages, preserve)), *rt::get(vector, "calls"), "collectToolCalls preserve=" + rt::numberToString(preserve));
  }
}

TEST("fitState reaches every recorded stage with the same state and tokens") {
  json::object transcripts;
  for (const json::value& entry : section("fitTranscripts")) transcripts[std::string(*rt::getString(entry, "name"))] = entry;
  std::size_t checked = 0;
  for (const json::value& entry : section("fitState")) {
    const json::object& vector = asObject(entry);
    const std::string name = text(rt::get(vector, "name"));
    const json::object& transcript = transcripts.at(name).as_object();
    const std::vector<Message> messages = messagesFromJson(*rt::get(transcript, "messages"));
    const double preserve = number(rt::get(vector, "preserve"));
    const std::vector<ToolCall> calls = jc::state::collectToolCalls(messages, preserve);
    expectJson(toJson(calls), *rt::get(transcript, "calls"), name + " calls");
    jc::types::ResolvedCompactOptions options;
    options.maxStateTokens = number(rt::get(vector, "cap"));
    options.preserveRecentMessages = preserve;
    options.goal = text(rt::get(vector, "goal"));
    const json::object& expected = *rt::getObject(vector, "result");
    const std::string label = name + " cap " + rt::numberToString(options.maxStateTokens);
    try {
      const jc::types::FittedState fitted = jc::state::fitState(messages, calls, options);
      const json::value state = jc::types::toJson(fitted.state);
      EXPECT_EQ(fitted.stage, text(rt::get(expected, "stage")));
      EXPECT_EQ(fitted.tokens, number(rt::get(expected, "tokens")));
      if (const json::value* full = rt::get(expected, "state"); full != nullptr && !full->is_null()) expectJson(state, *full, label + " state");
      EXPECT_EQ(sha256(rt::jsonStringify(state)), text(rt::get(expected, "stateSha256")));
    } catch (const rt::JsError& error) {
      expectJson(errorJson(error), json::value(expected), label + " error");
    }
    checked += 1;
  }
  EXPECT(checked > 30);
}

TEST("truncatedResultText matches the recorded truncations") {
  for (const json::value& entry : section("truncatedResultText")) {
    const json::object& vector = asObject(entry);
    EXPECT_EQ(jc::compact::truncatedResultText(text(rt::get(vector, "text")), *rt::getBool(vector, "isError"), number(rt::get(vector, "headChars"))),
              text(rt::get(vector, "result")));
  }
}

TEST("planWindows matches the recorded windows") {
  const json::object& input = *rt::getObject(vectors(), "planInput");
  const std::vector<Message> messages = messagesFromJson(*rt::get(input, "messages"));
  const std::vector<ToolCall> calls = jc::state::collectToolCalls(messages, 2);
  expectJson(toJson(calls), *rt::get(input, "calls"), "plan calls");
  for (const json::value& entry : section("planWindows")) {
    const json::object& vector = asObject(entry);
    json::array windows;
    for (const jc::compact::Window& window : jc::compact::planWindows(messages, calls, number(rt::get(vector, "max")))) {
      json::object item;
      rt::objectDefine(item, "start", rt::makeNumber(window.start));
      rt::objectDefine(item, "end", rt::makeNumber(window.end));
      windows.push_back(json::value(std::move(item)));
    }
    expectJson(json::value(std::move(windows)), *rt::get(vector, "windows"), "planWindows max=" + rt::numberToString(number(rt::get(vector, "max"))));
  }
}

ASYNC_TEST("compact matches the recorded decisions, stats, messages and Jev requests") {
  const json::array& transcripts = *rt::getArray(vectors(), "compactTranscripts");
  std::size_t checked = 0;
  for (const json::value& entry : section("compact")) {
    const json::object& vector = asObject(entry);
    const std::string name = text(rt::get(vector, "name"));
    std::vector<Message> messages;
    if (const json::object* spec = rt::getObject(vector, "spec")) {
      messages = windowedTranscript(static_cast<std::int64_t>(number(rt::get(*spec, "count"))), static_cast<std::int64_t>(number(rt::get(*spec, "repeat"))),
                                    static_cast<std::int64_t>(number(rt::get(*spec, "noteEvery"))));
    } else {
      messages = messagesFromJson(transcripts.at(static_cast<std::size_t>(number(rt::get(vector, "transcript")))));
    }
    const json::object& options = *rt::getObject(vector, "options");
    jc::types::CompactOptions compactOptions;
    if (const auto goal = rt::getString(options, "goal")) compactOptions.goal = std::string(*goal);
    compactOptions.keepThreshold = rt::getNumber(options, "keepThreshold");
    compactOptions.preserveRecentMessages = rt::getNumber(options, "preserveRecentMessages");
    compactOptions.maxStateTokens = rt::getNumber(options, "maxStateTokens");
    compactOptions.maxRequestTokens = rt::getNumber(options, "maxRequestTokens");
    compactOptions.truncateHeadChars = rt::getNumber(options, "truncateHeadChars");
    // NaN is recorded as null: the generator passed NaN for keepThreshold in "nan options".
    if (const json::value* threshold = rt::get(options, "keepThreshold"); threshold != nullptr && threshold->is_null()) compactOptions.keepThreshold = rt::Number::NaN;
    if (const json::value* head = rt::get(options, "truncateHeadChars"); head != nullptr && head->is_null()) compactOptions.truncateHeadChars = rt::Number::POSITIVE_INFINITY;
    auto asker = std::make_shared<RecordingAsker>(text(rt::get(vector, "mode")));
    const json::object& expected = *rt::getObject(vector, "result");
    std::optional<CompactResult> result;
    std::optional<json::value> failure;
    try {
      auto task = jc::compact::compact(messages, asker, compactOptions);
      CompactResult value = co_await std::move(task);
      result = std::move(value);
    } catch (const rt::JsError& error) {
      failure = errorJson(error);
    }
    expectJson(json::value(asker->log), *rt::get(vector, "log"), name + " Jev requests");
    if (failure) {
      expectJson(*failure, json::value(expected), name + " error");
    } else if (const json::object* value = rt::getObject(expected, "value")) {
      json::array decisions;
      for (const CallDecision& decision : result->decisions) decisions.push_back(toJson(decision));
      expectJson(json::value(std::move(decisions)), *rt::get(*value, "decisions"), name + " decisions");
      expectJson(toJson(result->stats), *rt::get(*value, "stats"), name + " stats");
      const json::value messagesJson = toJson(result->messages);
      if (const json::value* full = rt::get(*value, "messages"); full != nullptr && !full->is_null()) expectJson(messagesJson, *full, name + " messages");
      EXPECT_EQ(sha256(rt::jsonStringify(messagesJson)), text(rt::get(*value, "messagesSha256")));
      EXPECT(result->stats.ms >= 0);
    } else {
      ::uos::rt::test::report(__FILE__, __LINE__, name + ": compact resolved where the TS rejected", false);
    }
    checked += 1;
  }
  EXPECT(checked >= 15);
}

TEST("parseCodexInput and renderSummary match the recorded transcripts and summaries") {
  for (const json::value& entry : section("codexItems")) {
    const json::object& vector = asObject(entry);
    const json::value& input = *rt::get(vector, "input");
    const std::optional<jc::codex_items::CodexTranscript> transcript = jc::codex_items::parseCodexInput(input);
    const json::value& expected = *rt::get(vector, "transcript");
    if (expected.is_null()) {
      EXPECT(!transcript.has_value());
      continue;
    }
    ASSERT(transcript.has_value());
    expectJson(toJson(*transcript), expected, "transcript of " + rt::jsonStringify(input).substr(0, 80));
    const std::vector<ToolCall> calls = jc::state::collectToolCalls(transcript->messages, 6);
    expectJson(toJson(calls), *rt::get(vector, "calls"), "codex calls");
    for (const json::value& render : *rt::getArray(vector, "renders")) {
      const json::object& options = *rt::getObject(render, "options");
      jc::codex_items::RenderOptions renderOptions;
      for (const json::value& decision : *rt::getArray(options, "decisions")) renderOptions.decisions.push_back(decisionFromJson(decision.as_object()));
      for (const json::value& pair : *rt::getArray(options, "callIds")) renderOptions.callIds.set(text(rt::get(pair.as_array(), 0)), text(rt::get(pair.as_array(), 1)));
      renderOptions.headChars = number(rt::get(options, "headChars"));
      if (const json::object* stats = rt::getObject(options, "stats")) renderOptions.stats = statsFromJson(*stats);
      EXPECT_EQ(jc::codex_items::renderSummary(*transcript, renderOptions), text(rt::get(render.as_object(), "summary")));
    }
  }
  EXPECT(!jc::codex_items::parseCodexInput(static_cast<const json::value*>(nullptr)).has_value());
}

TEST("parseTurnMetadata and isResponsesCompaction match the recorded headers") {
  for (const json::value& entry : section("turnMetadata")) {
    const json::object& vector = asObject(entry);
    const json::value& header = *rt::get(vector, "header");
    const std::optional<std::string> value = header.is_null() ? std::nullopt : std::optional<std::string>(text(&header));
    const std::optional<jc::codex_items::TurnMetadata> metadata = jc::codex_items::parseTurnMetadata(value);
    const json::value& expected = *rt::get(vector, "metadata");
    if (expected.is_null()) EXPECT(!metadata.has_value());
    else if (metadata) expectJson(json::value(*metadata), expected, "metadata of " + value.value_or("null"));
    else ::uos::rt::test::report(__FILE__, __LINE__, "metadata missing for " + value.value_or("null"), false);
    EXPECT_EQ(jc::codex_items::isResponsesCompaction(metadata), *rt::getBool(vector, "compaction"));
  }
}

TEST("buildJevRequest matches the recorded requests") {
  for (const json::value& entry : section("buildJevRequest")) {
    const json::object& vector = asObject(entry);
    const json::object& params = *rt::getObject(vector, "params");
    jc::request::BuildJevRequestParams buildParams;
    buildParams.apiKey = text(rt::get(params, "apiKey"));
    if (const auto model = rt::getString(params, "model")) buildParams.model = std::string(*model);
    if (const auto baseUrl = rt::getString(params, "baseUrl")) buildParams.baseUrl = std::string(*baseUrl);
    JevQuestions questions;
    for (const auto& [name, question] : *rt::getObject(vector, "questions")) {
      jc::types::NoulQuestion noul;
      noul.type = text(rt::get(question, "type"));
      noul.instructions = text(rt::get(question, "instructions"));
      questions.set(std::string(name), noul);
    }
    const jc::request::JevRequest request = jc::request::buildJevRequest(buildParams, *rt::get(vector, "state"), questions);
    json::object headers;
    for (const auto& [name, value] : request.headers) rt::objectDefine(headers, name, json::value(value));
    json::object actual;
    rt::objectDefine(actual, "url", json::value(request.url));
    rt::objectDefine(actual, "method", json::value(request.method));
    rt::objectDefine(actual, "headers", json::value(std::move(headers)));
    rt::objectDefine(actual, "body", json::value(request.body));
    expectJson(json::value(actual), *rt::get(vector, "request"), "buildJevRequest");
    EXPECT_EQ(rt::jsonStringify(json::value(actual)), rt::jsonStringify(*rt::get(vector, "request")));
  }
}

TEST("parseJevResponse matches the recorded results and errors") {
  for (const json::value& entry : section("parseJevResponse")) {
    const json::object& vector = asObject(entry);
    json::value actual;
    try {
      json::object value;
      rt::objectDefine(value, "value", json::value(jc::request::parseJevResponse(number(rt::get(vector, "status")), *rt::getBool(vector, "ok"), text(rt::get(vector, "text")))));
      actual = std::move(value);
    } catch (const rt::JsError& error) {
      actual = errorJson(error);
    }
    expectJson(actual, *rt::get(vector, "result"), "parseJevResponse " + text(rt::get(vector, "text")).substr(0, 40));
  }
}

TEST("noulAnswer matches the recorded answers and errors") {
  for (const json::value& entry : section("noulAnswer")) {
    const json::object& vector = asObject(entry);
    const std::string answersText = text(rt::get(vector, "answersJson"));
    json::value answers;
    if (answersText == R"({"a":{"noul":1e999}})") {
      EXPECT_THROWS_MATCHING(jc::request::parseJevResponse(200, true, "{\"answers\":" + answersText + "}"), "Jev returned malformed JSON");
      // Exercise the finite-number check with the value Bun's fixture parser produced.
      answers = json::object{{"a", json::object{{"noul", rt::makeNumber(rt::Number::POSITIVE_INFINITY)}}}};
    } else {
      answers = rt::jsonParse(answersText);
    }
    json::value actual;
    try {
      json::object value;
      rt::objectDefine(value, "value", rt::makeNumber(jc::request::noulAnswer(&answers, text(rt::get(vector, "name")))));
      actual = std::move(value);
    } catch (const rt::JsError& error) {
      actual = errorJson(error);
    }
    expectJson(actual, *rt::get(vector, "result"), "noulAnswer " + answersText);
  }
  for (const json::value& entry : section("noulAnswerNullish")) {
    const json::object& vector = asObject(entry);
    const json::value null = nullptr;
    const json::value* answers = text(rt::get(vector, "answers")) == "null" ? &null : nullptr;
    try {
      (void)jc::request::noulAnswer(answers, "call_t1");
      ::uos::rt::test::report(__FILE__, __LINE__, "noulAnswer accepted nullish answers", false);
    } catch (const rt::JsError& error) {
      expectJson(errorJson(error), *rt::get(vector, "result"), "noulAnswer nullish");
    }
  }
}

ASYNC_TEST("JevClient sends the recorded request and parses the recorded response") {
  for (const json::value& entry : section("client")) {
    const json::object& vector = asObject(entry);
    const json::object& options = *rt::getObject(vector, "options");
    auto seen = std::make_shared<json::array>();
    rt::AbortController controller;
    const rt::AbortSignal signal = controller.signal();
    const double status = number(rt::get(vector, "status"));
    const std::string body = text(rt::get(vector, "body"));
    rt::Fetch fetcher([seen, signal, status, body](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
      json::array headers;
      for (const auto& [name, value] : init.headers.value_or(rt::Headers()).entries()) {
        json::array pair;
        pair.push_back(json::value(name));
        pair.push_back(json::value(value));
        headers.push_back(json::value(std::move(pair)));
      }
      json::object record;
      rt::objectDefine(record, "input", json::value(std::get<std::string>(input)));
      rt::objectDefine(record, "method", json::value(init.method.value_or("")));
      rt::objectDefine(record, "headers", json::value(std::move(headers)));
      rt::objectDefine(record, "body", json::value(std::get<std::string>(init.body.value())));
      rt::objectDefine(record, "signal", json::value(init.signal.has_value() && *init.signal == signal));
      rt::objectDefine(record, "hasSignal", json::value(init.signal.has_value()));
      seen->push_back(json::value(std::move(record)));
      rt::ResponseInit responseInit;
      responseInit.status = status;
      return rt::Response(rt::BodyInit(body), responseInit);
    });
    jc::client::JevClientOptions clientOptions;
    clientOptions.apiKey = text(rt::get(options, "apiKey"));
    if (const auto model = rt::getString(options, "model")) clientOptions.model = std::string(*model);
    if (const auto baseUrl = rt::getString(options, "baseUrl")) clientOptions.baseUrl = std::string(*baseUrl);
    clientOptions.fetch = fetcher;
    if (rt::getBool(options, "signal") == std::optional<bool>(true)) clientOptions.signal = signal;
    auto client = std::make_shared<jc::client::JevClient>(clientOptions);
    json::object state;
    rt::objectDefine(state, "context", json::value("ctx"));
    rt::objectDefine(state, "goal", json::value(""));
    rt::objectDefine(state, "history", json::value(json::array()));
    JevQuestions questions;
    jc::types::NoulQuestion question;
    question.instructions = "keep the call?";
    questions.set("call_t1", question);
    json::value actual;
    try {
      const JevState askedState = json::value(state);
      auto task = client->ask(askedState, questions);
      const JevResponse response = co_await std::move(task);
      json::object value;
      rt::objectDefine(value, "value", json::value(response));
      actual = std::move(value);
    } catch (const rt::JsError& error) {
      actual = errorJson(error);
    }
    expectJson(json::value(*seen), *rt::get(vector, "seen"), "client requests for " + rt::jsonStringify(json::value(options)));
    expectJson(actual, *rt::get(vector, "result"), "client result for " + rt::jsonStringify(json::value(options)));
  }
}
