// Port of tests/codex-retry-after.test.ts.
// Links: tests/codex_retry_after_test.cpp src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
//        src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
//        src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/bounded_response_body.cpp
//        src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp
//        src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp
//        src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp
//        src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp
//        src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp
//        src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/http.cpp src/inference_deadline.cpp src/kv.cpp
//        src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp src/paid_fallback/ledger_state.cpp
//        src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp
//        src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
//        src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
//        src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp
//        src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/selection.cpp src/release.cpp src/runtime/http.cpp
//        src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/websocket.cpp
//        src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/value_compatibility.cpp src/types.cpp src/utils.cpp
#include "uos/codex/dispatch.hpp"
#include "uos/config.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>

namespace {
  namespace dispatch = uos::codex::dispatch;

  // Credential isolation (mandatory for binaries that link codex auth): nothing here may reach ~/.codex/auth.json.
  struct CredentialIsolation {
    uos::rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    CredentialIsolation() { uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialIsolation() { uos::config::config.codexAuthJsonB64 = savedSeed; }
  };
}

TEST("a weekly quota deadline is capped to a client-sized retry") {
  const CredentialIsolation isolation;
  const double now = 1'000'000;
  EXPECT_EQ(dispatch::codexRetryAfterSeconds(now + 7 * 24 * 60 * 60'000, now), dispatch::CODEX_CLIENT_RETRY_AFTER_MAX_SECONDS);
}

TEST("a short retry keeps its real remaining seconds") {
  const CredentialIsolation isolation;
  const double now = 1'000'000;
  EXPECT_EQ(dispatch::codexRetryAfterSeconds(now + 45'000, now), 45.0);
}

TEST("an elapsed or near deadline still asks for a positive retry") {
  const CredentialIsolation isolation;
  const double now = 1'000'000;
  EXPECT_EQ(dispatch::codexRetryAfterSeconds(now - 10'000, now), 1.0);
  EXPECT_EQ(dispatch::codexRetryAfterSeconds(now + 1, now), 1.0);
}
