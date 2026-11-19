// Links: tests/decisions_test.cpp
// src/admin/codex.cpp src/http.cpp src/provider/presentation.cpp src/request.cpp
// src/cache/prompt_analytics_read.cpp src/cache/scope_experiment_targets.cpp
// src/cache/prompt_analytics_core.cpp src/debug_routing.cpp src/cache/scope_experiment_model.cpp src/deepseek/waterfall.cpp
// src/codex/auth.cpp src/config.cpp src/provider/selection.cpp src/codex/account_routing.cpp
// src/codex/routing_state.cpp src/models/catalog.cpp src/auth/relay.cpp src/models/metadata.cpp
// src/authority/native_codex_mutations.cpp src/input_normalization.cpp src/runtime_config.cpp src/codex/experiment.cpp
// src/codex/dispatch.cpp src/types.cpp src/authority/native_codex.cpp src/provider/cerebras.cpp
// src/authority/native_codex_capacity.cpp src/catalog/types.cpp src/utils.cpp src/request_policy.cpp
// src/provider/capacity_events.cpp src/codex/routing_evaluation.cpp src/models/codex_models.cpp src/authority/native_codex_contracts.cpp
// src/provider/lithos_models.cpp src/provider/metered.cpp src/provider/claude_code_models.cpp src/provider/surplus.cpp
// src/cache/telemetry_gate.cpp src/recent_model_context.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
// src/authority/native_codex_evaluation.cpp src/codex/external_auth.cpp src/catalog/store.cpp src/provider/surplus_claude_request.cpp
// src/codex/capacity_routing.cpp src/kv.cpp src/deepseek/index.cpp src/catalog/index.cpp
// src/models/openrouter_models.cpp src/bounded_response_body.cpp src/provider/lithos.cpp src/cache/scope_targets.cpp
// src/provider/claude_code.cpp src/codex/overage_settings.cpp src/provider/claude_code_accounts.cpp src/provider/capacity_parse.cpp
// src/catalog/models.cpp src/responses_stream.cpp src/models/codex_models_whitelist.cpp
// src/defaults.cpp src/provider/cerebras_rate_limits.cpp src/provider/anthropic_replay.cpp src/provider/claude_code_credentials.cpp
// src/codex/auth_refresh.cpp src/provider/openrouter.cpp src/cache/scope_experiment.cpp src/models/codex_models_fetch.cpp
// src/provider/capacity.cpp src/provider/capacity_rollups.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_quota.cpp
// src/provider/capacity_history.cpp src/provider/lithos_rate_limits.cpp src/admin/api_keys.cpp src/models/codex_models_availability.cpp
// src/codex/banked_reset.cpp src/openai_telemetry.cpp src/api_key_policy.cpp src/provider/health.cpp
// src/provider/capacity_contract.cpp src/api_keys.cpp src/provider/claude_code_usage_schema.cpp src/release.cpp
// src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/provider/surplus_claude_chat_request.cpp src/admin/api_key_audit.cpp
// src/codex/native_auth.cpp src/provider/claude_capacity_history.cpp src/codex/reset_settings.cpp src/paid_fallback/ledger_state.cpp
// src/authority/native_api_key_audit.cpp src/authority/native_paid_contracts.cpp src/provider/anthropic_chat_responses.cpp src/codex/banked_reset_pool.cpp
// src/provider/capacity_analytics.cpp src/metered_quota.cpp src/codex/rate_limit_429.cpp src/paid_fallback/ledger_stats.cpp
// src/paid_fallback/index.cpp src/codex/banked_reset_claim.cpp src/storage/kv.cpp src/paid_fallback/ledger_admission.cpp
// src/authority/native_paid.cpp src/provider/anthropic_responses.cpp src/authority/native_paid_policy.cpp src/paid_fallback/ledger_backfill.cpp
// src/provider/anthropic_response_wire.cpp src/responses_failover_stream.cpp src/catalog/promotion.cpp src/storage/sqlite_readonly.cpp
// src/codex/banked_reset_submission.cpp src/paid_fallback/ledger_settlement.cpp src/authority/native_codex_429.cpp
// src/provider/removed_provider_telemetry.cpp src/codex/banked_reset_provider.cpp src/codex/native_auth_transport.cpp src/provider/anthropic_chat_stream.cpp
// src/storage/native_kv.cpp src/codex/responses_error.cpp src/runtime/websocket.cpp
// src/decisions/handlers.cpp src/openai.cpp src/openai_schema.cpp src/inference_deadline.cpp src/upstream_wire.cpp src/handler/http.cpp src/codex/quota.cpp tests/helpers/counting_kv.cpp
// Port of all nine cases in tests/decisions.test.ts plus resolver, timeout and source-recorded handler boundaries.
#include "uos/decisions/handlers.hpp"
#include "uos/handler/http.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "tests/helpers/counting_kv.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/admin/codex.hpp"
#include "uos/catalog/models.hpp"
#include "uos/models/catalog.hpp"
#include "uos/config.hpp"
#include "uos/codex/auth.hpp"
#include "uos/kv.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace upstream = uos::provider::openrouter;
  namespace models = uos::models::openrouter_models;
  namespace decisions = uos::decisions::handlers;
  namespace telemetry = uos::openai_telemetry;
  namespace selection = uos::provider::selection;
  namespace health = uos::provider::health;
  struct Scope {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w05c2"}, {"OPENROUTER_API_KEY", "fixture-key"}}};
    std::string savedAuth = uos::config::config.codexAuthJsonB64;
    Scope() { uos::config::config.codexAuthJsonB64.clear(); models::resetOpenRouterModelsCacheForTest(); }
    ~Scope() { models::setOpenRouterModelsFetchForTest({}); models::resetOpenRouterModelsCacheForTest(); uos::config::config.codexAuthJsonB64 = savedAuth; }
  };
  json::value catalogue() { return json::object{{"data", json::array{
    json::object{{"id", "vendor/text"}, {"architecture", json::object{{"output_modalities", json::array{"text"}}}}},
    json::object{{"id", "baseten/elephant-1:free"}, {"architecture", json::object{{"output_modalities", json::array{"decisions"}}}}},
    json::object{{"id", "~typesafe/jev-latest"}, {"architecture", json::object{{"output_modalities", json::array{"decisions"}}}}}}}}; }

  struct ServedCatalogue {
    Scope isolation;
    std::shared_ptr<uos::tests::helpers::counting_kv::CountingKv> store = std::make_shared<uos::tests::helpers::counting_kv::CountingKv>();
    ServedCatalogue() {
      selection::resetProviderSelectionCacheForTest(); health::resetProviderHealthThrottleForTest(); uos::kv::setKvForTest(store);
      models::setOpenRouterModelsFetchForTest(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(catalogue()); }));
    }
    ~ServedCatalogue() { selection::resetProviderSelectionCacheForTest(); health::resetProviderHealthThrottleForTest(); uos::kv::setKvForTest(nullptr); }
    rt::Task<void> prepare() { const auto loaded = co_await models::fetchOpenRouterModels(); EXPECT(loaded); }
  };
  rt::Request request(const json::value& body) {
    rt::RequestInit init; init.method = "POST"; init.headers = rt::Headers{{"content-type", "application/json"}}; init.body = rt::jsonStringify(body);
    return rt::Request("https://ai.ubq.fi/v1/decisions", init);
  }
  json::object minimalBody() { return json::object{{"model", "baseten/elephant-1:free"}, {"state", "x"},
    {"questions", json::object{{"yes", json::object{{"type", "noul"}, {"instructions", "Is x true?"}}}}}}; }
  json::value decisionBody() { return rt::jsonParse(R"({"model":"baseten/elephant-1:free","state":["Inspect the listing photo and ticket.",{"type":"image_url","image_url":{"url":"data:image/png;base64,AAAA"}}],"questions":{"is_bug":{"type":"noul","instructions":"Is the customer reporting a defect?","criteria":{"true":"Broken behavior.","false":"A question or feature request."}},"team":{"type":"choice","instructions":"Which team owns this?","criteria":{"payments":"Checkout or billing.","frontend":"Rendering or browser issues.","account":null}},"urgency":{"type":"score","instructions":"How urgent is this?","criteria":["Later","This week","Blocking revenue"]},"product":{"type":"box","instructions":"the product in the listing photo"}},"provider":{"allow_fallbacks":false},"session_id":"ticket-42","trace":{"trace_id":"trace-42","generation_name":"triage"},"user":"operator-1"})"); }
  json::value decisionPayload() { return rt::jsonParse(R"({"id":"gen-dec-test","model":"baseten/elephant-1-20261021","provider":"BaseTen","answers":{"is_bug":{"type":"noul","noul":0.97},"team":{"type":"choice","choice":"payments","confidence":0.8,"probabilities":{"payments":0.8,"frontend":0.2}},"urgency":{"type":"score","score":1.9,"confidence":0.95,"probabilities":{"0":0,"1":0.1,"2":0.9}},"product":{"type":"box","box":{"top":0.1,"bottom":0.9,"left":0.2,"right":0.8}}},"usage":{"input_tokens":321,"output_tokens":4,"cost":0}})"); }
  decisions::DecisionsHandlerDeps successfulDeps() {
    decisions::DecisionsHandlerDeps deps; deps.apiKey = [] { return std::optional<std::string>("or-test-key"); };
    deps.fetcher = rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(decisionPayload()); }); return deps;
  }
}

