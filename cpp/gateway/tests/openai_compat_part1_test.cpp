// Links: tests/openai_compat_part1_test.cpp
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
// src/health.cpp src/inference_admission.cpp src/provider/claude_code_access.cpp tests/helpers/admin_auth_harness.cpp src/kv_conversion/profile_json.cpp
// Port of the exact now-callable direct admin case in tests/openai-compat-part1.test.ts.
// Remaining source cases need Responses/Chat/handler assembly and belong to their final owners.
// This case touches KV get/set only: the existing admin auth KV fake is equivalent for this
// surface; openai_compat_harness remains owned by w09c4. No module/handler stand-in is used.
#include "tests/helpers/admin_auth_harness.hpp"
#include "uos/admin/index.hpp"
#include "uos/config.hpp"
#include "uos/debug_routing.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/test_async.hpp"
namespace {
  namespace rt = uos::rt;
  namespace harness = uos::tests::helpers::admin_auth_harness;
}
ASYNC_TEST("openai: the admin debug routing endpoint accepts the codex_503 scenario") {
  rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w08c2-openai"}}};
  const auto saved = uos::config::config.codexAuthJsonB64; uos::config::config.codexAuthJsonB64.clear(); harness::resetHarness();
  const auto cleanup = rt::scopeExit([saved] { harness::cleanupHarness(); uos::debug_routing::resetDebugRoutingCacheForTest(); uos::config::config.codexAuthJsonB64 = saved; });
  const rt::Request request("https://ai.ubq.fi/admin/debug/routing", rt::RequestInit{.method = "POST", .headers = rt::Headers{{"Content-Type", "application/json"}}, .body = rt::BodyInit(R"({"scenario":"codex_503","duration_ms":60000})")});
  const auto response = co_await uos::admin::index::handleAdminDebugRouting(request); EXPECT_EQ(response.status(), 200); const auto payload = co_await response.json(); const auto* routing = rt::getObject(payload, "routing"); ASSERT(routing); EXPECT_EQ(rt::getString(*routing, "scenario"), std::optional<std::string_view>("codex_503")); EXPECT(rt::getNumber(*routing, "expires_at_ms").has_value());
  harness::kvStore.delete_(harness::keyToString(uos::debug_routing::DEBUG_ROUTING_KEY));
}
