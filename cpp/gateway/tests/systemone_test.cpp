// Links: tests/systemone_test.cpp
// src/provider/health.cpp src/openai_telemetry.cpp src/provider/claude_code_accounts.cpp src/handler/http.cpp tests/helpers/counting_kv.cpp src/utils.cpp src/api_key_policy.cpp src/provider/claude_code_credentials.cpp
// src/types.cpp src/authority/native_api_key.cpp src/responses_failover_stream.cpp src/authority/native_api_key_policy.cpp src/api_keys.cpp src/provider/selection.cpp src/config.cpp tests/helpers/loopback_http.cpp
// src/provider/openrouter.cpp src/models/openrouter_models.cpp src/systemone/handlers.cpp src/kv.cpp src/http.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/responses_stream.cpp
// src/authority/native_paid_policy.cpp src/codex/quota.cpp src/runtime/http_early_sse.cpp src/provider/cerebras_rate_limits.cpp src/release.cpp src/request.cpp src/provider/removed_provider_telemetry.cpp src/storage/kv.cpp
// src/storage/sqlite_readonly.cpp src/auth/relay.cpp src/codex/responses_error.cpp src/runtime/http_upgrade.cpp src/upstream_wire.cpp src/runtime/http_tasks.cpp src/runtime/websocket.cpp src/storage/native_kv.cpp
// src/deepseek/index.cpp src/bounded_response_body.cpp src/provider/lithos.cpp src/provider/cerebras.cpp src/provider/lithos_rate_limits.cpp
// Port of the direct expired-dispatch case in tests/systemone.test.ts plus bounded decision-transport coverage.
// Includes the original handler/http route-predicate registration; it makes no ingress claim.
// The SystemOne handler cases and real ephemeral-loopback deadline case are ported below.
#include "uos/provider/openrouter.hpp"
#include "uos/systemone/handlers.hpp"
#include "uos/handler/http.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/runtime/http.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/url.hpp"
#include "tests/helpers/loopback_http.hpp"
#include "uos/api_keys.hpp"
#include "uos/api_key_policy.hpp"
#include "tests/helpers/counting_kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace provider = uos::provider::openrouter;
  namespace policy = uos::api_key_policy;
  using uos::tests::helpers::counting_kv::CountingKv;
  using uos::tests::helpers::counting_kv::encodeKey;
  json::object decisionBody() { return json::object{{"state", json::object{{"question", "fixture"}}}, {"questions", json::object{{"candidate", json::object{{"type", "choice"}}}}}}; }
  struct Counts { int fetch = 0; int cancelled = 0; int started = 0; int admitted = 0; int headers = 0; int dispatched = 0; };
  rt::Task<std::optional<policy::ApiKeyProviderDispatch>> delayedAdmission(policy::ApiKeyUsageReservation reservation, std::shared_ptr<Counts> counts) {
    const std::string name = "openrouter";
    rt::Promise<std::optional<policy::ApiKeyProviderDispatch>> admitting(reservation.beforeProviderDispatch(name));
    const auto dispatch = co_await admitting.wait(); EXPECT(dispatch);
    co_await rt::sleep(10);
    policy::ApiKeyProviderDispatch wrapped;
    wrapped.markTransportStarted = [counts, dispatch] { ++counts->started; dispatch->markTransportStarted(); };
    wrapped.cancelBeforeTransport = [counts, dispatch] { ++counts->cancelled; return dispatch->cancelBeforeTransport(); };
    co_return std::optional<policy::ApiKeyProviderDispatch>(wrapped);
  }
}

ASYNC_TEST("systemone cancels an expired pre-dispatch reservation before returning a bounded upstream error") {
  const rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w05c2"}}};
  const std::string tokenHash = "systemone-deadline";
  const uos::types::ApiKeyHashRecord record{
    .id = "deadline", .expires_at_ms = -1, .revoked_at_ms = std::nullopt, .usage_limit_requests = 1, .usage_requests = 0,
    .usage_reset_at_ms = rt::dateNow() + 60 * 60'000, .window_ms = 60 * 60'000, .usage_quota_version = 3,
    .paid_fallback_enabled = false, .paid_fallback_limit_microcredits = 0, .paid_fallback_spent_microcredits = 0,
    .paid_fallback_reserved_microcredits = 0, .paid_fallback_reservation_request_id = std::nullopt};
  const auto principal = policy::apiKeyPolicyFromHashRecord(tokenHash, record, rt::dateNow()); EXPECT(principal);
  const auto kv = std::make_shared<CountingKv>(); kv->seed(uos::api_keys::apiKeyHashKey(tokenHash), rt::KvValue(json::value_from(record)));
  policy::ReserveApiKeyUsageV3Options admissionOptions; admissionOptions.kv = kv; admissionOptions.deferWhenFull = true;
  auto admitting = policy::reserveApiKeyUsageV3(*principal, "systemone-deadline", "systemone", admissionOptions);
  const auto admitted = co_await std::move(admitting); EXPECT(std::holds_alternative<policy::ApiKeyUsageReservationDecisionOk>(admitted));
  const auto reservation = std::get<policy::ApiKeyUsageReservationDecisionOk>(admitted).reservation;
  const auto counts = std::make_shared<Counts>(); provider::OpenRouterDecisionInput input;
  input.body = decisionBody(); input.apiKey = std::string("or-test-key"); input.timeoutMs = 1;
  input.fetcher = rt::Fetch([counts](rt::RequestInfo, rt::RequestInit) { ++counts->fetch; return rt::Response::json(json::object{{"answers", json::object{}}}); });
  provider::OpenRouterDispatchHooks hooks; hooks.beforeDispatch = [reservation, counts] { return delayedAdmission(reservation, counts); }; input.hooks = hooks;
  auto requesting = provider::fetchOpenRouterSystemOne(input); bool rejected = false;
  try { const auto ignored = co_await std::move(requesting); (void)ignored; }
  catch (const provider::OpenRouterError& error) { rejected = error.code == "openrouter_upstream_unreachable" && error.status == 502 && error.beforeTransport; }
  EXPECT(rejected); EXPECT_EQ(counts->fetch, 0); EXPECT_EQ(counts->started, 0); EXPECT_EQ(counts->cancelled, 1);
  const auto* window = kv->entries.get(encodeKey(policy::apiKeyUsageV3WindowKey(*principal))); EXPECT(window);
  const auto row = window->value.toJson(); EXPECT_EQ(rt::getNumber(row, "committed_requests"), std::optional<double>(0)); EXPECT_EQ(rt::getNumber(row, "reserved_requests"), std::optional<double>(0));
  const auto* request = kv->entries.get(encodeKey(policy::apiKeyUsageV3RequestKey(*principal, "systemone-deadline"))); EXPECT(request);
  const auto requestRow = request->value.toJson(); EXPECT_EQ(rt::getString(requestRow, "release_reason"), std::optional<std::string_view>("transport_cancelled_before_fetch"));
}

