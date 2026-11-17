// Links: tests/surplus_claude_chat_request_test.cpp src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/provider/surplus.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/bounded_response_body.cpp src/inference_deadline.cpp src/responses_stream.cpp src/utils.cpp
// Port of tests/surplus-claude-chat-request.test.ts, plus differential vectors from the TS module under
// bun (tests/surplus_claude_chat_request_vectors.inc) that pin the exact JSON.stringify of the Chat body.
#include "uos/provider/surplus.hpp"
#include "uos/provider/surplus_claude_chat_request.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace uos;
using provider::surplus::fetchSurplusResponses;
using provider::surplus::SurplusAuthenticatedFetchOptions;
using provider::surplus::SurplusResponsesResult;
using provider::surplus_claude_chat_request::buildSurplusClaudeChatRequest;
using provider::surplus_claude_chat_request::ClaudeChatRequestOptions;

namespace {
  json::object parse(std::string_view text) { return rt::jsonParse(text).as_object(); }

  json::object base() { return parse(R"({"model":"claude-opus-5.5","input":"hello","max_output_tokens":128,"stream":false,"store":false})"); }

  // `{ ...base(), ...overrides }`
  json::object withBase(std::string_view overrides) { return rt::objectSpread(base(), parse(overrides)); }

  std::string fetchUrl(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->href();
    return std::get<rt::Request>(input).url();
  }

  std::string bodyText(const rt::RequestInit& init) {
    if (init.body && std::holds_alternative<std::string>(*init.body)) return std::get<std::string>(*init.body);
    return "undefined";
  }

  void expectJson(const json::value& actual, const json::value& expected, const char* text, const char* file, int line) {
    if (!rt::deepEqual(actual, expected))
      rt::test::report(file, line, std::string("expected ") + text + " to deep-equal " + json::serialize(expected) + "\n      actual:   " + json::serialize(actual),
                       false);
  }
#define EXPECT_JSON(actual, expected) expectJson((actual), (expected), #actual, __FILE__, __LINE__)

  const json::value& member(const json::object& object, std::string_view key) {
    static const json::value missing;
    const json::value* found = rt::get(object, key);
    return found ? *found : missing;
  }

  struct Vector {
    std::string_view name;
    std::string_view body;
    std::optional<double> catalog;
    std::string_view options;
    std::optional<std::string> result;
  };

  const std::vector<Vector>& vectors() {
    static const std::vector<Vector> list{
#include "tests/surplus_claude_chat_request_vectors.inc"
    };
    return list;
  }

  ClaudeChatRequestOptions optionsFrom(std::string_view text) {
    const json::object object = parse(text);
    ClaudeChatRequestOptions options;
    if (auto value = rt::getString(object, "provider")) options.provider = std::string(*value);
    if (auto value = rt::getString(object, "reasoningReplayScope")) options.reasoningReplayScope = std::string(*value);
    return options;
  }
}

TEST("Surplus Claude Chat preserves Max, affinity, stream usage and original instruction order") {
  const json::object body = withBase(R"({
    "stream": true,
    "instructions": "global",
    "input": [
      { "role": "user", "content": "first" },
      { "role": "developer", "content": "later instructions" },
      { "role": "user", "content": "next" }
    ],
    "reasoning": { "effort": "max", "summary": "detailed" },
    "prompt_cache_key": "stable-affinity"
  })");
  const std::string before = rt::jsonStringify(json::value(body));
  const auto result = buildSurplusClaudeChatRequest(body);
  ASSERT(result);
  EXPECT_JSON(json::value(result->body), rt::jsonParse(R"({
    "model": "claude-opus-5.5",
    "messages": [
      { "role": "system", "content": [{ "type": "text", "text": "global" }] },
      { "role": "user", "content": [{ "type": "text", "text": "first" }] },
      { "role": "developer", "content": [{ "type": "text", "text": "later instructions" }] },
      { "role": "user", "content": [{ "type": "text", "text": "next", "cache_control": { "type": "ephemeral" } }] }
    ],
    "stream": true,
    "stream_options": { "include_usage": true },
    "max_tokens": 128,
    "reasoning_effort": "max",
    "prompt_cache_key": "stable-affinity"
  })"));
  EXPECT_EQ(result->promptCacheMode, std::string("implicit"));
  EXPECT_EQ(rt::jsonStringify(json::value(body)), before);
  const auto ultra = buildSurplusClaudeChatRequest(withBase(R"({ "reasoning": { "effort": "ultra" } })"));
  ASSERT(ultra);
  EXPECT_EQ(rt::getString(ultra->body, "reasoning_effort"), std::optional<std::string_view>("max"));
}

