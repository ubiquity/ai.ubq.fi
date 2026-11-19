// Links: tests/metered_claude_cache_test.cpp src/provider/metered.cpp src/provider/surplus_claude_request.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_response_wire.cpp src/bounded_response_body.cpp src/inference_deadline.cpp src/responses_stream.cpp src/utils.cpp
// Port of tests/metered-claude-cache.test.ts.
#include "uos/provider/metered.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace uos;
using provider::metered::fetchMeteredModels;
using provider::metered::FetchMeteredModelsOptions;
using provider::metered::fetchMeteredResponses;
using provider::metered::fetchMeteredTokenLogs;
using provider::metered::MeteredAuthenticatedFetchOptions;
using provider::metered::MeteredFetch;
using provider::metered::MeteredResponsesResult;
using provider::metered::MeteredTokenLogEntry;
using provider::metered::MeteredTokenLogFetchOptions;
using provider::metered::resetMeteredModelsCacheForTest;

namespace {
  using JsonRecord = json::object;
  constexpr std::string_view model = "claude-opus-5-5";

  json::value parse(std::string_view text) { return rt::jsonParse(text); }

  // `new Response(JSON.stringify(body), { headers: { "content-type": "application/json", ...Object.fromEntries(new Headers(headers)) } })`
  rt::Response jsonResponse(const json::value& body, const rt::Headers& headers = rt::Headers()) {
    rt::Headers merged{{"content-type", "application/json"}};
    for (const auto& [name, value] : headers.entries()) merged.set(name, value);
    rt::ResponseInit init;
    init.headers = std::move(merged);
    return rt::Response(rt::BodyInit(rt::jsonStringify(body)), std::move(init));
  }

  json::object nativeMessage(std::string_view id = "native-openlux-cache", std::optional<json::array> content = std::nullopt) {
    json::object message = parse(R"({
      "type": "message",
      "role": "assistant",
      "stop_reason": "end_turn",
      "usage": {
        "input_tokens": 15,
        "cache_read_input_tokens": 1028,
        "cache_creation_input_tokens": 0,
        "output_tokens": 64,
        "output_tokens_details": { "thinking_tokens": 60 }
      }
    })")
                                 .as_object();
    json::object ordered;
    ordered["id"] = json::string(id);
    ordered["type"] = message["type"];
    ordered["role"] = message["role"];
    ordered["model"] = json::string(model);
    ordered["content"] = content ? json::value(*content) : parse(R"([{ "type": "text", "text": "OK" }])");
    ordered["stop_reason"] = message["stop_reason"];
    ordered["usage"] = message["usage"];
    return ordered;
  }

  std::string requestUrl(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->toString();
    return std::get<rt::Request>(input).url();
  }

  struct Call {
    std::string url;
    json::object body;
    rt::Headers headers;
  };

  struct Capture {
    std::shared_ptr<std::vector<Call>> calls;
    MeteredFetch fetcher;
  };

  Capture capture(std::function<rt::Response()> upstream = [] { return jsonResponse(json::value(nativeMessage())); }) {
    auto calls = std::make_shared<std::vector<Call>>();
    MeteredFetch fetcher([calls, upstream](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
      if (!init.body || !std::holds_alternative<std::string>(*init.body)) throw rt::JsError("expected a string request body");
      calls->push_back(Call{
        .url = requestUrl(input),
        .body = rt::jsonParse(std::get<std::string>(*init.body)).as_object(),
        .headers = rt::Headers(init.headers.value_or(rt::Headers())),
      });
      return upstream();
    });
    return Capture{.calls = calls, .fetcher = fetcher};
  }

  MeteredAuthenticatedFetchOptions fixtureOptions(const MeteredFetch& fetcher) {
    MeteredAuthenticatedFetchOptions options;
    options.apiKey = std::string("fixture-key");
    options.fetcher = fetcher;
    return options;
  }

  rt::Task<void> cancelBody(rt::Response response) {
    if (const std::optional<rt::ByteStream> body = response.body()) {
      auto cancelling = body->cancel(nullptr);
      co_await std::move(cancelling);
    }
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

  // `log(id, other, overrides)`
  json::value logRow(std::string_view id, std::string_view other, std::string_view overrides = "{}") {
    json::object metadata = parse(R"({ "request_id": "metadata-id" })").as_object();
    const json::object otherMembers = parse(other).as_object();
    for (const auto* entry : rt::objectEntries(otherMembers)) rt::objectDefine(metadata, entry->key(), entry->value());
    json::object row;
    row["request_id"] = json::string(id);
    row["model_name"] = json::string(model);
    row["prompt_tokens"] = 15;
    row["completion_tokens"] = 4;
    row["quota"] = 466;
    row["created_at"] = 1'791'704'855;
    row["other"] = rt::jsonStringify(json::value(metadata));
    const json::object overrideMembers = parse(overrides).as_object();
    for (const auto* entry : rt::objectEntries(overrideMembers)) rt::objectDefine(row, entry->key(), entry->value());
    return row;
  }

  rt::Task<std::vector<MeteredTokenLogEntry>> readLogs(json::array items) {
    json::object data;
    data["items"] = items;
    data["total"] = static_cast<std::int64_t>(items.size());
    json::object envelope;
    envelope["success"] = true;
    envelope["data"] = std::move(data);
    const json::value page(std::move(envelope));
    MeteredTokenLogFetchOptions options;
    options.apiKey = std::string("fixture-key");
    options.fetcher = MeteredFetch([page](rt::RequestInfo, rt::RequestInit) -> rt::Response { return jsonResponse(page); });
    auto reading = fetchMeteredTokenLogs(options);
    std::vector<MeteredTokenLogEntry> logs = co_await std::move(reading);
    co_return logs;
  }
}

