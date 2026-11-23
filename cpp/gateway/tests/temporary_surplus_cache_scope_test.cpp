// Links: tests/temporary_surplus_cache_scope_test.cpp
// src/api_keys.cpp src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
// src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp
// src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp
// src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_read.cpp src/codex/account_routing.cpp src/codex/auth.cpp
// src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp
// src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp
// src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp
// src/codex/responses_error.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
// src/codex/routing_state.cpp src/config.cpp src/deepseek/flash_aliases.cpp src/deepseek/index.cpp
// src/defaults.cpp src/http.cpp src/kv.cpp src/metered_quota.cpp
// src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/models/openrouter_models.cpp
// src/openai_telemetry.cpp src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp
// src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp
// src/paid_fallback/ledger_stats.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp
// src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
// src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
// src/provider/capacity_rollups.cpp src/provider/cerebras.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp
// src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp src/provider/claude_code_access.cpp src/provider/claude_code_accounts.cpp
// src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
// src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp
// src/provider/metered.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/surplus.cpp
// src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/release.cpp src/request_policy.cpp
// src/responses_failover_stream.cpp src/responses_stream.cpp src/runtime/websocket.cpp src/runtime_config.cpp
// src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp
// src/types.cpp src/upstream_wire.cpp src/utils.cpp
// Original temporary-surplus-cache-scope case and bounded source health-oracle cases.
#include "uos/paid_fallback/health.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/anthropic_replay.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace health = uos::paid_fallback::health;
  namespace telemetry = uos::openai_telemetry;
  namespace replay = uos::provider::anthropic_replay;
  struct Fixture {
    rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c1-surplus"}, {"SURPLUS_API_KEY", "surplus-scoped-replay-test-key"}}};
    std::string savedConfig = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<rt::MemoryKv> store = std::make_shared<rt::MemoryKv>(rt::dateNow);
    Fixture() { uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(store); uos::provider::health::resetProviderHealthThrottleForTest(); }
    ~Fixture() { uos::kv::setKvForTest(nullptr); uos::config::config.codexAuthJsonB64 = savedConfig; }
  };
  std::string urlOf(const rt::RequestInfo& input) {
    return std::visit([](const auto& value) -> std::string {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::string>) return value;
      else if constexpr (std::is_same_v<T, rt::URL>) return value.href();
      else return value.url();
    }, input);
  }
}
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)