ASYNC_TEST("OpenRouter discovery requests text and decisions and classifies both endpoint families") {
  Scope isolation; std::string requested;
  models::setOpenRouterModelsFetchForTest(rt::Fetch([&](rt::RequestInfo input, rt::RequestInit) { requested = std::get<std::string>(input); return rt::Response::json(catalogue()); }));
  models::FetchOpenRouterModelsOptions options; options.force = true;
  auto fetching = models::fetchOpenRouterModels(options); const auto snapshot = co_await std::move(fetching); EXPECT(snapshot);
  EXPECT(rt::endsWith(requested, "/api/v1/models?output_modalities=text%2Cdecisions"));
  const std::vector<std::string> texts{"vendor/text"}; const std::vector<std::string> decisions{"baseten/elephant-1:free", "~typesafe/jev-latest"};
  EXPECT_EQ(upstream::openRouterTextModelIds(), texts); EXPECT_EQ(upstream::openRouterDecisionModelIds(), decisions);
  const auto rows = models::openRouterModelsFromPayload(catalogue()); EXPECT_EQ(rows.size(), 3U); EXPECT(!models::openRouterModelSupportsText(rows[1])); EXPECT(models::openRouterModelSupportsDecisions(rows[1]));
  EXPECT_EQ(upstream::openRouterDecisionUpstreamModelFor("typesafe/jev-latest"), std::optional<std::string>("~typesafe/jev-latest"));
  EXPECT(!upstream::openRouterDecisionUpstreamModelFor("vendor/text")); EXPECT(!upstream::openRouterUpstreamModelFor("~typesafe/jev-latest"));
}

ASYNC_TEST("OpenRouter Decisions uses its alpha URL and sixty-second default deadline") {
  Scope isolation; upstream::OpenRouterDecisionInput input; input.apiKey = std::string("fixture-key"); input.body = json::object{{"state", "fixture"}};
  input.fetcher = rt::Fetch([](rt::RequestInfo url, rt::RequestInit init) -> rt::Task<rt::Response> {
    EXPECT_EQ(std::get<std::string>(url), std::string(upstream::OPENROUTER_DECISIONS_URL)); EXPECT(init.signal);
    co_await rt::sleep(20'001); EXPECT(!init.signal->aborted());
    co_return rt::Response::json(json::object{{"answers", json::object{}}});
  });
  auto requesting = upstream::fetchOpenRouterDecisions(input); const auto response = co_await std::move(requesting); EXPECT(response.contains("answers"));
  input.timeoutMs = 5;
  input.fetcher = rt::Fetch([](rt::RequestInfo, rt::RequestInit init) -> rt::Task<rt::Response> {
    co_await rt::sleep(10); EXPECT(init.signal->aborted()); throw rt::DOMException("deadline", "TimeoutError");
  });
  auto bounded = upstream::fetchOpenRouterDecisions(input); EXPECT_REJECTS_MESSAGE(std::move(bounded), upstream::OpenRouterError, "openrouter_upstream_unreachable");
}

ASYNC_TEST("OpenRouter decision resolver refreshes a cold snapshot and swallows catalogue failure") {
  Scope isolation; int fetches = 0;
  models::setOpenRouterModelsFetchForTest(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++fetches; return rt::Response::json(catalogue()); }));
  auto alias = upstream::resolveOpenRouterDecisionUpstreamModel(" typesafe/jev-latest "); const auto resolved = co_await std::move(alias); EXPECT_EQ(resolved, std::optional<std::string>("~typesafe/jev-latest")); EXPECT_EQ(fetches, 1);
  models::resetOpenRouterModelsCacheForTest(); models::setOpenRouterModelsFetchForTest(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("catalogue unavailable"); }));
  auto failed = upstream::resolveOpenRouterDecisionUpstreamModel("vendor/missing"); const auto absent = co_await std::move(failed); EXPECT(!absent);
}

ASYNC_TEST("decision models are exposed on the decision route and hidden from Codex text selection") {
  Scope scope; const auto saved = uos::config::config.codexAuthJsonB64; uos::config::config.codexAuthJsonB64.clear(); auto store = std::make_shared<rt::MemoryKv>(rt::dateNow); uos::kv::setKvForTest(store); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::codex::auth::resetCodexAuthCacheForTest();
  auto cleanup = rt::scopeExit([saved] { uos::kv::setKvForTest(nullptr); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::codex::auth::resetCodexAuthCacheForTest(); uos::config::config.codexAuthJsonB64 = saved; });
  models::FetchOpenRouterModelsOptions options; options.force = true; options.fetcher = [](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(catalogue()); }; auto task = models::fetchOpenRouterModels(options); const auto discovered = co_await std::move(task); ASSERT(discovered);
  const auto standard = co_await uos::models::catalog::handleModels(); const auto list = co_await standard.json(); const auto* data = rt::getArray(list, "data"); ASSERT(data); bool elephant = false, alias = false;
  for (const auto& row : *data) { if (rt::getString(row, "id") == "baseten/elephant-1:free") elephant = true; if (rt::getString(row, "id") == "~typesafe/jev-latest") alias = true; } EXPECT(elephant); EXPECT(alias);
  const auto capabilities = co_await uos::models::catalog::handleModelCapabilities(); const auto capabilityBody = co_await capabilities.json();
  for (const auto& row : capabilityBody.at("data").as_array()) { if (rt::getString(row, "id") == "baseten/elephant-1:free") EXPECT(rt::deepEqual(row.at("supported_endpoints"), json::array{"/v1/decisions"})); if (rt::getString(row, "id") == "vendor/text") EXPECT(rt::deepEqual(row.at("supported_endpoints"), json::array{"/v1/responses", "/v1/chat/completions"})); }
  const auto catalog = co_await uos::models::catalog::buildModelCatalogSnapshot(); EXPECT_EQ(catalog.sources.get("openrouter")->count, 4); for (const auto& row : catalog.models) if (row.id == "baseten/elephant-1:free") EXPECT_EQ(row.providers[0].supported_endpoints, (std::vector<std::string>{"/v1/decisions"}));
  const auto admin = co_await uos::admin::codex::handleAdminModelsCatalogGet(); EXPECT_EQ(admin.status(), 200); const auto adminBody = co_await admin.json(); for (const auto& row : adminBody.at("data").at("models").as_array()) if (rt::getString(row, "id") == "baseten/elephant-1:free") EXPECT(rt::deepEqual(row.at("providers").as_array()[0].at("supported_endpoints"), json::array{"/v1/decisions"}));
  const auto codex = uos::catalog::models::openRouterCodexModels(); EXPECT_EQ(codex.size(), 1u); EXPECT_EQ(rt::getString(codex[0], "slug"), "vendor/text");
}

