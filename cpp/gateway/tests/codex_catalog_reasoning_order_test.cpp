// Links: tests/codex_catalog_reasoning_order_test.cpp
// src/models/openrouter_models.cpp src/catalog/models.cpp src/models/metadata.cpp src/models/codex_models.cpp
// src/recent_model_context.cpp src/runtime_config.cpp src/models/codex_models_fetch.cpp src/utils.cpp
// src/provider/claude_code_models.cpp src/provider/claude_code.cpp src/deepseek/index.cpp src/defaults.cpp
// src/codex/dispatch.cpp src/request_policy.cpp src/openai_telemetry.cpp src/config.cpp
// src/codex/auth_refresh.cpp src/provider/openrouter.cpp src/provider/claude_code_accounts.cpp src/types.cpp
// src/catalog/types.cpp src/codex/auth.cpp src/provider/claude_code_credentials.cpp src/provider/selection.cpp
// src/provider/surplus_claude_request.cpp src/provider/claude_code_quota.cpp src/provider/anthropic_replay.cpp src/codex/native_auth.cpp
// src/codex/external_auth.cpp src/provider/lithos.cpp src/provider/cerebras.cpp src/kv.cpp
// src/codex/routing_mutations.cpp src/bounded_response_body.cpp src/models/codex_models_availability.cpp src/provider/capacity_events.cpp
// src/provider/capacity_contract.cpp src/storage/kv.cpp src/codex/capacity_routing.cpp src/authority/native_codex_contracts.cpp
// src/authority/native_codex_capacity.cpp src/codex/routing_state.cpp src/release.cpp src/codex/rate_limit_429.cpp
// src/authority/native_codex_mutations.cpp src/provider/anthropic_responses.cpp src/provider/metered.cpp src/provider/removed_provider_telemetry.cpp
// src/codex/native_auth_transport.cpp src/provider/surplus.cpp src/provider/claude_code_usage_schema.cpp src/provider/surplus_claude_chat_request.cpp
// src/authority/native_codex_evaluation.cpp src/provider/capacity.cpp src/storage/sqlite_readonly.cpp src/provider/capacity_history.cpp
// src/responses_stream.cpp src/provider/capacity_rollups.cpp src/provider/capacity_parse.cpp src/cache/prompt_analytics_read.cpp
// src/authority/native_codex_429.cpp src/cache/prompt_analytics_core.cpp src/provider/health.cpp src/paid_fallback/ledger_stats.cpp
// src/codex/experiment.cpp src/codex/banked_reset.cpp src/provider/claude_capacity_history.cpp src/provider/anthropic_chat_responses.cpp
// src/provider/anthropic_response_wire.cpp src/storage/native_kv.cpp src/provider/capacity_analytics.cpp
// src/provider/claude_capacity_rollups.cpp src/runtime/websocket.cpp src/codex/banked_reset_provider.cpp src/responses_failover_stream.cpp
// src/codex/responses_error.cpp src/http.cpp src/codex/account_routing.cpp src/codex/routing_serial.cpp
// src/provider/lithos_rate_limits.cpp src/codex/routing_evaluation.cpp src/metered_quota.cpp src/provider/cerebras_rate_limits.cpp
// src/codex/overage_settings.cpp src/codex/reset_settings.cpp src/authority/native_codex.cpp src/provider/anthropic_chat_stream.cpp
// src/auth/relay.cpp
// Port of tests/codex-catalog-reasoning-order.test.ts. Link closure filled after the compile-only check.
#include "uos/catalog/models.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/test_async.hpp"
namespace rt = uos::rt;
namespace json = uos::json;
namespace m = uos::catalog::models;
namespace o = uos::models::openrouter_models;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  const std::string modelId = "~anthropic/claude-opus-latest";
  const json::object paidModel{{"id", modelId}, {"owned_by", "anthropic"}, {"supported_endpoint_types", json::array{"openai-response"}}};
  json::array nativeEfforts(const json::object& row) { json::array result; const auto* levels = rt::getArray(row, "supported_reasoning_levels"); if (levels) for (const auto& level : *levels) result.push_back(*rt::get(level, "effort")); return result; }
  rt::Task<void> withExternalReasoning(json::array levels, std::string defaultEffort, std::function<void()> check) {
    rt::env::ScopedEnvironment env(std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c4-reasoning"}, {"OPENROUTER_API_KEY", "catalog-reasoning-order-test-key"}});
    o::resetOpenRouterModelsCacheForTest(); auto cleanup = rt::scopeExit([] { o::resetOpenRouterModelsCacheForTest(); });
    o::FetchOpenRouterModelsOptions options; options.force = true; options.fetcher = [levels, defaultEffort](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"data", json::array{json::object{{"id", modelId}, {"reasoning", json::object{{"supported_efforts", levels}, {"default_effort", defaultEffort}, {"mandatory", false}}}}}}}); };
    AWAIT(snapshot, o::fetchOpenRouterModels(options)); ASSERT(snapshot); check();
    ASSERT(snapshot->models[0].reasoning); EXPECT(rt::deepEqual(json::value_from(snapshot->models[0].reasoning->supported_efforts), levels));
  }
}
TEST("official DeepSeek rows replace a paid discovery duplicate") {
  rt::env::ScopedEnvironment env(std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c4-reasoning"}, {"DEEPSEEK_API_KEY", "fixture-deepseek-key"}});
  const json::object paid{{"slug", "deepseek-v4-pro"}, {"owned_by", "metered"}, {"supported_reasoning_levels", json::array{json::object{{"effort", "none"}}}}, {"default_reasoning_level", "none"}};
  const auto rows = m::withDeepSeekOfficialModels(json::array{paid}); const json::object* pro = nullptr; std::size_t count = 0;
  for (const auto& row : rows) if (rt::getString(row, "slug") == "deepseek-v4-pro") { pro = &row.as_object(); ++count; }
  ASSERT(pro); EXPECT_EQ(count, 1u); EXPECT_EQ(rt::getString(*pro, "owned_by"), "deepseek"); EXPECT(rt::deepEqual(nativeEfforts(*pro), json::array{"none", "low", "high", "max"})); EXPECT_EQ(rt::getString(*pro, "default_reasoning_level"), "high");
}
ASYNC_TEST("codex native catalog: reversed external Claude efforts become ascending without adding tiers") {
  json::array levels{"max", "xhigh", "high", "medium", "low"};
  auto check = [] { const auto rows = m::openRouterCodexModels(); ASSERT_EQ(rows.size(), 1u); const std::vector<json::object> both{rows[0].as_object(), m::meteredCodexModelRecord(paidModel)}; for (const auto& row : both) { EXPECT(rt::deepEqual(nativeEfforts(row), json::array{"low", "medium", "high", "xhigh", "max"})); EXPECT_EQ(rt::getString(row, "default_reasoning_level"), "high"); } };
  auto task = withExternalReasoning(levels, "high", check); co_await std::move(task);
}
ASYNC_TEST("codex native catalog: standard tiers sort while unknown tiers keep their strings and positions") {
  json::array levels{"ultra", "custom-v1", "max", "high", "CUSTOM", "xhigh", "medium", "minimal", "low", "none"};
  auto check = [] { auto rows = m::openRouterCodexModels(); rows.push_back(m::meteredCodexModelRecord(paidModel)); for (const auto& row : rows) { EXPECT(rt::deepEqual(nativeEfforts(row.as_object()), json::array{"none", "custom-v1", "minimal", "low", "CUSTOM", "medium", "high", "xhigh", "max", "ultra"})); EXPECT_EQ(rt::getString(row, "default_reasoning_level"), "custom-v1"); } };
  auto task = withExternalReasoning(levels, "custom-v1", check); co_await std::move(task);
}
ASYNC_TEST("codex native catalog: uploaded Codex effort order and default retain precedence over external metadata") {
  json::array levels{"low", "medium", "high", "xhigh", "max"};
  auto check = [] { json::array uploadedLevels{"max", "custom-v1", "xhigh", "high", "low"}; const json::object uploaded{{"slug", modelId}, {"supported_reasoning_levels", uploadedLevels}, {"default_reasoning_level", "max"}}; const auto row = m::meteredCodexModelRecord(paidModel, &uploaded); EXPECT(rt::deepEqual(nativeEfforts(row), uploadedLevels)); EXPECT_EQ(rt::getString(row, "default_reasoning_level"), "max"); EXPECT(rt::deepEqual(*rt::get(uploaded, "supported_reasoning_levels"), uploadedLevels)); };
  auto task = withExternalReasoning(levels, "medium", check); co_await std::move(task);
}
