// Links: tests/surplus_test.cpp src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/bounded_response_body.cpp src/inference_deadline.cpp src/responses_stream.cpp src/utils.cpp
// Port of tests/surplus.test.ts.
#include "uos/provider/surplus.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace uos;
using provider::surplus::fetchSurplusModels;
using provider::surplus::fetchSurplusResponses;
using provider::surplus::FetchSurplusModelsOptions;
using provider::surplus::resetSurplusModelsCacheForTest;
using provider::surplus::SurplusAuthenticatedFetchOptions;
using provider::surplus::SurplusFetch;
using provider::surplus::SurplusModelsSnapshot;
using provider::surplus::SurplusResponsesResult;

namespace {
  json::value parse(std::string_view text) { return rt::jsonParse(text); }

  /**
   * `jsonResponse` only composes extra plain headers on top of the JSON
   * content type, so it takes a record rather than a full `HeadersInit` (object
   * spread over a `Headers` instance or a `string[][]` silently loses entries).
   */
  rt::Response jsonResponse(const json::value& body, double status = 200) {
    rt::ResponseInit init;
    init.status = status;
    init.headers = rt::Headers{{"Content-Type", "application/json"}};
    return rt::Response(rt::BodyInit(rt::jsonStringify(body)), std::move(init));
  }

  /** Request URL as text; `String(input)` would render a `Request` as "[object Object]". */
  std::string fetchUrl(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->href();
    return std::get<rt::Request>(input).url();
  }

  /** Request body as text; the gateway always encodes it as a JSON string. */
  std::string bodyText(const rt::RequestInit& init) {
    if (init.body && std::holds_alternative<std::string>(*init.body)) return std::get<std::string>(*init.body);
    return "undefined";
  }

  struct Call {
    std::string url;
    rt::RequestInit init;
  };

  rt::Headers headersOf(const rt::RequestInit& init) { return rt::Headers(init.headers.value_or(rt::Headers())); }
}

ASYNC_TEST("fetchSurplusModels preserves exact IDs and exposes text-capable routes only") {
  resetSurplusModelsCacheForTest();
  std::vector<Call> calls;
  const SurplusFetch fetcher([&calls](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    calls.push_back(Call{.url = fetchUrl(input), .init = init});
    return jsonResponse(parse(R"({
      "object": "list",
      "data": [
        {
          "id": "gpt-5.6-sol",
          "created": 1735000000,
          "provider": "openai",
          "architecture": { "modality": "text->text" },
          "supported_parameters": ["reasoning", "tools", "tool_choice", "parallel_tool_calls"],
          "supported_features": ["streaming", "tools", "reasoning"],
          "pricing": {
            "prompt": "0.000001",
            "completion": "0.000003",
            "input_cache_read": "0.0000001",
            "input_cache_write": "0.000002"
          }
        },
        {
          "id": "image-model-that-must-not-route",
          "architecture": { "output_modalities": ["image"] }
        },
        {
          "id": "audio-to-text-model-that-must-not-route",
          "architecture": { "modality": "audio->text" }
        },
        {
          "id": "claude-opus-5",
          "provider": "anthropic",
          "architecture": { "input_modalities": ["text"], "output_modalities": ["text"] },
          "supported_parameters": ["tools"],
          "supported_features": ["tools"],
          "description": "test model",
          "top_provider": { "max_completion_tokens": 128000 }
        }
      ]
    })"));
  });

  FetchSurplusModelsOptions options;
  options.apiKey = std::string("inf_test");
  options.fetcher = fetcher;
  options.force = true;
  auto task = fetchSurplusModels(options);
  const std::optional<SurplusModelsSnapshot> snapshot = co_await std::move(task);

  ASSERT(snapshot);
  std::vector<std::string> ids;
  for (const auto& model : snapshot->models) ids.push_back(model.id);
  EXPECT_EQ(ids, (std::vector<std::string>{"gpt-5.6-sol", "claude-opus-5"}));
  ASSERT_EQ(snapshot->models.size(), std::size_t{2});
  EXPECT_EQ(snapshot->models[0].owned_by, std::string("openai"));
  EXPECT_EQ(snapshot->models[0].input_price_per_token, std::optional<double>(0.000001));
  EXPECT_EQ(snapshot->models[0].cache_read_price_per_token, std::optional<double>(0.0000001));
  EXPECT_EQ(snapshot->models[0].cache_write_price_per_token, std::optional<double>(0.000002));
  EXPECT_EQ(snapshot->models[0].supports_tools, std::optional<bool>(true));
  EXPECT_EQ(snapshot->models[0].supports_parallel_tool_calls, std::optional<bool>(true));
  EXPECT_EQ(snapshot->models[0].supported_endpoint_types, (std::vector<std::string>{"openai", "openai-response"}));
  EXPECT_EQ(snapshot->models[1].description, std::optional<std::string>("test model"));
  EXPECT_EQ(snapshot->models[1].max_output_tokens, std::optional<double>(128000));
  EXPECT_EQ(snapshot->models[1].supports_tools, std::optional<bool>());
  EXPECT_EQ(snapshot->models[1].supports_parallel_tool_calls, std::optional<bool>());
  ASSERT_EQ(calls.size(), std::size_t{1});
  EXPECT_EQ(calls[0].url, std::string("https://api.surplusintelligence.ai/v1/models"));
  EXPECT_EQ(headersOf(calls[0].init).get("Accept"), std::optional<std::string>("application/json"));
  EXPECT_EQ(headersOf(calls[0].init).has("Authorization"), false);
  EXPECT_EQ(headersOf(calls[0].init).has("X-SI-Mask-Secrets"), false);
}

