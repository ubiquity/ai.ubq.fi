// Links: tests/metered_test.cpp src/provider/metered.cpp src/provider/surplus_claude_request.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_response_wire.cpp src/bounded_response_body.cpp src/inference_deadline.cpp src/responses_stream.cpp src/utils.cpp
// Port of tests/metered.test.ts.
// "fetchMeteredTokenLogs aborts a stalled provider fetch at the bounded timeout" replaces
// AbortSignal.timeout in the TS; rt has no such seam, so the C++ case lets the real
// METERED_FETCH_TIMEOUT_MS timeout fire on the fake clock and checks the elapsed time instead.
#include "uos/provider/metered.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/url.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace uos;
using provider::metered::fetchMeteredResponses;
using provider::metered::fetchMeteredTokenLogs;
using provider::metered::initializeMeteredPricing;
using provider::metered::InitializeMeteredPricingOptions;
using provider::metered::METERED_FETCH_TIMEOUT_MS;
using provider::metered::MeteredAuthenticatedFetchOptions;
using provider::metered::MeteredError;
using provider::metered::MeteredFetch;
using provider::metered::MeteredPricingSnapshot;
using provider::metered::MeteredResponsesResult;
using provider::metered::MeteredTokenLogEntry;
using provider::metered::MeteredTokenLogFetchOptions;

namespace {
  json::value parse(std::string_view text) { return rt::jsonParse(text); }

  rt::Response jsonResponse(const json::value& body, double status = 200, rt::Headers headers = rt::Headers()) {
    rt::Headers responseHeaders(headers);
    if (!responseHeaders.has("Content-Type")) responseHeaders.set("Content-Type", "application/json");
    rt::ResponseInit init;
    init.status = status;
    init.headers = std::move(responseHeaders);
    return rt::Response(rt::BodyInit(rt::jsonStringify(body)), std::move(init));
  }

  /** The URL text of a Metered fetch input. */
  std::string requestUrl(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->toString();
    return std::get<rt::Request>(input).url();
  }

  /** The captured request body, which these transports always send as a JSON string. */
  std::string requestBodyText(const rt::RequestInit& init) {
    if (!init.body || !std::holds_alternative<std::string>(*init.body)) throw rt::JsError("expected a JSON string request body");
    return std::get<std::string>(*init.body);
  }

  struct Call {
    std::string url;
    rt::RequestInit init;
  };

  std::optional<std::string> header(const rt::RequestInit& init, std::string_view name) {
    return rt::Headers(init.headers.value_or(rt::Headers())).get(name);
  }
  bool hasHeader(const rt::RequestInit& init, std::string_view name) { return rt::Headers(init.headers.value_or(rt::Headers())).has(name); }

  template <class T> rt::Task<rt::Reason> rejectionOf(rt::Task<T> task) {
    rt::Reason reason;
    try {
      co_await std::move(task);
    } catch (...) {
      reason = std::current_exception();
    }
    co_return reason;
  }

  const MeteredError* meteredErrorOf(const rt::Reason& reason, std::shared_ptr<const std::exception>& holder) {
    if (!reason) return nullptr;
    try {
      std::rethrow_exception(reason);
    } catch (const MeteredError& error) {
      holder = std::make_shared<MeteredError>(error);
      return static_cast<const MeteredError*>(holder.get());
    } catch (...) {
      return nullptr;
    }
  }
}

