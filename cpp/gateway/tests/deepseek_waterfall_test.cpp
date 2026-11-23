// Links: tests/deepseek_waterfall_test.cpp
// src/recent_model_context.cpp
// src/api_keys.cpp src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
// src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp
// src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp
// src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_read.cpp src/cache/scope_targets.cpp src/catalog/models.cpp
// src/catalog/types.cpp src/codex/account_routing.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp
// src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp
// src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
// src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp
// src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp
// src/config.cpp src/deepseek/chat_projection.cpp src/deepseek/flash_aliases.cpp src/deepseek/forwarded_payload_policy.cpp
// src/deepseek/index.cpp src/deepseek/responses.cpp src/deepseek/responses_payload.cpp src/deepseek/responses_stream.cpp
// src/deepseek/waterfall.cpp src/defaults.cpp src/http.cpp src/input_normalization.cpp
// src/kv.cpp src/lib/jev_compaction/state.cpp src/lib/jev_compaction/types.cpp src/metered_quota.cpp
// src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/models/metadata.cpp
// src/models/openrouter_models.cpp src/openai_telemetry.cpp src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp
// src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp
// src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp
// src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp
// src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp
// src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp src/provider/cerebras.cpp src/provider/cerebras_rate_limits.cpp
// src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp src/provider/claude_code_access.cpp
// src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp
// src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_models.cpp
// src/provider/lithos_rate_limits.cpp src/provider/metered.cpp src/provider/openrouter.cpp src/provider/openrouter_request_controls.cpp
// src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/surplus_claude_request.cpp src/release.cpp src/request_policy.cpp src/responses_failover_stream.cpp
// src/responses_stream.cpp src/runtime/websocket.cpp src/runtime_config.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/types.cpp
// src/upstream_wire.cpp src/utils.cpp
// Deferred original handler and paid-pipeline cases require their final modules.
// Owned API parity from the original Bun tests. Upstream/handler assertions remain separate.
#include "uos/deepseek/waterfall.hpp"
#include "uos/catalog/models.hpp"
#include "uos/paid_fallback/health.hpp"
#include "uos/provider/lithos_rate_limits.hpp"
#include "helpers/deepseek_port_replay.hpp"
namespace {
#include "deepseek_waterfall_vectors.inc"
  const bool recorded = uos::tests::helpers::deepseek_port_replay::registerReplay(recordedVectors);
}
namespace { namespace waterfall = uos::deepseek::waterfall; }
TEST("waterfall: the synthetic id is versionless and uses provider-owned latest aliases") {
  EXPECT(waterfall::isDeepSeekWaterfallModel("  Ubiquity/DeepSeek-Flash ")); EXPECT(waterfall::isDeepSeekWaterfallModel("  Ubiquity/DeepSeek-V4.1-Flash ")); EXPECT(!waterfall::isDeepSeekWaterfallModel("deepseek-flash"));
  EXPECT_EQ(waterfall::DEEPSEEK_WATERFALL_MODEL_ID, "ubiquity/deepseek-flash"); EXPECT_EQ(waterfall::DEEPSEEK_WATERFALL_LEGACY_MODEL_ID, "ubiquity/deepseek-v4.1-flash");
  const auto& aliases = uos::deepseek::flash_aliases::DEEPSEEK_FLASH_FALLBACK_ALIASES.models;
  EXPECT_EQ(*waterfall::DEEPSEEK_WATERFALL_PROVIDER_MODEL.get("deepseek"), aliases.deepseek); EXPECT_EQ(*waterfall::DEEPSEEK_WATERFALL_PROVIDER_MODEL.get("lithos"), aliases.lithos); EXPECT_EQ(*waterfall::DEEPSEEK_WATERFALL_PROVIDER_MODEL.get("openrouter"), aliases.openrouter); EXPECT_EQ(*waterfall::DEEPSEEK_WATERFALL_PROVIDER_MODEL.get("surplus"), aliases.surplus); EXPECT_EQ(*waterfall::DEEPSEEK_WATERFALL_PROVIDER_MODEL.get("openlux"), aliases.openlux);
  const std::vector<std::string_view> order(waterfall::DEEPSEEK_WATERFALL_ORDER.begin(), waterfall::DEEPSEEK_WATERFALL_ORDER.end()); EXPECT_EQ(order, (std::vector<std::string_view>{"lithos", "deepseek", "openrouter", "surplus", "openlux"}));
  EXPECT_EQ(waterfall::DEEPSEEK_WATERFALL_CONTEXT_WINDOW_TOKENS, 800'000);
}
TEST("waterfall: the plan keeps order and drops switched-off or uncredentialed hops") {
  const waterfall::DeepSeekWaterfallPlanInput all{[](const std::string&) { return true; }, [](const std::string&) { return true; }};
  EXPECT_EQ(waterfall::deepSeekWaterfallPlan(all), (std::vector<std::string>{"lithos", "deepseek", "openrouter", "surplus", "openlux"}));
  const waterfall::DeepSeekWaterfallPlanInput narrowed{[](const std::string& provider) { return provider != "lithos"; }, [](const std::string& provider) { return provider != "deepseek"; }};
  EXPECT_EQ(waterfall::deepSeekWaterfallPlan(narrowed), (std::vector<std::string>{"openrouter", "surplus", "openlux"}));
  int configuredCalls = 0; const waterfall::DeepSeekWaterfallPlanInput none{[](const std::string&) { return false; }, [&](const std::string&) { ++configuredCalls; return true; }}; EXPECT(waterfall::deepSeekWaterfallPlan(none).empty()); EXPECT_EQ(configuredCalls, 0);
}
TEST("waterfall: aliases resolve the newest provider base without dropping any provider") {
  namespace aliases = uos::deepseek::flash_aliases;
  aliases::DeepSeekFlashCatalogInput input;
  input.lithos = std::vector<aliases::CatalogModel>{{"deepseek-ai/DeepSeek-V4.1-Flash-0910"}, {"deepseek-ai/DeepSeek-V4.1-Flash"}};
  input.openrouter = std::vector<aliases::CatalogModel>{{"deepseek/deepseek-v4.1-flash"}, {"~deepseek/deepseek-flash-latest"}};
  input.surplus = std::vector<aliases::CatalogModel>{{"deepseek-v4.1-flash"}, {"deepseek-v4.1-flash-1017"}};
  input.openlux = std::vector<aliases::CatalogModel>{{"deepseek-v4.1-flash"}};
  const auto resolved = aliases::resolveDeepSeekFlashAliases(input);
  EXPECT_EQ(resolved.models.lithos, "deepseek-ai/DeepSeek-V4.1-Flash"); EXPECT_EQ(resolved.models.deepseek, "deepseek-flash");
  EXPECT_EQ(resolved.models.openrouter, "~deepseek/deepseek-flash-latest"); EXPECT_EQ(resolved.models.surplus, "deepseek-v4.1-flash"); EXPECT_EQ(resolved.models.openlux, "deepseek-v4.1-flash");
  input = {}; input.surplus = std::vector<aliases::CatalogModel>{{"deepseek-v4.1-flash"}, {"deepseek-v4.2-flash"}, {"deepseek-v4.2-flash-1101"}};
  const auto newer = aliases::resolveDeepSeekFlashAliases(input); EXPECT_EQ(newer.models.surplus, "deepseek-v4.2-flash"); EXPECT_EQ(newer.models.lithos, aliases::DEEPSEEK_FLASH_FALLBACK_ALIASES.models.lithos);
  const auto fallback = aliases::resolveDeepSeekFlashAliases(); const auto& expected = aliases::DEEPSEEK_FLASH_FALLBACK_ALIASES.models;
  EXPECT_EQ(fallback.models.lithos, expected.lithos); EXPECT_EQ(fallback.models.deepseek, expected.deepseek); EXPECT_EQ(fallback.models.openrouter, expected.openrouter); EXPECT_EQ(fallback.models.surplus, expected.surplus); EXPECT_EQ(fallback.models.openlux, expected.openlux);
}
TEST("waterfall: each paid hop is pinned to its own paid tier") {
  EXPECT_EQ(waterfall::DEEPSEEK_WATERFALL_PAID_PIN.size(), 2u); EXPECT_EQ(*waterfall::DEEPSEEK_WATERFALL_PAID_PIN.get("surplus"), std::vector<std::string>{"surplus"}); EXPECT_EQ(*waterfall::DEEPSEEK_WATERFALL_PAID_PIN.get("openlux"), std::vector<std::string>{"metered"});
}
TEST("waterfall: only infrastructure or serving statuses advance the chain") {
  for (const auto status : {402, 403, 429, 500, 502, 504}) EXPECT(waterfall::isDeepSeekWaterfallRetryStatus(status));
  for (const auto status : {200, 400, 404, 422}) EXPECT(!waterfall::isDeepSeekWaterfallRetryStatus(status));
}
TEST("waterfall: a Lithos refusal advances without spending the vendor reset window") {
  EXPECT_EQ(uos::provider::lithos_rate_limits::LITHOS_IMMEDIATE_FAILOVER_WAIT_POLICY.maxWaits, 0); EXPECT_EQ(uos::provider::lithos_rate_limits::LITHOS_IMMEDIATE_FAILOVER_WAIT_POLICY.totalCapMs, 0);
}
TEST("waterfall: the requested effort reads both wire fields and stays inside the advertised set") {
  EXPECT_EQ(waterfall::deepSeekWaterfallRequestedEffort({{"reasoning", uos::json::object{{"effort", " max "}}}}), std::optional<std::string>("max")); EXPECT_EQ(waterfall::deepSeekWaterfallRequestedEffort({{"reasoning_effort", "low"}}), std::optional<std::string>("low")); EXPECT(!waterfall::deepSeekWaterfallRequestedEffort({}));
  EXPECT_EQ(waterfall::deepSeekWaterfallRequestedEffort({{"reasoning", uos::json::object{{"effort", " "}}}, {"reasoning_effort", " high "}}), std::optional<std::string>("high"));
  const std::vector<std::string_view> levels(waterfall::DEEPSEEK_WATERFALL_REASONING_LEVELS.begin(), waterfall::DEEPSEEK_WATERFALL_REASONING_LEVELS.end()); EXPECT_EQ(levels, (std::vector<std::string_view>{"low", "high", "max"})); EXPECT(waterfall::isDeepSeekWaterfallReasoningLevel("high")); EXPECT(!waterfall::isDeepSeekWaterfallReasoningLevel("none"));
  EXPECT_EQ(waterfall::deepSeekWaterfallFailureReason("openrouter", 503.0), "deepseek_waterfall:openrouter:503"); EXPECT_EQ(waterfall::deepSeekWaterfallFailureReason("lithos", std::string_view("transport")), "deepseek_waterfall:lithos:transport_failure"); EXPECT_EQ(waterfall::deepSeekWaterfallFailureReason("surplus", std::string_view("stream")), "deepseek_waterfall:surplus:stream_failure");
}
TEST("waterfall: gateway selection and each configured credential gate the catalogue") {
  uos::rt::env::ScopedEnvironment isolated{{{"HOME", "/nonexistent/w06c7-waterfall"}}};
  EXPECT(!waterfall::deepSeekWaterfallCatalogEnabled(std::nullopt));
  const std::vector<std::pair<std::string, std::string>> providers{{"lithos", "LITHOSAI_API_KEY"}, {"deepseek", "DEEPSEEK_API_KEY"}, {"openrouter", "OPENROUTER_API_KEY"}, {"surplus", "SURPLUS_API_KEY"}, {"openlux", "OPENLUX_API_KEY"}};
  for (const auto& [provider, key] : providers) {
    uos::rt::env::set(key, "fixture-key"); EXPECT(waterfall::deepSeekWaterfallProviderConfigured(provider)); EXPECT(waterfall::deepSeekWaterfallCatalogEnabled(std::nullopt));
    const uos::provider::selection::ProviderSelection disabled{{"deepseek"}, 1}, enabled{{"ubiquity"}, 1};
    EXPECT(!waterfall::deepSeekWaterfallCatalogEnabled(disabled)); EXPECT(waterfall::deepSeekWaterfallCatalogEnabled(enabled)); uos::rt::env::remove(key);
  }
}
TEST("waterfall: the Codex catalogue record carries the advertised contract") {
  const auto row = uos::catalog::models::buildDeepSeekWaterfallCodexRecord();
  EXPECT_EQ(uos::rt::getString(row, "slug"), std::optional<std::string_view>(waterfall::DEEPSEEK_WATERFALL_MODEL_ID));
  EXPECT_EQ(uos::rt::getString(row, "display_name"), std::optional<std::string_view>(waterfall::DEEPSEEK_WATERFALL_DISPLAY_NAME));
  EXPECT_EQ(uos::rt::getString(row, "default_reasoning_level"), std::optional<std::string_view>(waterfall::DEEPSEEK_WATERFALL_DEFAULT_REASONING_LEVEL));
  std::vector<std::string> levels; const auto* supported = uos::rt::getArray(row, "supported_reasoning_levels"); EXPECT(supported);
  for (const auto& level : *supported) levels.emplace_back(uos::rt::getString(level, "effort").value_or(""));
  const std::vector<std::string> expected{"low", "high", "max"}; EXPECT_EQ(levels, expected);
  const uos::json::value endpoints = uos::json::array{"openai-response"}; EXPECT(uos::rt::deepEqual(*uos::rt::get(row, "supported_endpoint_types"), endpoints));
  EXPECT_EQ(uos::rt::getNumber(row, "context_window"), 800'000);
  EXPECT_EQ(uos::rt::getNumber(row, "max_context_window"), 800'000);
  EXPECT_EQ(uos::rt::getNumber(row, "auto_compact_token_limit"), 720'000);
}
TEST("waterfall: the gateway replaces an upstream same-id row and preserves catalog order") {
  namespace json = uos::json;
  namespace rt = uos::rt;
  const json::object upstream{
    {"slug", waterfall::DEEPSEEK_WATERFALL_MODEL_ID},
    {"context_window", 1'048'576},
    {"max_context_window", 1'048'576},
    {"auto_compact_token_limit", 943'718},
  };
  const json::array models{json::object{{"slug", "upstream-before"}}, upstream, json::object{{"slug", "upstream-after"}}};
  const auto result = uos::catalog::models::withDeepSeekWaterfallModel(models);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(rt::getString(result[0], "slug"), "upstream-before");
  EXPECT_EQ(rt::getString(result[1], "slug"), waterfall::DEEPSEEK_WATERFALL_MODEL_ID);
  EXPECT_EQ(rt::getString(result[2], "slug"), "upstream-after");
  EXPECT_EQ(rt::getNumber(result[1], "context_window"), 800'000);
  EXPECT_EQ(rt::getNumber(result[1], "max_context_window"), 800'000);
  EXPECT_EQ(rt::getNumber(result[1], "auto_compact_token_limit"), 720'000);
}
TEST("waterfall: allowedPaidProviders pins the Surplus hop without changing the default order") {
  namespace health = uos::paid_fallback::health;
  const uos::rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c1-waterfall"}, {"SURPLUS_API_KEY", "fixture-surplus-key"}, {"METERED_API_KEY", "fixture-metered-key"}}};
  uos::provider::metered::MeteredModel metered{"deepseek-v4.1-flash", "model", 0, "openlux", {"openai-response"}};
  uos::provider::surplus::SurplusModel surplus{"deepseek-v4.1-flash", "model", 0, "surplus", {"openai-response"}};
  surplus.supports_tools = true; surplus.input_price_per_token = 0.0000003; surplus.output_price_per_token = 0.0000012;
  health::PaidRoutingStateInput input{{{{metered}, uos::rt::dateNow()}}, {{{surplus}, uos::rt::dateNow()}}, false, "openai-response", true, "deepseek-v4.1-flash", std::nullopt};
  const std::vector<std::string> both{"surplus", "metered"}, onlySurplus{"surplus"}, onlyMetered{"metered"};
  EXPECT_EQ(health::resolvePaidRoutingState(input).paidProviders, both); input.allowedPaidProviders = onlySurplus; EXPECT_EQ(health::resolvePaidRoutingState(input).paidProviders, onlySurplus);
  input.allowedPaidProviders = onlyMetered; EXPECT_EQ(health::resolvePaidRoutingState(input).paidProviders, onlyMetered);
  input.allowedPaidProviders.reset(); input.selection = uos::provider::selection::ProviderSelection{{"ubiquity"}, 1}; EXPECT(health::resolvePaidRoutingState(input).paidProviders.empty());
  input.allowedPaidProviders = onlySurplus; input.deepSeekWaterfallPaidHop = "surplus"; EXPECT_EQ(health::resolvePaidRoutingState(input).paidProviders, onlySurplus);
  input.allowedPaidProviders = onlyMetered; input.deepSeekWaterfallPaidHop = "openlux"; EXPECT_EQ(health::resolvePaidRoutingState(input).paidProviders, onlyMetered);
}
TEST("waterfall: verified OpenLux Responses capability stays scoped to the exact synthetic paid hop") {
  namespace health = uos::paid_fallback::health;
  const uos::rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c1-waterfall"}, {"METERED_API_KEY", "fixture-metered-key"}}};
  const uos::provider::metered::MeteredModel model{std::string(waterfall::DEEPSEEK_WATERFALL_PAID_MODEL_ID), "model", 0, "openlux", {"openai", "anthropic"}};
  health::PaidRoutingStateInput base{{{{model}, uos::rt::dateNow()}}, std::nullopt, false, "openai-response", true, std::string(waterfall::DEEPSEEK_WATERFALL_PAID_MODEL_ID),
    uos::provider::selection::ProviderSelection{{"ubiquity"}, 1}, std::vector<std::string>{"metered"}, "openlux"};
  const std::vector<std::string> expected{"metered"}; EXPECT_EQ(health::resolvePaidRoutingState(base).paidProviders, expected);
  std::vector<health::PaidRoutingStateInput> rejected;
  auto input = base; input.deepSeekWaterfallPaidHop.reset(); rejected.push_back(input);
  input = base; input.selection.reset(); input.deepSeekWaterfallPaidHop.reset(); rejected.push_back(input);
  input = base; input.meteredCatalog.reset(); rejected.push_back(input);
  input = base; input.meteredCatalog->models.clear(); rejected.push_back(input);
  input = base; input.model = "unverified-model"; input.meteredCatalog->models.front().id = input.model; rejected.push_back(input);
  input = base; input.endpointType = "anthropic"; rejected.push_back(input);
  input = base; input.selection = uos::provider::selection::ProviderSelection{{"codex"}, 1}; rejected.push_back(input);
  input = base; input.allowedPaidProviders.reset(); rejected.push_back(input);
  input = base; input.allowedPaidProviders = std::vector<std::string>{"surplus", "metered"}; rejected.push_back(input);
  input = base; input.allowedPaidProviders = std::vector<std::string>{"surplus"}; rejected.push_back(input);
  input = base; input.deepSeekWaterfallPaidHop = "surplus"; rejected.push_back(input);
  for (const auto& scenario : rejected) EXPECT(health::resolvePaidRoutingState(scenario).paidProviders.empty());
  uos::rt::env::remove("METERED_API_KEY"); EXPECT(health::resolvePaidRoutingState(base).paidProviders.empty());
}
