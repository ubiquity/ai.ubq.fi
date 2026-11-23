// Port of tests/harmony-probes.test.ts; every scenario uses a scripted transport or fake fetch.
// Links: tests/harmony_probes_test.cpp src/harmony/adapter.cpp src/harmony/probes.cpp src/harmony/probe_scenarios.cpp src/harmony/classifier.cpp
// src/harmony/conversation.cpp src/harmony/parse.cpp src/harmony/render.cpp src/harmony/types.cpp
// src/provider/cerebras.cpp src/sentinel/upstream_capture.cpp src/utils.cpp
#include "uos/harmony/probe_scenarios.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <memory>
#include <set>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace probes = uos::harmony::probes;
  namespace adapter = uos::harmony::adapter;
  namespace scenarios = uos::harmony::probe_scenarios;
  namespace classifier = uos::harmony::classifier;
  #define AWAIT(name, expr) auto name##Task = (expr); auto name = co_await std::move(name##Task)
  const scenarios::ProbeScenario& scenario(std::string_view id) {
    for (const auto& candidate : scenarios::PROBE_SCENARIOS) if (candidate.id == id) return candidate;
    throw rt::JsError("missing scenario " + std::string(id));
  }
  std::string scriptedCompletion(json::object message, std::string finishReason = "stop") {
    json::object assistant{{"role", "assistant"}}; rt::objectAssign(assistant, message);
    return rt::jsonStringify(json::object{{"id", "cmpl-scripted"}, {"object", "chat.completion"}, {"created", 1}, {"model", "gpt-oss-120b"},
      {"choices", json::array{json::object{{"index", 0}, {"message", std::move(assistant)}, {"finish_reason", finishReason}}}},
      {"usage", json::object{{"prompt_tokens", 10}, {"completion_tokens", 8}, {"total_tokens", 18}}}});
  }
  std::string errorResponse(std::string code, std::string message) { return rt::jsonStringify(json::object{{"error", json::object{{"code", code}, {"message", message}}}}); }
  const json::array& messagesOf(const json::object& body) {
    static const json::array empty;
    const auto* messages = rt::getArray(body, "messages"); return messages ? *messages : empty;
  }
  std::string promptOf(const json::array& messages) {
    for (std::size_t i = messages.size(); i > 0; --i) {
      const auto& message = messages[i - 1]; const auto content = rt::getString(message, "content");
      if (rt::getString(message, "role") == "user" && content) return std::string(*content);
    }
    return "";
  }
  bool mixedStrictnessOf(const json::object& body) {
    const auto* tools = rt::getArray(body, "tools"); if (!tools) return false;
    std::set<std::string> values;
    for (const auto& tool : *tools) {
      const auto* function = rt::get(tool, "function"); const auto* strict = function ? rt::get(*function, "strict") : nullptr;
      values.insert(strict ? rt::jsonStringify(*strict) : "undefined");
    }
    return values.size() > 1;
  }
  bool toolsOf(const json::object& body) {
    if (rt::getArray(body, "tools")) return true;
    for (const auto& message : messagesOf(body)) {
      const auto content = rt::getString(message, "content");
      if (rt::getString(message, "role") == "developer" && content && rt::includes(*content, "namespace functions")) return true;
    }
    return false;
  }
  bool hasToolResult(const json::array& messages) {
    for (const auto& message : messages) {
      if (rt::getString(message, "role") == "tool") return true;
      const auto content = rt::getString(message, "content");
      if (rt::getString(message, "role") == "user" && content && rt::startsWith(*content, "Tool result from")) return true;
    }
    return false;
  }
  bool responseFormatsOf(const json::object& body) {
    if (rt::hasOwn(body, "response_format")) return true;
    for (const auto& message : messagesOf(body)) {
      const auto content = rt::getString(message, "content");
      if (rt::getString(message, "role") == "developer" && content && rt::includes(*content, "# Response Formats")) return true;
    }
    return false;
  }
  std::string scriptedPayloadOf(const json::object& body) {
    const auto& messages = messagesOf(body);
    if (rt::toString(rt::get(body, "max_completion_tokens")) == "128") return scriptedCompletion({{"content", "true"}, {"reasoning_content", "The ledger advanced."}});
    for (const auto& message : messages) if (rt::getString(message, "role") == "assistant" && message.is_object() && rt::hasOwn(message.as_object(), "reasoning_content")) return errorResponse("invalid_request_error", "messages.1.assistant.reasoning_content is unsupported");
    if (mixedStrictnessOf(body)) return errorResponse("invalid_request_error", "Tools with mixed values for 'strict' are not allowed");
    if (rt::getArray(body, "tools") && rt::hasOwn(body, "response_format")) return errorResponse("invalid_request_error", "Unsupported: tools with response_format");
    if (hasToolResult(messages)) return scriptedCompletion({{"content", "San Francisco is sunny today."}, {"reasoning_content", "Summarize the weather."}});
    if (!toolsOf(body)) return responseFormatsOf(body) ? scriptedCompletion({{"content", "{\"answer\": 4}"}}) : scriptedCompletion({{"content", "4"}, {"reasoning_content", "Simple arithmetic."}});
    const bool twice = rt::includes(promptOf(messages), "twice");
    json::array calls{json::object{{"id", "call_a"}, {"type", "function"}, {"function", json::object{{"name", "get_weather"}, {"arguments", "{\"location\":\"San Francisco\"}"}}}}};
    if (twice) calls.push_back(json::object{{"id", "call_b"}, {"type", "function"}, {"function", json::object{{"name", "get_weather"}, {"arguments", "{\"location\":\"Tokyo\"}"}}}});
    json::object message{{"content", nullptr}}; if (!twice) message["reasoning_content"] = "Private chain of thought."; message["tool_calls"] = std::move(calls);
    return scriptedCompletion(message, "tool_calls");
  }
  struct Script { adapter::HarmonyTransport transport; std::shared_ptr<std::vector<json::object>> bodies; };
  Script scriptedTransport() {
    const auto bodies = std::make_shared<std::vector<json::object>>();
    adapter::HarmonyTransport transport = [bodies](json::object body) {
      bodies->push_back(body); const auto payload = scriptedPayloadOf(body);
      return rt::fulfilledTask(rt::Response(payload, {.status = rt::startsWith(payload, "{\"error\"") ? 400.0 : 200.0, .headers = rt::Headers{{"Content-Type", "application/json"}}}));
    };
    return {.transport = std::move(transport), .bodies = bodies};
  }
  probes::ProbeContext context(const Script& script) { return probes::createProbeContext(script.transport, [] { return rt::Date(0.0); }); }
  std::vector<std::string> roles(const json::object& body) {
    std::vector<std::string> out; for (const auto& message : messagesOf(body)) out.emplace_back(*rt::getString(message, "role")); return out;
  }
}