ASYNC_TEST("OpenRouter decision transport forwards verbatim envelopes and calls every dispatch hook once") {
  const rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w05c2"}}};
  const auto counts = std::make_shared<Counts>(); std::vector<std::string> order; provider::OpenRouterDecisionInput input;
  input.body = decisionBody(); input.apiKey = std::string("explicit-fixture-key");
  provider::OpenRouterDispatchHooks hooks;
  hooks.beforeDispatch = [&] { ++counts->admitted; order.push_back("before"); policy::ApiKeyProviderDispatch dispatch;
    dispatch.markTransportStarted = [&] { ++counts->started; order.push_back("started"); }; dispatch.cancelBeforeTransport = [] { return rt::fulfilledTask(); };
    return rt::fulfilledTask(std::optional<policy::ApiKeyProviderDispatch>(dispatch)); };
  hooks.onDispatch = [&] { ++counts->dispatched; order.push_back("dispatch"); }; hooks.onHeaders = [&] { ++counts->headers; order.push_back("headers"); }; input.hooks = hooks;
  const json::object answer{{"answers", json::object{{"candidate", json::object{{"type", "choice"}, {"choice", "fixture"}}}}}};
  input.fetcher = rt::Fetch([&](rt::RequestInfo url, rt::RequestInit init) {
    ++counts->fetch; order.push_back("fetch"); EXPECT_EQ(std::get<std::string>(url), std::string(provider::OPENROUTER_SYSTEMONE_URL));
    EXPECT_EQ(init.method, std::optional<std::string>("POST")); EXPECT(init.headers); EXPECT_EQ(init.headers->get("authorization"), std::optional<std::string>("Bearer explicit-fixture-key"));
    EXPECT(init.body); EXPECT(rt::deepEqual(rt::jsonParse(std::get<std::string>(*init.body)), input.body)); EXPECT(init.signal); return rt::Response::json(answer);
  });
  auto requesting = provider::fetchOpenRouterSystemOne(input); const auto payload = co_await std::move(requesting); EXPECT(rt::deepEqual(payload, answer));
  EXPECT_EQ(counts->admitted, 1); EXPECT_EQ(counts->started, 1); EXPECT_EQ(counts->dispatched, 1); EXPECT_EQ(counts->fetch, 1); EXPECT_EQ(counts->headers, 1);
  const std::vector<std::string> expected{"before", "started", "dispatch", "fetch", "headers"}; EXPECT_EQ(order, expected);
}

ASYNC_TEST("OpenRouter decision transport maps every HTTP failure and refuses non-object JSON") {
  const rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w05c2"}}};
  for (const int status : std::vector<int>{400, 401, 403, 413, 429, 500, 503}) {
    provider::OpenRouterDecisionInput input; input.body = decisionBody(); input.apiKey = std::string("fixture-key");
    input.fetcher = rt::Fetch([status](rt::RequestInfo, rt::RequestInit) { return rt::Response("failure", rt::ResponseInit{.status = status}); });
    auto requesting = provider::fetchOpenRouterSystemOne(input); bool rejected = false;
    try { const auto ignored = co_await std::move(requesting); (void)ignored; }
    catch (const provider::OpenRouterError& error) { rejected = error.code == "openrouter_upstream_error" && error.upstreamStatus == status && error.status == (status == 400 || status == 413 || status == 429 ? status : 502); }
    EXPECT(rejected);
  }
  for (const std::string& text : std::vector<std::string>{"not json", "null", "[]", "7", "\"text\"", "true", ""}) {
    provider::OpenRouterDecisionInput input; input.body = decisionBody(); input.apiKey = std::string("fixture-key");
    input.fetcher = rt::Fetch([text](rt::RequestInfo, rt::RequestInit) { return rt::Response(text); });
    auto requesting = provider::fetchOpenRouterSystemOne(input); bool rejected = false;
    try { const auto ignored = co_await std::move(requesting); (void)ignored; }
    catch (const provider::OpenRouterError& error) { rejected = error.code == "openrouter_upstream_invalid_response" && error.status == 502 && !error.upstreamStatus; }
    EXPECT(rejected);
  }
}

ASYNC_TEST("OpenRouter decision transport keeps local hook and caller cancellation reasons") {
  const rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w05c2"}}};
  const auto refusal = rt::makeReason(rt::JsError("quota refused")); provider::OpenRouterDecisionInput input; input.body = decisionBody(); input.apiKey = std::string("fixture-key");
  int calls = 0; input.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++calls; return rt::Response(); });
  provider::OpenRouterDispatchHooks hooks; hooks.beforeDispatch = [refusal]() -> rt::Task<std::optional<policy::ApiKeyProviderDispatch>> { std::rethrow_exception(refusal); }; input.hooks = hooks;
  auto requesting = provider::fetchOpenRouterSystemOne(input); EXPECT_REJECTS_REASON(std::move(requesting), refusal); EXPECT_EQ(calls, 0);
  input.hooks = std::nullopt; const auto cancelled = rt::makeReason(rt::JsError("caller gone")); rt::AbortController caller;
  input.signal = caller.signal(); input.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit) -> rt::Response { caller.abort(cancelled); std::rethrow_exception(cancelled); });
  auto cancellation = provider::fetchOpenRouterSystemOne(input); EXPECT_REJECTS_REASON(std::move(cancellation), cancelled);
  input.signal = std::nullopt; input.fetcher = rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("network"); });
  auto network = provider::fetchOpenRouterSystemOne(input); EXPECT_REJECTS_MESSAGE(std::move(network), provider::OpenRouterError, "openrouter_upstream_unreachable");
}

