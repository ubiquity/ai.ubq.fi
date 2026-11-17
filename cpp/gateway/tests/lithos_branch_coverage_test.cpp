// Links: tests/lithos_branch_coverage_test.cpp src/provider/metered.cpp src/provider/surplus_claude_request.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_response_wire.cpp src/bounded_response_body.cpp src/inference_deadline.cpp src/responses_stream.cpp src/utils.cpp
// Port of tests/lithos-branch-coverage.test.ts (branch coverage for the OpenLux / Metered provider).
// Not portable, because a json::value cannot hold them: the circular request body and the
// `{ toJSON: () => undefined }` body (both "JSON-serializable body" rejections), and the untyped
// `codexModelIds: "gpt-6-astra"` catalog (InitializeMeteredPricingOptions::codexModelIds is a
// std::vector). `withDiscoveryKey` also clears OPENLUX_API_KEY, which the TS assumes absent
// (discovery prefers it over METERED_API_KEY).
#include "uos/provider/metered.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace uos;
using provider::metered::fetchMeteredModels;
using provider::metered::FetchMeteredModelsOptions;
using provider::metered::fetchMeteredResponses;
using provider::metered::fetchMeteredTokenLogs;
using provider::metered::initializeMeteredPricing;
using provider::metered::InitializeMeteredPricingOptions;
using provider::metered::METERED_BASE_URL;
using provider::metered::MeteredAuthenticatedFetchOptions;
using provider::metered::MeteredError;
using provider::metered::MeteredFetch;
using provider::metered::MeteredModelsSnapshot;
using provider::metered::MeteredPricingSnapshot;
using provider::metered::MeteredResponsesResult;
using provider::metered::MeteredTokenLogEntry;
using provider::metered::MeteredTokenLogFetchOptions;
using provider::metered::resetMeteredModelsCacheForTest;
using provider::metered::setMeteredModelsFetchForTest;

namespace {
  const std::string API_KEY_ENV = "METERED_API_KEY";

  /** A discovery key in the environment and a clean models cache for the scope. */
  struct DiscoveryKey {
    rt::env::ScopedOverride environment{{API_KEY_ENV, std::string("metered-fixture-key")}, {"OPENLUX_API_KEY", std::nullopt}};
    DiscoveryKey() { resetMeteredModelsCacheForTest(); }
    ~DiscoveryKey() { resetMeteredModelsCacheForTest(); }
    DiscoveryKey(const DiscoveryKey&) = delete;
    DiscoveryKey& operator=(const DiscoveryKey&) = delete;
  };

  json::value parse(std::string_view text) { return rt::jsonParse(text); }

  rt::Response jsonResponse(const json::value& value, double status = 200) {
    rt::ResponseInit init;
    init.status = status;
    return rt::Response(rt::BodyInit(rt::jsonStringify(value)), std::move(init));
  }

  std::string urlOf(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->toString();
    return std::get<rt::Request>(input).url();
  }

  rt::Response emptyModelsResponse() { return jsonResponse(parse(R"({ "data": [] })")); }

  MeteredFetch respond(std::function<rt::Response()> response) {
    return MeteredFetch([response](rt::RequestInfo, rt::RequestInit) -> rt::Response { return response(); });
  }

  MeteredFetch rejecting(std::string message) {
    return MeteredFetch([message](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
      throw rt::JsError(message);
      co_return rt::Response();
    });
  }

  rt::Task<std::optional<MeteredModelsSnapshot>> discover(FetchMeteredModelsOptions options) {
    auto task = fetchMeteredModels(std::move(options));
    std::optional<MeteredModelsSnapshot> snapshot = co_await std::move(task);
    co_return snapshot;
  }

  FetchMeteredModelsOptions discoveryOptions(MeteredFetch fetcher, bool force = false) {
    FetchMeteredModelsOptions options;
    options.fetcher = std::move(fetcher);
    if (force) options.force = true;
    return options;
  }

  std::size_t modelCount(const std::optional<MeteredModelsSnapshot>& snapshot) { return snapshot ? snapshot->models.size() : std::size_t{0}; }

  json::value pricingEnvelopeJson(std::string_view overrides = "{}") {
    json::object data = parse(R"({
      "model_ratio": { "gpt-6-astra": 2, "ignored-model": 0 },
      "model_price": { "gpt-6-astra": 0.5, "free-model": 0 },
      "completion_ratio": { "gpt-6-astra": 1.5, "ignored-model": 4 }
    })")
                          .as_object();
    const json::object extra = parse(overrides).as_object();
    for (const auto* entry : rt::objectEntries(extra)) rt::objectDefine(data, entry->key(), entry->value());
    json::object envelope;
    envelope["success"] = true;
    envelope["data"] = std::move(data);
    return envelope;
  }
  rt::Response pricingEnvelope(std::string_view overrides = "{}") { return jsonResponse(pricingEnvelopeJson(overrides)); }

  rt::Response statusEnvelope(std::string_view overrides = "{}") {
    json::object data = parse(R"({ "setup": true, "quota_per_unit": 500000 })").as_object();
    const json::object extra = parse(overrides).as_object();
    for (const auto* entry : rt::objectEntries(extra)) rt::objectDefine(data, entry->key(), entry->value());
    json::object envelope;
    envelope["success"] = true;
    envelope["data"] = std::move(data);
    return jsonResponse(envelope);
  }

  // The same two Response objects answer every call, as in the TS.
  MeteredFetch pricingFetcher(rt::Response pricing, rt::Response status) {
    return MeteredFetch([pricing, status](rt::RequestInfo input, rt::RequestInit) -> rt::Response {
      return rt::endsWith(urlOf(input), "/api/ratio_config") ? pricing : status;
    });
  }

  template <class T> rt::Task<rt::Reason> rejectionOf(rt::Task<T> task) {
    rt::Reason reason;
    try {
      co_await std::move(task);
    } catch (...) {
      reason = std::current_exception();
    }
    co_return reason;
  }

  std::optional<MeteredError> meteredErrorOf(const rt::Reason& reason) {
    if (!reason) return std::nullopt;
    try {
      std::rethrow_exception(reason);
    } catch (const MeteredError& error) {
      return error;
    } catch (...) {
      return std::nullopt;
    }
  }

  void expectJson(const json::value& actual, const json::value& expected, const char* text, const char* file, int line) {
    if (!rt::deepEqual(actual, expected))
      rt::test::report(file, line, std::string("expected ") + text + " to deep-equal " + json::serialize(expected) + "\n      actual:   " + json::serialize(actual),
                       false);
  }