ASYNC_TEST("OpenLux Claude uses native block caching and preserves actual gross cache/thinking usage") {
  resetMeteredModelsCacheForTest();
  json::object body;
  body["model"] = json::string(model);
  body["instructions"] = "Reusable instructions";
  body["input"] = "Reply exactly OK.";
  body["max_output_tokens"] = 128;
  body["stream"] = false;
  const json::object unchanged = body;
  const Capture captured = capture([] {
    return jsonResponse(json::value(nativeMessage()),
                        rt::Headers{{"x-api-request-id", "provider-billing-id"}, {"x-oneapi-request-id", "legacy-id"}, {"x-request-id", "trace-id"}});
  });
  auto task = fetchMeteredResponses(json::value(body), fixtureOptions(captured.fetcher));
  const MeteredResponsesResult result = co_await std::move(task);
  ASSERT_EQ(captured.calls->size(), std::size_t{1});
  const Call& call = captured.calls->at(0);
  EXPECT_EQ(call.url, std::string("https://api.openlux.ai/v1/messages"));
  json::object expected = parse(R"({
    "max_tokens": 128,
    "stream": false,
    "system": [{ "type": "text", "text": "Reusable instructions" }],
    "messages": [{ "role": "user", "content": [{ "type": "text", "text": "Reply exactly OK.", "cache_control": { "type": "ephemeral" } }] }]
  })")
                            .as_object();
  expected["model"] = json::string(model);
  EXPECT_JSON(json::value(call.body), json::value(expected));
  EXPECT_EQ(call.headers.get("anthropic-version"), std::optional<std::string>("2023-06-01"));
  EXPECT_EQ(call.headers.get("accept"), std::optional<std::string>("application/json"));
  EXPECT_EQ(result.request_id, std::optional<std::string>("provider-billing-id"));
  EXPECT_EQ(result.prompt_cache_mode, std::optional<std::string>("implicit"));
  EXPECT_EQ(result.explicit_breakpoint_count, std::optional<double>(0));
  auto reading = result.response.json();
  const json::value response = co_await std::move(reading);
  EXPECT_EQ(rt::getString(response, "model"), std::optional<std::string_view>(model));
  EXPECT_JSON(response.at("usage"), parse(R"({
    "input_tokens": 1043,
    "input_tokens_details": { "cached_tokens": 1028, "cache_write_tokens": 0 },
    "output_tokens": 64,
    "output_tokens_details": { "reasoning_tokens": 60 },
    "total_tokens": 1107
  })"));
  EXPECT_JSON(json::value(body), json::value(unchanged));
}