TEST("the manifest covers every required protocol question exactly once") {
  std::set<std::string> ids; std::set<std::string> groups;
  for (const auto& candidate : scenarios::PROBE_SCENARIOS) { ids.insert(candidate.id); groups.insert(candidate.group); }
  EXPECT_EQ(ids.size(), scenarios::PROBE_SCENARIOS.size()); EXPECT_EQ(groups, (std::set<std::string>{"classifier", "parallel", "reasoning", "replay", "strictness", "structured", "tools"}));
  for (const auto id : {"reasoning.effort.low", "reasoning.effort.medium", "reasoning.effort.high", "reasoning.replay.after-final", "reasoning.replay.echo",
    "tools.generic.sequence", "tools.native.sequence", "tools.native.user-result", "tools.generic.consecutive", "strictness.mixed", "strictness.all-false", "strictness.all-true",
    "structured.json-object", "structured.json-schema", "structured.native-formats", "structured.with-tools", "parallel.native", "parallel.generic-flag", "classifier.low", "classifier.medium"}) EXPECT(ids.contains(id));
  EXPECT_EQ(scenario("classifier.low").style, "classifier"); EXPECT_EQ(scenario("tools.native.sequence").style, "native"); EXPECT_EQ(scenario("tools.generic.sequence").style, "generic");
}