ASYNC_TEST("OpenRouter decision credentials preserve undefined null empty and whitespace input semantics") {
  const rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w05c2"}, {"OPENROUTER_API_KEY", "  env-key  "}}};
  provider::OpenRouterDecisionInput input; input.body = decisionBody(); std::string authorization;
  input.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { authorization = init.headers->get("authorization").value_or(""); return rt::Response::json(json::object{}); });
  auto fromEnvironment = provider::fetchOpenRouterSystemOne(input); auto payload = co_await std::move(fromEnvironment); EXPECT(payload.empty()); EXPECT_EQ(authorization, "Bearer env-key");
  input.apiKey = nullptr; auto nullKey = provider::fetchOpenRouterSystemOne(input); EXPECT_REJECTS_MESSAGE(std::move(nullKey), provider::OpenRouterError, "openrouter_api_key_missing");
  input.apiKey = std::string(); auto emptyKey = provider::fetchOpenRouterSystemOne(input); EXPECT_REJECTS_MESSAGE(std::move(emptyKey), provider::OpenRouterError, "openrouter_api_key_missing");
  input.apiKey = std::string("  "); auto whitespace = provider::fetchOpenRouterSystemOne(input); payload = co_await std::move(whitespace); EXPECT(payload.empty()); EXPECT_EQ(authorization, "Bearer");
}

namespace systemone = uos::systemone::handlers;
namespace telemetry = uos::openai_telemetry;
namespace selection = uos::provider::selection;
namespace health = uos::provider::health;
namespace {
  json::value originalState() {
    return json::object{{"question", json::object{{"capabilityKey", "search.person.filter_group"}, {"filterKey", "search.lead.filter.industry"},
      {"prompt", "Which candidate is the filter group container?"}, {"options", json::array{json::object{{"optionId", "g-industry"}, {"selector", "fieldset[data-x-search-filter=\"INDUSTRY\"]"}}}}}}};
  }
  json::value originalQuestions() {
    return json::object{{"candidate", json::object{{"type", "choice"}, {"instructions", "Choose one offered candidate or not_stated."},
      {"criteria", json::object{{"g-industry", "The INDUSTRY fieldset."}, {"not_stated", "No sound repair."}}}}}};
  }
  json::object answerPayload() {
    return json::object{{"model", "typesafe/jev-1.13-20260917"}, {"answers", json::object{{"candidate", json::object{{"type", "choice"}, {"choice", "g-industry"}, {"confidence", 0.99}}}}},
      {"usage", json::object{{"input_tokens", 1118}, {"output_tokens", 117}, {"cost", 4.6956e-5}}}};
  }
  json::object originalBody() { return json::object{{"state", originalState()}, {"questions", originalQuestions()}}; }
  rt::Request systemRequest(const json::value& body) {
    return rt::Request("https://fixture.example.invalid/v1/systemone", rt::RequestInit{.method = "POST", .headers = rt::Headers{{"content-type", "application/json"}}, .body = rt::jsonStringify(body)});
  }
  struct SystemScope {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c7"}}};
    std::string originalAuth = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<CountingKv> kv = std::make_shared<CountingKv>();
    SystemScope() { uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(kv); selection::resetProviderSelectionCacheForTest(); health::resetProviderHealthThrottleForTest(); }
    ~SystemScope() { uos::kv::setKvForTest(nullptr); selection::resetProviderSelectionCacheForTest(); health::resetProviderHealthThrottleForTest(); uos::config::config.codexAuthJsonB64 = originalAuth; }
  };
  systemone::SystemOneHandlerDeps systemDeps(rt::Fetch fetcher = {}) {
    systemone::SystemOneHandlerDeps deps;
    deps.apiKey = [] { return std::optional<std::string>("or-test-key"); };
    deps.fetcher = fetcher ? std::move(fetcher) : rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(answerPayload()); });
    return deps;
  }
  struct SystemLedger { std::shared_ptr<CountingKv> kv; policy::ApiKeyPolicy principal; };
  SystemLedger ledgerFixture(const std::string& id, double limit) {
    const std::string tokenHash = "systemone-" + id;
    const uos::types::ApiKeyHashRecord record{.id = id, .expires_at_ms = -1, .revoked_at_ms = std::nullopt, .usage_limit_requests = limit, .usage_requests = 0,
      .usage_reset_at_ms = rt::dateNow() + 60 * 60'000, .window_ms = 60 * 60'000, .usage_quota_version = 3, .paid_fallback_enabled = false,
      .paid_fallback_limit_microcredits = 0, .paid_fallback_spent_microcredits = 0, .paid_fallback_reserved_microcredits = 0, .paid_fallback_reservation_request_id = std::nullopt};
    const auto principal = policy::apiKeyPolicyFromHashRecord(tokenHash, record, rt::dateNow()); EXPECT(principal);
    const auto kv = std::make_shared<CountingKv>(); kv->seed(uos::api_keys::apiKeyHashKey(tokenHash), rt::KvValue(json::value_from(record)));
    return {kv, *principal};
  }
  rt::Task<policy::ApiKeyUsageReservation> admissionFor(SystemLedger fixture, std::string requestId) {
    policy::ReserveApiKeyUsageV3Options options; options.kv = fixture.kv; options.deferWhenFull = true;
    auto admitting = policy::reserveApiKeyUsageV3(fixture.principal, requestId, "systemone", options); const auto decision = co_await std::move(admitting);
    EXPECT(std::holds_alternative<policy::ApiKeyUsageReservationDecisionOk>(decision)); co_return std::get<policy::ApiKeyUsageReservationDecisionOk>(decision).reservation;
  }
  telemetry::UsageContext contextWith(const policy::ApiKeyUsageReservation& reservation) {
    telemetry::UsageContext context{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt}; context.beforeProviderDispatch = reservation.beforeProviderDispatch; return context;
  }
  json::value storedWindow(const SystemLedger& fixture) {
    const auto* entry = fixture.kv->entries.get(encodeKey(policy::apiKeyUsageV3WindowKey(fixture.principal))); EXPECT(entry); return entry->value.toJson();
  }
  std::optional<json::value> storedRequest(const SystemLedger& fixture, const std::string& id) {
    const auto* entry = fixture.kv->entries.get(encodeKey(policy::apiKeyUsageV3RequestKey(fixture.principal, id))); return entry ? std::optional<json::value>(entry->value.toJson()) : std::nullopt;
  }
  rt::Task<std::optional<policy::ApiKeyProviderDispatch>> expiredSystemDispatch(std::string path, std::shared_ptr<int> cancellations) {
    rt::test::clock().advance(60'001); co_await rt::sleep(1);
    policy::ApiKeyQuotaDispatchErrorOptions options; options.status = 429; options.code = "rate_limit_exceeded"; options.errorType = "rate_limit_error"; options.retryAfter = "7";
    options.headers = rt::OrderedMap<std::string, std::string>{{"RateLimit-Limit", "1"}, {"RateLimit-Remaining", "0"}};
    const policy::ApiKeyQuotaDispatchError quotaError("Synthetic quota refusal", options);
    if (path == "/quota") throw quotaError;
    policy::ApiKeyProviderDispatch dispatch;
    dispatch.markTransportStarted = [] { EXPECT(false); };
    dispatch.cancelBeforeTransport = [cancellations, path, quotaError] {
      ++*cancellations;
      if (path == "/cancel-quota") return rt::Promise<void>::reject(rt::makeReason(quotaError)).wait();
      return rt::fulfilledTask();
    };
    co_return dispatch;
  }
}