ASYNC_TEST("initializeMeteredPricing intersects the current Codex catalog and returns a compact snapshot") {
  std::vector<Call> calls;
  const MeteredFetch fetcher([&calls](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    const std::string url = requestUrl(input);
    calls.push_back(Call{.url = url, .init = init});
    if (url == "https://api.openlux.ai/api/ratio_config") {
      return jsonResponse(parse(R"({
        "success": true,
        "message": "",
        "data": {
          "model_ratio": { "gpt-5.6-sol": 2.5, "not-in-codex": 1, "disabled-model": 0 },
          "model_price": { "gpt-fixed": 0.25 }
        }
      })"));
    }
    if (url == "https://api.openlux.ai/api/status") {
      return jsonResponse(parse(R"({ "success": true, "message": "", "data": { "setup": true, "quota_per_unit": 500000, "server_name_en": "not retained" } })"));
    }
    throw rt::JsError("Unexpected URL: " + url);
  });

  InitializeMeteredPricingOptions options{.codexModelIds = {"gpt-fixed", "missing", "gpt-5.6-sol", "gpt-fixed"}};
  options.fetcher = fetcher;
  options.now = [] { return 1'234'567.0; };
  auto task = initializeMeteredPricing(options);
  const MeteredPricingSnapshot snapshot = co_await std::move(task);

  EXPECT(rt::deepEqual(json::value_from(snapshot), parse(R"({
    "eligible_model_ids": ["gpt-fixed", "gpt-5.6-sol"],
    "quota_per_credit": 500000,
    "model_quota_coefficients": { "gpt-fixed": 0.25, "gpt-5.6-sol": 5 },
    "checked_at_ms": 1234567
  })")));
  ASSERT_EQ(calls.size(), std::size_t{2});
  EXPECT_EQ(calls[0].url, std::string("https://api.openlux.ai/api/ratio_config"));
  EXPECT_EQ(calls[1].url, std::string("https://api.openlux.ai/api/status"));
  for (const Call& call : calls) {
    EXPECT_EQ(call.init.method, std::optional<std::string>("GET"));
    EXPECT_EQ(header(call.init, "Accept"), std::optional<std::string>("application/json"));
    EXPECT_EQ(hasHeader(call.init, "Authorization"), false);
  }
}

ASYNC_TEST("initializeMeteredPricing fails closed and never returns an earlier snapshot") {
  bool statusIsValid = true;
  const MeteredFetch fetcher([&statusIsValid](rt::RequestInfo input, rt::RequestInit) -> rt::Response {
    if (rt::endsWith(requestUrl(input), "/api/ratio_config")) {
      return jsonResponse(parse(R"({ "success": true, "data": { "model_ratio": { "gpt-5.6-sol": 2.5 }, "model_price": {} } })"));
    }
    return jsonResponse(statusIsValid ? parse(R"({ "success": true, "data": { "setup": true, "quota_per_unit": 500000 } })")
                                      : parse(R"({ "success": true, "data": { "setup": true, "quota_per_unit": "500000" } })"));
  });

  InitializeMeteredPricingOptions options{.codexModelIds = {"gpt-5.6-sol"}};
  options.fetcher = fetcher;
  auto first = initializeMeteredPricing(options);
  const MeteredPricingSnapshot snapshot = co_await std::move(first);
  EXPECT_EQ(snapshot.eligible_model_ids, std::vector<std::string>{"gpt-5.6-sol"});

  statusIsValid = false;
  auto second = rejectionOf(initializeMeteredPricing(options));
  const rt::Reason reason = co_await std::move(second);
  std::shared_ptr<const std::exception> holder;
  const MeteredError* error = meteredErrorOf(reason, holder);
  ASSERT(error);
  EXPECT_EQ(error->code, std::string("metered_status_invalid"));
}

ASYNC_TEST("fetchMeteredResponses applies Metered Sol reasoning suffixes and forwards cancellation") {
  const rt::AbortController controller;
  const json::value canonicalBody = parse(R"({
    "model": "gpt-5.6-sol",
    "input": [{ "role": "user", "content": [{ "type": "input_text", "text": "hello" }] }],
    "reasoning": { "effort": "high" },
    "stream": true
  })");
  std::vector<Call> calls;
  const MeteredFetch fetcher([&calls](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    calls.push_back(Call{.url = requestUrl(input), .init = init});
    rt::ResponseInit responseInit;
    responseInit.status = 429;
    responseInit.headers = rt::Headers{
      {"X-Request-Id", " intermediary-request-1 "},
      {"X-Oneapi-Request-Id", " metered-fallback-request-1 "},
      {"X-Api-Request-Id", " metered-request-1 "},
    };
    return rt::Response(rt::BodyInit(std::string("rate limited")), std::move(responseInit));
  });

  MeteredAuthenticatedFetchOptions options;
  options.apiKey = std::string("test-metered-key");
  options.fetcher = fetcher;
  options.signal = controller.signal();
  auto task = fetchMeteredResponses(canonicalBody, options);
  const MeteredResponsesResult result = co_await std::move(task);

  ASSERT_EQ(calls.size(), std::size_t{1});
  EXPECT_EQ(calls[0].url, std::string("https://api.openlux.ai/v1/responses"));
  EXPECT_EQ(calls[0].init.method, std::optional<std::string>("POST"));
  // The request signal also carries the provider header deadline, so it is a
  // composed signal rather than the caller's signal by reference.
  ASSERT(calls[0].init.signal);
  EXPECT_EQ(calls[0].init.signal->aborted(), false);
  EXPECT(rt::deepEqual(rt::jsonParse(requestBodyText(calls[0].init)),
                       parse(R"({ "model": "gpt-5.6-sol-high", "input": [{ "role": "user", "content": [{ "type": "input_text", "text": "hello" }] }], "stream": true })")));
  EXPECT_EQ(header(calls[0].init, "Authorization"), std::optional<std::string>("Bearer test-metered-key"));
  EXPECT_EQ(header(calls[0].init, "Accept"), std::optional<std::string>("text/event-stream"));
  EXPECT_EQ(header(calls[0].init, "Content-Type"), std::optional<std::string>("application/json"));
  EXPECT_EQ(result.response.status(), 429);
  EXPECT_EQ(result.request_id, std::optional<std::string>("metered-request-1"));
}