TEST("Surplus Claude Chat preserves namespaced strict functions and reversible freeform tools") {
  const auto result = buildSurplusClaudeChatRequest(withBase(R"json({
    "tools": [
      { "type": "namespace", "name": "functions", "tools": [{ "type": "function", "name": "read", "strict": true, "parameters": { "type": "object", "properties": {} } }] },
      { "type": "custom", "name": "execute", "description": "Run source", "format": { "type": "text" } }
    ],
    "input": [
      { "role": "user", "content": "run" },
      { "type": "function_call", "namespace": "functions", "name": "read", "call_id": "c1", "arguments": "{}" },
      { "type": "custom_tool_call", "name": "execute", "call_id": "c2", "input": "print(1)" },
      { "type": "function_call_output", "call_id": "c1", "output": "file" },
      { "type": "custom_tool_call_output", "call_id": "c2", "output": "1" }
    ],
    "tool_choice": { "type": "function", "namespace": "functions", "name": "read" },
    "parallel_tool_calls": false
  })json"));
  ASSERT(result);
  const auto* read = result->toolNames.get("functions_read");
  ASSERT(read);
  EXPECT_EQ(read->name, std::string("read"));
  EXPECT_EQ(read->namespace_, std::optional<std::string>("functions"));
  EXPECT_EQ(result->customToolNames.has("execute"), true);
  EXPECT_JSON(member(result->body, "tool_choice"), rt::jsonParse(R"({ "type": "function", "function": { "name": "functions_read" } })"));
  const json::array& tools = member(result->body, "tools").as_array();
  EXPECT_JSON(tools.at(0).at("function").at("strict"), json::value(true));
  const json::array& messages = member(result->body, "messages").as_array();
  EXPECT_JSON(messages.at(1).at("tool_calls"), rt::jsonParse(R"([
    { "id": "c1", "type": "function", "function": { "name": "functions_read", "arguments": "{}" } },
    { "id": "c2", "type": "function", "function": { "name": "execute", "arguments": "{\"input\":\"print(1)\"}" } }
  ])"));
  EXPECT_JSON(messages.at(3), rt::jsonParse(R"({ "role": "tool", "tool_call_id": "c2", "content": [{ "type": "text", "text": "1", "cache_control": { "type": "ephemeral" } }] })"));
}

TEST("Surplus Claude Chat retains image precision and explicit cache policy without adding a marker") {
  const auto result = buildSurplusClaudeChatRequest(withBase(R"({
    "input": [
      {
        "role": "user",
        "content": [
          { "type": "input_text", "text": "stable", "prompt_cache_breakpoint": { "mode": "explicit" } },
          { "type": "input_image", "image_url": "https://example.test/a.png", "detail": "original" },
          { "type": "input_text", "text": "changing" }
        ]
      }
    ]
  })"));
  ASSERT(result);
  EXPECT_EQ(result->promptCacheMode, std::string("explicit"));
  EXPECT_EQ(result->explicitBreakpointCount, 1.0);
  EXPECT_JSON(member(result->body, "messages"), rt::jsonParse(R"([
    {
      "role": "user",
      "content": [
        { "type": "text", "text": "stable", "cache_control": { "type": "ephemeral" } },
        { "type": "image_url", "image_url": { "url": "https://example.test/a.png", "detail": "original" } },
        { "type": "text", "text": "changing" }
      ]
    }
  ])"));
  const auto disabled = buildSurplusClaudeChatRequest(withBase(R"({ "prompt_cache_options": { "mode": "explicit" } })"));
  ASSERT(disabled);
  EXPECT_JSON(member(disabled->body, "messages"), rt::jsonParse(R"([{ "role": "user", "content": [{ "type": "text", "text": "hello" }] }])"));
}