#define EXPECT_JSON(actual, expected) expectJson((actual), (expected), #actual, __FILE__, __LINE__)

  std::string bodyOf(const rt::RequestInit& init) {
    return init.body && std::holds_alternative<std::string>(*init.body) ? std::get<std::string>(*init.body) : std::string();
  }
}

ASYNC_TEST("metered models: discovery reads the upstream list, normalizes each row and skips duplicates") {
  const DiscoveryKey key;
  const json::value payload = parse(R"({
    "data": [
      {
        "id": "gpt-6-astra",
        "created": 1800000000,
        "owned_by": "openlux",
        "model_type": "chat",
        "description": "frontier",
        "tags": ["fast"],
        "supported_endpoint_types": ["/v1/responses", 5]
      },
      { "id": "gpt-6-astra" },
      { "id": "   " },
      { "created": 1 },
      42,
      { "id": "thin" },
      { "id": "tagged-model", "tags": "fast,cheap", "owned_by": "   ", "created": -5, "supported_endpoint_types": "none" }
    ]
  })");
  auto task = discover(discoveryOptions(respond([payload] { return jsonResponse(payload); })));
  const std::optional<MeteredModelsSnapshot> snapshot = co_await std::move(task);

  ASSERT(snapshot);
  ASSERT_EQ(snapshot->models.size(), std::size_t{3});
  // Non-string endpoint entries are dropped, and a non-string `tags` value
  // (an array here) is not part of the published model at all.
  EXPECT_JSON(json::value_from(snapshot->models[0]), parse(R"({
    "id": "gpt-6-astra",
    "object": "model",
    "created": 1800000000,
    "owned_by": "openlux",
    "supported_endpoint_types": ["/v1/responses"],
    "model_type": "chat",
    "description": "frontier"
  })"));
  // A row with only an id keeps the defaults: created 0, owned_by openlux,
  // and no optional fields at all.
  EXPECT_JSON(json::value_from(snapshot->models[1]),
              parse(R"({ "id": "thin", "object": "model", "created": 0, "owned_by": "openlux", "supported_endpoint_types": [] })"));
  // A blank owner, a negative timestamp and a non-array endpoint list all fall
  // back to their defaults, while a string `tags` value is preserved verbatim.
  EXPECT_JSON(json::value_from(snapshot->models[2]), parse(R"({
    "id": "tagged-model",
    "object": "model",
    "created": 0,
    "owned_by": "openlux",
    "supported_endpoint_types": [],
    "tags": "fast,cheap"
  })"));
}

ASYNC_TEST("metered models: an unusable payload, a rejecting fetch and a missing key all keep the cache") {
  {
    const DiscoveryKey key;
    // A successful first read publishes a snapshot.
    auto publishing = discover(discoveryOptions(respond([] { return jsonResponse(parse(R"({ "data": [{ "id": "gpt-6-astra" }] })")); })));
    const std::optional<MeteredModelsSnapshot> published = co_await std::move(publishing);
    EXPECT_EQ(modelCount(published), std::size_t{1});

    // A payload without a data array is a failure and returns the cache.
    auto malforming = discover(discoveryOptions(respond([] { return jsonResponse(parse(R"({ "data": "none" })")); }), true));
    const std::optional<MeteredModelsSnapshot> malformed = co_await std::move(malforming);
    EXPECT_EQ(modelCount(malformed), std::size_t{1});
    // A non-record payload takes the same path.
    auto scalarTask = discover(discoveryOptions(respond([] { return jsonResponse(json::value(7)); }), true));
    const std::optional<MeteredModelsSnapshot> scalar = co_await std::move(scalarTask);
    EXPECT_EQ(modelCount(scalar), std::size_t{1});
  }

  {
    const DiscoveryKey key;
    // A rejecting fetch is swallowed and reports the (empty) cache.
    auto rejectingTask = discover(discoveryOptions(rejecting("connect refused")));
    const std::optional<MeteredModelsSnapshot> rejected = co_await std::move(rejectingTask);
    EXPECT(!rejected);

    // A non-JSON body is a failure too.
    auto nonJsonTask = discover(discoveryOptions(respond([] {
                                                   rt::ResponseInit init;
                                                   init.status = 200;
                                                   return rt::Response(rt::BodyInit(std::string("<html>")), std::move(init));
                                                 }),
                                                 true));
    const std::optional<MeteredModelsSnapshot> nonJson = co_await std::move(nonJsonTask);
    EXPECT(!nonJson);

    // Without a fetcher and without the default fetch the cache is returned.
    FetchMeteredModelsOptions forced;
    forced.force = true;
    auto unavailableTask = discover(forced);
    const std::optional<MeteredModelsSnapshot> unavailable = co_await std::move(unavailableTask);
    EXPECT(!unavailable);

    // An already-aborted caller signal is not reported as an upstream failure,
    // so the call resolves with the cache rather than a backoff.
    const rt::AbortController controller;
    controller.abort();
    FetchMeteredModelsOptions abortedOptions = discoveryOptions(respond([] { return emptyModelsResponse(); }), true);
    abortedOptions.signal = controller.signal();
    auto abortedTask = discover(abortedOptions);
    const std::optional<MeteredModelsSnapshot> aborted = co_await std::move(abortedTask);
    EXPECT(!aborted);
  }
}

