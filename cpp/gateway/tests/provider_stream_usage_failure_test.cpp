// Links: tests/provider_stream_usage_failure_test.cpp
// src/openai_telemetry.cpp src/responses_failover_stream.cpp src/provider/stream_relay.cpp
// src/deepseek/responses.cpp src/utils.cpp src/deepseek/index.cpp
// src/config.cpp src/provider/cerebras.cpp src/provider/lithos.cpp
// src/release.cpp src/chat/stream_translation.cpp src/http.cpp
// src/provider/removed_provider_telemetry.cpp src/deepseek/responses_stream.cpp src/auth/relay.cpp
// src/deepseek/responses_payload.cpp src/upstream_wire.cpp src/provider/lithos_rate_limits.cpp
// src/request_policy.cpp src/codex/responses_error.cpp src/models/codex_models_fetch.cpp
// src/responses_stream.cpp src/codex/dispatch.cpp src/bounded_response_body.cpp
// src/codex/auth.cpp src/codex/external_auth.cpp src/models/codex_models_availability.cpp
// src/provider/cerebras_rate_limits.cpp src/codex/auth_refresh.cpp src/models/codex_models.cpp
// src/kv.cpp src/runtime_config.cpp src/provider/capacity_events.cpp
// src/storage/kv.cpp src/defaults.cpp src/codex/native_auth.cpp
// src/codex/banked_reset.cpp src/storage/sqlite_readonly.cpp src/types.cpp
// src/provider/selection.cpp src/codex/capacity_routing.cpp src/codex/routing_state.cpp
// src/authority/native_codex_capacity.cpp src/provider/capacity_contract.cpp src/authority/native_codex_contracts.cpp
// src/provider/claude_code_models.cpp src/provider/surplus.cpp src/codex/routing_mutations.cpp
// src/authority/native_codex_mutations.cpp src/provider/metered.cpp src/provider/surplus_claude_request.cpp
// src/provider/claude_code_credentials.cpp src/provider/health.cpp src/codex/native_auth_transport.cpp
// src/provider/claude_code_accounts.cpp src/storage/native_kv.cpp src/codex/banked_reset_provider.cpp
// src/provider/surplus_claude_chat_request.cpp src/authority/native_codex_evaluation.cpp
// src/codex/reset_settings.cpp src/provider/capacity.cpp src/paid_fallback/ledger_stats.cpp
// src/provider/anthropic_replay.cpp src/provider/capacity_history.cpp src/cache/prompt_analytics_core.cpp
// src/provider/capacity_parse.cpp src/provider/anthropic_chat_responses.cpp src/provider/capacity_analytics.cpp
// src/provider/claude_code_quota.cpp src/provider/anthropic_response_wire.cpp src/provider/claude_capacity_history.cpp
// src/metered_quota.cpp src/provider/claude_code.cpp src/provider/claude_capacity_rollups.cpp
// src/provider/capacity_rollups.cpp src/cache/prompt_analytics_read.cpp src/provider/anthropic_responses.cpp
// src/codex/rate_limit_429.cpp src/runtime/websocket.cpp src/provider/anthropic_chat_stream.cpp
// src/codex/experiment.cpp src/provider/claude_code_usage_schema.cpp src/authority/native_codex_429.cpp
// src/codex/account_routing.cpp src/codex/routing_serial.cpp src/codex/routing_evaluation.cpp
// src/authority/native_codex.cpp src/codex/overage_settings.cpp
// Port of tests/provider-stream-usage-failure.test.ts.
#include "uos/provider/stream_relay.hpp"
#include "uos/config.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

namespace rt = uos::rt;
namespace json = uos::json;
namespace relay = uos::provider::stream_relay;
namespace telemetry = uos::openai_telemetry;