TEST("Surplus Claude Chat uses the catalog output limit and falls back without losing incompatible controls") {
  // `{ ...base(), max_output_tokens: undefined }`: JSON has no undefined, so the member is absent.
  json::object withoutCap = base();
  rt::objectDelete(withoutCap, "max_output_tokens");
  const auto capped = buildSurplusClaudeChatRequest(withoutCap, 128000.0);
  ASSERT(capped);
  EXPECT_EQ(rt::getNumber(capped->body, "max_tokens"), std::optional<double>(128000));
  const std::vector<std::string_view> overrides{
    R"({ "model": "gpt-6-sol" })",
    R"({ "input": [{ "type": "reasoning", "encrypted_content": "opaque" }] })",
    R"({ "input": [{ "type": "function_call", "name": "read", "call_id": "c", "arguments": "{}", "prompt_cache_breakpoint": { "mode": "explicit" } }] })",
    R"({ "tools": [{ "type": "custom", "name": "patch", "format": { "type": "grammar", "syntax": "lark", "definition": "start: WORD" } }] })",
    R"({ "tools": [{ "type": "namespace", "name": "functions", "tools": [{ "type": "web_search" }] }] })",
    R"({ "tools": [{ "type": "web_search" }] })",
    R"({ "prompt_cache_retention": "24h" })",
    R"({ "prompt_cache_options": { "ttl": "30m" } })",
    R"({ "previous_response_id": "resp_unknown" })",
    R"({ "context_management": [{ "type": "compaction" }] })",
    R"({ "text": { "verbosity": "high" } })",
  };
  for (const std::string_view override_ : overrides) {
    const json::object body = withBase(override_);
    const std::string before = rt::jsonStringify(json::value(body));
    if (buildSurplusClaudeChatRequest(body)) rt::test::report(__FILE__, __LINE__, "expected null for " + before, false);
    EXPECT_EQ(rt::jsonStringify(json::value(body)), before);
  }
}

