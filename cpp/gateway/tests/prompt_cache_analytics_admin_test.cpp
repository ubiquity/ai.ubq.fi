// Port of all three tests/prompt-cache-analytics-admin.test.ts cases.
// Calls actual admin/index exports; registration/authentication remain with handler/index.
// Links: tests/prompt_cache_analytics_admin_test.cpp
// src/admin/codex.cpp src/http.cpp src/provider/presentation.cpp src/request.cpp src/cache/prompt_analytics_read.cpp
// src/cache/scope_experiment_targets.cpp tests/helpers/codex_diff_kv.cpp src/cache/prompt_analytics_core.cpp src/debug_routing.cpp src/cache/scope_experiment_model.cpp
// src/deepseek/waterfall.cpp src/codex/auth.cpp src/config.cpp src/provider/selection.cpp src/codex/account_routing.cpp
// src/codex/routing_state.cpp src/models/catalog.cpp src/auth/relay.cpp src/models/metadata.cpp src/authority/native_codex_mutations.cpp
// src/input_normalization.cpp src/runtime_config.cpp src/codex/experiment.cpp src/codex/dispatch.cpp src/types.cpp
// src/authority/native_codex.cpp src/provider/cerebras.cpp src/authority/native_codex_capacity.cpp src/catalog/types.cpp src/utils.cpp
// src/request_policy.cpp src/provider/capacity_events.cpp src/codex/routing_evaluation.cpp src/models/codex_models.cpp src/authority/native_codex_contracts.cpp
// src/provider/lithos_models.cpp src/provider/metered.cpp src/provider/claude_code_models.cpp src/provider/surplus.cpp src/cache/telemetry_gate.cpp
// src/recent_model_context.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/authority/native_codex_evaluation.cpp src/codex/external_auth.cpp
// src/catalog/store.cpp src/provider/surplus_claude_request.cpp src/codex/capacity_routing.cpp src/kv.cpp src/deepseek/index.cpp
// src/catalog/index.cpp src/models/openrouter_models.cpp src/bounded_response_body.cpp src/provider/lithos.cpp src/cache/scope_targets.cpp
// src/provider/claude_code.cpp src/codex/overage_settings.cpp src/provider/claude_code_accounts.cpp src/provider/capacity_parse.cpp src/catalog/models.cpp
// src/responses_stream.cpp src/models/codex_models_whitelist.cpp src/defaults.cpp src/provider/cerebras_rate_limits.cpp src/provider/anthropic_replay.cpp
// src/provider/claude_code_credentials.cpp src/codex/auth_refresh.cpp src/provider/openrouter.cpp src/cache/scope_experiment.cpp src/models/codex_models_fetch.cpp
// src/provider/capacity.cpp src/provider/capacity_rollups.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_quota.cpp src/provider/capacity_history.cpp
// src/provider/lithos_rate_limits.cpp src/admin/api_keys.cpp src/models/codex_models_availability.cpp src/codex/banked_reset.cpp src/openai_telemetry.cpp
// src/api_key_policy.cpp src/provider/health.cpp src/provider/capacity_contract.cpp src/api_keys.cpp src/provider/claude_code_usage_schema.cpp
// src/release.cpp src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/provider/surplus_claude_chat_request.cpp src/admin/api_key_audit.cpp
// src/codex/native_auth.cpp src/provider/claude_capacity_history.cpp src/codex/reset_settings.cpp src/paid_fallback/ledger_state.cpp src/authority/native_api_key_audit.cpp
// src/authority/native_paid_contracts.cpp src/provider/anthropic_chat_responses.cpp src/codex/banked_reset_pool.cpp src/provider/capacity_analytics.cpp src/metered_quota.cpp
// src/codex/rate_limit_429.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/index.cpp src/codex/banked_reset_claim.cpp src/storage/kv.cpp
// src/paid_fallback/ledger_admission.cpp src/authority/native_paid.cpp src/provider/anthropic_responses.cpp src/authority/native_paid_policy.cpp src/paid_fallback/ledger_backfill.cpp
// src/provider/anthropic_response_wire.cpp src/responses_failover_stream.cpp src/catalog/promotion.cpp src/storage/sqlite_readonly.cpp src/codex/banked_reset_submission.cpp
// src/paid_fallback/ledger_settlement.cpp src/authority/native_codex_429.cpp src/provider/removed_provider_telemetry.cpp src/codex/banked_reset_provider.cpp
// src/codex/native_auth_transport.cpp src/provider/anthropic_chat_stream.cpp src/storage/native_kv.cpp src/codex/responses_error.cpp src/runtime/websocket.cpp
// src/health.cpp src/inference_admission.cpp src/provider/claude_code_access.cpp
#include "uos/admin/index.hpp"
#include "uos/cache/prompt_analytics_core.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"

#include <string>

namespace rt = uos::rt;
namespace json = uos::json;
namespace analytics = uos::cache::prompt_analytics_core;

