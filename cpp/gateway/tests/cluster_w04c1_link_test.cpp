// Links: tests/cluster_w04c1_link_test.cpp
//        src/analytics.cpp src/api_key_policy.cpp src/api_keys.cpp src/auth/local_development_key.cpp
//        src/auth/relay.cpp src/authority/native_api_key.cpp src/authority/native_api_key_policy.cpp src/authority/native_codex.cpp
//        src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
//        src/authority/native_codex_mutations.cpp src/authority/native_contracts.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp
//        src/authority/native_paid_policy.cpp src/bounded_response_body.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp
//        src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp
//        src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_claim.cpp
//        src/codex/banked_reset_pool.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset_submission.cpp src/codex/capacity_routing.cpp
//        src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/index.cpp
//        src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp
//        src/codex/reset_settings.cpp src/codex/responses_error.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp
//        src/codex/responses_state.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
//        src/codex/routing_state.cpp src/config.cpp src/deepseek/index.cpp src/deepseek/responses.cpp
//        src/defaults.cpp src/http.cpp src/inference_deadline.cpp src/kv.cpp
//        src/metered_quota.cpp src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp
//        src/openai_telemetry.cpp src/optional_telemetry_queue.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp
//        src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp
//        src/paid_fallback/rollups.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp
//        src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
//        src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
//        src/provider/capacity_rollups.cpp src/provider/cerebras.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp
//        src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
//        src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage.cpp src/provider/claude_code_usage_schema.cpp
//        src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_rate_limits.cpp src/provider/metered.cpp
//        src/provider/presentation.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/surplus.cpp
//        src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/release.cpp src/request_policy.cpp
//        src/responses_failover_stream.cpp src/responses_stream.cpp src/runtime/http.cpp src/runtime/http_body.cpp
//        src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/websocket.cpp
//        src/runtime_config.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
//        src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp
//        src/utils.cpp
// All 58 cyclic cluster implementations and their already-ported dependency closure.

#include "uos/provider/capacity.hpp"
#include "uos/provider/capacity_analytics.hpp"
#include "uos/provider/claude_capacity_history.hpp"
#include "uos/provider/claude_capacity_rollups.hpp"
#include "uos/provider/capacity_contract.hpp"
#include "uos/provider/claude_code_usage.hpp"
#include "uos/codex/responses_operations.hpp"
#include "uos/codex/responses_runners.hpp"
#include "uos/metered_quota.hpp"
#include "uos/provider/claude_code.hpp"
#include "uos/provider/surplus_claude_chat_request.hpp"
#include "uos/codex/index.hpp"
#include "uos/codex/responses_state.hpp"
#include "uos/deepseek/responses.hpp"
#include "uos/provider/lithos_rate_limits.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/provider/surplus_claude_request.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/presentation.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/health.hpp"
#include "uos/authority/native_api_key_policy.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/auth/local_development_key.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/banked_reset_pool.hpp"
#include "uos/codex/banked_reset_submission.hpp"
#include "uos/codex/native_auth.hpp"
#include "uos/codex/routing_serial.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/codex/banked_reset.hpp"
#include "uos/codex/banked_reset_claim.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/experiment.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/codex/rate_limit_429.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/codex/routing_evaluation.hpp"
#include "uos/codex/routing_mutations.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/paid_fallback/ledger_admission.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/paid_fallback/ledger_settlement.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/capacity_history.hpp"
#include "uos/provider/capacity_parse.hpp"
#include "uos/provider/claude_code_fwd.hpp"
#include "uos/provider/claude_code_usage_fwd.hpp"
#include "uos/provider/selection.hpp"
#include "uos/request_policy.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <array>
#include <utility>