ASYNC_TEST("reasoning effort probe runs deterministically through the fake transport") {
  const auto script = scriptedTransport(); const auto ctx = context(script); AWAIT(result, scenario("reasoning.effort.low").run(ctx));
  EXPECT_EQ(result.outcome, "ok"); EXPECT_EQ(result.turns.size(), std::size_t{1}); ASSERT(result.turns[0].response); EXPECT(result.turns[0].response->reasoningPresent);
  EXPECT_EQ(result.turns[0].response->reasoningChars, 18); EXPECT_EQ(result.turns[0].response->contentPreview, std::optional<std::string>("4"));
  ASSERT(result.turns[0].request); EXPECT_EQ(result.turns[0].request->roles, (std::vector<std::string>{"user"})); EXPECT_EQ(result.durationMs, 0);
}

ASYNC_TEST("native probe records the projected Cerebras message roles") {
  rt::env::ScopedEnvironment environment({}); std::vector<std::string> wireRoles;
  const auto transport = adapter::createCerebrasTransport({.apiKey = "test-key", .fetcher = rt::Fetch([&wireRoles](rt::RequestInfo, rt::RequestInit init) {
    const auto* raw = init.body ? std::get_if<std::string>(&*init.body) : nullptr; const auto body = rt::jsonParse(raw ? *raw : "{}").as_object(); wireRoles = roles(body);
    return rt::Response(scriptedCompletion({{"content", "done"}}), {.status = 200, .headers = rt::Headers{{"Content-Type", "application/json"}}});
  })});
  const auto ctx = probes::createProbeContext(transport, [] { return rt::Date(0.0); }); const auto state = probes::user("hello"); const probes::ProbeTurnOptions options{.style = "native"};
  AWAIT(result, ctx.runTurn(state, options)); EXPECT_EQ(result.record.outcome, "ok"); EXPECT_EQ(wireRoles, (std::vector<std::string>{"system", "user"})); ASSERT(result.record.request); EXPECT_EQ(result.record.request->roles, wireRoles);
}

ASYNC_TEST("generic tool sequence reproduces call/result shape and replays the result") {
  const auto script = scriptedTransport(); const auto ctx = context(script); AWAIT(result, scenario("tools.generic.sequence").run(ctx));
  EXPECT_EQ(result.outcome, "ok"); ASSERT(result.turns.size() == 2); const auto& first = result.turns[0]; const auto& second = result.turns[1]; ASSERT(first.response); ASSERT(second.response);
  EXPECT_EQ(first.response->toolCalls.size(), std::size_t{1}); EXPECT_EQ(first.response->toolCalls[0].name, "get_weather"); EXPECT(first.response->toolCalls[0].argumentsJsonValid); EXPECT(second.response->contentPresent);
  const auto& body = script.bodies->at(1); EXPECT_EQ(roles(body), (std::vector<std::string>{"user", "assistant", "tool"}));
  const auto expected = rt::jsonParse("[{\"id\":\"call_a\",\"type\":\"function\",\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"location\\\":\\\"San Francisco\\\"}\"}}]");
  EXPECT(rt::deepEqual(messagesOf(body)[1].at("tool_calls"), expected)); EXPECT(!rt::includes(rt::jsonStringify(body), "Private chain of thought"));
}

ASYNC_TEST("mixed strictness probe records the upstream rejection and matches its expectation") {
  const auto script = scriptedTransport(); const auto ctx = context(script); AWAIT(result, scenario("strictness.mixed").run(ctx));
  EXPECT_EQ(result.outcome, "upstream_rejected"); EXPECT_EQ(std::optional<std::string>(result.outcome), result.expectedOutcome);
  EXPECT_EQ(result.turns[0].status, std::optional<double>(400)); ASSERT(result.turns[0].upstreamError); EXPECT(rt::includes(result.turns[0].upstreamError->message.value_or(""), "mixed values"));
}

