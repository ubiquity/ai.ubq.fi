// Links: tests/codex_catalog_stale_while_revalidate_test.cpp
// src/models/metadata.cpp src/models/catalog.cpp src/utils.cpp src/provider/lithos.cpp
// src/provider/selection.cpp src/provider/surplus.cpp src/provider/metered.cpp src/input_normalization.cpp
// src/provider/cerebras.cpp src/models/openrouter_models.cpp src/models/codex_models_whitelist.cpp src/provider/anthropic_responses.cpp
// src/codex/auth.cpp src/types.cpp src/config.cpp src/models/codex_models.cpp
// src/http.cpp src/codex/capacity_routing.cpp src/provider/anthropic_response_wire.cpp src/codex/native_auth.cpp
// src/catalog/index.cpp src/authority/native_codex_capacity.cpp src/catalog/models.cpp src/catalog/store.cpp
// src/auth/relay.cpp src/catalog/types.cpp src/request_policy.cpp src/authority/native_codex_contracts.cpp
// src/provider/claude_code_models.cpp src/defaults.cpp src/models/codex_models_fetch.cpp src/provider/surplus_claude_request.cpp
// src/provider/anthropic_replay.cpp src/models/codex_models_availability.cpp src/provider/claude_code_accounts.cpp src/codex/auth_refresh.cpp
// src/codex/routing_mutations.cpp src/runtime_config.cpp src/release.cpp src/responses_stream.cpp
// src/authority/native_codex_mutations.cpp src/codex/external_auth.cpp src/provider/claude_code_credentials.cpp src/codex/dispatch.cpp
// src/provider/surplus_claude_chat_request.cpp src/codex/native_auth_transport.cpp src/authority/native_codex_evaluation.cpp src/codex/rate_limit_429.cpp
// src/openai_telemetry.cpp src/deepseek/waterfall.cpp src/codex/routing_state.cpp src/kv.cpp
// src/bounded_response_body.cpp src/recent_model_context.cpp src/provider/health.cpp src/provider/openrouter.cpp
// src/deepseek/index.cpp src/provider/lithos_rate_limits.cpp src/provider/anthropic_chat_responses.cpp src/responses_failover_stream.cpp
// src/provider/capacity_events.cpp src/authority/native_codex_429.cpp src/provider/cerebras_rate_limits.cpp src/runtime/websocket.cpp
// src/codex/banked_reset.cpp src/provider/claude_code.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
// src/codex/responses_error.cpp src/provider/capacity_contract.cpp src/provider/capacity.cpp src/provider/removed_provider_telemetry.cpp
// src/provider/capacity_parse.cpp src/storage/kv.cpp src/provider/claude_capacity_history.cpp src/codex/banked_reset_provider.cpp
// src/codex/reset_settings.cpp src/provider/capacity_rollups.cpp src/provider/claude_capacity_rollups.cpp src/cache/prompt_analytics_core.cpp
// src/provider/capacity_history.cpp src/codex/experiment.cpp src/paid_fallback/ledger_stats.cpp src/codex/account_routing.cpp
// src/codex/routing_serial.cpp src/provider/anthropic_chat_stream.cpp src/authority/native_codex.cpp src/codex/routing_evaluation.cpp
// src/storage/sqlite_readonly.cpp src/metered_quota.cpp src/provider/capacity_analytics.cpp
// src/cache/prompt_analytics_read.cpp src/storage/native_kv.cpp src/codex/overage_settings.cpp
// Port of tests/codex-catalog-stale-while-revalidate.test.ts; real catalog handler, synthetic auth and memory KV.
#include "uos/catalog/index.hpp"
#include "uos/catalog/store.hpp"
#include "uos/catalog/types.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/selection.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/test_async.hpp"
namespace rt = uos::rt;
namespace json = uos::json;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace store = uos::catalog::store;
  namespace types = uos::catalog::types;
  namespace openrouter = uos::models::openrouter_models;
  std::string catalogBody(const std::string& version, bool refreshed = false) { json::object value{{"models", json::array{json::object{{"slug", "gpt-" + version}, {"display_name", "Rich " + version}}}}}; if (refreshed) value["refreshed"] = true; return rt::jsonStringify(value); }
  rt::Request request(const std::string& version) { rt::RequestInit init; init.headers = rt::Headers{{"Authorization", "Bearer gateway-client-token"}}; return rt::Request("https://ai.ubq.fi/v1/models?client_version=" + version, init); }
  rt::Response response(const std::string& version) { rt::ResponseInit init; init.headers = rt::Headers{{"Content-Type", "application/json"}}; return rt::Response(catalogBody(version, true), init); }
}
ASYNC_TEST("codex catalog: stale cache returns before its background refresh settles") {
  rt::env::ScopedEnvironment environment(std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c4-swr"}});
  const auto savedAuth = uos::config::config.codexAuthJsonB64; uos::config::config.codexAuthJsonB64.clear();
  auto kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); }); uos::kv::setKvForTest(kv); uos::codex::auth::resetCodexAuthCacheForTest(); store::resetCodexCatalogMemoForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); openrouter::resetOpenRouterModelsCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest();
  uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const auto&) {}); openrouter::setOpenRouterModelsFetchForTest(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"data", json::array{}}}); }));
  auto cleanup = rt::scopeExit([&] { uos::kv::setKvForTest(nullptr); uos::codex::auth::resetCodexAuthCacheForTest(); store::resetCodexCatalogMemoForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); openrouter::resetOpenRouterModelsCacheForTest(); openrouter::setOpenRouterModelsFetchForTest({}); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest({}); uos::config::config.codexAuthJsonB64 = savedAuth; });
  const std::string version = "0.170.0", generation = "stale-while-revalidate-auth"; const auto now = rt::dateNow();
  const rt::KvValue generationValue(generation); AWAIT(g, kv->set(types::CODEX_CATALOG_AUTH_GENERATION_KEY, generationValue)); (void)g;
  const json::object auth{{"accounts", json::array{json::object{{"access_token", "server-access"}, {"refresh_token", "server-refresh"}, {"account_id", "server-account"}, {"updated_at_ms", now}}}}, {"updated_at_ms", now}}; const rt::KvKey authKey{"ubq_ai", "codex_auth"}; const rt::KvValue authValue(auth); AWAIT(a, kv->set(authKey, authValue)); (void)a;
  const json::object snapshot{{"source", "chatgpt_codex"}, {"client_version", version}, {"updated_at_ms", now}, {"models", json::array{json::object{{"slug", "snapshot-0.170.0"}}}}}; const rt::KvKey snapshotKey{"ubq_ai", "codex_models"}; const rt::KvValue snapshotValue(snapshot); AWAIT(s, kv->set(snapshotKey, snapshotValue)); (void)s;
  const json::object runtime{{"version", 2}, {"default_model", "snapshot-0.170.0"}, {"default_reasoning_effort", "medium"}, {"codex_models", snapshot}, {"updated_at_ms", now}}; const rt::KvValue runtimeValue(runtime); AWAIT(c, kv->set(uos::runtime_config::RUNTIME_CONFIG_V2_KEY, runtimeValue)); (void)c;
  store::StoreCodexCatalogInput input{.clientVersion = version, .authGeneration = generation, .body = catalogBody(version), .fetchedAtMs = now - types::CODEX_CATALOG_FRESH_MS - 1}; AWAIT(stored, store::storeCodexCatalog(*kv, input)); EXPECT(stored);
  auto [started, markStarted, rejectStarted] = rt::withResolvers<void>(); (void)rejectStarted; auto [deferred, resolveFetch, rejectFetch] = rt::withResolvers<rt::Response>(); (void)rejectFetch;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { markStarted(); return deferred.wait(); })); bool released = false; auto release = [&] { if (!released) { released = true; resolveFetch(response(version)); } }; auto releaseOnFailure = rt::scopeExit(release);
  auto req = request(version); rt::Promise<rt::Response> pending(uos::catalog::index::handleCodexCatalogModels(req, version)); co_await started.wait();
  auto completed = pending.then([](rt::Response value) { return std::optional<rt::Response>(value); }); auto deadline = rt::newPromise<std::optional<rt::Response>>([](auto resolve, auto) { rt::setTimeout([resolve] { resolve(std::nullopt); }, 20); }); std::vector<rt::Promise<std::optional<rt::Response>>> racers{completed, deadline}; AWAIT(stale, rt::race(std::move(racers)).wait());
  EXPECT(stale.has_value()); if (stale) { EXPECT_EQ(stale->headers().get("x-uos-cache"), "stale"); AWAIT(payload, stale->json()); EXPECT_EQ(rt::getString(payload.at("models").as_array()[0], "slug"), "gpt-" + version); }
  release(); AWAIT(initial, pending.wait()); (void)initial; const rt::KvKey leaseKey{types::CODEX_CATALOG_LEASE_PREFIX, version};
  for (int attempt = 0; attempt < 200; ++attempt) { AWAIT(lease, kv->get(leaseKey)); if (!lease.versionstamp) break; if (attempt == 199) throw rt::JsError("catalog refresh did not release its lease"); co_await rt::sleep(5); }
  auto refreshedReq = request(version); AWAIT(refreshed, uos::catalog::index::handleCodexCatalogModels(refreshedReq, version)); EXPECT_EQ(refreshed.headers().get("x-uos-cache"), "hit"); AWAIT(body, refreshed.json()); EXPECT_EQ(rt::getBool(body, "refreshed"), true); EXPECT_EQ(rt::getString(body.at("models").as_array()[0], "slug"), "gpt-" + version);
}