TEST("w04c1 all cluster implementations link and initialise") {
  EXPECT(uos::provider::presentation::PROVIDER_PRESENTATION.size() == 10);
  EXPECT(uos::provider::capacity_contract::SHA256_HEX.test(std::string(64, 'f')));
}

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace responses = uos::deepseek::responses;
  namespace quota = uos::provider::claude_code_quota;

  const json::value& moduleOracle() {
    // Bun 1.4.0 imports, recorded 2026-11-16 with empty HOME/TMPDIR, --no-env-file --no-install,
    // a rejecting KV opener and mocked fetch. Input hashes and full output: progress-C0-evidence.
    static const json::value value = rt::jsonParse(R"oracle({
  "profiles": [
    {
      "id": "deepseek",
      "label": "DeepSeek",
      "requiresStreamUsageOption": true,
      "upstreamModels": [
        [
          "deepseek-flash",
          "deepseek-flash"
        ],
        [
          "deepseek-v4-flash",
          "deepseek-flash"
        ],
        [
          "deepseek-v4-pro",
          "deepseek-v4-pro"
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash",
          null
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash-fast",
          null
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash-ultra",
          null
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat",
          null
        ],
        [
          "moonshotai/Kimi-K3",
          null
        ],
        [
          "moonshotai/Kimi-K3-fast",
          null
        ],
        [
          "moonshotai/Kimi-K3-ultra",
          null
        ],
        [
          "moonshotai/Kimi-K3-ultra-chat",
          null
        ],
        [
          "gpt-oss-120b",
          null
        ],
        [
          "qwen-3.8-27b",
          null
        ],
        [
          "unknown",
          null
        ]
      ],
      "reasoning": [
        [
          "",
          ""
        ],
        [
          "none",
          "none"
        ],
        [
          "minimal",
          "minimal"
        ],
        [
          "low",
          "low"
        ],
        [
          "medium",
          "medium"
        ],
        [
          "high",
          "high"
        ],
        [
          "xhigh",
          "xhigh"
        ],
        [
          "max",
          "max"
        ],
        [
          "ultra",
          "max"
        ],
        [
          " HIGH ",
          " HIGH "
        ],
        [
          "NONE",
          "NONE"
        ]
      ]
    },
    {
      "id": "lithos",
      "label": "LithosAI",
      "requiresStreamUsageOption": false,
      "upstreamModels": [
        [
          "deepseek-flash",
          null
        ],
        [
          "deepseek-v4-flash",
          null
        ],
        [
          "deepseek-v4-pro",
          null
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash",
          "deepseek-ai/DeepSeek-V4.1-Flash"
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash-fast",
          "deepseek-ai/DeepSeek-V4.1-Flash-fast"
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash-ultra",
          "deepseek-ai/DeepSeek-V4.1-Flash-ultra"
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat",
          "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat"
        ],
        [
          "moonshotai/Kimi-K3",
          "moonshotai/Kimi-K3"
        ],
        [
          "moonshotai/Kimi-K3-fast",
          "moonshotai/Kimi-K3-fast"
        ],
        [
          "moonshotai/Kimi-K3-ultra",
          "moonshotai/Kimi-K3-ultra"
        ],
        [
          "moonshotai/Kimi-K3-ultra-chat",
          "moonshotai/Kimi-K3-ultra-chat"
        ],
        [
          "gpt-oss-120b",
          null
        ],
        [
          "qwen-3.8-27b",
          null
        ],
        [
          "unknown",
          null
        ]
      ],
      "reasoning": [
        [
          "",
          null
        ],
        [
          "none",
          "none"
        ],
        [
          "minimal",
          "minimal"
        ],
        [
          "low",
          "low"
        ],
        [
          "medium",
          "medium"
        ],
        [
          "high",
          "high"
        ],
        [
          "xhigh",
          "xhigh"
        ],
        [
          "max",
          "max"
        ],
        [
          "ultra",
          null
        ],
        [
          " HIGH ",
          "high"
        ],
        [
          "NONE",
          "none"
        ]
      ]
    },
    {
      "id": "cerebras",
      "label": "Cerebras",
      "requiresStreamUsageOption": false,
      "upstreamModels": [
        [
          "deepseek-flash",
          null
        ],
        [
          "deepseek-v4-flash",
          null
        ],
        [
          "deepseek-v4-pro",
          null
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash",
          null
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash-fast",
          null
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash-ultra",
          null
        ],
        [
          "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat",
          null
        ],
        [
          "moonshotai/Kimi-K3",
          null
        ],
        [
          "moonshotai/Kimi-K3-fast",
          null
        ],
        [
          "moonshotai/Kimi-K3-ultra",
          null
        ],
        [
          "moonshotai/Kimi-K3-ultra-chat",
          null
        ],
        [
          "gpt-oss-120b",
          "gpt-oss-120b"
        ],
        [
          "qwen-3.8-27b",
          "qwen-3.8-27b"
        ],
        [
          "unknown",
          null
        ]
      ],
      "reasoning": [
        [
          "",
          null
        ],
        [
          "none",
          "none"
        ],
        [
          "minimal",
          null
        ],
        [
          "low",
          "low"
        ],
        [
          "medium",
          "medium"
        ],
        [
          "high",
          "high"
        ],
        [
          "xhigh",
          null
        ],
        [
          "max",
          null
        ],
        [
          "ultra",
          null
        ],
        [
          " HIGH ",
          "high"
        ],
        [
          "NONE",
          "none"
        ]
      ]
    }
  ],
  "lithosLevels": [
    "none",
    "minimal",
    "low",
    "medium",
    "high",
    "xhigh",
    "max"
  ],
  "cerebrasLevels": [
    "low",
    "medium",
    "high",
    "none"
  ],
  "lithosHint": {
    "context_window_tokens": 1048576,
    "max_context_window_tokens": 1048576,
    "effective_context_window_percent": 100,
    "supported_reasoning_levels": [
      "none",
      "minimal",
      "low",
      "medium",
      "high",
      "xhigh",
      "max"
    ],
    "default_reasoning_effort": "medium"
  },
  "cerebrasHints": [
    [
      "gpt-oss-120b",
      {
        "context_window_tokens": 131072,
        "max_context_window_tokens": 131072,
        "supported_reasoning_levels": [
          "low",
          "medium",
          "high"
        ],
        "default_reasoning_effort": "medium"
      }
    ],
    [
      "qwen-3.8-27b",
      {
        "context_window_tokens": 131072,
        "max_context_window_tokens": 131072,
        "supported_reasoning_levels": [
          "none",
          "low",
          "medium",
          "high"
        ],
        "default_reasoning_effort": "high"
      }
    ]
  ],
  "meteredUrls": [
    "https://api.openlux.ai/api/usage/token/"
  ],
  "claudeUsageUrl": "https://api.anthropic.com/api/oauth/usage"
})oracle");
    return value;
  }

  json::object hintJson(const uos::models::metadata::ModelMetadataHint& hint) {
    json::object out;
    rt::objectSet(out, "context_window_tokens", hint.context_window_tokens);
    rt::objectSet(out, "max_context_window_tokens", hint.max_context_window_tokens);
    rt::objectSet(out, "auto_compact_token_limit_tokens", hint.auto_compact_token_limit_tokens);
    rt::objectSet(out, "effective_context_window_percent", hint.effective_context_window_percent);
    rt::objectSet(out, "supported_reasoning_levels", hint.supported_reasoning_levels);
    rt::objectSet(out, "default_reasoning_effort", hint.default_reasoning_effort);
    return out;
  }

  rt::Task<void> unrelatedRecorder(std::shared_ptr<rt::Kv>, quota::ClaudeCodeAcceptedObservation) { co_return; }
}