namespace {
  // `readFileSync(new URL("../static/admin.js", import.meta.url), "utf8")`
  std::string adminScript() { return rt::fs::readFileSync(rt::path::resolve(rt::path::dirname(__FILE__), "../../../static/admin.js"), "utf8"); }
  analytics::PromptCacheAnalyticsView readyView(const std::vector<std::string>& groupBy) {
    auto value = rt::jsonParse(R"({"status":"ready","bucket_ms":900000,"window_start_at_ms":1799000000000,"window_end_at_ms":1800000900000,"group_by":[],"max_buckets":512,"cardinality_limited":false,"truncated":false,"buckets":[{"bucket_start_at_ms":1800000000000,"bucket_end_at_ms":1800000900000,"input_tokens":100,"cached_input_tokens":50,"cache_write_input_tokens":25,"cache_write_reported_sample_count":1,"cached_percentage":50,"sample_count":1,"group":{"prompt_cache_key_present":true},"token_hit_percentage":50,"request_cache_hit_sample_count":1,"request_hit_percentage":100,"cache_reads_per_write":2,"usage_reported_sample_count":1,"usage_invalid_sample_count":0,"usage_missing_sample_count":0,"usage_telemetry_coverage_percentage":100,"dimension_cardinality_limited_sample_count":0}]})");
    value.as_object()["group_by"] = json::value_from(groupBy);
    return json::value_to<analytics::PromptCacheAnalyticsView>(value);
  }
}

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
ASYNC_TEST("prompt-cache analytics admin API validates a bounded group_by before reading KV") {
  std::vector<analytics::PromptCacheAnalyticsReadOptions> reads;
  uos::admin::codex::ReadAnalytics reader = [&](analytics::PromptCacheAnalyticsReadOptions input) { reads.push_back(input); return rt::fulfilledTask(readyView(input.groupBy.value_or(std::vector<std::string>()))); };
  AWAIT(response, uos::admin::index::handleAdminPromptCacheAnalytics(rt::Request("https://ai.ubq.fi/admin/prompt-cache-analytics?group_by=provider,route"), reader));
  EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("cache-control"), std::optional<std::string>("no-store")); ASSERT_EQ(reads.size(), std::size_t(1)); EXPECT_EQ(reads[0].groupBy, std::optional(std::vector<std::string>{"provider", "route"})); AWAIT(body, response.json()); EXPECT(rt::deepEqual(*rt::get(body, "group_by"), json::array{"provider", "route"}));
  AWAIT(defaultGroup, uos::admin::index::handleAdminPromptCacheAnalytics(rt::Request("https://ai.ubq.fi/admin/prompt-cache-analytics"), reader)); AWAIT(defaultBody, defaultGroup.json()); EXPECT(rt::deepEqual(*rt::get(defaultBody, "group_by"), json::array{"key_presence"}));
  const std::vector<std::string> invalidUrls{"https://ai.ubq.fi/admin/prompt-cache-analytics?group_by=provider,unknown", "https://ai.ubq.fi/admin/prompt-cache-analytics?group_by=provider,provider", "https://ai.ubq.fi/admin/prompt-cache-analytics?group_by=provider,route,mode", "https://ai.ubq.fi/admin/prompt-cache-analytics?group_by=provider&group_by=route", "https://ai.ubq.fi/admin/prompt-cache-analytics?model=raw-model-must-not-be-accepted"};
  std::size_t unexpectedReads = 0; uos::admin::codex::ReadAnalytics refusedReader = [&](analytics::PromptCacheAnalyticsReadOptions) -> rt::Task<analytics::PromptCacheAnalyticsView> { ++unexpectedReads; throw rt::JsError("reader must not run for invalid input"); co_return readyView({}); };
  for (const auto& url : invalidUrls) { AWAIT(invalid, uos::admin::index::handleAdminPromptCacheAnalytics(rt::Request(url), refusedReader)); EXPECT_EQ(invalid.status(), 400); AWAIT(error, invalid.json()); EXPECT_EQ(rt::getString(*rt::get(error, "error"), "type"), std::optional<std::string_view>("invalid_request_error")); }
  EXPECT_EQ(unexpectedReads, std::size_t(0));
}

ASYNC_TEST("prompt-cache analytics admin API preserves an explicit unavailable state") {
  uos::admin::codex::ReadAnalytics unavailableReader = [](analytics::PromptCacheAnalyticsReadOptions input) { auto view = readyView(input.groupBy.value_or(std::vector<std::string>())); view.status = "unavailable"; view.buckets.clear(); return rt::fulfilledTask(view); };
  AWAIT(unavailable, uos::admin::index::handleAdminPromptCacheAnalytics(rt::Request("https://ai.ubq.fi/admin/prompt-cache-analytics?group_by=key_presence"), unavailableReader)); EXPECT_EQ(unavailable.status(), 200); AWAIT(body, unavailable.json()); EXPECT_EQ(rt::getString(body, "status"), std::optional<std::string_view>("unavailable"));
  const std::string secret = "prompt-cache-reader-secret"; uos::admin::codex::ReadAnalytics failedReader = [secret](analytics::PromptCacheAnalyticsReadOptions) -> rt::Task<analytics::PromptCacheAnalyticsView> { throw rt::JsError(secret); co_return readyView({}); };
  AWAIT(failed, uos::admin::index::handleAdminPromptCacheAnalytics(rt::Request("https://ai.ubq.fi/admin/prompt-cache-analytics?group_by=key_presence"), failedReader)); EXPECT_EQ(failed.status(), 503); EXPECT_EQ(failed.headers().get("cache-control"), std::optional<std::string>("no-store")); AWAIT(text, failed.text()); EXPECT(!rt::includes(text, secret));
}

TEST("admin bundle omits the standalone prompt-cache analytics section") {
  const std::string script = adminScript();
  EXPECT(!rt::Regex("Prompt-cache analytics").test(script));
  EXPECT(!rt::Regex("promptCacheAnalytics").test(script));
  EXPECT(!rt::Regex("\\/admin\\/prompt-cache-analytics").test(script));
}