ASYNC_TEST("Metered billing correlation prefers provider IDs over generic trace IDs") {
  const std::string billingRequestId = "openlux-billing-request";
  const MeteredFetch fetcher([billingRequestId](rt::RequestInfo input, rt::RequestInit) -> rt::Response {
    const std::string url = requestUrl(input);
    if (url == "https://api.openlux.ai/v1/responses") {
      rt::ResponseInit init;
      init.status = 200;
      init.headers = rt::Headers{
        {"Content-Type", "application/json"},
        {"X-Request-Id", "proxy-trace-request"},
        {"X-Api-Request-Id", billingRequestId},
        {"X-Oneapi-Request-Id", "openlux-legacy-request"},
      };
      return rt::Response(rt::BodyInit(std::string("{}")), std::move(init));
    }
    if (rt::startsWith(url, "https://api.openlux.ai/api/log/token?")) {
      json::object other;
      other["request_id"] = billingRequestId;
      json::value page = parse(R"({ "success": true, "data": { "items": [
        { "quota": 100, "prompt_tokens": 1, "completion_tokens": 1, "model_name": "gpt-5.6-sol", "created_at": 1752960000 }
      ] } })");
      page.at("data").at("items").at(0).as_object()["other"] = rt::jsonStringify(json::value(other));
      return jsonResponse(page);
    }
    throw rt::JsError("Unexpected URL: " + url);
  });

  MeteredAuthenticatedFetchOptions options;
  options.apiKey = std::string("test-metered-key");
  options.fetcher = fetcher;
  auto responding = fetchMeteredResponses(parse(R"({ "model": "gpt-5.6-sol", "input": "hello" })"), options);
  const MeteredResponsesResult result = co_await std::move(responding);
  MeteredTokenLogFetchOptions logOptions;
  logOptions.apiKey = std::string("test-metered-key");
  logOptions.fetcher = fetcher;
  auto reading = fetchMeteredTokenLogs(logOptions);
  const std::vector<MeteredTokenLogEntry> logs = co_await std::move(reading);

  EXPECT_EQ(result.request_id, std::optional<std::string>(billingRequestId));
  ASSERT_EQ(logs.size(), std::size_t{1});
  EXPECT_EQ(std::optional<std::string>(logs[0].request_id), result.request_id);
}