ASYNC_TEST("decisions forwards the documented envelope to OpenRouter and relays typed answers") {
  ServedCatalogue scope; co_await scope.prepare(); std::string url; std::optional<std::string> authorization; json::value sent;
  auto deps = successfulDeps(); deps.fetcher = rt::Fetch([&](rt::RequestInfo input, rt::RequestInit init) {
    url = std::get<std::string>(input); authorization = init.headers->get("authorization"); sent = rt::jsonParse(std::get<std::string>(*init.body));
    return rt::Response::json(decisionPayload());
  });
  const auto body = decisionBody(); auto sending = decisions::handleDecisions(request(body), nullptr, deps); const auto response = co_await std::move(sending);
  EXPECT_EQ(response.status(), 200); EXPECT_EQ(url, std::string(upstream::OPENROUTER_DECISIONS_URL)); EXPECT_EQ(authorization, std::optional<std::string>("Bearer or-test-key"));
  EXPECT(rt::deepEqual(sent, body)); EXPECT_EQ(response.headers().get("x-uos-upstream"), std::optional<std::string>("openrouter"));
  const auto payload = co_await response.json(); EXPECT(rt::deepEqual(payload, decisionPayload())); co_await rt::sleep(1);
}

ASYNC_TEST("decisions keeps the stable Jev alias and rejects text or unknown model ids") {
  ServedCatalogue scope; co_await scope.prepare(); std::vector<std::string> seen; auto deps = successfulDeps();
  deps.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { const auto body = rt::jsonParse(std::get<std::string>(*init.body)); seen.push_back(std::string(*rt::getString(body, "model"))); return rt::Response::json(decisionPayload()); });
  auto body = minimalBody(); rt::objectSet(body, "model", "typesafe/jev-latest"); auto sending = decisions::handleDecisions(request(body), nullptr, deps); const auto alias = co_await std::move(sending);
  EXPECT_EQ(alias.status(), 200); EXPECT_EQ(seen, (std::vector<std::string>{"~typesafe/jev-latest"}));
  const std::vector<std::string> modelsToRequest{"vendor/text", "unknown/model", "baseten/elephant-1:free"};
  for (const auto& model : modelsToRequest) {
    rt::objectSet(body, "model", json::value(model)); auto task = decisions::handleDecisions(request(body), nullptr, deps); const auto response = co_await std::move(task);
    EXPECT_EQ(response.status(), model == "baseten/elephant-1:free" ? 200 : 400);
    if (response.status() == 400) { const auto payload = co_await response.json(); EXPECT_EQ(rt::getString(payload.at("error"), "param"), "model"); }
  }
  EXPECT_EQ(seen, (std::vector<std::string>{"~typesafe/jev-latest", "baseten/elephant-1:free"})); co_await rt::sleep(1);
}

ASYNC_TEST("decisions validates state, question primitives, metadata and unsupported keys") {
  ServedCatalogue scope; co_await scope.prepare(); int calls = 0; auto deps = successfulDeps();
  deps.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit) -> rt::Response { ++calls; throw rt::JsError("must not reach upstream"); });
  const auto valid = minimalBody(); std::vector<json::object> cases;
  auto missing = valid; rt::objectDelete(missing, "state"); cases.push_back(missing);
  cases.push_back(rt::objectSpread(valid, json::object{{"state", 4}})); cases.push_back(rt::objectSpread(valid, json::object{{"questions", json::object{}}}));
  const json::array questions{
    json::object{{"type", "unsupported"}, {"instructions", "x"}}, json::object{{"type", "noul"}},
    json::object{{"type", "noul"}, {"instructions", "x"}, {"criteria", json::object{{"true", "yes"}}}},
    json::object{{"type", "choice"}, {"instructions", "x"}, {"criteria", json::object{}}},
    json::object{{"type", "score"}, {"instructions", "x"}, {"criteria", json::array{}}},
    json::object{{"type", "box"}, {"instructions", "x"}, {"criteria", json::object{{"top", 0}}}},
    json::object{{"type", "noul"}, {"instructions", "x"}, {"extra", true}}};
  for (const auto& question : questions) cases.push_back(rt::objectSpread(valid, json::object{{"questions", json::object{{"q", question}}}}));
  cases.push_back(rt::objectSpread(valid, json::object{{"provider", "openai"}})); cases.push_back(rt::objectSpread(valid, json::object{{"trace", json::array{}}}));
  cases.push_back(rt::objectSpread(valid, json::object{{"session_id", std::string(257, 'x')}})); cases.push_back(rt::objectSpread(valid, json::object{{"user", ""}}));
  cases.push_back(rt::objectSpread(valid, json::object{{"extra", true}})); EXPECT_EQ(cases.size(), 15U);
  for (const auto& body : cases) {
    auto task = decisions::handleDecisions(request(body), nullptr, deps); const auto response = co_await std::move(task); EXPECT_EQ(response.status(), 400);
    const auto payload = co_await response.json(); EXPECT_EQ(rt::getString(payload.at("error"), "code"), "decisions_request_invalid");
  }
  EXPECT_EQ(calls, 0); co_await rt::sleep(1);
}

ASYNC_TEST("decisions maps upstream and configuration failures without dispatching twice") {
  ServedCatalogue scope; co_await scope.prepare(); int calls = 0; auto deps = successfulDeps(); deps.apiKey = [] { return std::optional<std::string>{}; };
  deps.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit) -> rt::Response { ++calls; throw rt::JsError("must not dispatch"); });
  const auto body = minimalBody(); auto missing = decisions::handleDecisions(request(body), nullptr, deps); const auto response = co_await std::move(missing);
  EXPECT_EQ(response.status(), 503); const auto payload = co_await response.json(); EXPECT_EQ(rt::getString(payload.at("error"), "code"), "openrouter_api_key_missing"); EXPECT_EQ(calls, 0);
  deps.apiKey = [] { return std::optional<std::string>("or-test-key"); };
  const std::vector<std::pair<int, int>> statuses{{400, 400}, {413, 413}, {429, 429}, {500, 502}, {503, 502}};
  for (const auto& [status, expected] : statuses) {
    deps.fetcher = rt::Fetch([&, status](rt::RequestInfo, rt::RequestInit) { ++calls; return rt::Response::json(json::object{}, rt::ResponseInit{.status = status}); });
    auto task = decisions::handleDecisions(request(body), nullptr, deps); const auto failure = co_await std::move(task); EXPECT_EQ(failure.status(), expected);
  }
  EXPECT_EQ(calls, 5); deps.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++calls; return rt::Response::json(json::object{{"answers", nullptr}}); });
  auto invalid = decisions::handleDecisions(request(body), nullptr, deps); const auto invalidResponse = co_await std::move(invalid);
  EXPECT_EQ(invalidResponse.status(), 502); const auto invalidPayload = co_await invalidResponse.json(); EXPECT_EQ(rt::getString(invalidPayload.at("error"), "code"), "openrouter_upstream_invalid_response"); EXPECT_EQ(calls, 6); co_await rt::sleep(1);
}