TEST("a new loop can discover Surplus models after the shared request was abandoned") {
  const rt::env::ScopedOverride environment({{"SURPLUS_API_KEY", std::string("discovery-fixture-key")}});
  resetSurplusModelsCacheForTest();
  const auto cleanup = rt::scopeExit([] { resetSurplusModelsCacheForTest(); });
  {
    rt::FakeClock clock;
    rt::Loop first(clock);
    const rt::LoopScope scope(first);
    const auto entered = rt::withResolvers();
    const rt::Promise<rt::Response> never;
    const rt::GlobalFetchOverride fetcher(SurplusFetch([start = entered.resolve, never](rt::RequestInfo, rt::RequestInit) {
      start();
      return never.wait();
    }));
    const rt::Promise<std::optional<SurplusModelsSnapshot>> pending(fetchSurplusModels());
    rt::blockOn(first, entered.promise.wait());
    EXPECT(pending.pending());
  }
  {
    rt::FakeClock clock;
    rt::Loop second(clock);
    const rt::LoopScope scope(second);
    const rt::GlobalFetchOverride fetcher(SurplusFetch([](rt::RequestInfo, rt::RequestInit) {
      return jsonResponse(parse(R"({"data":[{"id":"mock-surplus-model"}]})"));
    }));
    const auto snapshot = rt::blockOn(second, fetchSurplusModels());
    ASSERT(snapshot);
    ASSERT_EQ(snapshot->models.size(), std::size_t{1});
    EXPECT_EQ(snapshot->models[0].id, std::string("mock-surplus-model"));
  }
}