ASYNC_TEST("metered models: the cached snapshot is served for a fresh cache and for cachedOnly callers") {
  {
    const DiscoveryKey key;
    const auto calls = std::make_shared<int>(0);
    const MeteredFetch fetcher([calls](rt::RequestInfo, rt::RequestInit) -> rt::Response {
      *calls += 1;
      return jsonResponse(parse(R"({ "data": [{ "id": "gpt-6-astra" }] })"));
    });
    auto first = discover(discoveryOptions(fetcher));
    co_await std::move(first);
    EXPECT_EQ(*calls, 1);

    // A fresh cache short-circuits, and cachedOnly never reaches the transport.
    auto cachedTask = discover(discoveryOptions(fetcher));
    const std::optional<MeteredModelsSnapshot> cached = co_await std::move(cachedTask);
    FetchMeteredModelsOptions cachedOnlyOptions = discoveryOptions(fetcher);
    cachedOnlyOptions.cachedOnly = true;
    auto cachedOnlyTask = discover(cachedOnlyOptions);
    const std::optional<MeteredModelsSnapshot> cachedOnly = co_await std::move(cachedOnlyTask);
    EXPECT_EQ(*calls, 1);
    EXPECT_EQ(modelCount(cached), std::size_t{1});
    EXPECT_EQ(modelCount(cachedOnly), std::size_t{1});
  }

  // Without a discovery key there is nothing to fetch.
  const rt::env::ScopedOverride withoutKeyEnvironment({{API_KEY_ENV, std::nullopt}, {"OPENLUX_API_KEY", std::nullopt}});
  resetMeteredModelsCacheForTest();
  auto withoutKeyTask = discover(discoveryOptions(respond([] { return emptyModelsResponse(); }), true));
  const std::optional<MeteredModelsSnapshot> withoutKey = co_await std::move(withoutKeyTask);
  EXPECT(!withoutKey);
  resetMeteredModelsCacheForTest();
}

ASYNC_TEST("metered models: the test fetch seam is used when no fetcher is supplied") {
  const DiscoveryKey key;
  setMeteredModelsFetchForTest(MeteredFetch([](rt::RequestInfo input, rt::RequestInit) -> rt::Response {
    const std::string url = urlOf(input);
    EXPECT(rt::Regex("\\/v1\\/models$").test(url));
    return jsonResponse(parse(R"({ "data": [{ "id": "seam-model" }] })"));
  }));
  auto task = discover(FetchMeteredModelsOptions{});
  const std::optional<MeteredModelsSnapshot> snapshot = co_await std::move(task);
  setMeteredModelsFetchForTest(MeteredFetch());
  ASSERT(snapshot && !snapshot->models.empty());
  EXPECT_EQ(snapshot->models[0].id, std::string("seam-model"));
}