ASYNC_TEST("systemone loopback deadline responses preserve quota hook errors and headers") {
  const SystemScope scope; const uos::tests::helpers::loopback_http::ScopedLoopbackFetch network;
  const auto calls = std::make_shared<int>(0); const auto cancellations = std::make_shared<int>(0);
  auto deps = systemDeps(rt::Fetch([calls](rt::RequestInfo, rt::RequestInit) { ++*calls; return rt::Response::json(answerPayload()); }));
  uos::runtime::http::ServeHttpOptions options; options.hostname = "127.0.0.1"; options.port = 0;
  const uos::runtime::http::ServeHandler handler = [deps, cancellations](rt::Request req, uos::runtime::http::ServeHandlerInfo) {
    telemetry::UsageContext context{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt}; const std::string path = rt::URL(req.url()).pathname();
    context.beforeProviderDispatch = [path, cancellations](std::string) { return expiredSystemDispatch(path, cancellations); };
    return systemone::handleSystemOne(req, context, deps);
  };
  const auto server = uos::runtime::http::serveHttp(options, handler); const auto address = co_await server.ready.wait();
  const std::vector<std::string> paths{"/v1/systemone", "/quota", "/cancel-quota"};
  for (const auto& path : paths) {
    const std::string url = "http://127.0.0.1:" + rt::numberToString(address.port) + path; const rt::RequestInit init{.method = "POST", .headers = rt::Headers{{"content-type", "application/json"}}, .body = rt::jsonStringify(originalBody())};
    auto fetching = rt::fetch(url, init); auto response = co_await std::move(fetching); const bool quota = path != "/v1/systemone"; EXPECT_EQ(response.status(), quota ? 429 : 502);
    auto reading = response.json(); const auto actual = co_await std::move(reading);
    json::object error{{"message", quota ? "Synthetic quota refusal" : "System One upstream unreachable"}, {"type", quota ? "rate_limit_error" : "invalid_request_error"}, {"code", quota ? "rate_limit_exceeded" : "openrouter_upstream_unreachable"}};
    if (quota) error["param"] = nullptr;
    EXPECT(rt::deepEqual(actual, json::object{{"error", error}}));
    EXPECT_EQ(response.headers().get("retry-after"), quota ? std::optional<std::string>("7") : std::nullopt); EXPECT_EQ(response.headers().get("ratelimit-limit"), quota ? std::optional<std::string>("1") : std::nullopt); EXPECT_EQ(response.headers().get("ratelimit-remaining"), quota ? std::optional<std::string>("0") : std::nullopt);
  }
  EXPECT_EQ(*calls, 0); EXPECT_EQ(*cancellations, 2); co_await rt::sleep(1); const auto status = co_await health::getOpenRouterProviderHealth(); EXPECT_EQ(status.state, std::string("unknown")); co_await server.shutdown().wait();
}

ASYNC_TEST("systemone commits the api-key request reservation exactly once before dispatch") {
  const SystemScope scope; const auto fixture = ledgerFixture("commit-once", 2); auto admission = admissionFor(fixture, "systemone-commit-once"); const auto reservation = co_await std::move(admission);
  int calls = 0; auto deps = systemDeps(rt::Fetch([&calls](rt::RequestInfo, rt::RequestInit) { ++calls; return rt::Response::json(answerPayload()); }));
  auto handling = systemone::handleSystemOne(systemRequest(originalBody()), contextWith(reservation), deps); const auto response = co_await std::move(handling); EXPECT_EQ(response.status(), 200); EXPECT_EQ(calls, 1);
  const auto window = storedWindow(fixture); EXPECT_EQ(rt::getNumber(window, "committed_requests"), std::optional<double>(1)); EXPECT_EQ(rt::getNumber(window, "reserved_requests"), std::optional<double>(0));
  const auto row = storedRequest(fixture, "systemone-commit-once"); EXPECT(row); EXPECT_EQ(rt::getString(*row, "state"), std::optional<std::string_view>("dispatched")); EXPECT_EQ(rt::getString(*row, "provider"), std::optional<std::string_view>("openrouter")); co_await rt::sleep(1);
}