TEST("module-load Responses profiles and derived reasoning sets match isolated Bun imports") {
  const std::array<const responses::ChatOnlyResponsesProfile*, 3> profiles{
    &responses::DEEPSEEK_RESPONSES_PROFILE, &responses::LITHOS_RESPONSES_PROFILE, &responses::CEREBRAS_RESPONSES_PROFILE};
  const json::array& expected = *rt::getArray(moduleOracle(), "profiles");
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    const auto& profile = *profiles[index];
    const json::value& wanted = expected[index];
    EXPECT_EQ(std::string(profile.id), *rt::getString(wanted, "id"));
    EXPECT_EQ(std::string(profile.label), *rt::getString(wanted, "label"));
    EXPECT_EQ(profile.requiresStreamUsageOption, *rt::getBool(wanted, "requiresStreamUsageOption"));
    for (const json::value& pair : *rt::getArray(wanted, "upstreamModels")) {
      const std::string model(pair.at(0).get_string());
      const auto actual = profile.upstreamModelFor(model);
      EXPECT(rt::deepEqual(actual ? json::value(*actual) : json::value(nullptr), pair.at(1)));
    }
    for (const json::value& pair : *rt::getArray(wanted, "reasoning")) {
      const std::string effort(pair.at(0).get_string());
      const rt::Nullable<json::value> actual = profile.projectReasoningEffort(effort);
      EXPECT(rt::deepEqual(actual.toJson().value_or(nullptr), pair.at(1)));
    }
  }
  json::array lithos;
  for (const std::string_view level : uos::provider::lithos::LITHOS_REASONING_LEVELS) lithos.emplace_back(json::string(level));
  EXPECT(rt::deepEqual(lithos, *rt::get(moduleOracle(), "lithosLevels")));

  json::array cerebras;
  for (const std::string_view model : uos::provider::cerebras::CEREBRAS_MODELS) {
    const auto hint = uos::request_policy::cerebrasProviderHint(std::string(model));
    ASSERT(hint.supported_reasoning_levels.hasValue());
    for (const json::value& level : *hint.supported_reasoning_levels) {
      if (!level.is_string()) continue;
      const json::value normalized(rt::toLowerCase(rt::trim(level.get_string())));
      if (!rt::includes(cerebras, normalized)) cerebras.push_back(normalized);
    }
  }
  EXPECT(rt::deepEqual(cerebras, *rt::get(moduleOracle(), "cerebrasLevels")));
}