ASYNC_TEST("OpenLux omitted generation budget uses the exact documented Opus5.5 maximum") {
  resetMeteredModelsCacheForTest();
  const Capture captured = capture();
  json::object body;
  body["model"] = json::string(model);
  body["input"] = "Hello";
  auto task = fetchMeteredResponses(json::value(body), fixtureOptions(captured.fetcher));
  const MeteredResponsesResult result = co_await std::move(task);
  ASSERT_EQ(captured.calls->size(), std::size_t{1});
  EXPECT_EQ(rt::getNumber(captured.calls->at(0).body, "max_tokens"), std::optional<double>(128'000));
  EXPECT_EQ(rt::getString(captured.calls->at(0).body, "model"), std::optional<std::string_view>(model));
  auto cancelling = cancelBody(result.response);
  co_await std::move(cancelling);
}

ASYNC_TEST("OpenLux uses catalog native capability and output caps without inventing limits for other Claude models") {
  const rt::env::ScopedOverride environment({{"METERED_API_KEY", std::string("discovery-fixture-key")}});
  resetMeteredModelsCacheForTest();
  rt::Reason failure;
  try {
    FetchMeteredModelsOptions discovery;
    discovery.force = true;
    discovery.fetcher = MeteredFetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response {
      return jsonResponse(parse(R"({
        "data": [
          { "id": "claude-catalog-native", "supported_endpoint_types": ["anthropic", "openai"], "top_provider": { "max_completion_tokens": 2048 } },
          { "id": "claude-unknown-cap", "supported_endpoint_types": ["anthropic"], "max_output_tokens": "2048" },
          { "id": "claude-openai-only", "supported_endpoint_types": ["openai"], "max_completion_tokens": 2048 }
        ]
      })"));
    });
    auto discovering = fetchMeteredModels(discovery);
    co_await std::move(discovering);
    const std::vector<std::tuple<std::string, std::string, std::optional<double>>> expectations{
      {"claude-catalog-native", "https://api.openlux.ai/v1/messages", 2048},
      {"claude-unknown-cap", "https://api.openlux.ai/v1/responses", std::nullopt},
      {"claude-openai-only", "https://api.openlux.ai/v1/responses", std::nullopt},
    };
    for (const auto& [id, expectedUrl, expectedMax] : expectations) {
      const Capture captured = capture();
      json::object body;
      body["model"] = json::string(id);
      body["input"] = "Hello";
      auto task = fetchMeteredResponses(json::value(body), fixtureOptions(captured.fetcher));
      const MeteredResponsesResult result = co_await std::move(task);
      ASSERT_EQ(captured.calls->size(), std::size_t{1});
      EXPECT_EQ(captured.calls->at(0).url, expectedUrl);
      EXPECT_EQ(rt::getNumber(captured.calls->at(0).body, "max_tokens"), expectedMax);
      auto cancelling = cancelBody(result.response);
      co_await std::move(cancelling);
    }
  } catch (...) {
    failure = std::current_exception();
  }
  resetMeteredModelsCacheForTest();
  if (failure) std::rethrow_exception(failure);
}

TEST("a new loop can discover Metered models after the shared request was abandoned") {
  const rt::env::ScopedOverride environment({{"METERED_API_KEY", std::string("discovery-fixture-key")}});
  resetMeteredModelsCacheForTest();
  const auto cleanup = rt::scopeExit([] {
    provider::metered::setMeteredModelsFetchForTest({});
    resetMeteredModelsCacheForTest();
  });
  {
    rt::FakeClock clock;
    rt::Loop first(clock);
    const rt::LoopScope scope(first);
    const auto entered = rt::withResolvers();
    const rt::Promise<rt::Response> never;
    provider::metered::setMeteredModelsFetchForTest(MeteredFetch([start = entered.resolve, never](rt::RequestInfo, rt::RequestInit) {
      start();
      return never.wait();
    }));
    const rt::Promise<std::optional<provider::metered::MeteredModelsSnapshot>> pending(fetchMeteredModels());
    rt::blockOn(first, entered.promise.wait());
    EXPECT(pending.pending());
  }
  {
    rt::FakeClock clock;
    rt::Loop second(clock);
    const rt::LoopScope scope(second);
    provider::metered::setMeteredModelsFetchForTest(MeteredFetch([](rt::RequestInfo, rt::RequestInit) {
      return jsonResponse(parse(R"({"data":[{"id":"mock-metered-model"}]})"));
    }));
    const auto snapshot = rt::blockOn(second, fetchMeteredModels());
    ASSERT(snapshot);
    ASSERT_EQ(snapshot->models.size(), std::size_t{1});
    EXPECT_EQ(snapshot->models[0].id, std::string("mock-metered-model"));
  }
}

ASYNC_TEST("OpenLux native streaming exposes reasoning and text through canonical Responses events") {
  json::object start = nativeMessage("native-openlux-stream");
  start["content"] = json::array();
  start["usage"] = parse(R"({ "input_tokens": 15, "cache_read_input_tokens": 1028, "output_tokens": 0 })");
  json::object messageStart;
  messageStart["type"] = "message_start";
  messageStart["message"] = std::move(start);
  json::array events;
  events.push_back(json::value(std::move(messageStart)));
  const json::value tail = parse(R"([
    { "type": "content_block_start", "index": 0, "content_block": { "type": "thinking", "thinking": "", "signature": "" } },
    { "type": "content_block_delta", "index": 0, "delta": { "type": "thinking_delta", "thinking": "Inspect the prompt" } },
    { "type": "content_block_delta", "index": 0, "delta": { "type": "signature_delta", "signature": "stream-signature" } },
    { "type": "content_block_stop", "index": 0 },
    { "type": "content_block_start", "index": 1, "content_block": { "type": "text", "text": "" } },
    { "type": "content_block_delta", "index": 1, "delta": { "type": "text_delta", "text": "OK" } },
    { "type": "content_block_stop", "index": 1 },
    { "type": "message_delta", "delta": { "stop_reason": "end_turn" }, "usage": { "output_tokens": 64, "output_tokens_details": { "thinking_tokens": 60 } } },
    { "type": "message_stop" }
  ])");
  for (const json::value& event : tail.as_array()) events.push_back(event);
  std::string stream;
  for (const json::value& event : events) stream += "event: " + std::string(event.at("type").as_string()) + "\ndata: " + rt::jsonStringify(event) + "\n\n";
  const Capture captured = capture([stream] {
    rt::ResponseInit init;
    init.headers = rt::Headers{{"content-type", "text/event-stream"}};
    return rt::Response(rt::BodyInit(stream), std::move(init));
  });
  json::object body;
  body["model"] = json::string(model);
  body["input"] = "Hello";
  body["stream"] = true;
  body["reasoning"] = parse(R"({ "summary": "auto" })");
  auto task = fetchMeteredResponses(json::value(body), fixtureOptions(captured.fetcher));
  const MeteredResponsesResult result = co_await std::move(task);
  ASSERT_EQ(captured.calls->size(), std::size_t{1});
  EXPECT_EQ(captured.calls->at(0).url, std::string("https://api.openlux.ai/v1/messages"));
  EXPECT_EQ(rt::getBool(captured.calls->at(0).body, "stream"), std::optional<bool>(true));
  EXPECT_EQ(captured.calls->at(0).headers.get("accept"), std::optional<std::string>("text/event-stream"));
  const std::optional<rt::ByteStream> responseBody = result.response.body();
  ASSERT(responseBody);
  std::vector<json::object> delivered;
  const responses_stream::ResponsesStreamIterator iterator = responses_stream::readResponsesStream(*responseBody);
  for (;;) {
    rt::IteratorResult<responses_stream::ResponsesStreamEvent> step = co_await iterator.next();
    if (step.done) break;
    delivered.push_back(step.value->value);
  }
  bool reasoningDelta = false;
  bool textDelta = false;
  for (const json::object& event : delivered) {
    reasoningDelta = reasoningDelta || (rt::getString(event, "type") == std::optional<std::string_view>("response.reasoning_summary_text.delta") &&
                                        rt::getString(event, "delta") == std::optional<std::string_view>("Inspect the prompt"));
    textDelta = textDelta || (rt::getString(event, "type") == std::optional<std::string_view>("response.output_text.delta") &&
                              rt::getString(event, "delta") == std::optional<std::string_view>("OK"));
  }
  EXPECT(reasoningDelta);
  EXPECT(textDelta);
  ASSERT(!delivered.empty());
  const json::object* terminal = rt::getObject(delivered.back(), "response");
  ASSERT(terminal && rt::hasOwn(*terminal, "usage"));
  EXPECT_JSON(member(*terminal, "usage"), parse(R"({
    "input_tokens": 1043,
    "input_tokens_details": { "cached_tokens": 1028 },
    "output_tokens": 64,
    "output_tokens_details": { "reasoning_tokens": 60 },
    "total_tokens": 1107
  })"));
}