ASYNC_TEST("systemone refuses an exhausted api-key window before the upstream fetch") {
  const SystemScope scope; const auto fixture = ledgerFixture("exhausted", 1); auto firstTask = admissionFor(fixture, "systemone-exhausted-1"); const auto first = co_await std::move(firstTask);
  const std::string providerName = "openrouter"; auto dispatching = first.beforeProviderDispatch(providerName); const auto dispatch = co_await std::move(dispatching); (void)dispatch;
  auto secondTask = admissionFor(fixture, "systemone-exhausted-2"); const auto second = co_await std::move(secondTask); int calls = 0;
  auto deps = systemDeps(rt::Fetch([&calls](rt::RequestInfo, rt::RequestInit) { ++calls; return rt::Response::json(answerPayload()); }));
  auto handling = systemone::handleSystemOne(systemRequest(originalBody()), contextWith(second), deps); auto response = co_await std::move(handling); EXPECT_EQ(response.status(), 429);
  auto reading = response.json(); const auto payload = co_await std::move(reading); const auto* error = rt::get(payload, "error"); EXPECT_EQ(rt::getString(*error, "code"), std::optional<std::string_view>("rate_limit_exceeded")); EXPECT_EQ(rt::getString(*error, "type"), std::optional<std::string_view>("rate_limit_error"));
  EXPECT_EQ(response.headers().get("ratelimit-limit"), std::optional<std::string>("1")); EXPECT_EQ(response.headers().get("ratelimit-remaining"), std::optional<std::string>("0")); EXPECT(rt::stringToNumber(response.headers().get("retry-after").value_or("")) >= 1); EXPECT_EQ(calls, 0);
  const auto window = storedWindow(fixture); EXPECT_EQ(rt::getNumber(window, "committed_requests"), std::optional<double>(1)); EXPECT_EQ(rt::getNumber(window, "reserved_requests"), std::optional<double>(0)); EXPECT(!storedRequest(fixture, "systemone-exhausted-2")); co_await rt::sleep(1);
}

ASYNC_TEST("systemone proxies one typed question set upstream with the server key") {
  const SystemScope scope; std::string capturedUrl; std::optional<rt::Headers> headers; json::value sent;
  auto deps = systemDeps(rt::Fetch([&](rt::RequestInfo url, rt::RequestInit init) { capturedUrl = std::get<std::string>(url); headers = init.headers; sent = rt::jsonParse(std::get<std::string>(*init.body)); return rt::Response::json(answerPayload()); }));
  auto handling = systemone::handleSystemOne(systemRequest(originalBody()), std::nullopt, deps); auto response = co_await std::move(handling); EXPECT_EQ(response.status(), 200); EXPECT_EQ(capturedUrl, std::string(provider::OPENROUTER_SYSTEMONE_URL)); EXPECT_EQ(headers->get("authorization"), std::optional<std::string>("Bearer or-test-key"));
  EXPECT_EQ(rt::getString(sent, "model"), std::optional<std::string_view>(systemone::SYSTEMONE_DEFAULT_MODEL)); EXPECT(rt::deepEqual(*rt::get(sent, "state"), originalState())); EXPECT(rt::deepEqual(*rt::get(sent, "questions"), originalQuestions()));
  auto reading = response.json(); const auto payload = co_await std::move(reading); const auto* candidate = rt::get(*rt::get(payload, "answers"), "candidate"); EXPECT_EQ(rt::getString(*candidate, "choice"), std::optional<std::string_view>("g-industry")); EXPECT_EQ(rt::getNumber(*candidate, "confidence"), std::optional<double>(0.99)); co_await rt::sleep(1);
}

ASYNC_TEST("systemone accepts an explicit typesafe model and rejects other models") {
  const SystemScope scope; std::vector<std::string> seen; auto deps = systemDeps(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { const auto sent = rt::jsonParse(std::get<std::string>(*init.body)); seen.emplace_back(*rt::getString(sent, "model")); return rt::Response::json(answerPayload()); }));
  auto input = originalBody(); input["model"] = "~typesafe/jev-1.13.0"; auto handling = systemone::handleSystemOne(systemRequest(input), std::nullopt, deps); const auto response = co_await std::move(handling); EXPECT_EQ(response.status(), 200);
  const std::vector<std::string> invalid{"openai/gpt-6-sol", "typesafe/", "~typesafe/" + rt::repeat("a", 90)};
  for (const auto& model : invalid) { input["model"] = model; auto rejecting = systemone::handleSystemOne(systemRequest(input), std::nullopt, deps); const auto rejected = co_await std::move(rejecting); EXPECT_EQ(rejected.status(), 400); }
  EXPECT_EQ(seen, std::vector<std::string>{"~typesafe/jev-1.13.0"}); co_await rt::sleep(1);
}

ASYNC_TEST("systemone requires configuration and refuses malformed question sets") {
  const SystemScope scope; auto deps = systemDeps(); deps.apiKey = [] { return std::optional<std::string>{}; };
  auto missing = systemone::handleSystemOne(systemRequest(originalBody()), std::nullopt, deps); auto unconfigured = co_await std::move(missing); EXPECT_EQ(unconfigured.status(), 503); auto reading = unconfigured.json(); const auto payload = co_await std::move(reading); EXPECT_EQ(rt::getString(*rt::get(payload, "error"), "code"), std::optional<std::string_view>("openrouter_api_key_missing"));
  deps = systemDeps(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("must not reach upstream"); }));
  std::vector<json::value> cases; auto extra = originalBody(); extra["extra"] = true; cases.emplace_back(extra); cases.emplace_back(json::object{{"state", "not-an-object"}, {"questions", originalQuestions()}}); cases.emplace_back(json::object{{"state", originalState()}, {"questions", json::object{}}}); cases.emplace_back(json::object{{"state", originalState()}, {"questions", json::object{{"a", json::object{{"type", "unsupported"}}}}}}); cases.emplace_back(json::object{{"state", originalState()}, {"questions", json::object{{"a", json::object{{"instructions", "missing type"}}}}}});
  for (const auto& value : cases) { auto rejecting = systemone::handleSystemOne(systemRequest(value), std::nullopt, deps); const auto rejected = co_await std::move(rejecting); EXPECT_EQ(rejected.status(), 400); } co_await rt::sleep(1);
}

