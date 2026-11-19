// Links: tests/codex_catalog_metered_enrichment_test.cpp
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
// Port of tests/codex-catalog-metered-enrichment.test.ts; real paid-only catalog path and metadata fetch seam.
#include "uos/catalog/index.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/test_async.hpp"
namespace rt = uos::rt;
namespace json = uos::json;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
ASYNC_TEST("metered-only Codex catalog fallback starts OpenRouter enrichment") {
  rt::env::ScopedEnvironment environment(std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c4-enrichment"}, {"METERED_API_KEY", "metered-catalog-enrichment-test-key"}}); const auto savedAuth = uos::config::config.codexAuthJsonB64; uos::config::config.codexAuthJsonB64.clear();
  namespace metered = uos::provider::metered; namespace openrouter = uos::models::openrouter_models; int openRouterFetches = 0; uos::kv::setKvForTest(nullptr); metered::resetMeteredModelsCacheForTest(); openrouter::resetOpenRouterModelsCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest();
  metered::setMeteredModelsFetchForTest(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"data", json::array{json::object{{"id", "metered-only-model"}, {"owned_by", "openlux"}, {"supported_endpoint_types", json::array{"openai-response"}}}}}}); }));
  openrouter::setOpenRouterModelsFetchForTest(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++openRouterFetches; return rt::Response::json(json::object{{"data", json::array{}}}); })); auto cleanup = rt::scopeExit([&] { metered::setMeteredModelsFetchForTest({}); openrouter::setOpenRouterModelsFetchForTest({}); metered::resetMeteredModelsCacheForTest(); openrouter::resetOpenRouterModelsCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::kv::setKvForTest(nullptr); uos::config::config.codexAuthJsonB64 = savedAuth; });
  rt::Request request("https://ai.ubq.fi/v1/models?client_version=0.250.0"); const std::string version = "0.250.0"; AWAIT(response, uos::catalog::index::handleCodexCatalogModels(request, version)); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); const auto* models = rt::getArray(payload, "models"); ASSERT(models); bool found = false; for (const auto& model : *models) if (rt::getString(model, "slug") == "metered-only-model") found = true; EXPECT(found); EXPECT_EQ(openRouterFetches, 1);
}