ASYNC_TEST("Surplus Claude Chat fetch preserves dispatch guards, Max, measured cache and authoritative cost") {
  std::vector<std::string> actions;
  std::string requested;
  json::value wire;
  rt::Headers headers;
  SurplusAuthenticatedFetchOptions options;
  options.apiKey = std::string("test-provider-key");
  options.beforeDispatch = [&actions] {
    actions.push_back("quota");
    return rt::fulfilledTask(std::optional<api_key_policy::ApiKeyProviderDispatch>());
  };
  options.onDispatch = [&actions] { actions.push_back("dispatch"); };
  options.fetcher = rt::Fetch([&](rt::RequestInfo url, rt::RequestInit init) -> rt::Response {
    actions.push_back("fetch");
    requested = fetchUrl(url);
    headers = rt::Headers(init.headers.value_or(rt::Headers()));
    wire = rt::jsonParse(bodyText(init));
    rt::ResponseInit responseInit;
    responseInit.headers = rt::Headers{{"X-Request-Id", "chat-request"}};
    return rt::Response::json(rt::jsonParse(R"({
      "id": "chatcmpl_test",
      "object": "chat.completion",
      "model": "claude-opus-5.5",
      "created": 1,
      "choices": [{ "index": 0, "finish_reason": "stop", "message": { "role": "assistant", "content": "OK" } }],
      "usage": { "prompt_tokens": 1043, "completion_tokens": 4, "total_tokens": 1047, "prompt_tokens_details": { "cached_tokens": 1028 }, "buyer_cost_micro": 69 }
    })"),
                              std::move(responseInit));
  });
  auto task = fetchSurplusResponses(json::value(withBase(R"({ "reasoning": { "effort": "max" } })")), options);
  const SurplusResponsesResult result = co_await std::move(task);

  EXPECT_EQ(requested, std::string("https://api.surplusintelligence.ai/v1/chat/completions"));
  EXPECT_EQ(headers.get("Authorization"), std::optional<std::string>("Bearer test-provider-key"));
  EXPECT_EQ(headers.get("X-SI-Mask-Secrets"), std::optional<std::string>("on"));
  for (const std::string_view privateHeader : {"conversation_id", "session-id", "thread-id", "x-client-request-id"}) EXPECT_EQ(headers.has(privateHeader), false);
  EXPECT_EQ(actions, (std::vector<std::string>{"quota", "dispatch", "fetch"}));
  const auto expectedWire = buildSurplusClaudeChatRequest(withBase(R"({ "reasoning": { "effort": "max" } })"));
  ASSERT(expectedWire);
  EXPECT_JSON(wire, json::value(expectedWire->body));
  EXPECT_EQ(result.request_id, std::optional<std::string>("chat-request"));
  EXPECT_EQ(result.prompt_cache_mode, std::optional<std::string>("implicit"));
  auto reading = result.response.json();
  const json::value response = co_await std::move(reading);
  EXPECT_EQ(rt::getString(response, "object"), std::optional<std::string_view>("response"));
  const json::value& usage = response.at("usage");
  EXPECT_EQ(rt::getNumber(usage, "input_tokens"), std::optional<double>(1043));
  EXPECT_EQ(rt::getNumber(usage.at("input_tokens_details"), "cached_tokens"), std::optional<double>(1028));
  EXPECT_EQ(rt::getNumber(usage, "buyer_cost_micro"), std::optional<double>(69));
}

ASYNC_TEST("Surplus incompatible Claude requests retain the original Responses wire and an honest cache warning") {
  const json::object body = withBase(R"({ "input": [{ "type": "reasoning", "encrypted_content": "opaque" }], "reasoning": { "effort": "max" } })");
  std::string requested;
  json::value wire;
  SurplusAuthenticatedFetchOptions options;
  options.apiKey = std::string("test-provider-key");
  options.fetcher = rt::Fetch([&](rt::RequestInfo url, rt::RequestInit init) -> rt::Response {
    requested = fetchUrl(url);
    wire = rt::jsonParse(bodyText(init));
    return rt::Response::json(rt::jsonParse(R"({ "object": "response", "status": "completed", "usage": null })"));
  });
  auto task = fetchSurplusResponses(json::value(body), options);
  const SurplusResponsesResult result = co_await std::move(task);
  EXPECT_EQ(requested, std::string("https://api.surplusintelligence.ai/v1/responses"));
  EXPECT_JSON(wire, json::value(body));
  EXPECT_EQ(result.response.headers().get("x-uos-warning"), std::optional<std::string>("surplus_claude_native_prompt_cache_unavailable"));
}