ASYNC_TEST("metered pricing: a well-formed pair publishes the coefficients for the eligible Codex ids") {
  const rt::Response pricing = jsonResponse(parse(R"({
    "success": true,
    "data": {
      "model_ratio": { "gpt-6-astra": 2, "ratio-only": 4, "zero-ratio": 0 },
      "model_price": { "gpt-6-astra": 0.5 },
      "completion_ratio": { "gpt-6-astra": 1.5, "ratio-only": 0.5, "zero-ratio": 4 }
    }
  })"));
  InitializeMeteredPricingOptions options{.codexModelIds = {"gpt-6-astra", "ratio-only", "unpriced-model"}};
  options.now = [] { return 1'800'000'000'000.0; };
  options.fetcher = pricingFetcher(pricing, statusEnvelope());
  auto task = initializeMeteredPricing(options);
  const MeteredPricingSnapshot snapshot = co_await std::move(task);

  EXPECT_EQ(snapshot.quota_per_credit, 500'000.0);
  EXPECT_EQ(snapshot.checked_at_ms, 1'800'000'000'000.0);
  // Only ids the pricing maps actually price reach the snapshot, in caller order.
  EXPECT_EQ(snapshot.eligible_model_ids, (std::vector<std::string>{"gpt-6-astra", "ratio-only"}));
  // ratio-only: 4 * (1 + 0.5) = 6; gpt-6-astra keeps its fixed price 0.5.
  EXPECT_JSON(json::value(snapshot.model_quota_coefficients), parse(R"({ "gpt-6-astra": 0.5, "ratio-only": 6 })"));
}

ASYNC_TEST("metered pricing: every malformed envelope and configuration is rejected by code") {
  struct Rejection {
    MeteredFetch fetcher;
    std::string code;
    std::optional<double> status;
  };
  std::vector<Rejection> rejections;
  // A transport failure is unavailable, and an unreadable body is invalid.
  rejections.push_back({rejecting("offline"), "metered_pricing_unavailable", 502});
  rejections.push_back({respond([] {
                          rt::ResponseInit init;
                          init.status = 200;
                          return rt::Response(rt::BodyInit(std::string("<html>")), std::move(init));
                        }),
                        "metered_pricing_invalid", 502});
  rejections.push_back({respond([] { return jsonResponse(parse(R"({ "success": false })")); }), "metered_pricing_invalid", 502});
  rejections.push_back({respond([] { return jsonResponse(parse(R"({ "success": true, "data": "none" })")); }), "metered_pricing_invalid", 502});
  // The status envelope carries the quota conversion.
  rejections.push_back({pricingFetcher(pricingEnvelope(), jsonResponse(parse(R"({ "success": false })"))), "metered_status_invalid", 502});
  rejections.push_back({pricingFetcher(pricingEnvelope(), statusEnvelope(R"({ "setup": false })")), "metered_status_invalid", 502});
  rejections.push_back({pricingFetcher(pricingEnvelope(), statusEnvelope(R"({ "quota_per_unit": 1.5 })")), "metered_status_invalid", 502});
  rejections.push_back({pricingFetcher(pricingEnvelope(), statusEnvelope(R"({ "quota_per_unit": -1 })")), "metered_status_invalid", 502});
  // A pricing configuration without both required maps is invalid.
  rejections.push_back({pricingFetcher(jsonResponse(parse(R"({ "success": true, "data": { "model_ratio": {}, "model_price": "none" } })")), statusEnvelope()),
                        "metered_pricing_invalid", 502});
  rejections.push_back({pricingFetcher(jsonResponse(parse(R"({ "success": true, "data": { "model_ratio": "none", "model_price": {} } })")), statusEnvelope()),
                        "metered_pricing_invalid", 502});
  for (const Rejection& rejection : rejections) {
    InitializeMeteredPricingOptions options{.codexModelIds = {"gpt-6-astra"}};
    options.fetcher = rejection.fetcher;
    auto task = rejectionOf(initializeMeteredPricing(options));
    const rt::Reason reason = co_await std::move(task);
    const std::optional<MeteredError> error = meteredErrorOf(reason);
    if (!error) {
      rt::test::report(__FILE__, __LINE__, "expected a MeteredError, got " + rt::reasonToString(reason), false);
      continue;
    }
    EXPECT_EQ(error->code, rejection.code);
    if (rejection.status) EXPECT_EQ(error->status, *rejection.status);
  }

  // A non-record completion_ratio is tolerated rather than rejected: every
  // ratio-priced model falls back to a completion ratio of 1.
  InitializeMeteredPricingOptions fallbackOptions{.codexModelIds = {"gpt-6-astra"}};
  fallbackOptions.now = [] { return 1.0; };
  fallbackOptions.fetcher =
    pricingFetcher(jsonResponse(parse(R"({ "success": true, "data": { "model_ratio": { "gpt-6-astra": 3 }, "model_price": {}, "completion_ratio": "none" } })")),
                   statusEnvelope());
  auto fallbackTask = initializeMeteredPricing(fallbackOptions);
  const MeteredPricingSnapshot completionFallback = co_await std::move(fallbackTask);
  EXPECT_EQ(completionFallback.eligible_model_ids, std::vector<std::string>{"gpt-6-astra"});
  EXPECT_JSON(json::value(completionFallback.model_quota_coefficients), parse(R"({ "gpt-6-astra": 6 })"));

  // An invalid clock and an unusable Codex model id are rejected before publication.
  const MeteredFetch validFetcher = pricingFetcher(pricingEnvelope(), statusEnvelope());
  InitializeMeteredPricingOptions clockOptions{.codexModelIds = {"gpt-6-astra"}};
  clockOptions.now = [] { return -1.0; };
  clockOptions.fetcher = validFetcher;
  auto clockTask = rejectionOf(initializeMeteredPricing(clockOptions));
  const rt::Reason clockReason = co_await std::move(clockTask);
  const std::optional<MeteredError> clockError = meteredErrorOf(clockReason);
  ASSERT(clockError);
  EXPECT_EQ(clockError->code, std::string("metered_status_invalid"));

  InitializeMeteredPricingOptions idOptions{.codexModelIds = {"   "}};
  idOptions.now = [] { return 1.0; };
  idOptions.fetcher = validFetcher;
  auto idTask = rejectionOf(initializeMeteredPricing(idOptions));
  const rt::Reason idReason = co_await std::move(idTask);
  const std::optional<MeteredError> idError = meteredErrorOf(idReason);
  ASSERT(idError);
  EXPECT_EQ(idError->code, std::string("metered_pricing_invalid"));
}

ASYNC_TEST("metered responses: the projection refuses unusable bodies and applies the reasoning suffix") {
  MeteredAuthenticatedFetchOptions keyOnly;
  keyOnly.apiKey = std::string("sk-test");
  auto stringBody = rejectionOf(fetchMeteredResponses(json::value("not-an-object"), keyOnly));
  const rt::Reason rejection = co_await std::move(stringBody);
  const std::optional<MeteredError> error = meteredErrorOf(rejection);
  ASSERT(error);
  EXPECT_EQ(error->code, std::string("metered_request_invalid"));
  EXPECT_EQ(error->status, 400.0);

  // The reasoning tier rides the model id for the models that support it, and the
  // request id comes from the upstream response headers rather than the body.
  const auto bodies = std::make_shared<std::vector<std::string>>();
  const MeteredFetch fetcher([bodies](rt::RequestInfo, rt::RequestInit init) -> rt::Response {
    bodies->push_back(bodyOf(init));
    rt::ResponseInit responseInit;
    responseInit.status = 200;
    responseInit.headers = rt::Headers{{"X-Api-Request-Id", "resp_1"}, {"Content-Type", "application/json"}};
    return rt::Response(rt::BodyInit(rt::jsonStringify(parse(R"({ "id": "resp_1" })"))), std::move(responseInit));
  });
  MeteredAuthenticatedFetchOptions options;
  options.apiKey = std::string("sk-test");
  options.fetcher = fetcher;
  MeteredAuthenticatedFetchOptions withDispatch = options;
  withDispatch.beforeDispatch = [] {
    api_key_policy::ApiKeyProviderDispatch dispatch{.markTransportStarted = [] {}, .cancelBeforeTransport = [] { return rt::fulfilledTask(); }};
    return rt::fulfilledTask(std::optional<api_key_policy::ApiKeyProviderDispatch>(std::move(dispatch)));
  };
  auto capturing = fetchMeteredResponses(parse(R"({ "model": "gpt-5.6-sol", "reasoning": { "effort": "none" } })"), withDispatch);
  const MeteredResponsesResult captured = co_await std::move(capturing);
  EXPECT_EQ(captured.request_id, std::optional<std::string>("resp_1"));
  EXPECT_EQ(captured.response.status(), 200);
  ASSERT_EQ(bodies->size(), std::size_t{1});
  const json::value lowered = rt::jsonParse(bodies->at(0));
  EXPECT_EQ(rt::getString(lowered, "model"), std::optional<std::string_view>("gpt-5.6-sol-low"));
  // The translated body replaces the effort argument with the model suffix.
  EXPECT_EQ(rt::hasOwn(lowered.as_object(), "reasoning"), false);

  auto minimal = fetchMeteredResponses(parse(R"({ "model": "gpt-5.6-sol", "reasoning": { "effort": "minimal" } })"), options);
  co_await std::move(minimal);
  EXPECT_EQ(rt::getString(rt::jsonParse(bodies->at(1)), "model"), std::optional<std::string_view>("gpt-5.6-sol-low"));
  auto ultra = fetchMeteredResponses(parse(R"({ "model": "gpt-5.6-sol", "reasoning": { "effort": "ultra" } })"), options);
  co_await std::move(ultra);
  EXPECT_EQ(rt::getString(rt::jsonParse(bodies->at(2)), "model"), std::optional<std::string_view>("gpt-5.6-sol-max"));
  auto otherModel = fetchMeteredResponses(parse(R"({ "model": "other-model", "reasoning": { "effort": "max" } })"), options);
  co_await std::move(otherModel);
  EXPECT_EQ(rt::getString(rt::jsonParse(bodies->at(3)), "model"), std::optional<std::string_view>("other-model"));
  // A model the suffix table does not list keeps its own id and its reasoning.
  EXPECT_JSON(rt::jsonParse(bodies->at(3)).at("reasoning"), parse(R"({ "effort": "max" })"));
  auto high = fetchMeteredResponses(parse(R"({ "model": "gpt-5.6-sol", "reasoning": { "effort": "high" } })"), options);
  co_await std::move(high);
  EXPECT_EQ(rt::getString(rt::jsonParse(bodies->at(4)), "model"), std::optional<std::string_view>("gpt-5.6-sol-high"));
}

ASYNC_TEST("metered responses: dispatch admission, abort and transport failures keep their own identity") {
  // An already-aborted caller signal fails before the transport is touched.
  const rt::AbortController controller;
  const rt::Reason reason = rt::makeReason(rt::JsError("client left"));
  controller.abort(reason);
  const auto dispatched = std::make_shared<bool>(false);
  const auto cancelled = std::make_shared<bool>(false);
  MeteredAuthenticatedFetchOptions aborted;
  aborted.apiKey = std::string("sk-test");
  aborted.signal = controller.signal();
  aborted.fetcher = MeteredFetch([dispatched](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    *dispatched = true;
    return jsonResponse(json::object());
  });
  aborted.beforeDispatch = [cancelled] {
    api_key_policy::ApiKeyProviderDispatch dispatch{
      .markTransportStarted = [] {},
      .cancelBeforeTransport =
        [cancelled] {
          *cancelled = true;
          return rt::fulfilledTask();
        },
    };
    return rt::fulfilledTask(std::optional<api_key_policy::ApiKeyProviderDispatch>(std::move(dispatch)));
  };
  auto abortedTask = fetchMeteredResponses(parse(R"({ "model": "gpt-6-astra" })"), aborted);
  EXPECT_REJECTS_REASON(std::move(abortedTask), reason);
  EXPECT_EQ(*dispatched, false);
  EXPECT_EQ(*cancelled, true);

  // The dispatch hooks run in order when the request does go out.
  const auto events = std::make_shared<std::vector<std::string>>();
  MeteredAuthenticatedFetchOptions ordered;
  ordered.apiKey = std::string("sk-test");
  ordered.fetcher = respond([] { return jsonResponse(parse(R"({ "id": "resp_2" })")); });
  ordered.beforeDispatch = [events] {
    api_key_policy::ApiKeyProviderDispatch dispatch{
      .markTransportStarted = [events] { events->push_back("started"); },
      .cancelBeforeTransport = [] { return rt::fulfilledTask(); },
    };
    return rt::fulfilledTask(std::optional<api_key_policy::ApiKeyProviderDispatch>(std::move(dispatch)));
  };
  ordered.onDispatch = [events] { events->push_back("dispatched"); };
  auto orderedTask = fetchMeteredResponses(parse(R"({ "model": "gpt-6-astra" })"), ordered);
  const MeteredResponsesResult response = co_await std::move(orderedTask);
  EXPECT_EQ(response.response.status(), 200);
  EXPECT_EQ(*events, (std::vector<std::string>{"started", "dispatched"}));

  // A rejecting transport becomes the unreachable error.
  MeteredAuthenticatedFetchOptions offline;
  offline.apiKey = std::string("sk-test");
  offline.fetcher = rejecting("offline");
  auto offlineTask = rejectionOf(fetchMeteredResponses(parse(R"({ "model": "gpt-6-astra" })"), offline));
  const rt::Reason offlineReason = co_await std::move(offlineTask);
  const std::optional<MeteredError> unreachable = meteredErrorOf(offlineReason);
  ASSERT(unreachable);
  EXPECT_EQ(unreachable->code, std::string("metered_upstream_unreachable"));
  EXPECT_EQ(unreachable->status, 502.0);

  // A caller abort mid-flight keeps the caller's own reason.
  const rt::AbortController caller;
  const rt::Reason abortReason = rt::makeReason(rt::JsError("caller cancelled"));
  MeteredAuthenticatedFetchOptions midFlight;
  midFlight.apiKey = std::string("sk-test");
  midFlight.signal = caller.signal();
  midFlight.fetcher = MeteredFetch([caller, abortReason](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
    caller.abort(abortReason);
    throw rt::JsError("aborted");
    co_return rt::Response();
  });
  auto midFlightTask = fetchMeteredResponses(parse(R"({ "model": "gpt-6-astra" })"), midFlight);
  EXPECT_REJECTS_REASON(std::move(midFlightTask), abortReason);

  // Without a configured key the paid provider reports itself unavailable.
  const rt::env::ScopedOverride withoutKey({{API_KEY_ENV, std::nullopt}});
  MeteredAuthenticatedFetchOptions emptyKey;
  emptyKey.apiKey = std::string("");
  auto missingTask = rejectionOf(fetchMeteredResponses(parse(R"({ "model": "gpt-6-astra" })"), emptyKey));
  const rt::Reason missingReason = co_await std::move(missingTask);
  const std::optional<MeteredError> missingKey = meteredErrorOf(missingReason);
  ASSERT(missingKey);
  EXPECT_EQ(missingKey->code, std::string("metered_api_key_missing"));
  EXPECT_EQ(missingKey->status, 503.0);
}

ASYNC_TEST("metered token logs: a successful page is parsed, filtered and paginated") {
  const json::value entries = parse(R"([
    { "request_id": "req-1", "model_name": "gpt-6-astra", "quota": 1, "prompt_tokens": 2, "cached_prompt_tokens": 1, "completion_tokens": 3, "created_at": 10 },
    { "request_id": "  ", "other": "{\"request_id\":\"req-2\"}", "model_name": "gpt-6-astra", "quota": 1, "prompt_tokens": 2, "completion_tokens": 3, "created_at": 10 },
    { "request_id": "req-bad-other", "other": "not json", "model_name": "gpt-6-astra", "quota": 1, "prompt_tokens": 2, "completion_tokens": 3, "created_at": 10 },
    { "model_name": "gpt-6-astra", "quota": 1, "prompt_tokens": 2, "completion_tokens": 3, "created_at": 10 },
    { "request_id": "req-blank-model", "model_name": "   ", "quota": 1, "prompt_tokens": 2, "completion_tokens": 3, "created_at": 10 },
    { "request_id": "req-negative", "model_name": "gpt-6-astra", "quota": -1, "prompt_tokens": 2, "completion_tokens": 3, "created_at": 10 },
    { "request_id": "req-no-time", "model_name": "gpt-6-astra", "quota": 1, "prompt_tokens": 2, "completion_tokens": 3 },
    "skip"
  ])");
  const auto requested = std::make_shared<std::vector<std::string>>();
  json::object data;
  data["items"] = entries;
  data["total"] = static_cast<std::int64_t>(entries.as_array().size());
  json::object page;
  page["success"] = true;
  page["data"] = std::move(data);
  const json::value pageValue(std::move(page));
  MeteredTokenLogFetchOptions options;
  options.apiKey = std::string("sk-test");
  options.fetcher = MeteredFetch([requested, pageValue](rt::RequestInfo input, rt::RequestInit) -> rt::Response {
    requested->push_back(urlOf(input));
    return jsonResponse(pageValue);
  });
  options.requestIds = std::vector<std::string>{"req-2"};
  options.startAtMs = 1'000;
  options.endAtMs = 9'000;
  auto task = fetchMeteredTokenLogs(options);
  const std::vector<MeteredTokenLogEntry> logs = co_await std::move(task);

  // The requested id was found on the first page, so the walk stops there.
  ASSERT_EQ(requested->size(), std::size_t{1});
  EXPECT(rt::Regex("\\/api\\/log\\/token\\?key=sk-test&page=1&page_size=100").test(requested->at(0)));
  EXPECT(rt::Regex("start_timestamp=1").test(requested->at(0)));
  EXPECT(rt::Regex("end_timestamp=9").test(requested->at(0)));
  // The `other` JSON fallback supplies the request id, and unusable rows are dropped.
  std::vector<std::string> ids;
  std::vector<std::string> models;
  for (const MeteredTokenLogEntry& log : logs) {
    ids.push_back(log.request_id);
    models.push_back(log.model);
  }
  EXPECT_EQ(ids, (std::vector<std::string>{"req-1", "req-2", "req-bad-other"}));
  ASSERT_EQ(logs.size(), std::size_t{3});
  EXPECT_EQ(logs[0].cached_prompt_tokens, std::optional<double>(1));
  EXPECT_EQ(logs[1].cached_prompt_tokens, std::optional<double>());
  EXPECT_EQ(logs[2].cached_prompt_tokens, std::optional<double>());
  EXPECT_EQ(models, (std::vector<std::string>{"gpt-6-astra", "gpt-6-astra", "gpt-6-astra"}));
  EXPECT_EQ(logs[0].created_at, 10.0);
}

