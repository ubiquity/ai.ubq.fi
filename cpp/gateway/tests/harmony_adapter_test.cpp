// Port of tests/harmony-adapter.test.ts. All transport calls use fake fetch and a synthetic key.
// Reliability-harness integration is deferred to w07c3 (the harness module is not available).
// Links: tests/harmony_adapter_test.cpp src/harmony/adapter.cpp src/harmony/probes.cpp src/harmony/classifier.cpp
// src/harmony/conversation.cpp src/harmony/parse.cpp src/harmony/render.cpp src/harmony/types.cpp
// src/provider/cerebras.cpp src/sentinel/upstream_capture.cpp src/utils.cpp
#include "uos/harmony/adapter.hpp"
#include "uos/harmony/conversation.hpp"
#include "uos/harmony/probes.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace adapter = uos::harmony::adapter;
  namespace types = uos::harmony::types;
  namespace conversation = uos::harmony::conversation;
  namespace probes = uos::harmony::probes;
  #define AWAIT(name, expr) auto name##Task = (expr); auto name = co_await std::move(name##Task)
  json::object payload(json::object message = {{"role", "assistant"}, {"content", "It is sunny in San Francisco."}}, std::string finish = "stop") {
    return {{"id", "cmpl-test"}, {"object", "chat.completion"}, {"created", 1234}, {"model", "gpt-oss-120b"},
      {"choices", json::array{json::object{{"index", 0}, {"message", std::move(message)}, {"finish_reason", std::move(finish)}}}},
      {"usage", json::object{{"prompt_tokens", 10}, {"completion_tokens", 8}, {"total_tokens", 18}}}};
  }
  rt::Response okResponse(const json::object& value) { return rt::Response::json(value); }
  json::object toolCall(std::string id, std::string name, std::string arguments) {
    return {{"id", std::move(id)}, {"type", "function"}, {"function", json::object{{"name", std::move(name)}, {"arguments", std::move(arguments)}}}};
  }
  types::NormalizedAssistantResponse normalized(const json::value& value) {
    const auto result = adapter::normalizeHarmonyChatCompletion(value);
    if (const auto* error = std::get_if<adapter::HarmonyNormalizationError>(&result)) throw rt::JsError(error->error);
    return std::get<types::NormalizedAssistantResponse>(result);
  }
  const adapter::RunTurnResultOk& successful(const adapter::RunTurnResult& value) {
    const auto* success = std::get_if<adapter::RunTurnResultOk>(&value);
    if (!success) throw rt::JsError("expected ok");
    return *success;
  }
  std::vector<std::string> roles(const json::object& body) {
    std::vector<std::string> values;
    for (const auto& message : body.at("messages").as_array()) values.emplace_back(*rt::getString(message, "role"));
    return values;
  }
  conversation::Conversation user() { return probes::user("Weather?"); }
  adapter::HarmonyRequestOptions options(std::string style = "generic") { return {.style = std::move(style), .turns = user().turns}; }
}

TEST("generic request carries normalized tools, reasoning effort and no analysis") {
  auto state = conversation::createConversation({types::ConversationTurnUser{.content = "Weather?"},
    types::AssistantTurn{.content = "Calling the weather tool.", .analysis = {"Need to use function get_weather."},
      .toolCalls = {{.id = "call-1", .name = "get_weather", .arguments = "{\"location\":\"SF\"}"}}, .finishReason = "tool_calls"},
    types::ToolResultTurn{.toolCallId = "call-1", .name = "get_weather", .content = "{\"sunny\": true}"}});
  auto request = options(); request.turns = state.turns;
  auto weather = probes::WEATHER_TOOL; weather.strict = true;
  request.tools = std::vector<types::ToolDefinition>{weather, probes::NOTE_TOOL}; request.reasoningEffort = "low"; request.maxCompletionTokens = 64;
  const auto built = adapter::buildCerebrasHarmonyRequest(request);
  EXPECT_EQ(rt::getString(built.body, "model").value_or(""), "gpt-oss-120b");
  EXPECT_EQ(rt::getString(built.body, "reasoning_effort").value_or(""), "low");
  EXPECT_EQ(rt::getNumber(built.body, "max_completion_tokens"), std::optional<double>(64));
  const auto& tools = built.body.at("tools").as_array(); EXPECT_EQ(tools.size(), std::size_t{2});
  EXPECT(tools[0].at("function").at("strict") == false); EXPECT(tools[1].at("function").at("strict") == false);
  EXPECT_EQ(roles(built.body), (std::vector<std::string>{"user", "assistant", "tool"}));
  EXPECT(!rt::includes(rt::jsonStringify(built.body), "Need to use function get_weather"));
}