ASYNC_TEST("systemone maps upstream upsets to bounded errors") {
  const SystemScope scope; auto deps = systemDeps(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("connect timeout"); }));
  auto unreachableTask = systemone::handleSystemOne(systemRequest(originalBody()), std::nullopt, deps); auto unreachable = co_await std::move(unreachableTask); EXPECT_EQ(unreachable.status(), 502); auto reading = unreachable.json(); const auto payload = co_await std::move(reading); EXPECT_EQ(rt::getString(*rt::get(payload, "error"), "code"), std::optional<std::string_view>("openrouter_upstream_unreachable"));
  const std::vector<std::pair<double, double>> statuses{{429, 429}, {500, 502}, {503, 502}};
  for (const auto& [status, expected] : statuses) { deps = systemDeps(rt::Fetch([status](rt::RequestInfo, rt::RequestInit) { return rt::Response("{}", rt::ResponseInit{.status = status}); })); auto failing = systemone::handleSystemOne(systemRequest(originalBody()), std::nullopt, deps); const auto failed = co_await std::move(failing); EXPECT_EQ(failed.status(), expected); }
  deps = systemDeps(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"model", "typesafe/jev-1.13-20260917"}}); })); auto invalidTask = systemone::handleSystemOne(systemRequest(originalBody()), std::nullopt, deps); auto invalid = co_await std::move(invalidTask); EXPECT_EQ(invalid.status(), 502); auto invalidRead = invalid.json(); const auto invalidBody = co_await std::move(invalidRead); EXPECT_EQ(rt::getString(*rt::get(invalidBody, "error"), "code"), std::optional<std::string_view>("openrouter_upstream_invalid_response")); co_await rt::sleep(1);
}

TEST("systemone is a terminal inference route with its own quota route") {
  EXPECT_EQ(uos::handler::http::terminalRouteForRequest("POST", "/v1/systemone"), std::optional<std::string>("systemone"));
  EXPECT_EQ(uos::handler::http::kernelQuotaRouteForRequest("POST", "/v1/systemone"), std::optional<std::string>("systemone"));
  EXPECT_EQ(uos::handler::http::terminalRouteForRequest("POST", "/uos/systemone"), std::nullopt);
}

ASYNC_TEST("systemone attaches reported usage telemetry to its response") {
  const SystemScope scope; auto deps = systemDeps(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"model", "typesafe/jev-1.13-20260917"}, {"answers", json::object{{"candidate", json::object{{"type", "choice"}, {"choice", "yes"}, {"confidence", 0.9}}}}}, {"usage", json::object{{"input_tokens", 310}, {"output_tokens", 20}, {"cost", 0.00001302}}}}); }));
  auto handling = systemone::handleSystemOne(systemRequest(originalBody()), std::nullopt, deps); const auto response = co_await std::move(handling); EXPECT_EQ(response.status(), 200); const auto state = telemetry::getResponseTelemetry(response); EXPECT(state);
  EXPECT_EQ(state->model, std::optional<std::string>(systemone::SYSTEMONE_DEFAULT_MODEL)); EXPECT_EQ(state->provider, std::string("openrouter")); EXPECT_EQ(state->inputTokens, std::optional<double>(310)); EXPECT_EQ(state->cachedInputTokens, std::optional<double>(0)); EXPECT_EQ(state->outputTokens, std::optional<double>(20)); EXPECT_EQ(state->totalTokens, std::optional<double>(330)); EXPECT(state->usageObserved); EXPECT_EQ(state->usageTelemetryStatus, std::string("reported")); EXPECT(state->completed); EXPECT_EQ(state->stream, std::optional<bool>(false)); co_await rt::sleep(1);
}

namespace {
  rt::Task<void> systemHealthCase(std::optional<double> status, std::string upstreamBody, std::string expectedEvent, std::string expectedState, double clientStatus) {
    const SystemScope scope; int calls = 0; auto deps = systemDeps(rt::Fetch([&](rt::RequestInfo url, rt::RequestInit) -> rt::Response { EXPECT_EQ(std::get<std::string>(url), std::string(provider::OPENROUTER_SYSTEMONE_URL)); ++calls; if (!status) throw rt::JsError("connect timeout"); return rt::Response(upstreamBody, rt::ResponseInit{.status = *status}); }));
    auto handling = systemone::handleSystemOne(systemRequest(originalBody()), std::nullopt, deps); auto response = co_await std::move(handling); EXPECT_EQ(calls, 1); EXPECT_EQ(response.status(), clientStatus); auto reading = response.json(); const auto payload = co_await std::move(reading);
    const json::object expected{{"error", json::object{{"message", status ? "System One upstream error" : "System One upstream unreachable"}, {"type", "invalid_request_error"}, {"code", status ? "openrouter_upstream_error" : "openrouter_upstream_unreachable"}}}}; EXPECT(rt::deepEqual(payload, expected));
    co_await rt::sleep(1); const auto result = co_await health::getOpenRouterProviderHealth(); EXPECT_EQ(result.last_event, std::optional<std::string>(expectedEvent)); EXPECT_EQ(result.last_status, status); EXPECT_EQ(result.state, expectedState); EXPECT_EQ(result.last_429_at_ms.has_value(), expectedState == "exhausted"); co_await rt::sleep(1);
  }
}
#define SYSTEMONE_HEALTH_CASE(name, status, body, event, state, client) ASYNC_TEST(name) { auto pending = systemHealthCase(status, body, event, state, client); co_await std::move(pending); }
SYSTEMONE_HEALTH_CASE("systemone records upstream 402 health for {\"error\":{\"message\":\"Insufficient credits\"}}", 402, "{\"error\":{\"message\":\"Insufficient credits\"}}", "quota_exhausted", "exhausted", 502)
SYSTEMONE_HEALTH_CASE("systemone records upstream 402 health for {}", 402, "{}", "quota_exhausted", "exhausted", 502)
SYSTEMONE_HEALTH_CASE("systemone records upstream 429 health for {}", 429, "{}", "quota_exhausted", "exhausted", 429)
SYSTEMONE_HEALTH_CASE("systemone records upstream 403 health for {\"error\":{\"message\":\"Insufficient credits\"}}", 403, "{\"error\":{\"message\":\"Insufficient credits\"}}", "auth_invalid", "invalid", 502)
SYSTEMONE_HEALTH_CASE("systemone records upstream 500 health for {\"error\":{\"message\":\"Insufficient credits\"}}", 500, "{\"error\":{\"message\":\"Insufficient credits\"}}", "upstream_error", "degraded", 502)
SYSTEMONE_HEALTH_CASE("systemone records upstream 503 health for {}", 503, "{}", "upstream_error", "degraded", 502)
SYSTEMONE_HEALTH_CASE("systemone records upstream 400 health for {\"error\":{\"message\":\"Insufficient credits\"}}", 400, "{\"error\":{\"message\":\"Insufficient credits\"}}", "reachable", "degraded", 400)
SYSTEMONE_HEALTH_CASE("systemone records upstream transport failure health for ", std::nullopt, "", "upstream_error", "degraded", 502)
#undef SYSTEMONE_HEALTH_CASE