ASYNC_TEST("metered token logs: a bare array page and an enveloped empty page both terminate the walk") {
  // A bare array page carries no total, so the page's own length ends the walk.
  const auto bareCalls = std::make_shared<int>(0);
  MeteredTokenLogFetchOptions bareOptions;
  bareOptions.apiKey = std::string("sk-test");
  bareOptions.fetcher = MeteredFetch([bareCalls](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    *bareCalls += 1;
    return jsonResponse(
      parse(R"({ "success": true, "data": [{ "request_id": "req-1", "model_name": "m", "quota": 1, "prompt_tokens": 1, "completion_tokens": 1, "created_at": 1 }] })"));
  });
  auto bareTask = fetchMeteredTokenLogs(bareOptions);
  const std::vector<MeteredTokenLogEntry> bare = co_await std::move(bareTask);
  EXPECT_EQ(bare.size(), std::size_t{1});
  EXPECT_EQ(*bareCalls, 1);

  // An enveloped page with a larger total continues to a second page; its empty
  // item list ends the walk.
  const auto envelopedCalls = std::make_shared<int>(0);
  MeteredTokenLogFetchOptions envelopedOptions;
  envelopedOptions.apiKey = std::string("sk-test");
  envelopedOptions.fetcher = MeteredFetch([envelopedCalls](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    *envelopedCalls += 1;
    json::value items = *envelopedCalls == 1
                          ? parse(R"([{ "request_id": "req-1", "model_name": "m", "quota": 1, "prompt_tokens": 1, "completion_tokens": 1, "created_at": 1 }])")
                          : json::value(json::array());
    json::object data;
    data["items"] = std::move(items);
    data["total"] = 250;
    json::object page;
    page["success"] = true;
    page["data"] = std::move(data);
    return jsonResponse(page);
  });
  auto envelopedTask = fetchMeteredTokenLogs(envelopedOptions);
  const std::vector<MeteredTokenLogEntry> enveloped = co_await std::move(envelopedTask);
  EXPECT_EQ(enveloped.size(), std::size_t{1});
  EXPECT_EQ(*envelopedCalls, 2);
}