TEST("native request renders Harmony system/developer and omits tools and reasoning_effort fields") {
  auto request = options("native"); request.tools = std::vector<types::ToolDefinition>{probes::WEATHER_TOOL}; request.instructions = "You are a weather assistant."; request.reasoningEffort = "high";
  const auto built = adapter::buildCerebrasHarmonyRequest(request);
  EXPECT(!rt::hasOwn(built.body, "tools")); EXPECT(!rt::hasOwn(built.body, "reasoning_effort"));
  EXPECT_EQ(roles(built.body), (std::vector<std::string>{"system", "developer", "user"}));
  const auto& messages = built.body.at("messages").as_array();
  const std::string system(*rt::getString(messages[0], "content"));
  const std::string developer(*rt::getString(messages[1], "content"));
  EXPECT(rt::includes(system, "Reasoning: high")); EXPECT(rt::includes(system, "Current date: 2026-01-01"));
  EXPECT(rt::includes(developer, "namespace functions {")); EXPECT(rt::includes(developer, "type get_weather = (_: {"));
}

TEST("tools combined with a structured response format is blocked unless probed") {
  auto request = options(); request.tools = std::vector<types::ToolDefinition>{probes::WEATHER_TOOL};
  request.responseFormat = types::ResponseFormatParamJsonSchema{.json_schema = {.name = "answer", .strict = true, .schema = {}}};
  bool refused = false;
  try { (void)adapter::buildCerebrasHarmonyRequest(request); } catch (const types::HarmonyAdapterError& error) { refused = error.code == "unproven-combination"; }
  EXPECT(refused);
  request.responseFormat = types::ResponseFormatParamJsonObject{}; request.combinationPolicy = "probe";
  const auto built = adapter::buildCerebrasHarmonyRequest(request);
  EXPECT(built.body.at("response_format").at("type") == "json_object"); EXPECT(built.body.at("tools").is_array());
}

TEST("preserve mode keeps per-tool strictness flags for protocol evidence") {
  auto request = options(); auto weather = probes::WEATHER_TOOL; weather.strict = true;
  request.tools = std::vector<types::ToolDefinition>{weather, probes::NOTE_TOOL}; request.toolStrictnessMode = "preserve";
  const auto built = adapter::buildCerebrasHarmonyRequest(request);
  const auto& tools = built.body.at("tools").as_array(); EXPECT(tools[0].at("function").at("strict") == true); EXPECT(!rt::hasOwn(tools[1].at("function").as_object(), "strict"));
}

TEST("normalizes reasoning_content, tool calls and finish reason from provider payloads") {
  const auto response = normalized(payload({{"role", "assistant"}, {"content", nullptr}, {"reasoning_content", "Need to use function get_weather."},
    {"tool_calls", json::array{toolCall("call_1", "get_weather", "{\"location\": \"SF\"}")}}}, "tool_calls"));
  EXPECT_EQ(response.analysis, (std::vector<std::string>{"Need to use function get_weather."}));
  EXPECT_EQ(response.toolCalls, (std::vector<types::ToolCall>{{.id = "call_1", .name = "get_weather", .arguments = "{\"location\":\"SF\"}"}}));
  EXPECT_EQ(response.finishReason, std::optional<std::string>("tool_calls")); EXPECT_EQ(response.shape.reasoningField, "reasoning_content");
}

TEST("parses Harmony raw output from content when the provider does not translate calls") {
  const auto response = normalized(payload({{"role", "assistant"}, {"content", "<|channel|>analysis<|message|>Need to use function get_weather.<|end|>"
    "<|start|>assistant<|channel|>commentary to=functions.get_weather <|constrain|>json<|message|>{\"location\":\"SF\"}<|call|>"}}, "tool_calls"));
  EXPECT_EQ(response.toolCalls, (std::vector<types::ToolCall>{{.id = "harmony-call-1", .name = "get_weather", .arguments = "{\"location\":\"SF\"}"}}));
}

TEST("supports multiple parallel tool calls with distinct ids") {
  const auto response = normalized(payload({{"role", "assistant"}, {"content", nullptr}, {"tool_calls", json::array{
    toolCall("call_a", "get_weather", "{\"location\":\"SF\"}"), toolCall("call_b", "get_weather", "{\"location\":\"Tokyo\"}")}}}, "tool_calls"));
  EXPECT_EQ(response.toolCalls.size(), std::size_t{2}); EXPECT_EQ(response.toolCalls[0].id, "call_a"); EXPECT_EQ(response.toolCalls[1].id, "call_b");
}

TEST("rejects a model mismatch in the upstream reply") {
  auto value = payload(); value["model"] = "some-other-model";
  const auto response = adapter::normalizeHarmonyChatCompletion(json::value(value));
  const auto* error = std::get_if<adapter::HarmonyNormalizationError>(&response); ASSERT(error != nullptr); EXPECT(rt::includes(error->error, "instead of"));
}