ASYNC_TEST("OpenLux preserves explicit cache breakpoints without adding another marker") {
  const Capture captured = capture();
  json::object body = parse(R"({
    "input": [
      {
        "role": "user",
        "content": [
          { "type": "input_text", "text": "stable prefix", "prompt_cache_breakpoint": { "mode": "explicit" } },
          { "type": "input_text", "text": "tail" }
        ]
      }
    ],
    "prompt_cache_options": { "mode": "explicit" },
    "max_output_tokens": 128
  })")
                        .as_object();
  json::object ordered;
  ordered["model"] = json::string(model);
  for (const auto* entry : rt::objectEntries(body)) rt::objectDefine(ordered, entry->key(), entry->value());
  auto task = fetchMeteredResponses(json::value(ordered), fixtureOptions(captured.fetcher));
  const MeteredResponsesResult result = co_await std::move(task);
  EXPECT_EQ(result.prompt_cache_mode, std::optional<std::string>("explicit"));
  EXPECT_EQ(result.explicit_breakpoint_count, std::optional<double>(1));
  ASSERT_EQ(captured.calls->size(), std::size_t{1});
  EXPECT_JSON(member(captured.calls->at(0).body, "messages"), parse(R"([
    {
      "role": "user",
      "content": [
        { "type": "text", "text": "stable prefix", "cache_control": { "type": "ephemeral" } },
        { "type": "text", "text": "tail" }
      ]
    }
  ])"));
  auto cancelling = cancelBody(result.response);
  co_await std::move(cancelling);
}