ASYNC_TEST("fetchMeteredResponses maps no-reasoning and ultra Sol presets to live aliases") {
  std::vector<json::value> bodies;
  const MeteredFetch fetcher([&bodies](rt::RequestInfo, rt::RequestInit init) -> rt::Response {
    bodies.push_back(rt::jsonParse(requestBodyText(init)));
    rt::ResponseInit responseInit;
    responseInit.status = 200;
    return rt::Response(rt::BodyInit(std::string("{}")), std::move(responseInit));
  });

  MeteredAuthenticatedFetchOptions options;
  options.apiKey = std::string("test-metered-key");
  options.fetcher = fetcher;
  auto minimal = fetchMeteredResponses(parse(R"({ "model": "gpt-5.6-sol", "input": "hello", "reasoning": { "effort": "minimal" } })"), options);
  co_await std::move(minimal);
  auto ultra = fetchMeteredResponses(parse(R"({ "model": "gpt-5.6-sol", "input": "hello", "reasoning": { "effort": "ultra" } })"), options);
  co_await std::move(ultra);

  json::array expected = parse(R"([{ "model": "gpt-5.6-sol-low", "input": "hello" }, { "model": "gpt-5.6-sol-max", "input": "hello" }])").as_array();
  json::array actual;
  for (const json::value& body : bodies) actual.push_back(body);
  EXPECT(rt::deepEqual(json::value(actual), json::value(expected)));
}

ASYNC_TEST("fetchMeteredResponses propagates client cancellation through the header deadline signal") {
  const rt::AbortController controller;
  struct Observed {
    std::optional<rt::AbortSignal> signal;
  };
  const auto observed = std::make_shared<Observed>();
  const MeteredFetch fetcher([observed](rt::RequestInfo, rt::RequestInit init) -> rt::Task<rt::Response> {
    observed->signal = init.signal;
    const rt::AbortSignal signal = *init.signal;
    const rt::Promise<rt::Response> pending = rt::newPromise<rt::Response>([signal](rt::Resolve<rt::Response>, rt::Reject reject) {
      signal.addEventListener(
        "abort",
        rt::EventListener([signal, reject] {
          const rt::Reason reason = signal.reason();
          reject(rt::isError(reason) ? reason : rt::makeReason(rt::DOMException("Aborted", "AbortError")));
        }),
        {.once = true});
    });
    rt::Response response = co_await pending.wait();
    co_return response;
  });

  MeteredAuthenticatedFetchOptions options;
  options.apiKey = std::string("test-metered-key");
  options.fetcher = fetcher;
  options.signal = controller.signal();
  const rt::Promise<MeteredResponsesResult> pending(fetchMeteredResponses(parse(R"({ "model": "gpt-5.6-sol", "input": "hello", "stream": true })"), options));
  const rt::Reason cancellation = rt::makeReason(rt::DOMException("Client disconnected", "AbortError"));
  controller.abort(cancellation);

  EXPECT_REJECTS_REASON(pending.wait(), cancellation);
  ASSERT(observed->signal);
  EXPECT_EQ(observed->signal->aborted(), true);
  EXPECT(observed->signal->reason() == cancellation);
}

ASYNC_TEST("fetchMeteredTokenLogs returns only strict allowlisted billing fields") {
  std::string capturedUrl;
  std::optional<rt::RequestInit> capturedInit;
  const MeteredFetch fetcher([&](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    capturedUrl = requestUrl(input);
    capturedInit = init;
    return jsonResponse(parse(R"({
      "success": true,
      "message": "",
      "data": {
        "total": 2,
        "page": 1,
        "page_size": 100,
        "items": [
          {
            "id": 9001,
            "quota": 2914,
            "prompt_tokens": 71,
            "completion_tokens": 231,
            "model_name": "gpt-5.6-sol",
            "created_at": 1752960000,
            "username": "must-not-leak",
            "token_name": "must-not-leak",
            "ip": "must-not-leak",
            "other": "{\"request_id\":\"request-abc\",\"must\":\"not leak\"}"
          },
          {
            "other": "{\"request_id\":\"malformed-entry\"}",
            "quota": "2914",
            "prompt_tokens": 71,
            "completion_tokens": 231,
            "model_name": "gpt-5.6-sol",
            "created_at": 1752960000
          }
        ]
      }
    })"));
  });

  MeteredTokenLogFetchOptions options;
  options.apiKey = std::string("test-metered-key");
  options.fetcher = fetcher;
  auto reading = fetchMeteredTokenLogs(options);
  const std::vector<MeteredTokenLogEntry> logs = co_await std::move(reading);

  const rt::URL captured(capturedUrl);
  EXPECT_EQ(captured.origin() + captured.pathname(), std::string("https://api.openlux.ai/api/log/token"));
  EXPECT_EQ(captured.searchParams().get("key"), std::optional<std::string>("test-metered-key"));
  EXPECT_EQ(captured.searchParams().get("page"), std::optional<std::string>("1"));
  EXPECT_EQ(captured.searchParams().get("page_size"), std::optional<std::string>("100"));
  ASSERT(capturedInit);
  EXPECT_EQ(capturedInit->method, std::optional<std::string>("GET"));
  EXPECT_EQ(header(*capturedInit, "Authorization"), std::optional<std::string>());
  EXPECT_EQ(header(*capturedInit, "Accept"), std::optional<std::string>("application/json"));
  const std::vector<MeteredTokenLogEntry> expected{MeteredTokenLogEntry{
    .request_id = "request-abc",
    .quota = 2914,
    .prompt_tokens = 71,
    .completion_tokens = 231,
    .model = "gpt-5.6-sol",
    .created_at = 1'752'960'000,
  }};
  EXPECT(logs == expected);
  // Object.keys(logs[0]) is request_id, quota, prompt_tokens, completion_tokens, model, created_at:
  // the optional cache counters are absent.
  ASSERT_EQ(logs.size(), std::size_t{1});
  EXPECT(!logs[0].cached_prompt_tokens);
  EXPECT(!logs[0].cache_write_prompt_tokens);
}