TEST("module-load request policy provider hints match isolated Bun imports") {
  EXPECT(rt::deepEqual(hintJson(uos::request_policy::LITHOS_PROVIDER_HINT()), *rt::get(moduleOracle(), "lithosHint")));
  for (const json::value& pair : *rt::getArray(moduleOracle(), "cerebrasHints")) {
    const std::string model(pair.at(0).get_string());
    EXPECT(rt::deepEqual(hintJson(uos::request_policy::cerebrasProviderHint(model)), pair.at(1)));
  }
}

ASYNC_TEST("module-load metered quota and Claude usage URLs match isolated Bun imports") {
  json::array urls;
  uos::metered_quota::FetchMeteredQuotaObservationOptions options;
  options.now = [] { return 1'800'000'000'000.0; };
  options.fetcher = rt::Fetch([&urls](rt::RequestInfo input, rt::RequestInit) -> rt::Response {
    const auto* url = std::get_if<std::string>(&input);
    if (!url) throw rt::JsError("expected a string quota URL");
    urls.emplace_back(*url);
    return rt::Response::json(rt::jsonParse(R"({"success":true,"data":{"unlimited_quota":false,"total_available":10,"total_granted":20,"total_used":10}})"));
  });
  const uos::metered_quota::MeteredAccountCredentials credentials{.apiKey = "c0-fixture"};
  auto fetching = uos::metered_quota::fetchMeteredQuotaObservation(credentials, options);
  const auto observation = co_await std::move(fetching);
  EXPECT_EQ(observation.total_available, std::optional<double>(10));
  EXPECT(rt::deepEqual(urls, *rt::get(moduleOracle(), "meteredUrls")));
  EXPECT_EQ(std::string(uos::provider::claude_code_usage::CLAUDE_CODE_USAGE_URL), *rt::getString(moduleOracle(), "claudeUsageUrl"));
}

TEST("linking capacity installs its Claude observation recorder before main") {
  EXPECT_THROWS_MESSAGE(quota::registerClaudeCodeObservationRecorder(&unrelatedRecorder), rt::JsError, "Claude observation recorder is already installed");
  auto disposal = quota::registerClaudeCodeObservationRecorder(&uos::provider::claude_capacity_history::recordClaudeCapacityObservation);
  EXPECT(static_cast<bool>(disposal));
}