ASYNC_TEST("SystemOne full response and forwarded envelopes match 14 exact isolated Bun oracle records") {
  const SystemScope scope; const auto vectors = rt::jsonParse(R"oracle([{"suite":"system","input":null,"status":400,"body":"{\"error\":{\"message\":\"Invalid JSON body\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","sent":null},{"suite":"system","input":[],"status":400,"body":"{\"error\":{\"message\":\"state must be an object\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","sent":null},{"suite":"system","input":[1],"status":400,"body":"{\"error\":{\"message\":\"Unsupported key: 0\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","sent":null},{"suite":"system","input":{"state":{"question":"fixture"},"questions":{"candidate":{"type":"choice"}},"extra":true},"status":400,"body":"{\"error\":{\"message\":\"Unsupported key: extra\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","sent":null},{"suite":"system","input":{"state":null,"questions":{"candidate":{"type":"choice"}}},"status":400,"body":"{\"error\":{\"message\":\"state must be an object\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","sent":null},{"suite":"system","input":{"state":{"question":"fixture"},"questions":null},"status":400,"body":"{\"error\":{\"message\":\"questions must be an object\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","sent":null},{"suite":"system","input":{"state":{"question":"fixture"},"questions":{}},"status":400,"body":"{\"error\":{\"message\":\"questions must hold between 1 and 24 questions\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","sent":null},{"suite":"system","input":{"state":{"question":"fixture"},"questions":{" ":{"type":"noul"}}},"status":400,"body":"{\"error\":{\"message\":\"questions must be typed noul, choice, or score objects\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","sent":null},{"suite":"system","input":{"state":{"question":"fixture"},"questions":{"candidate":{"type":"other"}}},"status":400,"body":"{\"error\":{\"message\":\"questions must be typed noul, choice, or score objects\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","sent":null},{"suite":"system","input":{"state":{"question":"fixture"},"questions":{"candidate":{"type":"choice"}},"model":3},"status":400,"body":"{\"error\":{\"message\":\"model must be a string\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","sent":null},{"suite":"system","input":{"state":{"question":"fixture"},"questions":{"candidate":{"type":"choice"}},"model":"openai/x"},"status":400,"body":"{\"error\":{\"message\":\"model must be a typesafe model id\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","sent":null},{"suite":"system","input":{"state":{"question":"fixture"},"questions":{"candidate":{"type":"choice"}},"model":"typesafe/jev-1"},"status":200,"body":"{\"model\":\"typesafe/jev-fixture\",\"answers\":{\"candidate\":{\"choice\":\"fixture\"}},\"usage\":{\"input_tokens\":310,\"output_tokens\":20,\"cost\":0.00001302}}","sent":{"model":"~typesafe/jev-1","state":{"question":"fixture"},"questions":{"candidate":{"type":"choice"}}}},{"suite":"system","input":{"state":[],"questions":[{"type":"score"}]},"status":200,"body":"{\"model\":\"typesafe/jev-fixture\",\"answers\":{\"candidate\":{\"choice\":\"fixture\"}},\"usage\":{\"input_tokens\":310,\"output_tokens\":20,\"cost\":0.00001302}}","sent":{"model":"~typesafe/jev-latest","state":[],"questions":[{"type":"score"}]}},{"suite":"system","input":{"state":{"question":"fixture"},"questions":{"candidate":{"type":"choice"}}},"status":200,"body":"{\"model\":\"typesafe/jev-fixture\",\"answers\":{\"candidate\":{\"choice\":\"fixture\"}},\"usage\":{\"input_tokens\":310,\"output_tokens\":20,\"cost\":0.00001302}}","sent":{"model":"~typesafe/jev-latest","state":{"question":"fixture"},"questions":{"candidate":{"type":"choice"}}}}])oracle");
  for (const auto& row : vectors.as_array()) {
    json::value sent; auto deps = systemDeps(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { sent = rt::jsonParse(std::get<std::string>(*init.body)); return rt::Response::json(json::object{{"model", "typesafe/jev-fixture"}, {"answers", json::object{{"candidate", json::object{{"choice", "fixture"}}}}}, {"usage", json::object{{"input_tokens", 310}, {"output_tokens", 20}, {"cost", 0.00001302}}}}); }));
    auto handling = systemone::handleSystemOne(systemRequest(*rt::get(row,"input")), std::nullopt, deps); auto response = co_await std::move(handling); EXPECT_EQ(response.status(), *rt::getNumber(row,"status")); auto reading = response.text(); const auto body = co_await std::move(reading); EXPECT_EQ(body, std::string(*rt::getString(row,"body"))); EXPECT(rt::deepEqual(sent,*rt::get(row,"sent"))); co_await rt::sleep(1);
  }
}