ASYNC_TEST("OpenLux keeps requested effort and unrepresentable controls on the original Responses route") {
  resetMeteredModelsCacheForTest();
  const std::vector<std::string_view> extras{
    R"({ "reasoning": { "effort": "max", "summary": "auto" } })",
    R"({ "reasoning": { "effort": "none" } })",
    R"({ "prompt_cache_retention": "24h" })",
    R"({ "input": [{ "type": "reasoning", "id": "external-reasoning", "encrypted_content": "opaque" }] })",
    R"({ "model": "claude-unadvertised", "max_output_tokens": 128 })",
  };
  for (const std::string_view extra : extras) {
    json::object body;
    body["model"] = json::string(model);
    body["input"] = "Hello";
    body["max_output_tokens"] = 128;
    const json::object extraMembers = parse(extra).as_object();
    for (const auto* entry : rt::objectEntries(extraMembers)) rt::objectDefine(body, entry->key(), entry->value());
    const Capture captured = capture([] {
      rt::ResponseInit init;
      init.status = 400;
      init.headers = rt::Headers{{"x-uos-warning", "existing-warning"}};
      return rt::Response(rt::BodyInit(std::string("upstream rejected")), std::move(init));
    });
    MeteredAuthenticatedFetchOptions options = fixtureOptions(captured.fetcher);
    options.reasoningReplayScope = "fixture-principal";
    auto task = fetchMeteredResponses(json::value(body), options);
    const MeteredResponsesResult result = co_await std::move(task);
    ASSERT_EQ(captured.calls->size(), std::size_t{1});
    EXPECT_EQ(captured.calls->at(0).url, std::string("https://api.openlux.ai/v1/responses"));
    EXPECT_JSON(json::value(captured.calls->at(0).body), json::value(body));
    EXPECT_EQ(result.response.status(), 400);
    EXPECT_EQ(result.response.headers().get("x-uos-warning"), std::optional<std::string>("existing-warning, openlux_claude_native_prompt_cache_unavailable"));
    EXPECT_EQ(result.prompt_cache_mode, std::optional<std::string>());
    auto cancelling = cancelBody(result.response);
    co_await std::move(cancelling);
  }
}