ASYNC_TEST("temporary Surplus routing forwards authenticated replay scope and records the selected native cache policy") {
  Fixture isolation;
  struct Scope { std::string scope; std::string keyId; std::optional<std::string> principal; bool explicit_; };
  const std::vector<Scope> scopes{{"temporary-principal-a", "temporary-key-a", "temporary-principal-a", false}, {"temporary-key-b", "temporary-key-b", std::nullopt, true}};
  const std::string model = "claude-opus-5.5";
  for (const auto& fixture : scopes) {
    const auto state = telemetry::createResponseTelemetryState(); state->promptCacheMode = "legacy_retention"; state->explicitBreakpointCount = 9;
    telemetry::UsageContext context; context.keyId = fixture.keyId; context.idempotencyPrincipal = rt::orUndefined(fixture.principal);
    context.requestId = fixture.keyId + "-request"; context.responseTelemetry = state;
    auto body = rt::jsonParse(R"({"model":"claude-opus-5.5","max_output_tokens":128,"stream":false,"tools":[{"type":"function","name":"lookup","parameters":{"type":"object","properties":{"value":{"type":"string"}}}}],"input":"use the tool"})").as_object();
    if (fixture.explicit_) body["input"] = rt::jsonParse(R"([{"type":"message","role":"user","content":[{"type":"input_text","text":"use the tool","prompt_cache_breakpoint":{"mode":"explicit"}}]}])");
    int calls = 0;
    const rt::Fetch mock([&](rt::RequestInfo input, rt::RequestInit init) {
      ++calls; EXPECT_EQ(urlOf(input), "https://api.surplusintelligence.ai/v1/chat/completions");
      const auto* encoded = init.body ? std::get_if<std::string>(&*init.body) : nullptr; EXPECT(encoded); EXPECT(encoded->find(fixture.scope) == std::string::npos);
      auto payload = rt::jsonParse(R"({"id":"chatcmpl_fixture","object":"chat.completion","created":1,"model":"claude-opus-5.5","choices":[{"index":0,"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"reasoning_details":[{"type":"reasoning.text","text":"Call the requested tool.","signature":"private-signature-fixture","index":0}],"tool_calls":[{"id":"tool_fixture","type":"function","function":{"name":"lookup","arguments":"{\"value\":\"fixture\"}"}}]}}],"usage":{"prompt_tokens":1043,"completion_tokens":4,"total_tokens":1047,"prompt_tokens_details":{"cache_write_tokens":1028},"buyer_cost_micro":1056}})").as_object();
      payload["id"] = "chatcmpl_" + fixture.keyId;
      return rt::Response::json(payload);
    });
    const rt::GlobalFetchOverride overrideFetch(mock);
    health::TemporaryFreeSurplusRoutedResponsesOptions options{model, &context};
    AWAIT(routed, health::fetchTemporaryFreeSurplusRoutedResponses(body, options)); EXPECT_EQ(calls, 1); EXPECT_EQ(routed.provider, "surplus"); EXPECT(!routed.paidFallback);
    AWAIT(payload, routed.response.json()); EXPECT(rt::jsonStringify(payload).find("private-signature") == std::string::npos);
    EXPECT_EQ(state->promptCacheMode, fixture.explicit_ ? "explicit" : "implicit"); EXPECT_EQ(state->explicitBreakpointCount, fixture.explicit_ ? 1 : 0);
    const auto* output = rt::getArray(payload, "output"); EXPECT(output);
    const replay::AnthropicReplayContext correct{fixture.scope, "surplus", model}, wrong{fixture.scope + "-other", "surplus", model};
    EXPECT(replay::restoreAnthropicChatReplay(*output, correct)); EXPECT(!replay::restoreAnthropicChatReplay(*output, wrong));
    if (fixture.principal) { const replay::AnthropicReplayContext key{fixture.keyId, "surplus", model}; EXPECT(!replay::restoreAnthropicChatReplay(*output, key)); }
  }
}
TEST("paid health: authoritative capacity classification preserves provider-specific status semantics") {
  const std::vector<std::pair<std::optional<double>, std::string>> metered{{std::nullopt,"upstream_error"},{0,""},{99,""},{100,"reachable"},{200,"reachable"},{400,"reachable"},{401,"auth_invalid"},{402,"quota_exhausted"},{403,"auth_invalid"},{429,"quota_exhausted"},{500,"upstream_error"},{503,"upstream_error"}};
  for (const auto& [status, event] : metered) {
    const auto actual = health::paidProviderHealthClassification("metered", status, false);
    EXPECT_EQ(actual.has_value(), !event.empty()); if (actual) { EXPECT_EQ(actual->event, event); EXPECT_EQ(actual->status, status); }
  }
  EXPECT_EQ(health::paidProviderHealthClassification("metered", 403, true)->event, "quota_exhausted");
  EXPECT_EQ(health::paidProviderHealthClassification("surplus", 403, true)->event, "auth_invalid");
  EXPECT_EQ(health::paidProviderHealthClassification("surplus", 402, false)->event, "upstream_error");
  EXPECT_EQ(health::paidProviderHealthClassification("surplus", 402, false, true)->event, "quota_exhausted");
  const std::vector<std::optional<double>> refused{std::nullopt, 400, 401, 403, 500, 503};
  for (const auto status : refused) EXPECT(!health::isIntermediatePaidProviderAttempt(0, 2, status));
  EXPECT(health::isIntermediatePaidProviderAttempt(0, 2, 402)); EXPECT(health::isIntermediatePaidProviderAttempt(0, 2, 429));
  EXPECT(!health::isIntermediatePaidProviderAttempt(1, 2, 429));
}
ASYNC_TEST("paid health: bounded body classification preserves delivered bytes and rejects incomplete evidence") {
  Fixture fixture;
  const std::vector<std::pair<std::string, bool>> metered{{R"({"error":{"code":" LOCAL:INSUFFICIENT_QUOTA "}})",true},{R"({"error":{"message":"The Quota Is Not Enough"}})",true},{R"({"code":"local:insufficient_quota"})",false},{"broken",false},{"[]",false},{"null",false}};
  for (const auto& [text, exhausted] : metered) {
    rt::Response response(text, {.status = 403});
    AWAIT(actual, health::meteredForbiddenIndicatesQuotaExhaustion(response, std::nullopt)); EXPECT_EQ(actual, exhausted);
    AWAIT(delivered, response.text()); EXPECT_EQ(delivered, text);
  }
  const std::vector<std::pair<std::string, bool>> surplus{{R"({"error":{"code":" INSUFFICIENT_CREDIT "}})",true},{R"({"type":"insufficient_credit"})",true},{R"({"error":{"type":"capacity"}})",false},{"broken",false}};
  for (const auto& [text, exhausted] : surplus) {
    rt::Response response(text, {.status = 402});
    AWAIT(actual, health::surplus402IndicatesQuotaExhaustion(response, std::nullopt)); EXPECT_EQ(actual, exhausted);
    AWAIT(delivered, response.text()); EXPECT_EQ(delivered, text);
  }
  rt::Response wrongStatus(R"({"error":{"code":"insufficient_credit"}})", {.status = 429});
  AWAIT(wrong, health::surplus402IndicatesQuotaExhaustion(wrongStatus, std::nullopt)); EXPECT(!wrong);
  const auto tooLarge = std::string(65537, ' '); rt::Response oversized(tooLarge, {.status = 402});
  AWAIT(incomplete, health::surplus402IndicatesQuotaExhaustion(oversized, std::nullopt)); EXPECT(!incomplete);
}
TEST("paid health: authenticated cache scope and abort reasons retain source precedence") {
  EXPECT(!health::codexCacheScopeForUsageContext(nullptr)); telemetry::UsageContext context; context.keyId = "key";
  EXPECT_EQ(health::codexCacheScopeForUsageContext(&context), std::optional<std::string>("key")); context.idempotencyPrincipal = "principal";
  EXPECT_EQ(health::codexCacheScopeForUsageContext(&context), std::optional<std::string>("principal")); context.idempotencyPrincipal = "";
  EXPECT_EQ(health::codexCacheScopeForUsageContext(&context), std::optional<std::string>("key"));
  const auto reason = rt::makeReason(rt::JsError("cancelled")); const auto signal = rt::AbortSignal::abort(reason); EXPECT(health::paidFallbackAbortReason(signal) == reason);
  EXPECT_EQ(rt::errorName(health::paidFallbackAbortReason(std::nullopt)), "AbortError");
  const auto nonError = rt::AbortSignal::abort(rt::thrown("cancelled")); EXPECT_EQ(rt::errorName(health::paidFallbackAbortReason(nonError)), "AbortError");
  EXPECT_EQ(health::forcedDebugCodexStatus("metered_first"), std::optional<double>(429)); EXPECT_EQ(health::forcedDebugCodexStatus("codex_403"), std::optional<double>(403)); EXPECT_EQ(health::forcedDebugCodexStatus("codex_401"), std::optional<double>(401)); EXPECT_EQ(health::forcedDebugCodexStatus("codex_503"), std::optional<double>(503)); EXPECT(!health::forcedDebugCodexStatus("other"));
}