ASYNC_TEST("Surplus Claude signed Chat thinking survives a namespaced tool loop only in its authenticated scope") {
  const json::value assistant = rt::jsonParse(R"({
    "role": "assistant",
    "content": "Reading",
    "reasoning_content": "Synthetic thought",
    "reasoning_details": [{ "type": "reasoning.text", "text": "Synthetic thought", "format": "anthropic-claude-v1", "index": 0, "signature": "signed-test-fixture" }],
    "tool_calls": [{ "id": "call-scoped", "type": "function", "function": { "name": "functions_read", "arguments": "{\"path\":\"a.txt\"}" } }]
  })");
  const json::object request = withBase(R"({
    "reasoning": { "effort": "max" },
    "tools": [{ "type": "namespace", "name": "functions", "tools": [{ "type": "function", "name": "read", "parameters": { "type": "object" } }] }],
    "input": [{ "role": "user", "content": "read" }]
  })");
  json::object completion = parse(R"({
    "id": "chatcmpl_scoped_test",
    "object": "chat.completion",
    "model": "claude-opus-5.5",
    "created": 1,
    "choices": [{ "index": 0, "finish_reason": "tool_calls" }],
    "usage": { "prompt_tokens": 1043, "completion_tokens": 20, "total_tokens": 1063, "prompt_tokens_details": { "cached_tokens": 1028 }, "buyer_cost_micro": 100 }
  })");
  completion["choices"].as_array().at(0).as_object()["message"] = assistant;
  SurplusAuthenticatedFetchOptions options;
  options.apiKey = std::string("test-provider-key");
  options.reasoningReplayScope = "caller-scope-chat-loop";
  options.fetcher = rt::Fetch([completion](rt::RequestInfo, rt::RequestInit) -> rt::Response { return rt::Response::json(json::value(completion)); });
  auto task = fetchSurplusResponses(json::value(request), options);
  const SurplusResponsesResult first = co_await std::move(task);
  auto reading = first.response.json();
  const json::value reply = co_await std::move(reading);

  json::object replay = request;
  json::array input = request.at("input").as_array();
  for (const json::value& item : reply.at("output").as_array()) input.push_back(item);
  input.push_back(rt::jsonParse(R"({ "type": "function_call_output", "call_id": "call-scoped", "output": "file" })"));
  replay["input"] = std::move(input);
  const auto converted =
    buildSurplusClaudeChatRequest(replay, std::nullopt, ClaudeChatRequestOptions{.provider = "surplus", .reasoningReplayScope = "caller-scope-chat-loop"});
  ASSERT(converted);
  const json::array& messages = member(converted->body, "messages").as_array();
  EXPECT_JSON(messages.at(1), assistant);
  EXPECT_EQ(rt::getString(converted->body, "reasoning_effort"), std::optional<std::string_view>("max"));
  EXPECT(!buildSurplusClaudeChatRequest(replay, std::nullopt, ClaudeChatRequestOptions{.provider = "surplus", .reasoningReplayScope = "other-caller"}));
  EXPECT(!buildSurplusClaudeChatRequest(replay, std::nullopt, ClaudeChatRequestOptions{.provider = "metered", .reasoningReplayScope = "caller-scope-chat-loop"}));
  EXPECT_EQ(rt::includes(rt::jsonStringify(reply), "signed-test-fixture"), false);
}

TEST("differential: Chat bodies match the TS module byte for byte") {
  for (const Vector& vector : vectors()) {
    const json::object body = parse(vector.body);
    const auto result = buildSurplusClaudeChatRequest(body, vector.catalog, optionsFrom(vector.options));
    std::optional<std::string> actual;
    if (result) {
      json::array toolNames;
      for (const auto& [projected, original] : result->toolNames) {
        json::object name;
        name["name"] = original.name;
        name["namespace"] = original.namespace_ ? json::value(*original.namespace_) : json::value(nullptr);
        toolNames.push_back(json::array{json::value(projected), json::value(std::move(name))});
      }
      json::array customToolNames;
      for (const std::string& custom : result->customToolNames) customToolNames.push_back(json::value(custom));
      json::object summary;
      summary["body"] = result->body;
      summary["promptCacheMode"] = result->promptCacheMode;
      summary["explicitBreakpointCount"] = rt::makeNumber(result->explicitBreakpointCount);
      summary["toolNames"] = std::move(toolNames);
      summary["customToolNames"] = std::move(customToolNames);
      actual = rt::jsonStringify(json::value(std::move(summary)));
    }
    if (actual != vector.result)
      rt::test::report(__FILE__, __LINE__,
                       std::string(vector.name) + "\n      actual:   " + actual.value_or("null") + "\n      expected: " + vector.result.value_or("null"), false);
  }
}