ASYNC_TEST("OpenLux native Messages preserves JSON-schema output config") {
  const Capture captured = capture();
  json::object body;
  body["model"] = json::string(model);
  body["input"] = "Generate a title";
  body["max_output_tokens"] = 128;
  body["text"] = parse(R"({ "format": { "type": "json_schema", "strict": true, "name": "answer", "schema": {
    "type": "object", "properties": { "title": { "type": "string" } }, "required": ["title"], "additionalProperties": false
  } } })");
  auto task = fetchMeteredResponses(json::value(body), fixtureOptions(captured.fetcher));
  const MeteredResponsesResult result = co_await std::move(task);
  ASSERT_EQ(captured.calls->size(), std::size_t{1});
  EXPECT_EQ(captured.calls->at(0).url, std::string("https://api.openlux.ai/v1/messages"));
  EXPECT_JSON(member(captured.calls->at(0).body, "output_config"), parse(R"({ "format": { "type": "json_schema", "schema": {
    "type": "object", "properties": { "title": { "type": "string" } }, "required": ["title"], "additionalProperties": false
  } } })"));
  auto cancelling = cancelBody(result.response);
  co_await std::move(cancelling);
}

ASYNC_TEST("OpenLux transport restores native signed tool history only within the authenticated provider scope") {
  const std::string scope = "metered-cache-fixture-principal";
  const json::array blocks = parse(R"([
    { "type": "thinking", "thinking": "Inspect the file", "signature": "signed-provider-fixture" },
    { "type": "tool_use", "id": "read-cache-file", "name": "read_file", "input": { "path": "a.txt" } }
  ])")
                               .as_array();
  const Capture first = capture([blocks] {
    json::object message = nativeMessage("native-openlux-tool-cache", blocks);
    message["stop_reason"] = "tool_use";
    return jsonResponse(json::value(std::move(message)));
  });
  json::object request;
  request["model"] = json::string(model);
  request["input"] = "Read a.txt";
  request["reasoning"] = parse(R"({ "summary": "auto" })");
  MeteredAuthenticatedFetchOptions firstOptions = fixtureOptions(first.fetcher);
  firstOptions.reasoningReplayScope = scope;
  auto task = fetchMeteredResponses(json::value(request), firstOptions);
  const MeteredResponsesResult result = co_await std::move(task);
  auto reading = result.response.json();
  const json::value response = co_await std::move(reading);
  ASSERT(response.at("output").is_array());
  EXPECT_EQ(rt::includes(rt::jsonStringify(response), "signed-provider-fixture"), false);
  json::array input;
  input.push_back(parse(R"({ "role": "user", "content": "Read a.txt" })"));
  for (const json::value& item : response.at("output").as_array()) input.push_back(item);
  input.push_back(parse(R"({ "type": "function_call_output", "call_id": "read-cache-file", "output": "file contents" })"));
  json::object body;
  body["model"] = json::string(model);
  body["input"] = std::move(input);

  const Capture same = capture();
  MeteredAuthenticatedFetchOptions sameOptions = fixtureOptions(same.fetcher);
  sameOptions.reasoningReplayScope = scope;
  auto replaying = fetchMeteredResponses(json::value(body), sameOptions);
  const MeteredResponsesResult replay = co_await std::move(replaying);
  ASSERT_EQ(same.calls->size(), std::size_t{1});
  EXPECT_EQ(same.calls->at(0).url, std::string("https://api.openlux.ai/v1/messages"));
  const json::value& messages = member(same.calls->at(0).body, "messages");
  ASSERT(messages.is_array());
  json::object assistant;
  assistant["role"] = "assistant";
  assistant["content"] = blocks;
  EXPECT_JSON(messages.as_array().at(1), json::value(assistant));
  auto cancellingReplay = cancelBody(replay.response);
  co_await std::move(cancellingReplay);

  const Capture other = capture([] { return jsonResponse(parse(R"({ "preserved": true })")); });
  MeteredAuthenticatedFetchOptions otherOptions = fixtureOptions(other.fetcher);
  otherOptions.reasoningReplayScope = "other-principal";
  auto rejecting = fetchMeteredResponses(json::value(body), otherOptions);
  const MeteredResponsesResult rejected = co_await std::move(rejecting);
  ASSERT_EQ(other.calls->size(), std::size_t{1});
  EXPECT_EQ(other.calls->at(0).url, std::string("https://api.openlux.ai/v1/responses"));
  EXPECT_JSON(json::value(other.calls->at(0).body), json::value(body));
  auto cancellingRejected = cancelBody(rejected.response);
  co_await std::move(cancellingRejected);
}

