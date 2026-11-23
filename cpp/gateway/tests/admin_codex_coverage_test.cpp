// Links: tests/admin_codex_coverage_test.cpp
// src/admin/codex.cpp src/http.cpp src/provider/presentation.cpp src/request.cpp
// src/cache/prompt_analytics_read.cpp src/cache/scope_experiment_targets.cpp tests/helpers/prompt_cache_scope_experiment_harness.cpp tests/helpers/codex_diff_kv.cpp
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
// src/catalog/models.cpp src/responses_stream.cpp src/models/codex_models_whitelist.cpp tests/helpers/admin_auth_harness.cpp
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
// Source: tests/admin-codex-coverage.test.ts and the deferred admin projections in prompt-cache-scope-experiment-part1.test.ts.
#include "uos/admin/codex.hpp"
#include "uos/catalog/store.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/rt/string.hpp"
#include "tests/helpers/admin_auth_harness.hpp"
#include "tests/helpers/prompt_cache_scope_experiment_harness.hpp"
#include "uos/config.hpp"
#include "uos/debug_routing.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_whitelist.hpp"
#include "uos/provider/selection.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/test_async.hpp"
namespace rt = uos::rt;
namespace json = uos::json;
namespace a = uos::admin::codex;
namespace h = uos::tests::helpers::admin_auth_harness;
namespace scopeHarness = uos::tests::helpers::prompt_cache_scope_experiment_harness;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c4-admin"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    Fixture() { uos::config::config.codexAuthJsonB64.clear(); h::resetHarness(); uos::codex::auth::resetCodexAuthCacheForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::debug_routing::resetDebugRoutingCacheForTest(); uos::provider::metered::resetMeteredModelsCacheForTest(); uos::provider::surplus::resetSurplusModelsCacheForTest(); uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest(); }
    ~Fixture() { h::cleanupHarness(); uos::codex::auth::resetCodexAuthCacheForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::debug_routing::resetDebugRoutingCacheForTest(); uos::provider::metered::resetMeteredModelsCacheForTest(); uos::provider::surplus::resetSurplusModelsCacheForTest(); uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest(); uos::catalog::store::resetCodexCatalogMemoForTest(); uos::config::config.codexAuthJsonB64 = saved; }
  };
  rt::Request post(const std::string& path, const json::value& body) { rt::RequestInit init; init.method = "POST"; init.body = rt::jsonStringify(body); return rt::Request("https://ai.ubq.fi" + path, init); }
  rt::Request rawPost(const std::string& path, const std::string& body) { rt::RequestInit init; init.method = "POST"; init.body = body; return rt::Request("https://ai.ubq.fi" + path, init); }
  rt::Task<json::object> payloadOf(rt::Response response) { const auto body = co_await response.json(); const auto* error = rt::getObject(body, "error"); co_return error ? *error : json::object{}; }
  uos::models::catalog::ModelCatalogSnapshot catalogFixture() {
    uos::models::catalog::ModelCatalogSnapshot result;
    result.models = {{.id = "gpt-5.6-sol", .providers = {{"codex", "openai", {"/v1/responses"}}}}, {.id = "kimi-k2", .providers = {{"surplus", "moonshot", {"/v1/chat/completions"}}}}};
    result.sources.set("codex", {.status = "available", .count = 1, .updated_at_ms = 1}); result.sources.set("surplus", {.status = "available", .count = 1, .updated_at_ms = 2, .configured = true}); result.sources.set("openlux", {.status = "unavailable", .count = 0, .updated_at_ms = std::nullopt}); return result;
  }
  uos::cache::prompt_analytics_core::PromptCacheAnalyticsView analyticsView(const std::vector<std::string>& groups) { return {"ready", 60000, 0, 60000, groups, 10, false, false, {}}; }
}
ASYNC_TEST("admin debug routing reads, clears and validates the scenario override") {
  Fixture fixture; AWAIT(initial, a::handleAdminDebugRouting(rt::Request("https://ai.ubq.fi/admin/debug/routing"))); EXPECT_EQ(initial.status(), 200); EXPECT_EQ(initial.headers().get("cache-control"), "no-store"); AWAIT(initialBody, initial.json()); EXPECT_EQ(rt::getString(initialBody.at("routing"), "scenario"), "normal");
  AWAIT(invalid, a::handleAdminDebugRouting(rawPost("/admin/debug/routing", "{"))); EXPECT_EQ(invalid.status(), 400);
  AWAIT(missing, a::handleAdminDebugRouting(post("/admin/debug/routing", json::object{}))); AWAIT(missingBody, payloadOf(missing)); EXPECT_EQ(rt::getString(missingBody, "param"), "scenario");
  const std::vector<double> durations{1.5, 3600001, -1}; for (const auto duration : durations) { auto req = post("/admin/debug/routing", json::object{{"scenario", "codex_429"}, {"duration_ms", duration}}); AWAIT(response, a::handleAdminDebugRouting(req)); EXPECT_EQ(response.status(), 400); AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "message"), "duration_ms must be between 0 and 3600000"); EXPECT_EQ(rt::getString(body, "param"), "duration_ms"); }
  AWAIT(applied, a::handleAdminDebugRouting(post("/admin/debug/routing", json::object{{"scenario", "codex_429"}, {"duration_ms", 60000}}))); EXPECT_EQ(applied.status(), 200); AWAIT(appliedBody, applied.json()); EXPECT_EQ(rt::getString(appliedBody.at("routing"), "scenario"), "codex_429");
  rt::RequestInit init; init.method = "DELETE"; AWAIT(cleared, a::handleAdminDebugRouting(rt::Request("https://ai.ubq.fi/admin/debug/routing", init))); EXPECT_EQ(cleared.status(), 200);
  AWAIT(unknown, a::handleAdminDebugRouting(post("/admin/debug/routing", json::object{{"scenario", "bogus"}}))); EXPECT_EQ(unknown.status(), 400);
}
ASYNC_TEST("admin codex recheck rejects unknown and unconfigured account slots") { Fixture fixture; const std::vector<double> slots{0, 3, 1.5, 1, 2}; for (auto slot : slots) { AWAIT(response, a::handleAdminCodexRecheck(slot)); EXPECT_EQ(response.status(), 404); } }
ASYNC_TEST("admin banked-reset shadow decisions serve the ledger and fail closed") {
  Fixture fixture; AWAIT(ready, a::handleAdminCodexBankedResetShadowDecisions()); EXPECT_EQ(ready.status(), 200); EXPECT_EQ(ready.headers().get("cache-control"), "no-store"); AWAIT(body, ready.json()); EXPECT(rt::getArray(body, "decisions"));
  uos::kv::setKvForTest(nullptr); AWAIT(unavailable, a::handleAdminCodexBankedResetShadowDecisions()); EXPECT_EQ(unavailable.status(), 503); AWAIT(failure, payloadOf(unavailable)); EXPECT_EQ(rt::getString(failure, "code"), "codex_banked_reset_shadow_unavailable"); EXPECT_EQ(unavailable.headers().get("cache-control"), "no-store");
}
ASYNC_TEST("admin prompt-cache scope experiment rejects request fields") { Fixture fixture; AWAIT(response, a::handleAdminCodexCacheScopeExperiment(post("/admin/codex/cache-scope-experiment", json::object{{"model", "not-allowed"}}))); EXPECT_EQ(response.status(), 400); AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "message"), "Prompt-cache scope experiment does not accept request fields"); }
ASYNC_TEST("admin prompt-cache telemetry baseline projects the reader result and fails closed") {
  Fixture fixture; const json::object expected{{"status", "ready"}, {"reason", nullptr}, {"release", "release-1"}, {"provider", "codex"}, {"aggregate", json::object{{"sample_count", 3}}}, {"routes", json::array{json::object{{"route", "/v1/responses"}}}}};
  a::ReadBaselineJson reader = [expected] { return rt::fulfilledTask(json::value(expected)); }; AWAIT(ready, a::handleAdminCodexCacheScopeExperimentTelemetryBaseline(reader)); EXPECT_EQ(ready.status(), 200); EXPECT_EQ(ready.headers().get("cache-control"), "no-store"); AWAIT(body, ready.json()); EXPECT(rt::deepEqual(body, expected));
  a::ReadBaselineJson failed = []() -> rt::Task<json::value> { throw rt::JsError("kv unavailable"); co_return nullptr; }; AWAIT(failing, a::handleAdminCodexCacheScopeExperimentTelemetryBaseline(failed)); EXPECT_EQ(failing.status(), 503); EXPECT_EQ(failing.headers().get("cache-control"), "no-store"); AWAIT(error, payloadOf(failing)); EXPECT_EQ(rt::getString(error, "code"), "prompt_cache_scope_experiment_unavailable"); EXPECT_EQ(rt::getString(error, "message"), "Prompt-cache Stage 0 telemetry baseline could not be read.");
  uos::kv::setKvForTest(nullptr); AWAIT(absent, a::handleAdminCodexCacheScopeExperimentTelemetryBaseline()); EXPECT_EQ(absent.status(), 503);
}
ASYNC_TEST("prompt-cache Stage 0 diagnostic is read-only and redacts the server-selected target") {
  scopeHarness::HarnessScope fixture; scopeHarness::seed(); auto seedTask = scopeHarness::seedStage0Baseline(std::string(scopeHarness::MODEL)); co_await std::move(seedTask);
  uos::cache::scope_experiment_model::PromptCacheScopeExperimentTelemetryBaselineTestOptions options; options.kv = scopeHarness::kv; options.release = std::string(scopeHarness::TELEMETRY_RELEASE);
  AWAIT(baseline, scopeHarness::readPromptCacheScopeExperimentTelemetryBaseline(options)); EXPECT_EQ(baseline.status, "eligible"); const auto calls = scopeHarness::kv->atomicCalls;
  a::ReadBaseline reader = [baseline] { return rt::fulfilledTask(baseline); }; AWAIT(response, a::handleAdminCodexCacheScopeExperimentTelemetryBaseline(reader)); AWAIT(body, response.text()); const auto value = rt::jsonParse(body);
  EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("cache-control"), "no-store"); EXPECT(!rt::get(value, "model_hash")); EXPECT(!rt::get(value, "target")); EXPECT(!rt::includes(body, scopeHarness::MODEL)); EXPECT_EQ(scopeHarness::kv->atomicCalls, calls);
}
ASYNC_TEST("prompt-cache Stage 0 diagnostic does not expose target-selection failures") {
  Fixture fixture; const std::string secret = "target-or-kv-detail-must-not-be-exposed"; rt::console::Capture capture({rt::console::Method::error}); a::ReadBaselineJson reader = [secret]() -> rt::Task<json::value> { throw rt::JsError(secret); co_return nullptr; };
  AWAIT(response, a::handleAdminCodexCacheScopeExperimentTelemetryBaseline(reader)); AWAIT(body, response.text()); EXPECT_EQ(response.status(), 503); EXPECT_EQ(response.headers().get("cache-control"), "no-store"); EXPECT(!rt::includes(body, secret)); for (const auto& call : capture.calls()) EXPECT(!rt::includes(call.text, secret));
}
ASYNC_TEST("admin prompt-cache analytics validates group_by and reports reader failures") {
  Fixture fixture; const std::vector<std::pair<std::string, std::string>> invalid{{"?model=gpt-5.6", "Only group_by is supported for prompt-cache analytics"}, {"?group_by=provider&group_by=model", "group_by may appear only once"}, {"?group_by=provider,provider", "group_by must contain up to two distinct values from provider, model, route, key_presence, mode, or fallback"}, {"?group_by=secret", "group_by must contain up to two distinct values from provider, model, route, key_presence, mode, or fallback"}};
  for (const auto& [query, message] : invalid) { AWAIT(response, a::handleAdminPromptCacheAnalytics(rt::Request("https://ai.ubq.fi/admin/prompt-cache/analytics" + query))); EXPECT_EQ(response.status(), 400); AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "message"), message); }
  std::vector<std::string> seen; a::ReadAnalytics reader = [&seen](auto options) { seen = options.groupBy.value_or(std::vector<std::string>{}); return rt::fulfilledTask(analyticsView(seen)); }; AWAIT(ready, a::handleAdminPromptCacheAnalytics(rt::Request("https://ai.ubq.fi/admin/prompt-cache/analytics?group_by=provider,model"), reader)); EXPECT_EQ(ready.status(), 200); EXPECT_EQ(ready.headers().get("cache-control"), "no-store"); EXPECT_EQ(seen, (std::vector<std::string>{"provider", "model"})); AWAIT(readyBody, ready.json()); EXPECT(rt::deepEqual(readyBody.at("group_by"), json::array{"provider", "model"}));
  a::ReadAnalytics failed = [](auto) -> rt::Task<uos::cache::prompt_analytics_core::PromptCacheAnalyticsView> { throw rt::JsError("analytics unavailable"); co_return analyticsView({}); }; AWAIT(failure, a::handleAdminPromptCacheAnalytics(rt::Request("https://ai.ubq.fi/admin/prompt-cache/analytics"), failed)); EXPECT_EQ(failure.status(), 503); AWAIT(error, payloadOf(failure)); EXPECT_EQ(rt::getString(error, "code"), "prompt_cache_analytics_unavailable");
}
ASYNC_TEST("admin codex auth rejects bodies that are not an auth.json and an unavailable KV") { Fixture fixture; AWAIT(invalid, a::handleAdminCodexAuth(post("/admin/codex/auth", json::object{}))); EXPECT_EQ(invalid.status(), 400); uos::kv::setKvForTest(nullptr); AWAIT(unavailable, a::handleAdminCodexAuth(post("/admin/codex/auth", json::object{}))); EXPECT_EQ(unavailable.status(), 500); }
ASYNC_TEST("admin models catalog serves the picker view and reports KV outages") {
  Fixture fixture; a::CatalogDependencies dependencies; dependencies.buildCatalog = [] { return rt::fulfilledTask(catalogFixture()); }; AWAIT(response, a::handleAdminModelsCatalogGet(dependencies)); EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("cache-control"), "no-store"); AWAIT(body, response.json()); EXPECT_EQ(rt::getBool(body.at("data"), "filter_active"), false); EXPECT_EQ(rt::getArray(body.at("data"), "models")->size(), 2u); uos::kv::setKvForTest(nullptr); AWAIT(absent, a::handleAdminModelsCatalogGet(dependencies)); EXPECT_EQ(absent.status(), 500);
}
ASYNC_TEST("admin provider selection get serves the roster and reports an unavailable KV") {
  Fixture fixture;
  a::CatalogDependencies dependencies;
  dependencies.buildCatalog = [] { return rt::fulfilledTask(catalogFixture()); };
  AWAIT(response, a::handleAdminProviderSelectionGet(dependencies));
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("cache-control"), "no-store");
  AWAIT(body, response.json());
  const auto* providers = rt::getArray(body.at("data"), "providers");
  ASSERT(providers);
  std::vector<std::string_view> ids;
  for (const auto& row : *providers) ids.push_back(rt::getString(row, "id").value_or(""));
  std::vector<std::string_view> expected;
  for (const auto id : uos::provider::selection::SELECTABLE_PROVIDER_IDS) if (id != "gemini") expected.push_back(id);
  EXPECT_EQ(ids, expected);
  EXPECT_EQ(rt::getBool(body.at("data"), "filter_active"), false);
  uos::kv::setKvForTest(nullptr);
  AWAIT(absent, a::handleAdminProviderSelectionGet(dependencies));
  EXPECT_EQ(absent.status(), 500);
}
namespace {
  json::object authPayload(const std::string& account = "acct") { return {{"tokens", json::object{{"access_token", "access"}, {"refresh_token", "refresh"}, {"account_id", account}}}}; }
  rt::Request authRequest(const std::string& account = "acct") { return post("/admin/codex/auth", json::object{{"auth", authPayload(account)}, {"models", json::object{{"client_version", "0.126.0"}}}}); }
  rt::Fetch constantFetch(const json::value& payload, double status = 200, rt::Headers headers = {}) { rt::ResponseInit init; init.status = status; init.headers = headers; return rt::Fetch([payload, init](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(payload, init); }); }
  void put(const rt::KvKey& key, const json::value& value) { h::kvStore.set(h::keyToString(key), rt::KvValue(value)); }
  bool has(const rt::KvKey& key) { return h::kvStore.has(h::keyToString(key)); }
  json::object upstreamCatalog() { return {{"models", json::array{json::object{{"slug", "gpt-5.3-codex-spark"}, {"display_name", "Spark"}}}}}; }
  json::array oversizedModels(const std::string& prefix, int count, bool descriptions) { json::array models; for (int i = 0; i < count; ++i) { json::object row{{"slug", prefix + rt::padStart(std::to_string(i), 4, "0")}, {"display_name", "Oversized " + std::to_string(i)}}; if (descriptions) row["description"] = std::string(400, 'x'); else row["supported_reasoning_levels"] = json::array{"none", "low", "medium", "high", "xhigh", "ultra"}; models.push_back(row); } return models; }
  class FaultKv : public rt::ForwardingKv {
    std::shared_ptr<rt::Kv> owned_;
  public:
    bool failSet = false;
    bool failCommit = false;
    std::optional<rt::KvKey> rejectRead;
    explicit FaultKv(std::shared_ptr<rt::Kv> innerStore) : rt::ForwardingKv(*innerStore), owned_(std::move(innerStore)) {}
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options) override {
      if (rejectRead && key == *rejectRead) throw rt::JsError("controlled admin read failure");
      auto pending = owned_->get(std::move(key), options);
      auto result = co_await std::move(pending);
      co_return result;
    }
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override { if (failSet) throw rt::JsError("catalog store is unavailable"); auto task = owned_->set(key, value, options); const auto result = co_await std::move(task); co_return result; }
    std::unique_ptr<rt::AtomicOperation> atomic() override { if (!failCommit) return owned_->atomic(); return std::make_unique<rt::GenericAtomicOperation>([](auto, auto) -> rt::Task<rt::KvCommitResult> { throw rt::JsError("models commit exploded"); co_return rt::KvCommitResult{}; }); }
  };
}
ASYNC_TEST("catalog callbacks survive a failed whitelist or provider selection read") {
  Fixture fixture;
  for (const bool selection : {false, true}) {
    const auto store = std::make_shared<FaultKv>(h::kvStub());
    store->rejectRead = selection ? uos::provider::selection::PROVIDER_SELECTION_KV_KEY
                                  : uos::models::codex_models_whitelist::CODEX_MODELS_WHITELIST_KV_KEY;
    uos::kv::setKvForTest(store);
    const auto resume = rt::withResolvers<void>();
    const auto done = rt::withResolvers<void>();
    auto capture = std::make_shared<std::string>("admin catalog callback");
    const std::weak_ptr observer = capture;
    a::CatalogDependencies dependencies;
    dependencies.buildCatalog = [capture = std::move(capture), waiting = resume.promise, finish = done.resolve]()
      -> rt::Task<uos::models::catalog::ModelCatalogSnapshot> {
      co_await waiting.wait();
      EXPECT_EQ(*capture, "admin catalog callback");
      finish();
      co_return catalogFixture();
    };
    auto pending = selection ? a::handleAdminProviderSelectionGet(std::move(dependencies)) : a::handleAdminModelsCatalogGet(std::move(dependencies));
    EXPECT_REJECTS_MESSAGE(std::move(pending), rt::JsError, "controlled admin read failure");
    ASSERT(!observer.expired());
    resume.resolve();
    co_await done.promise.wait();
    EXPECT(observer.expired());
  }
}
ASYNC_TEST("prompt-cache scope admin trigger requires a current-release Stage 0 baseline before any outbound work") {
  scopeHarness::HarnessScope fixture; scopeHarness::seed(); std::size_t calls = 0; rt::Fetch fetcher([&calls](rt::RequestInfo, rt::RequestInit) -> rt::Response { ++calls; throw rt::JsError("the Stage0 gate must run before upstream work"); }); const rt::GlobalFetchOverride fetch(fetcher); AWAIT(response, a::handleAdminCodexCacheScopeExperiment(rawPost("/admin/providers/codex/cache-scope-experiment", ""))); EXPECT_EQ(response.status(), 503); AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "code"), "prompt_cache_scope_experiment_unavailable"); EXPECT_EQ(calls, 0u);
}
ASYNC_TEST("admin codex auth maps upstream rejection and unreachable upstreams onto errors") {
  Fixture fixture; { const rt::GlobalFetchOverride fetch(constantFetch(json::object{{"statusCode", 401}, {"description", "Unauthorized"}}, 401)); AWAIT(response, a::handleAdminCodexAuth(authRequest())); EXPECT_EQ(response.status(), 401); AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "code"), "invalid_api_key"); EXPECT(rt::includes(rt::getString(body, "message").value_or(""), "Invalid Codex auth.json (upstream401") || rt::includes(rt::getString(body, "message").value_or(""), "Invalid Codex auth.json (upstream 401")); EXPECT(!has(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY)); }
  const rt::Fetch unavailable([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("upstream is unreachable"); }); const rt::GlobalFetchOverride fetch(unavailable); AWAIT(response, a::handleAdminCodexAuth(authRequest())); EXPECT_EQ(response.status(), 502); AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "code"), "codex_upstream_unreachable"); EXPECT_EQ(rt::getString(body, "type"), "invalid_request_error"); EXPECT(!has(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY));
}
ASYNC_TEST("admin codex auth requires an existing catalog for a rate-limited upload") {
  Fixture fixture; const rt::GlobalFetchOverride fetch(constantFetch(json::object{{"statusCode", 429}, {"description", "Too Many Requests"}}, 429)); AWAIT(response, a::handleAdminCodexAuth(authRequest())); EXPECT_EQ(response.status(), 409); AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "code"), "codex_catalog_required"); EXPECT(rt::includes(rt::getString(body, "message").value_or(""), "without an existing model catalog")); EXPECT(!has(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY));
}
ASYNC_TEST("admin codex auth keeps the stored catalog for a rate-limited upload") {
  Fixture fixture; put(uos::codex::auth::CODEX_MODELS_KV_KEY, json::object{{"source", "chatgpt_codex"}, {"updated_at_ms", 777}, {"models", json::array{json::object{{"slug", "gpt-5.6"}, {"display_name", "GPT-5.6"}}}}}); const rt::GlobalFetchOverride fetch(constantFetch(json::object{{"statusCode", 429}}, 429)); AWAIT(response, a::handleAdminCodexAuth(authRequest())); EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("x-uos-upstream"), "chatgpt_codex"); AWAIT(body, response.json()); EXPECT_EQ(rt::getNumber(body, "upstream_status"), 429); EXPECT_EQ(rt::getBool(body, "normalized_snapshot_updated"), false); EXPECT_EQ(rt::getBool(body, "catalog_seeded"), false); EXPECT_EQ(rt::getNumber(body.at("models"), "count"), 1); EXPECT(body.at("models").at("client_version").is_null()); EXPECT_EQ(rt::getNumber(body, "account_count"), 1); EXPECT(has(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY));
}
ASYNC_TEST("admin codex auth rejects an empty and an oversized upstream catalog") {
  Fixture fixture; { const rt::GlobalFetchOverride fetch(constantFetch(json::object{{"models", json::array{}}})); AWAIT(response, a::handleAdminCodexAuth(authRequest())); EXPECT_EQ(response.status(), 502); AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "message"), "Codex upstream models response did not include a non-empty model catalog"); }
  const rt::GlobalFetchOverride fetch(constantFetch(json::object{{"models", oversizedModels("gpt-5-oversized-", 600, true)}})); AWAIT(response, a::handleAdminCodexAuth(authRequest())); EXPECT_EQ(response.status(), 413); AWAIT(body, payloadOf(response)); EXPECT(rt::startsWith(rt::getString(body, "message").value_or(""), "models snapshot too large (")); EXPECT(!has(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY));
}
ASYNC_TEST("admin codex auth stores the upstream catalog and reports catalog seeding") {
  Fixture fixture; const rt::GlobalFetchOverride fetch(constantFetch(upstreamCatalog(), 200, rt::Headers{{"Content-Type", "application/json"}, {"ETag", "etag-1"}})); AWAIT(response, a::handleAdminCodexAuth(authRequest())); EXPECT_EQ(response.status(), 200); AWAIT(body, response.json()); EXPECT_EQ(rt::getBool(body, "stored"), true); EXPECT_EQ(rt::getBool(body, "refreshed"), false); EXPECT_EQ(rt::getBool(body, "normalized_snapshot_updated"), true); EXPECT_EQ(rt::getBool(body, "catalog_seeded"), true); EXPECT_EQ(rt::getString(body, "upstream_content_type"), "application/json"); EXPECT_EQ(rt::getString(body.at("models"), "client_version"), "0.126.0"); EXPECT(has(uos::codex::auth::CODEX_MODELS_KV_KEY)); EXPECT(rt::deepEqual(body.at("account_ids"), json::array{"acct"}));
}
ASYNC_TEST("admin codex auth reports a catalog seed failure without failing the upload") {
  Fixture fixture; auto failing = std::make_shared<FaultKv>(h::kvStub()); failing->failSet = true; uos::kv::setKvForTest(failing); const rt::GlobalFetchOverride fetch(constantFetch(upstreamCatalog())); AWAIT(response, a::handleAdminCodexAuth(authRequest())); EXPECT_EQ(response.status(), 200); AWAIT(body, response.json()); EXPECT_EQ(rt::getBool(body, "catalog_seeded"), false); EXPECT_EQ(rt::getBool(body, "normalized_snapshot_updated"), true); EXPECT(has(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY));
}
ASYNC_TEST("admin codex models set validates, stores and reports persistence failures") {
  Fixture fixture; AWAIT(invalid, a::handleAdminCodexModelsSet(rawPost("/admin/codex/models", "{"))); EXPECT_EQ(invalid.status(), 400); AWAIT(empty, a::handleAdminCodexModelsSet(post("/admin/codex/models", json::object{{"models", json::array{}}}))); EXPECT_EQ(empty.status(), 400);
  const json::object catalog{{"source", "chatgpt_codex"}, {"client_version", "0.126.0"}, {"models", json::array{json::object{{"slug", "gpt-5.6-sol"}, {"display_name", "GPT-5.6 Sol"}, {"supported_reasoning_levels", json::array{"low", "high"}}}}}}; AWAIT(response, a::handleAdminCodexModelsSet(post("/admin/codex/models", catalog))); EXPECT_EQ(response.status(), 200); AWAIT(body, response.json()); EXPECT_EQ(rt::getBool(body, "ok"), true); EXPECT_EQ(rt::getNumber(body, "count"), 1); EXPECT_EQ(rt::getString(body, "source"), "chatgpt_codex"); EXPECT_EQ(rt::getString(body, "client_version"), "0.126.0"); EXPECT(has(uos::codex::auth::CODEX_MODELS_KV_KEY));
  uos::kv::setKvForTest(nullptr); AWAIT(absent, a::handleAdminCodexModelsSet(post("/admin/codex/models", catalog))); EXPECT_EQ(absent.status(), 500); auto failing = std::make_shared<FaultKv>(h::kvStub()); failing->failCommit = true; uos::kv::setKvForTest(failing); auto request = post("/admin/codex/models", catalog); auto task = a::handleAdminCodexModelsSet(request); EXPECT_REJECTS_MESSAGE(std::move(task), rt::JsError, "models commit exploded");
}
ASYNC_TEST("admin codex models get serves the stored snapshot or a null payload") {
  Fixture fixture; AWAIT(empty, a::handleAdminCodexModelsGet()); AWAIT(emptyBody, empty.json()); EXPECT(rt::deepEqual(emptyBody, json::object{{"ok", true}, {"data", nullptr}})); put(uos::codex::auth::CODEX_MODELS_KV_KEY, json::object{{"source", "chatgpt_codex"}, {"client_version", "0.126.0"}, {"updated_at_ms", 5}, {"models", json::array{json::object{{"slug", "gpt-5.6-sol"}}}}}); AWAIT(stored, a::handleAdminCodexModelsGet()); AWAIT(body, stored.json()); EXPECT_EQ(rt::getString(body.at("data"), "client_version"), "0.126.0"); EXPECT_EQ(body.at("data").at("models").as_array().size(), 1u);
}
ASYNC_TEST("admin codex prompts purge deletes stored prompts and chunks") {
  Fixture fixture; const rt::KvKey top{"uos_ai", "codex_instructions"}, c0{"uos_ai", "codex_instructions_chunk", 0}, c1{"uos_ai", "codex_instructions_chunk", 1}; put(top, json::object{{"instructions", "top level"}}); put(c0, json::object{{"chunk", "first"}}); put(c1, json::object{{"chunk", "second"}}); AWAIT(response, a::handleAdminCodexPromptsPurge()); AWAIT(body, response.json()); EXPECT(rt::deepEqual(body, json::object{{"deleted", 3}})); EXPECT(!has(top)); EXPECT(!has(c0)); EXPECT(!has(c1)); AWAIT(again, a::handleAdminCodexPromptsPurge()); AWAIT(empty, again.json()); EXPECT(rt::deepEqual(empty, json::object{{"deleted", 0}})); uos::kv::setKvForTest(nullptr); AWAIT(absent, a::handleAdminCodexPromptsPurge()); EXPECT_EQ(absent.status(), 500);
}
ASYNC_TEST("admin codex model whitelist reads, stores and reports KV outages") {
  Fixture fixture; AWAIT(empty, a::handleAdminCodexModelsWhitelistGet()); AWAIT(emptyBody, empty.json()); EXPECT(rt::deepEqual(emptyBody, json::object{{"ok", true}, {"data", json::object{{"model_ids", json::array{}}, {"updated_at_ms", 0}}}}));
  const json::object valid{{"model_ids", json::array{" a ", "b", "a"}}}; AWAIT(stored, a::handleAdminCodexModelsWhitelistSet(post("/admin/codex/models/whitelist", valid))); EXPECT_EQ(stored.status(), 200); AWAIT(storedBody, stored.json()); EXPECT(rt::deepEqual(storedBody.at("model_ids"), json::array{"a", "b"})); AWAIT(get, a::handleAdminCodexModelsWhitelistGet()); AWAIT(getBody, get.json()); EXPECT(rt::deepEqual(getBody.at("data").at("model_ids"), json::array{"a", "b"}));
  const std::vector<json::value> invalid{json::object{{"model_ids", "a"}}, json::object{{"model_ids", json::array{1}}}}; for (const auto& raw : invalid) { AWAIT(response, a::handleAdminCodexModelsWhitelistSet(post("/admin/codex/models/whitelist", raw))); EXPECT_EQ(response.status(), 400); }
  uos::kv::setKvForTest(nullptr); AWAIT(absentGet, a::handleAdminCodexModelsWhitelistGet()); EXPECT_EQ(absentGet.status(), 500); AWAIT(absentSet, a::handleAdminCodexModelsWhitelistSet(post("/admin/codex/models/whitelist", valid))); EXPECT_EQ(absentSet.status(), 500);
}
ASYNC_TEST("admin provider selection set validates input and reports storage outages") {
  Fixture fixture; const std::vector<json::value> invalid{json::object{{"provider_ids", "surplus"}}, json::object{{"provider_ids", json::array{1}}}, json::object{{"provider_ids", json::array{"surplus", "not-a-provider"}}}}; for (const auto& raw : invalid) { AWAIT(response, a::handleAdminProviderSelectionSet(post("/admin/provider-selection", raw))); EXPECT_EQ(response.status(), 400); }
  AWAIT(stored, a::handleAdminProviderSelectionSet(post("/admin/provider-selection", json::object{{"provider_ids", json::array{"surplus", " deepseek "}}}))); EXPECT_EQ(stored.status(), 200); AWAIT(body, stored.json()); EXPECT(rt::deepEqual(body.at("provider_ids"), json::array{"surplus", "deepseek"}));
  auto failing = std::make_shared<FaultKv>(h::kvStub()); failing->failSet = true; uos::kv::setKvForTest(failing); AWAIT(failed, a::handleAdminProviderSelectionSet(post("/admin/provider-selection", json::object{{"provider_ids", json::array{"surplus"}}}))); EXPECT_EQ(failed.status(), 500); uos::kv::setKvForTest(nullptr); AWAIT(absent, a::handleAdminProviderSelectionSet(post("/admin/provider-selection", json::object{{"provider_ids", json::array{"surplus"}}}))); EXPECT_EQ(absent.status(), 500);
}
ASYNC_TEST("admin codex auth rejects a third account in a full auth pool") {
  Fixture fixture; const rt::GlobalFetchOverride fetch(constantFetch(upstreamCatalog())); const std::vector<std::string> accounts{"acct-a", "acct-b", "acct-c"}; for (std::size_t i = 0; i < accounts.size(); ++i) { AWAIT(response, a::handleAdminCodexAuth(authRequest(accounts[i]))); EXPECT_EQ(response.status(), i < 2 ? 200 : 409); if (i == 2) { AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "code"), "codex_auth_pool_full"); } }
  const auto* pool = h::kvStore.get(h::keyToString(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY)); ASSERT(pool); EXPECT_EQ(pool->toJson().at("accounts").as_array().size(), 2u);
}
ASYNC_TEST("admin codex auth reports a KV pool commit that never settles") {
  Fixture fixture; h::setAtomicCommitsToFail(3); const rt::GlobalFetchOverride fetch(constantFetch(upstreamCatalog())); AWAIT(response, a::handleAdminCodexAuth(authRequest())); EXPECT_EQ(response.status(), 500); AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "message"), "Deno KV could not persist Codex auth and models"); EXPECT(!has(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY));
}
ASYNC_TEST("admin codex models set reports oversized payloads and an oversized runtime config") {
  Fixture fixture; AWAIT(tooLarge, a::handleAdminCodexModelsSet(post("/admin/codex/models", json::object{{"source", "chatgpt_codex"}, {"models", oversizedModels("gpt-set-oversized-", 600, true)}}))); EXPECT_EQ(tooLarge.status(), 413); EXPECT(!has(uos::codex::auth::CODEX_MODELS_KV_KEY));
  AWAIT(runtimeLarge, a::handleAdminCodexModelsSet(post("/admin/codex/models", json::object{{"source", "chatgpt_codex"}, {"client_version", "0.126.0"}, {"models", oversizedModels("gpt-admin-runtime-", 200, false)}}))); EXPECT_EQ(runtimeLarge.status(), 413); AWAIT(body, payloadOf(runtimeLarge)); EXPECT_EQ(rt::getString(body, "code"), "runtime_config_invalid"); EXPECT(!has(uos::codex::auth::CODEX_MODELS_KV_KEY));
  AWAIT(unstamped, a::handleAdminCodexModelsSet(post("/admin/codex/models", upstreamCatalog()))); EXPECT_EQ(unstamped.status(), 200); AWAIT(unstampedBody, unstamped.json()); EXPECT(unstampedBody.at("client_version").is_null());
}
ASYNC_TEST("admin codex model whitelist rejects a payload over the KV byte ceiling") {
  Fixture fixture; json::array ids; for (int i = 0; i < 2000; ++i) ids.emplace_back("gpt-" + std::to_string(i) + "-" + std::string(48, 'z')); AWAIT(response, a::handleAdminCodexModelsWhitelistSet(post("/admin/codex/models/whitelist", json::object{{"model_ids", ids}}))); EXPECT_EQ(response.status(), 413); AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "message"), "model_ids payload too large (max 65536 bytes)."); EXPECT(!has(uos::models::codex_models_whitelist::CODEX_MODELS_WHITELIST_KV_KEY));
}
ASYNC_TEST("admin provider selection get lists configured Codex subscriptions") {
  Fixture fixture; put(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY, json::object{{"accounts", json::array{json::object{{"access_token", "not-a-jwt"}, {"refresh_token", "refresh"}, {"account_id", "acct-roster"}, {"updated_at_ms", rt::dateNow()}}}}, {"updated_at_ms", rt::dateNow()}}); a::CatalogDependencies dependencies; dependencies.buildCatalog = [] { return rt::fulfilledTask(catalogFixture()); }; AWAIT(response, a::handleAdminProviderSelectionGet(dependencies)); EXPECT_EQ(response.status(), 200); AWAIT(body, response.json()); for (const auto& row : body.at("data").at("providers").as_array()) if (rt::getString(row, "id") == "codex") { const auto& subscriptions = row.at("subscriptions").as_array(); ASSERT_EQ(subscriptions.size(), 1u); EXPECT(rt::getString(subscriptions[0], "id")); EXPECT_EQ(rt::getNumber(subscriptions[0], "slot"), 1); EXPECT(rt::getString(subscriptions[0], "label")); }
}
ASYNC_TEST("admin codex auth reports a catalog too large for the compact runtime config") {
  Fixture fixture; const rt::GlobalFetchOverride fetch(constantFetch(json::object{{"models", oversizedModels("gpt-auth-runtime-", 200, false)}})); AWAIT(response, a::handleAdminCodexAuth(authRequest())); EXPECT_EQ(response.status(), 413); AWAIT(body, payloadOf(response)); EXPECT_EQ(rt::getString(body, "code"), "runtime_config_invalid"); EXPECT(!has(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY));
}
ASYNC_TEST("admin models refresh reports every upstream refresh result") {
  Fixture fixture; a::ModelsRefreshDependencies dependencies;
  dependencies.refreshEnrichment = [] { return rt::fulfilledTask(std::shared_ptr<const uos::models::openrouter_models::OpenRouterModelsSnapshot>{}); };
  dependencies.refreshOpenlux = [](auto) { uos::provider::metered::MeteredModelsSnapshot value{{{"gpt-5.6-sol", "model", 0, "openlux", {}}, {"gpt-5.6-luna", "model", 0, "openlux", {}}}, 9}; return rt::fulfilledTask(std::optional(value)); };
  dependencies.refreshSurplus = [](auto) { return rt::fulfilledTask(std::optional<uos::provider::surplus::SurplusModelsSnapshot>{}); };
  dependencies.refreshLithos = [](auto) { return rt::fulfilledTask(std::optional<uos::provider::lithos_models::LithosModelsSnapshot>{}); };
  dependencies.refreshClaude = [](auto) { return rt::fulfilledTask(std::optional<uos::provider::claude_code_models::ClaudeCodeModelsSnapshot>{}); };
  AWAIT(failed, a::handleAdminModelsRefresh(dependencies)); EXPECT_EQ(failed.status(), 200); EXPECT_EQ(failed.headers().get("cache-control"), "no-store"); AWAIT(body, failed.json()); const auto& data = body.at("data"); EXPECT(rt::deepEqual(data.at("openrouter"), json::object{{"upstream_models", 0}, {"updated_at_ms", nullptr}, {"refreshed", false}})); EXPECT(rt::deepEqual(data.at("openlux"), json::object{{"models", 2}, {"updated_at_ms", 9}})); EXPECT(rt::deepEqual(data.at("surplus"), json::object{{"models", 0}, {"updated_at_ms", nullptr}})); EXPECT(rt::deepEqual(data.at("lithos"), json::object{{"models", 0}, {"updated_at_ms", nullptr}, {"refreshed", false}}));
  dependencies.refreshEnrichment = [] { auto value = std::make_shared<uos::models::openrouter_models::OpenRouterModelsSnapshot>(); value->models.push_back({.id = "a"}); value->updated_at_ms = 11; return rt::fulfilledTask(std::shared_ptr<const uos::models::openrouter_models::OpenRouterModelsSnapshot>(value)); };
  dependencies.refreshOpenlux = [](auto) { return rt::fulfilledTask(std::optional<uos::provider::metered::MeteredModelsSnapshot>{}); };
  dependencies.refreshSurplus = [](auto) { uos::provider::surplus::SurplusModelsSnapshot value{{{"b", "model", 0, "", {}}}, 12}; return rt::fulfilledTask(std::optional(value)); };
  dependencies.refreshLithos = [](auto) { return rt::fulfilledTask(std::optional(uos::provider::lithos_models::LithosModelsSnapshot{{{"c", 0}}, 13})); };
  AWAIT(refreshed, a::handleAdminModelsRefresh(dependencies)); AWAIT(refreshedBody, refreshed.json()); const auto& next = refreshedBody.at("data"); EXPECT(rt::deepEqual(next.at("openrouter"), json::object{{"upstream_models", 1}, {"updated_at_ms", 11}, {"refreshed", true}})); EXPECT(rt::deepEqual(next.at("surplus"), json::object{{"models", 1}, {"updated_at_ms", 12}})); EXPECT(rt::deepEqual(next.at("lithos"), json::object{{"models", 1}, {"updated_at_ms", 13}, {"refreshed", true}}));
}
ASYNC_TEST("admin models refresh performs keyless Surplus discovery") {
  Fixture fixture; std::size_t calls = 0; a::ModelsRefreshDependencies dependencies;
  dependencies.refreshEnrichment = [] { return rt::fulfilledTask(std::shared_ptr<const uos::models::openrouter_models::OpenRouterModelsSnapshot>{}); };
  dependencies.refreshOpenlux = [](auto) { return rt::fulfilledTask(std::optional<uos::provider::metered::MeteredModelsSnapshot>{}); };
  dependencies.refreshSurplus = [&calls](auto options) { EXPECT_EQ(options.requireApiKey, false); EXPECT_EQ(options.force, true); options.fetcher = [&calls](rt::RequestInfo, rt::RequestInit) { ++calls; return rt::Response::json(json::object{{"data", json::array{json::object{{"id", "surplus-test-model"}}}}}); }; return uos::provider::surplus::fetchSurplusModels(options); };
  dependencies.refreshLithos = [](auto) { return rt::fulfilledTask(std::optional<uos::provider::lithos_models::LithosModelsSnapshot>{}); };
  dependencies.refreshClaude = [](auto) { return rt::fulfilledTask(std::optional<uos::provider::claude_code_models::ClaudeCodeModelsSnapshot>{}); };
  AWAIT(response, a::handleAdminModelsRefresh(dependencies)); EXPECT_EQ(response.status(), 200); EXPECT_EQ(calls, 1u); AWAIT(body, response.json()); EXPECT_EQ(rt::getNumber(body.at("data").at("surplus"), "models"), 1);
}
ASYNC_TEST("a refresh callback can finish after a different provider rejects") {
  Fixture fixture;
  const auto resume = rt::withResolvers<void>();
  const auto done = rt::withResolvers<void>();
  auto capture = std::make_shared<std::string>("admin refresh callback");
  const std::weak_ptr observer = capture;
  a::ModelsRefreshDependencies dependencies;
  dependencies.refreshEnrichment = [] { return rt::fulfilledTask(std::shared_ptr<const uos::models::openrouter_models::OpenRouterModelsSnapshot>{}); };
  dependencies.refreshOpenlux = [capture = std::move(capture), waiting = resume.promise, finish = done.resolve](auto options)
    -> rt::Task<std::optional<uos::provider::metered::MeteredModelsSnapshot>> {
    co_await waiting.wait();
    EXPECT_EQ(*capture, "admin refresh callback");
    EXPECT(options.force);
    finish();
    co_return std::nullopt;
  };
  dependencies.refreshSurplus = [](auto) -> rt::Task<std::optional<uos::provider::surplus::SurplusModelsSnapshot>> {
    throw rt::JsError("controlled refresh failure");
    co_return std::nullopt;
  };
  dependencies.refreshLithos = [](auto) { return rt::fulfilledTask(std::optional<uos::provider::lithos_models::LithosModelsSnapshot>{}); };
  dependencies.refreshClaude = [](auto) { return rt::fulfilledTask(std::optional<uos::provider::claude_code_models::ClaudeCodeModelsSnapshot>{}); };
  auto pending = a::handleAdminModelsRefresh(std::move(dependencies));
  EXPECT_REJECTS_MESSAGE(std::move(pending), rt::JsError, "controlled refresh failure");
  ASSERT(!observer.expired());
  resume.resolve();
  co_await done.promise.wait();
  EXPECT(observer.expired());
}
ASYNC_TEST("admin models refresh keeps the last good snapshots when every upstream fails") {
  Fixture fixture; const rt::Fetch unavailable([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("upstream metadata is unreachable"); }); const rt::GlobalFetchOverride fetch(unavailable); AWAIT(response, a::handleAdminModelsRefresh()); EXPECT_EQ(response.status(), 200); AWAIT(body, response.json()); const auto& data = body.at("data"); EXPECT_EQ(rt::getNumber(data.at("openrouter"), "upstream_models"), 0); EXPECT(data.at("openrouter").at("updated_at_ms").is_null()); for (const auto key : {"openlux", "surplus", "lithos"}) EXPECT_EQ(rt::getNumber(data.at(key), "models"), 0);
}

ASYNC_TEST("prompt-cache Stage 0 nullish reader results preserve the suppressed 503 envelope") {
  Fixture fixture; a::ReadBaselineJson reader = [] { return rt::fulfilledTask(json::value(nullptr)); }; AWAIT(response, a::handleAdminCodexCacheScopeExperimentTelemetryBaseline(reader)); EXPECT_EQ(response.status(),503); EXPECT_EQ(response.headers().get("cache-control"), "no-store"); AWAIT(body, response.json()); EXPECT(rt::deepEqual(body,json::object{{"error",json::object{{"message","Prompt-cache Stage 0 telemetry baseline could not be read."},{"type","server_error"},{"code","prompt_cache_scope_experiment_unavailable"}}}}));
}