ASYNC_TEST("structured output with tools is recorded without being normalized away") {
  const auto script = scriptedTransport(); const auto ctx = context(script); AWAIT(result, scenario("structured.with-tools").run(ctx));
  EXPECT_EQ(result.outcome, "upstream_rejected"); ASSERT(result.turns[0].request); EXPECT_EQ(result.turns[0].request->responseFormat, "json_schema");
}

ASYNC_TEST("classifier probe returns a literal true verdict") {
  const auto script = scriptedTransport(); const auto ctx = context(script); AWAIT(result, scenario("classifier.low").run(ctx));
  EXPECT_EQ(result.outcome, "ok"); EXPECT_EQ(result.verdict, std::optional<classifier::BootstrapClassifierVerdict>(classifier::BootstrapClassifierVerdictTrue{.raw = "true"}));
}

ASYNC_TEST("the native tool result replay is sent as a user message in the user-role variant") {
  const auto script = scriptedTransport(); const auto ctx = context(script); AWAIT(result, scenario("tools.native.user-result").run(ctx));
  EXPECT_EQ(result.outcome, "ok"); const auto& body = script.bodies->at(1);
  EXPECT_EQ(roles(body), (std::vector<std::string>{"system", "developer", "user", "assistant", "user"})); EXPECT(rt::startsWith(*rt::getString(messagesOf(body)[4], "content"), "Tool result from get_weather:\n"));
}

ASYNC_TEST("probe results are sanitized: no prompts, reasoning, keys or observation data") {
  const auto script = scriptedTransport(); const auto ctx = context(script); std::vector<probes::ProbeScenarioResult> results; json::array serializedResults;
  for (const auto& candidate : scenarios::PROBE_SCENARIOS) { AWAIT(result, candidate.run(ctx)); serializedResults.push_back(probes::toJson(result)); results.push_back(std::move(result)); }
  const auto serialized = rt::jsonStringify(serializedResults);
  for (const auto forbidden : {"Private chain of thought", "The ledger advanced.", "What is 2 + 2?", "Call get_weather with location San Francisco", "probe-run-42", "secret-api-key", "\"location\":\"San Francisco\""}) EXPECT(!rt::includes(serialized, forbidden));
  for (const auto& result : results) for (const auto& turn : result.turns) {
    EXPECT((turn.response && turn.response->contentPreview ? rt::length(*turn.response->contentPreview) : 0) <= 120); ASSERT(turn.request); EXPECT_EQ(turn.request->model, "gpt-oss-120b");
    if (turn.request->style == "classifier") { EXPECT(!turn.request->tools); EXPECT_EQ(turn.request->responseFormat, "none"); EXPECT_EQ(turn.request->maxCompletionTokens, std::optional<double>(128)); }
  }
}

ASYNC_TEST("the full manifest completes under the scripted transport with no harness failures") {
  const auto script = scriptedTransport(); const auto ctx = context(script);
  for (const auto& candidate : scenarios::PROBE_SCENARIOS) { AWAIT(result, candidate.run(ctx)); EXPECT(result.outcome != "failed"); EXPECT(!result.failure); }
}

#include "tests/harmony_probes_differential_vectors.inc"