ASYNC_TEST("fetchSurplusResponses omits unsupported parallel-tool control and returns the request ID") {
  const json::value body = parse(R"({
    "model": "gpt-6-sol",
    "input": [{ "role": "user", "content": [{ "type": "input_text", "text": "hello" }] }],
    "stream": true,
    "parallel_tool_calls": true
  })");
  std::vector<Call> calls;
  const SurplusFetch fetcher([&calls](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    calls.push_back(Call{.url = fetchUrl(input), .init = init});
    rt::ResponseInit responseInit;
    responseInit.status = 200;
    responseInit.headers = rt::Headers{{"X-Request-Id", " surplus-request-1 "}};
    return rt::Response(rt::BodyInit(std::string("data: [DONE]\n\n")), std::move(responseInit));
  });

  SurplusAuthenticatedFetchOptions options;
  options.apiKey = std::string("inf_test");
  options.fetcher = fetcher;
  options.supportsParallelToolCalls = false;
  auto task = fetchSurplusResponses(body, options);
  const SurplusResponsesResult result = co_await std::move(task);

  ASSERT_EQ(calls.size(), std::size_t{1});
  EXPECT_EQ(calls[0].url, std::string("https://api.surplusintelligence.ai/v1/responses"));
  EXPECT_EQ(calls[0].init.method, std::optional<std::string>("POST"));
  EXPECT(rt::deepEqual(rt::jsonParse(bodyText(calls[0].init)),
                       parse(R"({ "model": "gpt-6-sol", "input": [{ "role": "user", "content": [{ "type": "input_text", "text": "hello" }] }], "stream": true })")));
  const rt::Headers headers = headersOf(calls[0].init);
  EXPECT_EQ(headers.get("Authorization"), std::optional<std::string>("Bearer inf_test"));
  EXPECT_EQ(headers.get("Accept"), std::optional<std::string>("text/event-stream"));
  EXPECT_EQ(headers.get("Content-Type"), std::optional<std::string>("application/json"));
  EXPECT_EQ(headers.get("X-SI-Mask-Secrets"), std::optional<std::string>("on"));
  EXPECT_EQ(result.request_id, std::optional<std::string>("surplus-request-1"));
}

ASYNC_TEST("fetchSurplusResponses translates Codex ultra reasoning to the upstream max preset") {
  std::optional<json::value> forwarded;
  const SurplusFetch fetcher([&forwarded](rt::RequestInfo, rt::RequestInit init) -> rt::Response {
    forwarded = rt::jsonParse(bodyText(init));
    rt::ResponseInit responseInit;
    responseInit.status = 200;
    return rt::Response(rt::BodyInit(std::string("{}")), std::move(responseInit));
  });

  SurplusAuthenticatedFetchOptions options;
  options.apiKey = std::string("inf_test");
  options.fetcher = fetcher;
  auto task = fetchSurplusResponses(parse(R"({ "model": "gpt-5.6-sol", "input": "hello", "reasoning": { "effort": "ultra" } })"), options);
  co_await std::move(task);

  ASSERT(forwarded);
  EXPECT(rt::deepEqual(*forwarded, parse(R"({ "model": "gpt-5.6-sol", "input": "hello", "reasoning": { "effort": "max" } })")));
}

ASYNC_TEST("fetchSurplusResponses maps Codex developer messages to Surplus system messages") {
  std::optional<json::value> forwardedInput;

  SurplusAuthenticatedFetchOptions options;
  options.apiKey = std::string("test-key");
  options.fetcher = SurplusFetch([&forwardedInput](rt::RequestInfo, rt::RequestInit init) -> rt::Response {
    forwardedInput = rt::jsonParse(bodyText(init)).at("input");
    rt::ResponseInit responseInit;
    responseInit.status = 200;
    return rt::Response(nullptr, std::move(responseInit));
  });
  auto task = fetchSurplusResponses(parse(R"({
      "model": "deepseek-v4-flash",
      "input": [
        { "type": "message", "role": "developer", "content": [{ "type": "input_text", "text": "Be concise." }] },
        { "type": "message", "role": "user", "content": [{ "type": "input_text", "text": "Reply." }] }
      ],
      "stream": true
    })"),
                                    options);
  co_await std::move(task);

  ASSERT(forwardedInput);
  EXPECT(rt::deepEqual(*forwardedInput, parse(R"([
    { "type": "message", "role": "system", "content": [{ "type": "input_text", "text": "Be concise." }] },
    { "type": "message", "role": "user", "content": [{ "type": "input_text", "text": "Reply." }] }
  ])")));
}