ASYNC_TEST("metered token logs: every malformed envelope is rejected with its code") {
  const auto rejectionFor = [](MeteredFetch fetcher) {
    MeteredTokenLogFetchOptions options;
    options.apiKey = std::string("sk-test");
    options.fetcher = std::move(fetcher);
    return rejectionOf(fetchMeteredTokenLogs(options));
  };

  auto rejectedTask = rejectionFor(rejecting("offline"));
  const rt::Reason rejectedReason = co_await std::move(rejectedTask);
  const std::optional<MeteredError> rejected = meteredErrorOf(rejectedReason);
  ASSERT(rejected);
  EXPECT_EQ(rejected->code, std::string("metered_logs_unavailable"));

  auto nonOkTask = rejectionFor(respond([] { return jsonResponse(parse(R"({ "success": true })"), 503); }));
  const rt::Reason nonOkReason = co_await std::move(nonOkTask);
  const std::optional<MeteredError> nonOk = meteredErrorOf(nonOkReason);
  ASSERT(nonOk);
  EXPECT_EQ(nonOk->code, std::string("metered_logs_unavailable"));
  // The client-visible status is the gateway's own 502; the upstream status rides along.
  EXPECT_EQ(nonOk->status, 502.0);
  EXPECT_EQ(nonOk->upstream_status, std::optional<double>(503));

  auto invalidJsonTask = rejectionFor(respond([] {
    rt::ResponseInit init;
    init.status = 200;
    return rt::Response(rt::BodyInit(std::string("<html>")), std::move(init));
  }));
  const rt::Reason invalidJsonReason = co_await std::move(invalidJsonTask);
  const std::optional<MeteredError> invalidJson = meteredErrorOf(invalidJsonReason);
  ASSERT(invalidJson);
  EXPECT_EQ(invalidJson->code, std::string("metered_logs_invalid"));

  auto failedEnvelopeTask = rejectionFor(respond([] { return jsonResponse(parse(R"({ "success": false })")); }));
  const rt::Reason failedEnvelopeReason = co_await std::move(failedEnvelopeTask);
  const std::optional<MeteredError> failedEnvelope = meteredErrorOf(failedEnvelopeReason);
  ASSERT(failedEnvelope);
  EXPECT_EQ(failedEnvelope->code, std::string("metered_logs_invalid"));

  auto missingItemsTask = rejectionFor(respond([] { return jsonResponse(parse(R"({ "success": true, "data": "none" })")); }));
  const rt::Reason missingItemsReason = co_await std::move(missingItemsTask);
  const std::optional<MeteredError> missingItems = meteredErrorOf(missingItemsReason);
  ASSERT(missingItems);
  EXPECT_EQ(missingItems->code, std::string("metered_logs_invalid"));

  // An empty page is valid and simply yields no logs.
  MeteredTokenLogFetchOptions emptyOptions;
  emptyOptions.apiKey = std::string("sk-test");
  emptyOptions.fetcher = respond([] { return jsonResponse(parse(R"({ "success": true, "data": [] })")); });
  auto emptyTask = fetchMeteredTokenLogs(emptyOptions);
  const std::vector<MeteredTokenLogEntry> empty = co_await std::move(emptyTask);
  EXPECT(empty.empty());
}