ASYNC_TEST("systemone enforces selected providers and every bounded question envelope") {
  const SystemScope scope; int calls = 0; auto deps = systemDeps(rt::Fetch([&calls](rt::RequestInfo, rt::RequestInit) { ++calls; return rt::Response::json(answerPayload()); }));
  const std::vector<std::string> enabled{"deepseek"}; auto storing = selection::storeProviderSelection(scope.kv, enabled); const auto stored = co_await std::move(storing); EXPECT(stored);
  auto disabledTask = systemone::handleSystemOne(systemRequest(originalBody()), std::nullopt, deps); auto disabled = co_await std::move(disabledTask); EXPECT_EQ(disabled.status(), 503); auto reading = disabled.json(); const auto payload = co_await std::move(reading); EXPECT_EQ(rt::getString(*rt::get(payload, "error"), "code"), std::optional<std::string_view>("systemone_provider_disabled")); EXPECT_EQ(calls, 0); EXPECT(!telemetry::getResponseTelemetry(disabled));
  const std::vector<std::string> all; auto clearing = selection::storeProviderSelection(scope.kv, all); const auto cleared = co_await std::move(clearing); EXPECT(cleared);
  json::object tooMany; for (int index = 0; index < 25; ++index) tooMany["q" + rt::numberToString(index)] = json::object{{"type", "noul"}};
  std::vector<json::value> invalid{json::object{{"state", originalState()}, {"questions", tooMany}}, json::object{{"state", originalState()}, {"questions", json::object{{rt::repeat("a", 121), json::object{{"type", "score"}}}}}}, json::object{{"state", originalState()}, {"questions", json::object{{rt::repeat("😀", 61), json::object{{"type", "choice"}}}}}}, json::object{{"state", originalState()}, {"questions", originalQuestions()}, {"model", nullptr}}};
  for (const auto& input : invalid) { auto failing = systemone::handleSystemOne(systemRequest(input), std::nullopt, deps); const auto failed = co_await std::move(failing); EXPECT_EQ(failed.status(), 400); }
  auto overLimit = originalBody(); overLimit["state"] = json::object{{"large", rt::repeat("x", systemone::SYSTEMONE_MAX_BODY_BYTES)}}; auto oversizedTask = systemone::handleSystemOne(systemRequest(overLimit), std::nullopt, deps); const auto oversized = co_await std::move(oversizedTask); EXPECT_EQ(oversized.status(), 400);
  const json::object valid{{"state", json::array{}}, {"questions", json::object{{rt::repeat("😀", 60), json::object{{"type", "score"}}}}}, {"model", "typesafe/jev-1"}}; auto validTask = systemone::handleSystemOne(systemRequest(valid), std::nullopt, deps); const auto accepted = co_await std::move(validTask); EXPECT_EQ(accepted.status(), 200); EXPECT_EQ(calls, 1); co_await rt::sleep(1);
}

namespace {
  rt::Task<void> systemSourceUsage(json::value row) {
    const SystemScope scope; const auto upstreamPayload = row.at("payload"); int calls = 0; json::value terminal;
    auto deps = systemDeps(rt::Fetch([upstreamPayload](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(upstreamPayload); }));
    telemetry::UsageContext context{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt}; context.responseTelemetry = telemetry::createResponseTelemetryState();
    context.onTerminalUsage = [&](std::optional<telemetry::UsageTokens> usage, bool completed) {
      ++calls; json::object value{{"present", usage.has_value()}, {"completed", completed}};
      rt::objectSet(value, "status", rt::orNull(usage ? std::optional<std::string>(usage->status) : std::nullopt)); terminal = value;
    };
    auto handling = systemone::handleSystemOne(systemRequest(originalBody()), context, deps); const auto response = co_await std::move(handling); const auto body = co_await response.json();
    const auto& expected = row.at("expected"); EXPECT_EQ(response.status(), rt::getNumber(expected, "status")); EXPECT(rt::deepEqual(body, expected.at("body")));
    EXPECT_EQ(calls, rt::getNumber(expected, "calls")); EXPECT(rt::deepEqual(terminal, expected.at("terminal")));
    const auto state = telemetry::getResponseTelemetry(response); ASSERT(state);
    json::object projection{{"provider", state->provider}, {"observed", state->usageObserved}, {"status", state->usageTelemetryStatus}, {"completed", state->completed}};
    rt::objectSet(projection, "model", rt::orNull(state->model)); rt::objectSet(projection, "input", rt::orNull(state->inputTokens)); rt::objectSet(projection, "cached", rt::orNull(state->cachedInputTokens));
    rt::objectSet(projection, "output", rt::orNull(state->outputTokens)); rt::objectSet(projection, "total", rt::orNull(state->totalTokens));
    EXPECT(rt::deepEqual(projection, expected.at("telemetry"))); co_await rt::sleep(1);
  }
}

// Four isolated Bun source observations retain null-versus-undefined terminal usage semantics.

ASYNC_TEST("systemone source usage normalization: missing") {
  const auto row = rt::jsonParse(R"json({"name":"missing","payload":{"answers":{}},"expected":{"status":200,"body":{"answers":{}},"calls":1,"terminal":{"present":true,"status":"invalid","completed":true},"telemetry":{"provider":"openrouter","model":"~typesafe/jev-latest","input":null,"cached":null,"output":null,"total":null,"observed":true,"status":"invalid","completed":true}}})json");
  auto running = systemSourceUsage(row); co_await std::move(running);
}

ASYNC_TEST("systemone source usage normalization: null") {
  const auto row = rt::jsonParse(R"json({"name":"null","payload":{"answers":{},"usage":null},"expected":{"status":200,"body":{"answers":{},"usage":null},"calls":1,"terminal":{"present":true,"status":"invalid","completed":true},"telemetry":{"provider":"openrouter","model":"~typesafe/jev-latest","input":null,"cached":null,"output":null,"total":null,"observed":true,"status":"invalid","completed":true}}})json");
  auto running = systemSourceUsage(row); co_await std::move(running);
}

ASYNC_TEST("systemone source usage normalization: invalid") {
  const auto row = rt::jsonParse(R"json({"name":"invalid","payload":{"answers":{},"usage":{"input_tokens":"5","output_tokens":2}},"expected":{"status":200,"body":{"answers":{},"usage":{"input_tokens":"5","output_tokens":2}},"calls":1,"terminal":{"present":true,"status":"invalid","completed":true},"telemetry":{"provider":"openrouter","model":"~typesafe/jev-latest","input":null,"cached":null,"output":null,"total":null,"observed":true,"status":"invalid","completed":true}}})json");
  auto running = systemSourceUsage(row); co_await std::move(running);
}

ASYNC_TEST("systemone source usage normalization: valid") {
  const auto row = rt::jsonParse(R"json({"name":"valid","payload":{"answers":{},"usage":{"input_tokens":5,"output_tokens":2}},"expected":{"status":200,"body":{"answers":{},"usage":{"input_tokens":5,"output_tokens":2}},"calls":1,"terminal":{"present":true,"status":"reported","completed":true},"telemetry":{"provider":"openrouter","model":"~typesafe/jev-latest","input":5,"cached":0,"output":2,"total":7,"observed":true,"status":"reported","completed":true}}})json");
  auto running = systemSourceUsage(row); co_await std::move(running);
}