ASYNC_TEST("fetchSurplusResponses completes sparse Surplus text streams for Codex") {
  const json::value events = parse(R"([
    { "type": "response.created", "response": { "id": "resp_test", "status": "in_progress" } },
    { "type": "response.output_text.delta", "output_index": 0, "content_index": 0, "delta": "hello" },
    { "type": "response.completed", "response": { "id": "resp_test", "status": "completed" } }
  ])");
  std::string sparse;
  for (const json::value& event : events.as_array()) {
    sparse += "event: " + std::string(event.at("type").as_string()) + "\ndata: " + rt::jsonStringify(event) + "\n\n";
  }

  SurplusAuthenticatedFetchOptions options;
  options.apiKey = std::string("test-key");
  options.fetcher = SurplusFetch([sparse](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    rt::ResponseInit responseInit;
    responseInit.headers = rt::Headers{{"Content-Type", "text/event-stream"}};
    return rt::Response(rt::BodyInit(sparse), std::move(responseInit));
  });
  auto task = fetchSurplusResponses(parse(R"({ "model": "deepseek-v4-flash", "input": "hello", "stream": true })"), options);
  const SurplusResponsesResult result = co_await std::move(task);
  auto reading = result.response.text();
  const std::string output = co_await std::move(reading);

  EXPECT(rt::Regex(R"("type":"response.output_text.done")").test(output));
  EXPECT(rt::Regex(R"("type":"response.output_item.done")").test(output));
  EXPECT(rt::Regex(R"("output":\[\{"type":"message","role":"assistant")").test(output));
  EXPECT(rt::Regex(R"("text":"hello")").test(output));
}

ASYNC_TEST("fetchSurplusResponses runs the quota hook immediately before transport") {
  std::vector<std::string> events;
  const SurplusFetch fetcher([&events](rt::RequestInfo, rt::RequestInit init) -> rt::Response {
    events.push_back("fetch");
    EXPECT(init.signal.has_value() && !init.signal->aborted());
    rt::ResponseInit responseInit;
    responseInit.status = 200;
    return rt::Response(rt::BodyInit(std::string("{}")), std::move(responseInit));
  });
  SurplusAuthenticatedFetchOptions options;
  options.apiKey = std::string("inf_test");
  options.fetcher = fetcher;
  options.beforeDispatch = [&events] {
    events.push_back("before-dispatch");
    api_key_policy::ApiKeyProviderDispatch dispatch{
      .markTransportStarted = [&events] { events.push_back("started"); },
      .cancelBeforeTransport = [&events]() -> rt::Task<void> {
        co_await rt::microtask();
        events.push_back("cancelled");
      },
    };
    return rt::fulfilledTask(std::optional<api_key_policy::ApiKeyProviderDispatch>(std::move(dispatch)));
  };
  auto task = fetchSurplusResponses(parse(R"({ "model": "gpt-5.6-sol", "input": "hello" })"), options);
  co_await std::move(task);
  EXPECT_EQ(events, (std::vector<std::string>{"before-dispatch", "started", "fetch"}));
}

TEST("abandoning a Surplus preheader request releases its combined abort signal") {
  const rt::AbortController caller;
  std::weak_ptr<rt::detail::AbortSignalState> signalState;
  {
    rt::FakeClock clock;
    rt::Loop loop(clock);
    const rt::LoopScope scope(loop);
    const auto entered = rt::withResolvers();
    const rt::Promise<rt::Response> never;
    SurplusAuthenticatedFetchOptions options;
    options.apiKey = "inf_test";
    options.signal = caller.signal();
    options.fetcher = SurplusFetch([&signalState, start = entered.resolve, never](rt::RequestInfo, rt::RequestInit init) {
      if (init.signal) signalState = init.signal->state();
      start();
      return never.wait();
    });
    const auto body = parse(R"({ "model": "gpt-5.6-sol", "input": "hello" })");
    const rt::Promise<SurplusResponsesResult> pending(fetchSurplusResponses(body, std::move(options)));
    rt::blockOn(loop, entered.promise.wait());
    EXPECT(pending.pending());
    EXPECT(!signalState.expired());
  }
  EXPECT(signalState.expired());
}