namespace {
  template <class T> json::value optionalJson(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }
  json::object normalizationJson(const adapter::NormalizeHarmonyChatCompletionResult& result) {
    if (const auto* error = std::get_if<adapter::HarmonyNormalizationError>(&result)) return {{"error", error->error}};
    const auto& response = std::get<uos::harmony::types::NormalizedAssistantResponse>(result);
    json::array turns;
    for (const auto& turn : response.turns) turns.push_back(std::visit([](const auto& item) -> json::value {
      json::object value{{"kind", item.kind}};
      if constexpr (std::is_same_v<std::decay_t<decltype(item)>, uos::harmony::types::HarmonyTurnToolCall>) {
        value["recipient"] = item.recipient; value["name"] = item.name; value["arguments"] = item.arguments;
      } else value["text"] = item.text;
      return value;
    }, turn));
    json::array calls;
    for (const auto& call : response.toolCalls) calls.push_back(json::object{{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
    const auto& shape = response.shape;
    const json::object shapeJson{{"contentPresent", shape.contentPresent}, {"contentChars", shape.contentChars}, {"reasoningField", shape.reasoningField},
      {"reasoningChars", shape.reasoningChars}, {"toolCallsField", shape.toolCallsField}, {"toolCallCount", shape.toolCallCount}, {"finishReason", optionalJson(shape.finishReason)}, {"refusal", shape.refusal}};
    return {{"turns", std::move(turns)}, {"analysis", json::value_from(response.analysis)}, {"content", optionalJson(response.content)}, {"toolCalls", std::move(calls)},
      {"finishReason", optionalJson(response.finishReason)}, {"refusal", optionalJson(response.refusal)}, {"shape", shapeJson}, {"id", response.id}, {"model", response.model}, {"created", response.created}};
  }
  const json::array& differentialVectors() {
    static const json::value vectors = rt::jsonParse(HARMONY_PROBES_DIFFERENTIAL_VECTORS); return vectors.as_array();
  }
}

TEST("differential: source normalizer validation and UTF-16 metadata") {
  std::size_t count = 0;
  for (const auto& row : differentialVectors()) {
    if (rt::getString(row, "kind") != "normalize") continue;
    const auto result = adapter::normalizeHarmonyChatCompletion(row.at("input"));
    const auto actual = normalizationJson(result);
    // The oracle vector is JSON: JSON.stringify maps -0 to 0. Preserve and check its sign separately.
    if (const auto* response = std::get_if<uos::harmony::types::NormalizedAssistantResponse>(&result))
      EXPECT_EQ(std::signbit(response->created), rt::getBool(row, "createdNegativeZero").value_or(false));
    if (rt::jsonStringify(actual) != rt::jsonStringify(row.at("expected"))) {
      std::fprintf(stderr, "    normalizer input: %s\n    actual: %s\n    expected: %s\n", rt::jsonStringify(row.at("input")).c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(row.at("expected")).c_str());
      EXPECT(false);
    }
    ++count;
  }
  EXPECT_EQ(count, std::size_t{26});
}

ASYNC_TEST("differential: source scenario results and complete dispatched bodies") {
  std::size_t count = 0;
  for (const auto& row : differentialVectors()) {
    if (rt::getString(row, "kind") != "scenario") continue;
    const auto replies = row.at("replies").as_array();
    const auto bodies = std::make_shared<json::array>(); const auto position = std::make_shared<std::size_t>(0); const auto tick = std::make_shared<double>(0);
    const adapter::HarmonyTransport transport = [replies, bodies, position](json::object body) {
      bodies->push_back(std::move(body)); const auto& reply = replies[std::min((*position)++, replies.size() - 1)];
      if (const auto reject = rt::getString(reply, "reject")) throw rt::JsError(std::string(*reject));
      return rt::fulfilledTask(rt::Response::json(reply.at("body"), {.status = rt::getNumber(reply, "status").value_or(200)}));
    };
    const auto ctx = probes::createProbeContext(transport, [tick] { return rt::Date((*tick)++); }); const std::string id(*rt::getString(row, "id"));
    AWAIT(result, scenario(id).run(ctx));
    const json::object actual{{"result", probes::toJson(result)}, {"bodies", *bodies}};
    if (!rt::deepEqual(actual, row.at("expected"))) {
      std::fprintf(stderr, "    differential scenario: %s\n    actual: %s\n    expected: %s\n", id.c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(row.at("expected")).c_str());
      EXPECT(false);
    }
    ++count;
  }
  EXPECT_EQ(count, std::size_t{26});
}