TEST("metered base url is the documented host") { EXPECT_EQ(std::string(METERED_BASE_URL), std::string("https://api.openlux.ai")); }

ASYNC_TEST("metered discovery and pricing cover the stale-generation, unusable-fetch and overflow guards") {
  const DiscoveryKey key;
  // A failure that lands after the cache generation moved on must not arm the
  // backoff, so the next ordinary call still reaches upstream.
  const rt::Resolvers<void> gate = rt::withResolvers<void>();
  const rt::Promise<void> gatePromise = gate.promise;
  const auto calls = std::make_shared<int>(0);
  const MeteredFetch fetcher([calls, gatePromise](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
    *calls += 1;
    if (*calls == 1) {
      co_await gatePromise.wait();
      rt::ResponseInit init;
      init.status = 503;
      rt::Response unavailable(rt::BodyInit(std::string("unavailable")), std::move(init));
      co_return unavailable;
    }
    rt::Response listed = jsonResponse(parse(R"({ "data": [{ "id": "after-reset" }] })"));
    co_return listed;
  });
  const rt::Promise<std::optional<MeteredModelsSnapshot>> stale(fetchMeteredModels(discoveryOptions(fetcher)));
  resetMeteredModelsCacheForTest();
  gate.resolve();
  const std::optional<MeteredModelsSnapshot> staleResult = co_await stale.wait();
  EXPECT(!staleResult);
  auto recoveredTask = discover(discoveryOptions(fetcher));
  const std::optional<MeteredModelsSnapshot> recovered = co_await std::move(recoveredTask);
  EXPECT_EQ(*calls, 2);
  ASSERT(recovered && !recovered->models.empty());
  EXPECT_EQ(recovered->models[0].id, std::string("after-reset"));

  // Without a fetcher option, a test seam or the module's own fetch binding,
  // discovery has no transport and reports the (empty) cache.
  setMeteredModelsFetchForTest(MeteredFetch());
  const auto replacedFetchCalls = std::make_shared<int>(0);
  {
    const rt::GlobalFetchOverride replaced(MeteredFetch([replacedFetchCalls](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
      *replacedFetchCalls += 1;
      throw rt::JsError("replaced fetch");
      co_return rt::Response();
    }));
    FetchMeteredModelsOptions forced;
    forced.force = true;
    auto cachedOnlyTask = discover(forced);
    const std::optional<MeteredModelsSnapshot> cachedOnly = co_await std::move(cachedOnlyTask);
    ASSERT(cachedOnly && !cachedOnly->models.empty());
    EXPECT_EQ(cachedOnly->models[0].id, std::string("after-reset"));
    EXPECT_EQ(*replacedFetchCalls, 0);
  }

  // A ratio that is finite on its own but overflows once the completion ratio
  // folds in stays eligible, while its unusable coefficient is omitted.
  InitializeMeteredPricingOptions overflowOptions{.codexModelIds = {"overflow-model"}};
  overflowOptions.now = [] { return 7.0; };
  overflowOptions.fetcher =
    pricingFetcher(jsonResponse(parse(R"({ "success": true, "data": { "model_ratio": { "overflow-model": 1e308 }, "model_price": {} } })")), statusEnvelope());
  auto overflowTask = initializeMeteredPricing(overflowOptions);
  const MeteredPricingSnapshot overflow = co_await std::move(overflowTask);
  EXPECT_EQ(overflow.eligible_model_ids, std::vector<std::string>{"overflow-model"});
  EXPECT_JSON(json::value(overflow.model_quota_coefficients), parse("{}"));
}