ASYNC_TEST("decisions propagates downstream cancellation as a bounded 499") {
  ServedCatalogue scope; co_await scope.prepare(); rt::AbortController controller; telemetry::UsageContext context{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt}; context.downstreamSignal = controller.signal();
  auto deps = successfulDeps(); deps.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) -> rt::Task<rt::Response> {
    EXPECT(init.signal); const auto signal = *init.signal;
    const auto pending = rt::newPromise<rt::Response>([&](auto, auto reject) {
      rt::EventListener aborted = [reject] { reject(rt::makeReason(rt::DOMException("Aborted", "AbortError"))); };
      signal.addEventListener("abort", aborted, {.once = true}); controller.abort();
    });
    auto response = co_await pending.wait(); co_return response;
  });
  const auto body = decisionBody(); auto sending = decisions::handleDecisions(request(body), &context, deps); const auto response = co_await std::move(sending);
  EXPECT_EQ(response.status(), 499); const auto payload = co_await response.json(); EXPECT_EQ(rt::getString(payload.at("error"), "code"), "request_cancelled"); co_await rt::sleep(1);
}

ASYNC_TEST("decisions attaches usage telemetry and records provider health") {
  ServedCatalogue scope; co_await scope.prepare(); const auto body = decisionBody(); const auto deps = successfulDeps();
  auto sending = decisions::handleDecisions(request(body), nullptr, deps); const auto response = co_await std::move(sending); EXPECT_EQ(response.status(), 200);
  const auto state = telemetry::getResponseTelemetry(response); ASSERT(state); EXPECT_EQ(state->provider, "openrouter"); EXPECT_EQ(state->model, rt::getString(body, "model"));
  EXPECT_EQ(state->inputTokens, 321); EXPECT_EQ(state->cachedInputTokens, 0); EXPECT_EQ(state->outputTokens, 4); EXPECT_EQ(state->totalTokens, 325); EXPECT(state->usageObserved); EXPECT(state->completed);
  co_await rt::sleep(1); const auto observed = co_await health::getOpenRouterProviderHealth(); EXPECT_EQ(observed.state, "healthy");
}

TEST("decisions is a terminal inference route with its own admission and quota route") {
  EXPECT_EQ(uos::handler::http::terminalRouteForRequest("POST", "/v1/decisions"), "decisions");
  EXPECT_EQ(uos::handler::http::kernelQuotaRouteForRequest("POST", "/v1/decisions"), "decisions");
  EXPECT(!uos::handler::http::terminalRouteForRequest("GET", "/v1/decisions"));
}

// Bun-recorded source boundary rows: 57; source 8ff0c67283aacaafdf3a550534bb267cc0f72817e2cfa0afd66b8590ad734e6a.

namespace {
  rt::Task<void> runSourceBoundary(json::value row) {
    ServedCatalogue scope;
    json::array data;
    const std::vector<std::string> ids{"baseten/elephant-1:free", "v/" + std::string(198, 'm'), "v/" + rt::repeat("😀", 99)};
    for (const auto& id : ids) data.push_back(json::object{{"id", id}, {"architecture", json::object{{"output_modalities", json::array{"decisions"}}}}});
    const json::value sourceCatalogue = json::object{{"data", data}};
    models::setOpenRouterModelsFetchForTest(rt::Fetch([sourceCatalogue](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(sourceCatalogue); }));
    co_await scope.prepare();
    if (rt::getBool(row, "disabled").value_or(false)) {
      const std::vector<std::string> providers{"cerebras"}; auto selecting = selection::storeProviderSelection(scope.store, providers); const auto selected = co_await std::move(selecting); EXPECT(selected);
    }
    int dispatches = 0; json::value forwarded;
    auto deps = successfulDeps(); deps.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
      ++dispatches; forwarded = rt::jsonParse(std::get<std::string>(*init.body)); return rt::Response::json(json::object{{"answers", json::object{}}});
    });
    rt::RequestInit init; init.method = "POST"; rt::Headers headers{{"content-type", "application/json"}};
    if (const auto value = rt::getString(row, "contentLength")) headers.set("content-length", std::string(*value));
    init.headers = headers; init.body = std::string(*rt::getString(row, "input"));
    const rt::Request incoming("https://ai.ubq.fi/v1/decisions", init);
    auto sending = decisions::handleDecisions(incoming, nullptr, deps); const auto response = co_await std::move(sending);
    const auto body = co_await response.json(); co_await rt::sleep(1); const auto view = co_await health::getOpenRouterProviderHealth();
    const auto& expected = row.at("expected"); EXPECT_EQ(response.status(), rt::getNumber(expected, "status")); EXPECT(rt::deepEqual(body, expected.at("body")));
    EXPECT_EQ(dispatches, rt::getNumber(expected, "dispatches")); EXPECT(rt::deepEqual(forwarded, expected.at("forwarded")));
    json::object observed{{"state", view.state}}; rt::objectSet(observed, "event", rt::orNull(view.last_event)); rt::objectSet(observed, "status", rt::orNull(view.last_status)); EXPECT(rt::deepEqual(observed, expected.at("health")));
    json::value projection;
    if (const auto state = telemetry::getResponseTelemetry(response)) {
      json::object value{{"provider", state->provider}, {"completed", state->completed}, {"usageObserved", state->usageObserved}}; rt::objectSet(value, "model", rt::orNull(state->model)); projection = value;
    }
    EXPECT(rt::deepEqual(projection, expected.at("telemetry")));
  }
}