ASYNC_TEST("fetchMeteredTokenLogs aborts a stalled provider fetch at the bounded timeout") {
  struct Observed {
    std::optional<rt::AbortSignal> signal;
  };
  const auto observed = std::make_shared<Observed>();
  const MeteredFetch fetcher([observed](rt::RequestInfo, rt::RequestInit init) -> rt::Task<rt::Response> {
    observed->signal = init.signal;
    const rt::AbortSignal signal = *init.signal;
    const rt::Promise<rt::Response> pending = rt::newPromise<rt::Response>([signal](rt::Resolve<rt::Response>, rt::Reject reject) {
      signal.addEventListener(
        "abort",
        rt::EventListener([signal, reject] {
          const rt::Reason reason = signal.reason();
          reject(rt::isError(reason) ? reason : rt::makeReason(rt::DOMException("Timed out", "AbortError")));
        }),
        {.once = true});
    });
    rt::Response response = co_await pending.wait();
    co_return response;
  });
  // AbortSignal.timeout's timer does not keep the loop alive.
  const rt::Timeout keepAlive = rt::setTimeout([] {}, METERED_FETCH_TIMEOUT_MS * 2);

  MeteredTokenLogFetchOptions options;
  options.apiKey = std::string("test-metered-key");
  options.fetcher = fetcher;
  const double startedAt = rt::performanceNow();
  const rt::Promise<std::vector<MeteredTokenLogEntry>> pending(fetchMeteredTokenLogs(options));
  ASSERT(observed->signal);
  EXPECT_EQ(observed->signal->aborted(), false);
  bool timedOut = false;
  try {
    co_await pending.wait();
  } catch (const rt::DOMException& error) {
    timedOut = error.name == "TimeoutError";
  } catch (...) {
  }
  EXPECT(timedOut);
  EXPECT_EQ(rt::performanceNow() - startedAt, METERED_FETCH_TIMEOUT_MS);
  rt::clearTimeout(keepAlive);
}

TEST("abandoning a Metered billing fetch releases its combined abort signal") {
  const rt::AbortController caller;
  std::weak_ptr<rt::detail::AbortSignalState> signalState;
  {
    rt::FakeClock clock;
    rt::Loop loop(clock);
    const rt::LoopScope scope(loop);
    const auto entered = rt::withResolvers();
    const rt::Promise<rt::Response> never;
    MeteredTokenLogFetchOptions options;
    options.apiKey = "test-metered-key";
    options.signal = caller.signal();
    options.fetcher = MeteredFetch([&signalState, start = entered.resolve, never](rt::RequestInfo, rt::RequestInit init) {
      if (init.signal) signalState = init.signal->state();
      start();
      return never.wait();
    });
    const rt::Promise<std::vector<MeteredTokenLogEntry>> pending(fetchMeteredTokenLogs(std::move(options)));
    rt::blockOn(loop, entered.promise.wait());
    EXPECT(pending.pending());
    EXPECT(!signalState.expired());
  }
  EXPECT(signalState.expired());
}