ASYNC_TEST("metered responses and logs reject unserializable bodies and wrap a non-Error abort reason") {
  // A reasoning tier the model's suffix table does not list leaves the body alone.
  const auto bodies = std::make_shared<std::vector<std::string>>();
  MeteredAuthenticatedFetchOptions options;
  options.apiKey = std::string("sk-test");
  options.fetcher = MeteredFetch([bodies](rt::RequestInfo, rt::RequestInit init) -> rt::Response {
    bodies->push_back(bodyOf(init));
    return jsonResponse(parse(R"({ "id": "resp_1" })"));
  });
  auto task = fetchMeteredResponses(parse(R"({ "model": "gpt-5.6-sol", "reasoning": { "effort": "lowest" } })"), options);
  co_await std::move(task);
  ASSERT_EQ(bodies->size(), std::size_t{1});
  EXPECT_JSON(rt::jsonParse(bodies->at(0)), parse(R"({ "model": "gpt-5.6-sol", "reasoning": { "effort": "lowest" } })"));

  // A non-Error abort reason is carried as the wrapper's cause instead of being lost.
  const rt::AbortController controller;
  MeteredTokenLogFetchOptions logOptions;
  logOptions.apiKey = std::string("sk-test");
  logOptions.signal = controller.signal();
  logOptions.fetcher = MeteredFetch([](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
    const rt::Promise<rt::Response> never;
    rt::Response response = co_await never.wait();
    co_return response;
  });
  const rt::Promise<std::vector<MeteredTokenLogEntry>> pending(fetchMeteredTokenLogs(logOptions));
  controller.abort(rt::thrown(json::value("caller string reason")));
  bool wrapped = false;
  try {
    co_await pending.wait();
  } catch (const rt::JsError& error) {
    wrapped = true;
    EXPECT_EQ(error.name, std::string("Error"));
    EXPECT_EQ(error.message, std::string("Metered request failed"));
    EXPECT_EQ(rt::thrownValue(rt::errorCause(std::current_exception()).value_or(nullptr)), std::optional<json::value>(json::value("caller string reason")));
  } catch (...) {
  }
  EXPECT(wrapped);
}