namespace {
  relay::ProviderStreamAdapter failingAdapter(std::optional<json::value> usage) {
    relay::ProviderStreamAdapter adapter;
    adapter.responseHeaders = [](std::optional<std::string>) { return rt::Headers(); };
    adapter.frames = [usage](rt::Response, std::string, relay::ProviderStreamFrameOptions) -> relay::ProviderStreamIterator {
      return rt::AsyncGenerator<relay::ProviderStreamFrame>([usage](rt::Yield<relay::ProviderStreamFrame> yield) -> rt::Task<void> {
        co_await rt::microtask();
        auto value = rt::jsonParse(R"({"choices":[{"index":0,"delta":{"role":"assistant","content":"partial answer"},"finish_reason":null}]})").as_object();
        if (usage) rt::objectDefine(value, "usage", *usage);
        relay::ProviderStreamFrame frame = uos::deepseek::stream::DeepSeekStreamFrameChunk{std::move(value)};
        co_await yield(std::move(frame));
        throw rt::JsError("upstream failed after reporting usage");
      });
    };
    adapter.recordResponseHealth = [](double, std::optional<std::string>) {};
    adapter.recordProviderError = [](std::optional<double>, std::optional<std::string>) {};
    adapter.recordCancellation = [](const telemetry::UsageContext*) {};
    adapter.recordIncompleteResponse = [](const telemetry::UsageContext*) {};
    adapter.recordFinishFailureKind = [](const telemetry::UsageContext*, std::optional<std::string>) {};
    adapter.recordTransportFailure = [](const telemetry::UsageContext*, rt::Reason, std::string) {};
    adapter.terminalTypeForError = [](rt::Reason, rt::AbortSignal) { return "error"; };
    adapter.streamErrorCode = "fixture_stream_failed";
    adapter.responsesProfile = uos::deepseek::responses::DEEPSEEK_RESPONSES_PROFILE;
    return adapter;
  }
}

ASYNC_TEST("Chat and translated Responses preserve observed cache usage after a stream failure") {
  rt::env::ScopedEnvironment env{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c2"}}};
  uos::config::config.codexAuthJsonB64.clear();
  const auto measuredUsage = rt::jsonParse(R"({"prompt_tokens":100,"completion_tokens":10,"total_tokens":110,"prompt_tokens_details":{"cached_tokens":40,"cache_write_tokens":20}})");
  for (int wire = 0; wire < 2; ++wire) {
    for (int measured = 0; measured < 2; ++measured) {
      const auto state = telemetry::createResponseTelemetryState();
      telemetry::UsageContext context{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt}; context.responseTelemetry = state;
      const auto adapter = failingAdapter(measured == 0 ? std::optional<json::value>(measuredUsage) : std::nullopt);
      const auto signal = rt::AbortController().signal();
      const rt::Response upstream(std::string("fixture"));
      const relay::RelayResponsesStreamOptions options{upstream, "deepseek-flash", "resp_failed_usage", 1'780'000'000,
        {json::array{}, "auto", true, std::nullopt}, {}, {}, std::nullopt, &context, signal, signal, "deepseek-flash"};
      const auto response = wire == 0 ? relay::relayChatCompletionStream(adapter, upstream, std::nullopt, &context, signal, signal, "deepseek-flash")
        : relay::relayResponsesStream(adapter, options);
      const auto text = co_await response.text(); EXPECT(rt::includes(text, "fixture_stream_failed"));
      EXPECT(!state->completed); EXPECT_EQ(state->streamTerminalType, std::optional<std::string>("error")); EXPECT_EQ(state->usageObserved, measured == 0);
      EXPECT_EQ(state->usageTelemetryStatus, measured == 0 ? "reported" : "missing");
      EXPECT_EQ(state->inputTokens, measured == 0 ? std::optional<double>(100) : std::nullopt);
      EXPECT_EQ(state->cachedInputTokens, measured == 0 ? std::optional<double>(40) : std::nullopt);
      EXPECT_EQ(state->cacheWriteInputTokens, measured == 0 ? std::optional<double>(20) : std::nullopt);
      EXPECT_EQ(state->outputTokens, measured == 0 ? std::optional<double>(10) : std::nullopt);
    }
  }
}
