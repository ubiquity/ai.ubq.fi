// Links: tests/lithos_stream_health_test.cpp
// src/provider/health.cpp src/config.cpp src/provider/lithos_streams.cpp
// src/provider/claude_code_accounts.cpp src/provider/stream_relay.cpp src/openai_telemetry.cpp
// src/provider/lithos.cpp src/provider/claude_code_credentials.cpp src/release.cpp
// src/utils.cpp src/provider/lithos_rate_limits.cpp src/kv.cpp
// src/deepseek/responses.cpp src/chat/stream_translation.cpp src/upstream_wire.cpp
// src/deepseek/index.cpp src/responses_stream.cpp src/provider/cerebras_rate_limits.cpp
// src/provider/cerebras.cpp src/responses_failover_stream.cpp src/provider/removed_provider_telemetry.cpp
// src/deepseek/responses_stream.cpp src/http.cpp src/deepseek/responses_payload.cpp
// src/storage/kv.cpp src/request_policy.cpp src/bounded_response_body.cpp
// src/provider/selection.cpp src/storage/sqlite_readonly.cpp src/defaults.cpp
// src/provider/surplus.cpp src/codex/responses_error.cpp
// src/provider/surplus_claude_request.cpp src/provider/surplus_claude_chat_request.cpp src/models/codex_models_fetch.cpp
// src/provider/claude_code_models.cpp src/auth/relay.cpp src/provider/metered.cpp
// src/models/codex_models.cpp src/codex/dispatch.cpp src/runtime_config.cpp
// src/provider/anthropic_replay.cpp src/provider/anthropic_chat_responses.cpp src/models/codex_models_availability.cpp
// src/codex/auth.cpp src/codex/auth_refresh.cpp src/storage/native_kv.cpp
// src/provider/capacity_events.cpp src/codex/routing_mutations.cpp src/codex/capacity_routing.cpp
// src/types.cpp src/codex/native_auth.cpp src/codex/external_auth.cpp
// src/provider/anthropic_responses.cpp src/provider/anthropic_response_wire.cpp src/authority/native_codex_capacity.cpp
// src/provider/claude_code.cpp src/authority/native_codex_mutations.cpp src/authority/native_codex_evaluation.cpp
// src/authority/native_codex_contracts.cpp src/codex/routing_state.cpp src/codex/rate_limit_429.cpp
// src/provider/capacity_contract.cpp src/provider/claude_code_quota.cpp src/authority/native_codex_429.cpp
// src/codex/banked_reset.cpp src/provider/claude_code_usage_schema.cpp src/provider/anthropic_chat_stream.cpp
// src/codex/banked_reset_provider.cpp src/codex/native_auth_transport.cpp src/provider/capacity.cpp
// src/paid_fallback/ledger_stats.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
// src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics_core.cpp src/provider/capacity_rollups.cpp
// src/provider/claude_capacity_rollups.cpp src/codex/reset_settings.cpp src/runtime/websocket.cpp
// src/provider/claude_capacity_history.cpp src/provider/capacity_analytics.cpp src/metered_quota.cpp
// src/codex/experiment.cpp src/codex/account_routing.cpp src/codex/routing_evaluation.cpp
// src/codex/overage_settings.cpp src/codex/routing_serial.cpp src/authority/native_codex.cpp
// Port of tests/lithos-stream-health.test.ts.
#include "uos/provider/lithos_streams.hpp"
#include "uos/provider/health.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

namespace rt = uos::rt;
namespace json = uos::json;
namespace streams = uos::provider::lithos_streams;
namespace health = uos::provider::health;

ASYNC_TEST("in-band Lithos 429 and 401 refusals retain their classified stream health") {
  rt::env::ScopedEnvironment env{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c2"}}};
  uos::config::config.codexAuthJsonB64.clear();
  const auto kv = std::make_shared<rt::MemoryKv>(rt::dateNow); uos::kv::setKvForTest(kv); health::resetProviderHealthThrottleForTest();
  const auto cleanup = rt::scopeExit([] { uos::kv::setKvForTest(nullptr); health::resetProviderHealthThrottleForTest(); });
  const auto signal = rt::AbortController().signal();
  const std::string model = "deepseek-ai/DeepSeek-V4.1-Flash";
  const std::vector<int> statuses{429, 401};
  for (int status : statuses) {
    for (int wire = 0; wire < 2; ++wire) {
      const json::object refusal{{"error", json::object{{"message", "fixture refusal"}, {"code", status == 429 ? "rate_limit_exceeded" : "invalid_api_key"}}}};
      const rt::ResponseInit init{.status = status}; const auto upstream = rt::Response::json(refusal, init);
      const uos::deepseek::responses_payload::DeepSeekResponsesEcho echo{json::array{}, "auto", true, std::nullopt};
      const auto response = wire == 0 ? streams::streamLithosChatCompletion(upstream, std::nullopt, nullptr, signal, signal, model, model)
        : streams::streamLithosResponses(upstream, model, "resp_" + std::to_string(status), 1'800'000'000, echo, {}, {}, std::nullopt, nullptr, signal, signal, model, model);
      EXPECT_EQ(response.status(), 200); const auto text = co_await response.text(); EXPECT(rt::includes(text, "fixture refusal"));
      co_await rt::sleep(1);
      const auto observed = co_await health::getLithosProviderHealth();
      EXPECT_EQ(observed.state, status == 429 ? "exhausted" : "invalid");
      EXPECT_EQ(observed.last_event, std::optional<std::string>(status == 429 ? "quota_exhausted" : "auth_invalid"));
      EXPECT_EQ(observed.last_status, std::optional<double>(status));
    }
  }
}