ASYNC_TEST("runHarmonyTurn surfaces upstream validation errors as sanitized fingerprints") {
  rt::env::ScopedEnvironment environment({});
  const auto transport = adapter::createCerebrasTransport({.apiKey = "test-key", .fetcher = rt::Fetch([](rt::RequestInfo, rt::RequestInit) {
    return rt::Response::json(json::object{{"error", json::object{{"message", "Tools with mixed values for 'strict' are not allowed"}, {"code", "invalid_request_error"}}}}, {.status = 400});
  })});
  auto request = options(); request.tools = std::vector<types::ToolDefinition>{probes::WEATHER_TOOL, probes::NOTE_TOOL};
  AWAIT(result, adapter::runHarmonyTurn(request, transport));
  const auto* failure = std::get_if<adapter::RunTurnResultNotOk>(&result); ASSERT(failure != nullptr); EXPECT_EQ(failure->status, 400);
  ASSERT(failure->upstreamError.has_value()); EXPECT_EQ(failure->upstreamError->code, std::optional<std::string>("invalid_request_error"));
  EXPECT(rt::includes(failure->upstreamError->message.value_or(""), "mixed values"));
}

ASYNC_TEST("runHarmonyTurn completes a tool call then a final answer through the real transport") {
  rt::env::ScopedEnvironment environment({});
  std::vector<json::object> bodies;
  const auto transport = adapter::createCerebrasTransport({.apiKey = "test-key", .fetcher = rt::Fetch([&bodies](rt::RequestInfo, rt::RequestInit init) {
    const auto* text = init.body ? std::get_if<std::string>(&*init.body) : nullptr;
    bodies.push_back(rt::jsonParse(text ? *text : "{}").as_object());
    return bodies.size() == 1 ? okResponse(payload({{"role", "assistant"}, {"content", nullptr}, {"reasoning_content", "Private chain of thought."},
      {"tool_calls", json::array{toolCall("call_a", "get_weather", "{\"location\":\"SF\"}")}}}, "tool_calls")) : okResponse(payload());
  })});
  auto request = options(); request.tools = std::vector<types::ToolDefinition>{probes::WEATHER_TOOL}; request.reasoningEffort = "low";
  AWAIT(first, adapter::runHarmonyTurn(request, transport));
  const auto& firstResponse = successful(first).normalized; EXPECT_EQ(firstResponse.toolCalls.size(), std::size_t{1}); EXPECT_EQ(firstResponse.toolCalls[0].name, "get_weather");
  const auto afterCall = conversation::advanceConversation(user(), firstResponse);
  const auto withResult = conversation::appendToolResult(afterCall, "call_a", "get_weather", "{\"sunny\": true}"); request.turns = withResult.turns;
  AWAIT(second, adapter::runHarmonyTurn(request, transport));
  const auto& secondResponse = successful(second).normalized; EXPECT_EQ(secondResponse.content, std::optional<std::string>("It is sunny in San Francisco.")); EXPECT(secondResponse.analysis.empty());
  EXPECT_EQ(roles(bodies[1]), (std::vector<std::string>{"user", "assistant", "tool"})); EXPECT(!rt::includes(rt::jsonStringify(bodies[1]), "Private chain of thought"));
  EXPECT(rt::deepEqual(bodies[1].at("messages").as_array()[1].at("tool_calls"), json::array{toolCall("call_a", "get_weather", "{\"location\":\"SF\"}")}));
}

TEST("native user-role replay renders the tool result as a user message") {
  const auto state = conversation::appendToolResult(user(), "call_a", "get_weather", "{\"sunny\": true}");
  auto request = options("native"); request.turns = state.turns; request.instructions = "x"; request.nativeToolResultStyle = "user-role";
  const auto built = adapter::buildCerebrasHarmonyRequest(request); const auto& message = built.body.at("messages").as_array()[3];
  EXPECT(message.at("role") == "user"); EXPECT(rt::startsWith(*rt::getString(message, "content"), "Tool result from get_weather:\n"));
}

ASYNC_TEST("consecutive generic turns keep the conversation consistent") {
  rt::env::ScopedEnvironment environment({});
  const auto transport = adapter::createCerebrasTransport({.apiKey = "test-key", .fetcher = rt::Fetch([](rt::RequestInfo, rt::RequestInit) {
    return okResponse(payload({{"role", "assistant"}, {"content", "Done."}, {"reasoning_content", "think"}}));
  })});
  auto state = conversation::appendUser(conversation::createConversation(), "First question."); auto request = options(); request.turns = state.turns; request.reasoningEffort = "low";
  AWAIT(first, adapter::runHarmonyTurn(request, transport)); state = conversation::advanceConversation(state, successful(first).normalized);
  state = conversation::appendUser(state, "Follow-up."); request.turns = state.turns;
  AWAIT(second, adapter::runHarmonyTurn(request, transport)); EXPECT_EQ(successful(second).normalized.analysis.size(), std::size_t{1});
}