ASYNC_TEST("Decisions source boundary: empty") {
  const auto row = rt::jsonParse(R"json({"name":"empty","input":"","expected":{"status":400,"body":{"error":{"message":"Invalid JSON body","type":"invalid_request_error","code":"invalid_request_error"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: malformed") {
  const auto row = rt::jsonParse(R"json({"name":"malformed","input":"{","expected":{"status":400,"body":{"error":{"message":"Invalid JSON body","type":"invalid_request_error","code":"invalid_request_error"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: scalar-null") {
  const auto row = rt::jsonParse(R"json({"name":"scalar-null","input":"null","expected":{"status":400,"body":{"error":{"message":"Invalid JSON body","type":"invalid_request_error","code":"invalid_request_error"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: scalar-number") {
  const auto row = rt::jsonParse(R"json({"name":"scalar-number","input":"4","expected":{"status":400,"body":{"error":{"message":"Invalid JSON body","type":"invalid_request_error","code":"invalid_request_error"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: scalar-string") {
  const auto row = rt::jsonParse(R"json({"name":"scalar-string","input":"\"x\"","expected":{"status":400,"body":{"error":{"message":"Invalid JSON body","type":"invalid_request_error","code":"invalid_request_error"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: root-empty-array") {
  const auto row = rt::jsonParse(R"json({"name":"root-empty-array","input":"[]","expected":{"status":400,"body":{"error":{"message":"model must be a string","type":"invalid_request_error","code":"decisions_request_invalid","param":"model"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: root-array") {
  const auto row = rt::jsonParse(R"json({"name":"root-array","input":"[{}]","expected":{"status":400,"body":{"error":{"message":"Unsupported key: 0","type":"invalid_request_error","code":"decisions_request_invalid"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: oversized-declared") {
  const auto row = rt::jsonParse(R"json({"name":"oversized-declared","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","contentLength":"33554433","expected":{"status":413,"body":{"error":{"message":"The Decisions request body is too large","type":"invalid_request_error","code":"decisions_request_too_large"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: invalid-declared") {
  const auto row = rt::jsonParse(R"json({"name":"invalid-declared","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","contentLength":"01","expected":{"status":400,"body":{"error":{"message":"Invalid JSON body","type":"invalid_request_error","code":"invalid_request_error"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: model-missing") {
  const auto row = rt::jsonParse(R"json({"name":"model-missing","input":"{\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":400,"body":{"error":{"message":"model must be a string","type":"invalid_request_error","code":"decisions_request_invalid","param":"model"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: model-number") {
  const auto row = rt::jsonParse(R"json({"name":"model-number","input":"{\"model\":1,\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":400,"body":{"error":{"message":"model must be a string","type":"invalid_request_error","code":"decisions_request_invalid","param":"model"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: model-empty") {
  const auto row = rt::jsonParse(R"json({"name":"model-empty","input":"{\"model\":\"\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":400,"body":{"error":{"message":"model must be an OpenRouter decision model id","type":"invalid_request_error","code":"decisions_request_invalid","param":"model"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: model-whitespace") {
  const auto row = rt::jsonParse(R"json({"name":"model-whitespace","input":"{\"model\":\" a/ b \",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":400,"body":{"error":{"message":"model must be an OpenRouter decision model id","type":"invalid_request_error","code":"decisions_request_invalid","param":"model"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: model-too-long") {
  const auto row = rt::jsonParse(R"json({"name":"model-too-long","input":"{\"model\":\"v/mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":400,"body":{"error":{"message":"model must be an OpenRouter decision model id","type":"invalid_request_error","code":"decisions_request_invalid","param":"model"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: model-ascii-200") {
  const auto row = rt::jsonParse(R"json({"name":"model-ascii-200","input":"{\"model\":\"v/mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"v/mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm","state":"x","questions":{"yes":{"type":"noul","instructions":"Is x true?"}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"v/mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: model-utf16-200") {
  const auto row = rt::jsonParse(R"json({"name":"model-utf16-200","input":"{\"model\":\"v/😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"v/😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀","state":"x","questions":{"yes":{"type":"noul","instructions":"Is x true?"}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"v/😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: model-trim") {
  const auto row = rt::jsonParse(R"json({"name":"model-trim","input":"{\"model\":\"  baseten/elephant-1:free  \",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"yes":{"type":"noul","instructions":"Is x true?"}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: state-null") {
  const auto row = rt::jsonParse(R"json({"name":"state-null","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":null,\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":400,"body":{"error":{"message":"state must be a string, object, or array","type":"invalid_request_error","code":"decisions_request_invalid","param":"state"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: state-object") {
  const auto row = rt::jsonParse(R"json({"name":"state-object","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":{},\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":{},"questions":{"yes":{"type":"noul","instructions":"Is x true?"}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: state-empty-string") {
  const auto row = rt::jsonParse(R"json({"name":"state-empty-string","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"","questions":{"yes":{"type":"noul","instructions":"Is x true?"}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: questions-array") {
  const auto row = rt::jsonParse(R"json({"name":"questions-array","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":[]}","expected":{"status":400,"body":{"error":{"message":"questions must be an object","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: questions-whitespace-name") {
  const auto row = rt::jsonParse(R"json({"name":"questions-whitespace-name","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\" \":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":400,"body":{"error":{"message":"question names must be non-empty and at most 120 characters","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: questions-name-utf16-120") {
  const auto row = rt::jsonParse(R"json({"name":"questions-name-utf16-120","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀":{"type":"noul","instructions":"Is x true?"}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: questions-name-utf16-122") {
  const auto row = rt::jsonParse(R"json({"name":"questions-name-utf16-122","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":400,"body":{"error":{"message":"question names must be non-empty and at most 120 characters","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: questions-24") {
  const auto row = rt::jsonParse(R"json({"name":"questions-24","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q0\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q1\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q2\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q3\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q4\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q5\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q6\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q7\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q8\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q9\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q10\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q11\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q12\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q13\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q14\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q15\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q16\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q17\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q18\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q19\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q20\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q21\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q22\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q23\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"q0":{"type":"noul","instructions":"Is x true?"},"q1":{"type":"noul","instructions":"Is x true?"},"q2":{"type":"noul","instructions":"Is x true?"},"q3":{"type":"noul","instructions":"Is x true?"},"q4":{"type":"noul","instructions":"Is x true?"},"q5":{"type":"noul","instructions":"Is x true?"},"q6":{"type":"noul","instructions":"Is x true?"},"q7":{"type":"noul","instructions":"Is x true?"},"q8":{"type":"noul","instructions":"Is x true?"},"q9":{"type":"noul","instructions":"Is x true?"},"q10":{"type":"noul","instructions":"Is x true?"},"q11":{"type":"noul","instructions":"Is x true?"},"q12":{"type":"noul","instructions":"Is x true?"},"q13":{"type":"noul","instructions":"Is x true?"},"q14":{"type":"noul","instructions":"Is x true?"},"q15":{"type":"noul","instructions":"Is x true?"},"q16":{"type":"noul","instructions":"Is x true?"},"q17":{"type":"noul","instructions":"Is x true?"},"q18":{"type":"noul","instructions":"Is x true?"},"q19":{"type":"noul","instructions":"Is x true?"},"q20":{"type":"noul","instructions":"Is x true?"},"q21":{"type":"noul","instructions":"Is x true?"},"q22":{"type":"noul","instructions":"Is x true?"},"q23":{"type":"noul","instructions":"Is x true?"}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: questions-25") {
  const auto row = rt::jsonParse(R"json({"name":"questions-25","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q0\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q1\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q2\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q3\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q4\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q5\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q6\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q7\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q8\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q9\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q10\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q11\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q12\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q13\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q14\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q15\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q16\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q17\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q18\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q19\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q20\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q21\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q22\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q23\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"},\"q24\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":400,"body":{"error":{"message":"questions must hold between 1 and 24 questions","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: question-array") {
  const auto row = rt::jsonParse(R"json({"name":"question-array","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":[]}}","expected":{"status":400,"body":{"error":{"message":"questions.q must be an object","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: question-type-number") {
  const auto row = rt::jsonParse(R"json({"name":"question-type-number","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":1,\"instructions\":\"x\"}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.type must be noul, choice, score, or box","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: instructions-number") {
  const auto row = rt::jsonParse(R"json({"name":"instructions-number","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"noul\",\"instructions\":3}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.instructions must be a string, object, or array","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: instructions-array") {
  const auto row = rt::jsonParse(R"json({"name":"instructions-array","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"noul\",\"instructions\":[]}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"q":{"type":"noul","instructions":[]}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: noul-null") {
  const auto row = rt::jsonParse(R"json({"name":"noul-null","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"noul\",\"instructions\":\"x\",\"criteria\":null}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.criteria must be an object","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: noul-array") {
  const auto row = rt::jsonParse(R"json({"name":"noul-array","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"noul\",\"instructions\":\"x\",\"criteria\":[]}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.criteria must be an object","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: noul-extra") {
  const auto row = rt::jsonParse(R"json({"name":"noul-extra","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"noul\",\"instructions\":\"x\",\"criteria\":{\"true\":\"\",\"false\":\"\",\"extra\":\"\"}}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.criteria must contain exactly true and false","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: noul-invalid-guidance") {
  const auto row = rt::jsonParse(R"json({"name":"noul-invalid-guidance","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"noul\",\"instructions\":\"x\",\"criteria\":{\"true\":false,\"false\":\"\"}}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.criteria values must be strings, objects, or arrays","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: noul-structured-guidance") {
  const auto row = rt::jsonParse(R"json({"name":"noul-structured-guidance","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"noul\",\"instructions\":\"x\",\"criteria\":{\"true\":{},\"false\":[]}}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"q":{"type":"noul","instructions":"x","criteria":{"true":{},"false":[]}}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: choice-missing") {
  const auto row = rt::jsonParse(R"json({"name":"choice-missing","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"choice\",\"instructions\":\"x\"}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.criteria must be a non-empty object","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: choice-whitespace-name") {
  const auto row = rt::jsonParse(R"json({"name":"choice-whitespace-name","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"choice\",\"instructions\":\"x\",\"criteria\":{\" \":null}}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.criteria entries must have names and string, object, array, or null guidance","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: choice-invalid-guidance") {
  const auto row = rt::jsonParse(R"json({"name":"choice-invalid-guidance","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"choice\",\"instructions\":\"x\",\"criteria\":{\"a\":0}}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.criteria entries must have names and string, object, array, or null guidance","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: choice-structured-guidance") {
  const auto row = rt::jsonParse(R"json({"name":"choice-structured-guidance","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"choice\",\"instructions\":{},\"criteria\":{\"a\":null,\"b\":[],\"c\":{},\"d\":\"\"}}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"q":{"type":"choice","instructions":{},"criteria":{"a":null,"b":[],"c":{},"d":""}}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: score-missing") {
  const auto row = rt::jsonParse(R"json({"name":"score-missing","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"score\",\"instructions\":\"x\"}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.criteria must be a non-empty array","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: score-null-entry") {
  const auto row = rt::jsonParse(R"json({"name":"score-null-entry","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"score\",\"instructions\":\"x\",\"criteria\":[null]}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.criteria entries must be strings, objects, or arrays","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: score-structured-guidance") {
  const auto row = rt::jsonParse(R"json({"name":"score-structured-guidance","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"score\",\"instructions\":\"x\",\"criteria\":[\"\",{},[]]}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"q":{"type":"score","instructions":"x","criteria":["",{},[]]}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: box-no-criteria") {
  const auto row = rt::jsonParse(R"json({"name":"box-no-criteria","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"box\",\"instructions\":[]}}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"q":{"type":"box","instructions":[]}}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: box-null-criteria") {
  const auto row = rt::jsonParse(R"json({"name":"box-null-criteria","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"box\",\"instructions\":\"x\",\"criteria\":null}}}","expected":{"status":400,"body":{"error":{"message":"questions.q.criteria is not valid for box questions","type":"invalid_request_error","code":"decisions_request_invalid","param":"questions"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: provider-array") {
  const auto row = rt::jsonParse(R"json({"name":"provider-array","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}},\"provider\":[]}","expected":{"status":400,"body":{"error":{"message":"provider must be an object or null","type":"invalid_request_error","code":"decisions_request_invalid","param":"provider"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: provider-null") {
  const auto row = rt::jsonParse(R"json({"name":"provider-null","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}},\"provider\":null}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"yes":{"type":"noul","instructions":"Is x true?"}},"provider":null},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: trace-null") {
  const auto row = rt::jsonParse(R"json({"name":"trace-null","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}},\"trace\":null}","expected":{"status":400,"body":{"error":{"message":"trace must be an object","type":"invalid_request_error","code":"decisions_request_invalid","param":"trace"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: trace-object") {
  const auto row = rt::jsonParse(R"json({"name":"trace-object","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}},\"trace\":{}}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"yes":{"type":"noul","instructions":"Is x true?"}},"trace":{}},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: session-null") {
  const auto row = rt::jsonParse(R"json({"name":"session-null","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}},\"session_id\":null}","expected":{"status":400,"body":{"error":{"message":"session_id must be a non-empty string of at most 256 characters","type":"invalid_request_error","code":"decisions_request_invalid","param":"session_id"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: session-utf16-256") {
  const auto row = rt::jsonParse(R"json({"name":"session-utf16-256","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}},\"session_id\":\"😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀\"}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"yes":{"type":"noul","instructions":"Is x true?"}},"session_id":"😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀"},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: session-utf16-258") {
  const auto row = rt::jsonParse(R"json({"name":"session-utf16-258","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}},\"session_id\":\"😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀\"}","expected":{"status":400,"body":{"error":{"message":"session_id must be a non-empty string of at most 256 characters","type":"invalid_request_error","code":"decisions_request_invalid","param":"session_id"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: metadata-whitespace") {
  const auto row = rt::jsonParse(R"json({"name":"metadata-whitespace","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}},\"session_id\":\" \",\"user\":\" \"}","expected":{"status":200,"body":{"answers":{}},"dispatches":1,"forwarded":{"model":"baseten/elephant-1:free","state":"x","questions":{"yes":{"type":"noul","instructions":"Is x true?"}},"session_id":" ","user":" "},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true}}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: user-number") {
  const auto row = rt::jsonParse(R"json({"name":"user-number","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}},\"user\":4}","expected":{"status":400,"body":{"error":{"message":"user must be a non-empty string of at most 256 characters","type":"invalid_request_error","code":"decisions_request_invalid","param":"user"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: user-257") {
  const auto row = rt::jsonParse(R"json({"name":"user-257","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}},\"user\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}","expected":{"status":400,"body":{"error":{"message":"user must be a non-empty string of at most 256 characters","type":"invalid_request_error","code":"decisions_request_invalid","param":"user"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: integer-unknown-order") {
  const auto row = rt::jsonParse(R"json({"name":"integer-unknown-order","input":"{\"2\":true,\"9\":true,\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","expected":{"status":400,"body":{"error":{"message":"Unsupported key: 2","type":"invalid_request_error","code":"decisions_request_invalid"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: prototype-unknown") {
  const auto row = rt::jsonParse(R"json({"name":"prototype-unknown","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}},\"__proto__\":{\"x\":1}}","expected":{"status":400,"body":{"error":{"message":"Unsupported key: __proto__","type":"invalid_request_error","code":"decisions_request_invalid"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source boundary: provider-disabled") {
  const auto row = rt::jsonParse(R"json({"name":"provider-disabled","input":"{\"model\":\"baseten/elephant-1:free\",\"state\":\"x\",\"questions\":{\"yes\":{\"type\":\"noul\",\"instructions\":\"Is x true?\"}}}","disabled":true,"expected":{"status":503,"body":{"error":{"message":"Decisions provider is disabled by the provider selection","type":"invalid_request_error","code":"decisions_provider_disabled"}},"dispatches":0,"forwarded":null,"health":{"state":"unknown","event":null,"status":null},"telemetry":null}})json");
  auto running = runSourceBoundary(row); co_await std::move(running);
}

namespace {
  struct DispatchCounts { int before = 0; int started = 0; int cancelled = 0; int fetch = 0; };
  rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>> sourceAdmission(std::string name, std::string provider, std::shared_ptr<DispatchCounts> counts, rt::Reason sentinel) {
    EXPECT_EQ(provider, "openrouter"); ++counts->before;
    if (name == "hook-quota") throw uos::api_key_policy::ApiKeyQuotaDispatchError("fixture quota");
    if (name == "hook-abort") throw rt::DOMException("fixture abort", "AbortError");
    if (name == "hook-timeout") throw rt::DOMException("fixture timeout", "TimeoutError");
    if (name == "hook-error" || name == "hook-thrown") std::rethrow_exception(sentinel);
    uos::api_key_policy::ApiKeyProviderDispatch dispatch;
    dispatch.markTransportStarted = [counts] { ++counts->started; };
    dispatch.cancelBeforeTransport = [counts] { ++counts->cancelled; return rt::fulfilledTask(); };
    co_return std::optional<uos::api_key_policy::ApiKeyProviderDispatch>(dispatch);
  }
  rt::Task<void> runSourceDispatch(json::value row) {
    ServedCatalogue scope; co_await scope.prepare(); const std::string name(*rt::getString(row, "name"));
    const auto counts = std::make_shared<DispatchCounts>(); rt::AbortController controller; if (name == "pre-aborted") controller.abort();
    const auto sentinel = name == "hook-error" ? rt::makeReason(rt::JsError("fixture hook")) : rt::thrown(json::object{{"fixture", "unknown"}});
    const auto state = telemetry::createResponseTelemetryState(); if (name == "existing-state") state->provider = "cerebras";
    telemetry::UsageContext context{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt};
    context.downstreamSignal = controller.signal(); context.startedAtMonotonicMs = rt::performanceNow(); context.responseTelemetry = state;
    context.beforeProviderDispatch = [name, counts, sentinel](std::string provider) { return sourceAdmission(name, std::move(provider), counts, sentinel); };
    auto deps = successfulDeps(); deps.apiKey = [name] { return name == "missing-key" ? std::optional<std::string>{} : std::optional<std::string>("fixture-key"); };
    deps.fetcher = rt::Fetch([name, counts, controller, sentinel, row](rt::RequestInfo, rt::RequestInit) -> rt::Response {
      ++counts->fetch;
      if (name == "caller-thrown") { controller.abort(sentinel); std::rethrow_exception(sentinel); }
      if (name == "network") throw rt::JsError("fixture network");
      if (name == "invalid-json") return rt::Response("not json");
      if (name == "payload-array") return rt::Response::json(json::array{});
      if (name == "answers-array") return rt::Response::json(json::object{{"answers", json::array{}}});
      json::value usage = json::object{{"input_tokens", 5}, {"output_tokens", 2}, {"cost", 0}};
      if (name == "usage-null") usage = nullptr;
      if (name == "usage-invalid") usage = json::object{{"input_tokens", "5"}, {"output_tokens", 2}};
      if (name == "usage-zero") usage = json::object{{"input_tokens", 0}, {"output_tokens", 0}};
      if (name == "usage-fractional") usage = json::object{{"input_tokens", 1.5}, {"output_tokens", 2.5}};
      return rt::Response::json(json::object{{"answers", json::object{}}, {"usage", usage}}, rt::ResponseInit{.status = rt::getNumber(row, "status")});
    });
    std::unique_ptr<rt::GlobalFetchOverride> fetchOverride; if (name == "default-deps") fetchOverride = std::make_unique<rt::GlobalFetchOverride>(deps.fetcher);
    json::value outcome; rt::Reason failure;
    try {
      const auto body = minimalBody(); auto sending = name == "default-deps" ? decisions::handleDecisions(request(body), &context) : decisions::handleDecisions(request(body), &context, deps);
      const auto response = co_await std::move(sending); const auto payload = co_await response.json();
      outcome = json::object{{"status", response.status()}, {"body", payload}, {"attached", telemetry::getResponseTelemetry(response).has_value()}};
    } catch (...) { failure = std::current_exception(); }
    if (failure) outcome = json::object{{"thrown", true}, {"identityMatches", failure == sentinel}};
    co_await rt::sleep(1); const auto view = co_await health::getOpenRouterProviderHealth(); const auto& expected = row.at("expected");
    EXPECT(rt::deepEqual(outcome, expected.at("outcome")));
    const json::object actualCounts{{"before", counts->before}, {"started", counts->started}, {"cancelled", counts->cancelled}, {"fetch", counts->fetch}};
    EXPECT(rt::deepEqual(actualCounts, expected.at("counts")));
    json::object observed{{"state", view.state}}; rt::objectSet(observed, "event", rt::orNull(view.last_event)); rt::objectSet(observed, "status", rt::orNull(view.last_status)); EXPECT(rt::deepEqual(observed, expected.at("health")));
    json::object projection{{"completed", state->completed}, {"usageObserved", state->usageObserved}, {"usageTelemetryStatus", state->usageTelemetryStatus},
      {"attempted", json::value_from(state->attemptedProviders)}, {"dispatched", state->firstProviderDispatchMs.has_value()}, {"headers", state->firstProviderHeadersMs.has_value()}};
    rt::objectSet(projection, "provider", rt::orNull(state->provider)); rt::objectSet(projection, "model", rt::orNull(state->model));
    rt::objectSet(projection, "input", rt::orNull(state->inputTokens)); rt::objectSet(projection, "cached", rt::orNull(state->cachedInputTokens));
    rt::objectSet(projection, "output", rt::orNull(state->outputTokens)); rt::objectSet(projection, "total", rt::orNull(state->totalTokens));
    EXPECT(rt::deepEqual(projection, expected.at("telemetry")));
  }
}

// Bun-recorded source dispatch rows: 27; synthetic credentials/providers only.

ASYNC_TEST("Decisions source dispatch: http-400") {
  const auto row = rt::jsonParse(R"json({"name":"http-400","status":400,"expected":{"outcome":{"status":400,"body":{"error":{"message":"Decisions upstream error","type":"invalid_request_error","code":"openrouter_upstream_error"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"degraded","event":"reachable","status":400},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: http-401") {
  const auto row = rt::jsonParse(R"json({"name":"http-401","status":401,"expected":{"outcome":{"status":502,"body":{"error":{"message":"Decisions upstream error","type":"invalid_request_error","code":"openrouter_upstream_error"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"invalid","event":"auth_invalid","status":401},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: http-402") {
  const auto row = rt::jsonParse(R"json({"name":"http-402","status":402,"expected":{"outcome":{"status":502,"body":{"error":{"message":"Decisions upstream error","type":"invalid_request_error","code":"openrouter_upstream_error"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"exhausted","event":"quota_exhausted","status":402},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: http-403") {
  const auto row = rt::jsonParse(R"json({"name":"http-403","status":403,"expected":{"outcome":{"status":502,"body":{"error":{"message":"Decisions upstream error","type":"invalid_request_error","code":"openrouter_upstream_error"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"invalid","event":"auth_invalid","status":403},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: http-404") {
  const auto row = rt::jsonParse(R"json({"name":"http-404","status":404,"expected":{"outcome":{"status":502,"body":{"error":{"message":"Decisions upstream error","type":"invalid_request_error","code":"openrouter_upstream_error"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"degraded","event":"reachable","status":404},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: http-413") {
  const auto row = rt::jsonParse(R"json({"name":"http-413","status":413,"expected":{"outcome":{"status":413,"body":{"error":{"message":"Decisions upstream error","type":"invalid_request_error","code":"openrouter_upstream_error"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"degraded","event":"reachable","status":413},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: http-429") {
  const auto row = rt::jsonParse(R"json({"name":"http-429","status":429,"expected":{"outcome":{"status":429,"body":{"error":{"message":"Decisions upstream error","type":"invalid_request_error","code":"openrouter_upstream_error"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"exhausted","event":"quota_exhausted","status":429},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: http-500") {
  const auto row = rt::jsonParse(R"json({"name":"http-500","status":500,"expected":{"outcome":{"status":502,"body":{"error":{"message":"Decisions upstream error","type":"invalid_request_error","code":"openrouter_upstream_error"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"degraded","event":"upstream_error","status":500},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: http-503") {
  const auto row = rt::jsonParse(R"json({"name":"http-503","status":503,"expected":{"outcome":{"status":502,"body":{"error":{"message":"Decisions upstream error","type":"invalid_request_error","code":"openrouter_upstream_error"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"degraded","event":"upstream_error","status":503},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: network") {
  const auto row = rt::jsonParse(R"json({"name":"network","status":200,"expected":{"outcome":{"status":502,"body":{"error":{"message":"Decisions upstream unreachable","type":"invalid_request_error","code":"openrouter_upstream_unreachable"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"degraded","event":"upstream_error","status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":false}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: invalid-json") {
  const auto row = rt::jsonParse(R"json({"name":"invalid-json","status":200,"expected":{"outcome":{"status":502,"body":{"error":{"message":"Decisions upstream invalid response","type":"invalid_request_error","code":"openrouter_upstream_invalid_response"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"degraded","event":"upstream_error","status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: payload-array") {
  const auto row = rt::jsonParse(R"json({"name":"payload-array","status":200,"expected":{"outcome":{"status":502,"body":{"error":{"message":"Decisions upstream invalid response","type":"invalid_request_error","code":"openrouter_upstream_invalid_response"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"degraded","event":"upstream_error","status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: answers-array") {
  const auto row = rt::jsonParse(R"json({"name":"answers-array","status":200,"expected":{"outcome":{"status":502,"body":{"error":{"message":"Decisions upstream invalid response","type":"invalid_request_error","code":"openrouter_upstream_invalid_response"}},"attached":false},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"degraded","event":"upstream_error","status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: missing-key") {
  const auto row = rt::jsonParse(R"json({"name":"missing-key","status":200,"expected":{"outcome":{"status":503,"body":{"error":{"message":"Decisions upstream is not configured","type":"invalid_request_error","code":"openrouter_api_key_missing"}},"attached":false},"counts":{"before":0,"started":0,"cancelled":0,"fetch":0},"health":{"state":"unknown","event":null,"status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":[],"dispatched":false,"headers":false}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: default-deps") {
  const auto row = rt::jsonParse(R"json({"name":"default-deps","status":200,"expected":{"outcome":{"status":200,"body":{"answers":{},"usage":{"input_tokens":5,"output_tokens":2,"cost":0}},"attached":true},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true,"usageTelemetryStatus":"reported","input":5,"cached":0,"output":2,"total":7,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: pre-aborted") {
  const auto row = rt::jsonParse(R"json({"name":"pre-aborted","status":200,"expected":{"outcome":{"status":502,"body":{"error":{"message":"Decisions upstream unreachable","type":"invalid_request_error","code":"openrouter_upstream_unreachable"}},"attached":false},"counts":{"before":1,"started":0,"cancelled":1,"fetch":0},"health":{"state":"unknown","event":null,"status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":[],"dispatched":false,"headers":false}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: hook-quota") {
  const auto row = rt::jsonParse(R"json({"name":"hook-quota","status":200,"expected":{"outcome":{"status":503,"body":{"error":{"message":"fixture quota","type":"server_error","code":"api_key_quota_reservation_unavailable","param":null}},"attached":false},"counts":{"before":1,"started":0,"cancelled":0,"fetch":0},"health":{"state":"unknown","event":null,"status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":[],"dispatched":false,"headers":false}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: hook-abort") {
  const auto row = rt::jsonParse(R"json({"name":"hook-abort","status":200,"expected":{"outcome":{"status":499,"body":{"error":{"message":"Request was cancelled.","type":"server_error","code":"request_cancelled","param":null}},"attached":false},"counts":{"before":1,"started":0,"cancelled":0,"fetch":0},"health":{"state":"unknown","event":null,"status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":[],"dispatched":false,"headers":false}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: hook-timeout") {
  const auto row = rt::jsonParse(R"json({"name":"hook-timeout","status":200,"expected":{"outcome":{"status":499,"body":{"error":{"message":"Request was cancelled.","type":"server_error","code":"request_cancelled","param":null}},"attached":false},"counts":{"before":1,"started":0,"cancelled":0,"fetch":0},"health":{"state":"unknown","event":null,"status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":[],"dispatched":false,"headers":false}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: hook-error") {
  const auto row = rt::jsonParse(R"json({"name":"hook-error","status":200,"expected":{"outcome":{"thrown":true,"identityMatches":true},"counts":{"before":1,"started":0,"cancelled":0,"fetch":0},"health":{"state":"unknown","event":null,"status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":[],"dispatched":false,"headers":false}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: hook-thrown") {
  const auto row = rt::jsonParse(R"json({"name":"hook-thrown","status":200,"expected":{"outcome":{"thrown":true,"identityMatches":true},"counts":{"before":1,"started":0,"cancelled":0,"fetch":0},"health":{"state":"unknown","event":null,"status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":[],"dispatched":false,"headers":false}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: caller-thrown") {
  const auto row = rt::jsonParse(R"json({"name":"caller-thrown","status":200,"expected":{"outcome":{"thrown":true,"identityMatches":true},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"unknown","event":null,"status":null},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":false,"usageObserved":false,"usageTelemetryStatus":"missing","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":false}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: usage-null") {
  const auto row = rt::jsonParse(R"json({"name":"usage-null","status":200,"expected":{"outcome":{"status":200,"body":{"answers":{},"usage":null},"attached":true},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true,"usageTelemetryStatus":"invalid","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: usage-invalid") {
  const auto row = rt::jsonParse(R"json({"name":"usage-invalid","status":200,"expected":{"outcome":{"status":200,"body":{"answers":{},"usage":{"input_tokens":"5","output_tokens":2}},"attached":true},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true,"usageTelemetryStatus":"invalid","input":null,"cached":null,"output":null,"total":null,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: usage-zero") {
  const auto row = rt::jsonParse(R"json({"name":"usage-zero","status":200,"expected":{"outcome":{"status":200,"body":{"answers":{},"usage":{"input_tokens":0,"output_tokens":0}},"attached":true},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true,"usageTelemetryStatus":"reported","input":0,"cached":0,"output":0,"total":0,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: usage-fractional") {
  const auto row = rt::jsonParse(R"json({"name":"usage-fractional","status":200,"expected":{"outcome":{"status":200,"body":{"answers":{},"usage":{"input_tokens":1.5,"output_tokens":2.5}},"attached":true},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true,"usageTelemetryStatus":"invalid","input":null,"cached":0,"output":null,"total":4,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}

ASYNC_TEST("Decisions source dispatch: existing-state") {
  const auto row = rt::jsonParse(R"json({"name":"existing-state","status":200,"expected":{"outcome":{"status":200,"body":{"answers":{},"usage":{"input_tokens":5,"output_tokens":2,"cost":0}},"attached":true},"counts":{"before":1,"started":1,"cancelled":0,"fetch":1},"health":{"state":"healthy","event":"success","status":200},"telemetry":{"provider":"openrouter","model":"baseten/elephant-1:free","completed":true,"usageObserved":true,"usageTelemetryStatus":"reported","input":5,"cached":0,"output":2,"total":7,"attempted":["openrouter"],"dispatched":true,"headers":true}}})json");
  auto running = runSourceDispatch(row); co_await std::move(running);
}