ASYNC_TEST("OpenLux settled native logs preserve actual cache reads/writes and authoritative quota") {
  json::array items;
  items.push_back(logRow("cache-write", R"({ "claude": true, "cache_tokens": 0, "cache_creation_tokens": 1028, "cache_creation_tokens_5m": 1028 })"));
  items.push_back(logRow("cache-read", R"({ "claude": true, "cache_tokens": 1028 })", R"({ "quota": 136, "completion_tokens": 64 })"));
  auto reading = readLogs(std::move(items));
  const std::vector<MeteredTokenLogEntry> logs = co_await std::move(reading);
  const std::vector<MeteredTokenLogEntry> expected{
    MeteredTokenLogEntry{
      .request_id = "cache-write",
      .quota = 466,
      .prompt_tokens = 1043,
      .cached_prompt_tokens = 0,
      .cache_write_prompt_tokens = 1028,
      .completion_tokens = 4,
      .model = std::string(model),
      .created_at = 1'791'704'855,
    },
    MeteredTokenLogEntry{
      .request_id = "cache-read",
      .quota = 136,
      .prompt_tokens = 1043,
      .cached_prompt_tokens = 1028,
      .completion_tokens = 64,
      .model = std::string(model),
      .created_at = 1'791'704'855,
    },
  };
  EXPECT(logs == expected);
}

ASYNC_TEST("OpenLux does not infer native net input for unknown routes or malformed cache metadata") {
  json::array rows;
  rows.push_back(logRow("unknown-model", R"({ "claude": true, "cache_tokens": 1028 })", R"({ "model_name": "gpt-5.6-sol" })"));
  rows.push_back(logRow("unknown-flag", R"({ "cache_tokens": 1028 })"));
  rows.push_back(logRow("invalid-read", R"({ "claude": true, "cache_tokens": "1028" })"));
  rows.push_back(logRow("invalid-write", R"({ "claude": true, "cache_tokens": 1028, "cache_creation_tokens": -1 })"));
  rows.push_back(logRow("overflow", R"({ "claude": true, "cache_tokens": 9007199254740991 })"));
  rows.push_back(logRow("malformed", "{}", R"({ "other": "{" })"));
  const std::size_t rowCount = rows.size();
  auto reading = readLogs(std::move(rows));
  const std::vector<MeteredTokenLogEntry> logs = co_await std::move(reading);
  EXPECT_EQ(logs.size(), rowCount);
  for (const MeteredTokenLogEntry& normalized : logs) {
    EXPECT_EQ(normalized.prompt_tokens, 15.0);
    EXPECT_EQ(normalized.cached_prompt_tokens, std::optional<double>());
    EXPECT_EQ(normalized.cache_write_prompt_tokens, std::optional<double>());
    EXPECT_EQ(normalized.quota, 466.0);
  }
  json::array standardRows;
  standardRows.push_back(logRow("standard-cache", R"({ "cache_tokens": 999 })", R"({ "model_name": "other-model", "cached_prompt_tokens": 7 })"));
  auto readingStandard = readLogs(std::move(standardRows));
  const std::vector<MeteredTokenLogEntry> standard = co_await std::move(readingStandard);
  ASSERT_EQ(standard.size(), std::size_t{1});
  EXPECT_EQ(standard[0].prompt_tokens, 15.0);
  EXPECT_EQ(standard[0].cached_prompt_tokens, std::optional<double>(7));
}
