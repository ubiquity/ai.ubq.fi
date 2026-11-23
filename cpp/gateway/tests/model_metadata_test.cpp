// Links: tests/model_metadata_test.cpp
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
// Port of tests/model-metadata.test.ts.
#include "uos/models/metadata.hpp"
#include "uos/models/catalog.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/test_async.hpp"

#include "uos/models/openrouter_models.hpp"
#include "uos/recent_model_context.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::models::metadata;
  using uos::models::openrouter_models::OpenRouterModelMetadata;
  using uos::models::openrouter_models::OpenRouterReasoning;
  using uos::recent_model_context::CODEX_EFFECTIVE_CONTEXT_WINDOW_PERCENT;

  using Levels = std::optional<std::vector<std::string>>;

  OpenRouterModelMetadata enrichment() {
    OpenRouterModelMetadata metadata;
    metadata.id = "openai/gpt-5.6-sol";
    metadata.context_window_tokens = 1'050'000;
    metadata.max_context_window_tokens = 1'050'000;
    metadata.reasoning = OpenRouterReasoning{.supported_efforts = {"max", "high", "medium"}, .default_effort = "medium", .mandatory = false};
    return metadata;
  }


  ModelMetadataHint hintOf(const char* text) { return json::value_to<ModelMetadataHint>(rt::jsonParse(text)); }
}

TEST("an id no source describes resolves to unknown instead of a curated guess") {
  // These ids were previously answered by the curated tables, which are now
  // disabled: with no source, the honest answer is "nothing is known".
  for (const char* id : {"glm-5.3", "claude-sonnet-5", "deepseek-v4-pro", "gpt-5.6-terra"}) {
    ModelMetadataSources sources;
    sources.openRouter = nullptr;
    const ResolvedModelMetadata resolved = resolveModelMetadata(id, sources);
    EXPECT_EQ(resolved.context_window_tokens, std::nullopt);
    EXPECT_EQ(resolved.max_context_window_tokens, std::nullopt);
    EXPECT_EQ(resolved.auto_compact_token_limit_tokens, std::nullopt);
    EXPECT_EQ(resolved.effective_context_window_percent, std::nullopt);
    EXPECT_EQ(resolved.supported_reasoning_levels, std::nullopt);
    EXPECT_EQ(resolved.default_reasoning_effort, std::nullopt);
    EXPECT_EQ(resolved.context_source, "unknown");
    EXPECT_EQ(resolved.reasoning_source, "unknown");
  }
}

TEST("reasoning stays Codex-authoritative while the window follows the widest source") {
  ModelMetadataSources sources;
  sources.codex = hintOf(R"({"context_window_tokens":272000,"max_context_window_tokens":400000,"supported_reasoning_levels":["low","high"],"default_reasoning_effort":"high"})");
  sources.provider = hintOf(R"({"context_window_tokens":1000000,"supported_reasoning_levels":["none"]})");
  sources.openRouter = enrichment();
  const ResolvedModelMetadata resolved = resolveModelMetadata("gpt-5.6-sol", sources);
  EXPECT_EQ(resolved.context_window_tokens, 1'050'000.0);
  EXPECT_EQ(resolved.max_context_window_tokens, 1'050'000.0);
  EXPECT_EQ(resolved.context_source, "openrouter");
  EXPECT_EQ(resolved.supported_reasoning_levels, (Levels{{"low", "high"}}));
  EXPECT_EQ(resolved.default_reasoning_effort, "high");
  EXPECT_EQ(resolved.reasoning_source, "codex_upload");
}

TEST("a serving provider's own declaration outranks a narrower enrichment entry") {
  ModelMetadataSources sources;
  sources.provider = hintOf(R"({"context_window_tokens":1000000,"max_context_window_tokens":1000000})");
  OpenRouterModelMetadata narrow = enrichment();
  narrow.id = "~deepseek/deepseek-flash-latest";
  narrow.context_window_tokens = 65'536;
  narrow.max_context_window_tokens = 65'536;
  sources.openRouter = narrow;
  const ResolvedModelMetadata resolved = resolveModelMetadata("deepseek-flash", sources);
  EXPECT_EQ(resolved.context_window_tokens, 1'000'000.0);
  EXPECT_EQ(resolved.context_source, "provider_discovery");
  // Reasoning was not declared by the provider, so enrichment supplies it.
  EXPECT_EQ(resolved.reasoning_source, "openrouter");
  EXPECT_EQ(resolved.supported_reasoning_levels, (Levels{{"max", "high", "medium"}}));
}

TEST("enrichment is the last resort, and the resolved context derives the auto-compact limit") {
  ModelMetadataSources sources;
  sources.openRouter = enrichment();
  const ResolvedModelMetadata resolved = resolveModelMetadata("gpt-5.6-sol", sources);
  EXPECT_EQ(resolved.context_window_tokens, 1'050'000.0);
  EXPECT_EQ(resolved.max_context_window_tokens, 1'050'000.0);
  EXPECT_EQ(resolved.auto_compact_token_limit_tokens, 945'000.0);
  EXPECT_EQ(resolved.context_source, "openrouter");
  EXPECT_EQ(resolved.effective_context_window_percent, CODEX_EFFECTIVE_CONTEXT_WINDOW_PERCENT);
  EXPECT_EQ(resolved.default_reasoning_effort, "medium");
}

TEST("a declared auto-compact limit is preserved inside the resolved window") {
  ModelMetadataSources narrowSources;
  narrowSources.codex = hintOf(R"({"context_window_tokens":272000,"auto_compact_token_limit_tokens":200000})");
  narrowSources.openRouter = nullptr;
  const ResolvedModelMetadata narrow = resolveModelMetadata("gpt-5.6-sol", narrowSources);
  EXPECT_EQ(narrow.context_window_tokens, 272'000.0);
  EXPECT_EQ(narrow.auto_compact_token_limit_tokens, 200'000.0);

  // Widening the advertised window does not discard a first-party compaction
  // limit: it is advice the client may follow or override.
  ModelMetadataSources widenedSources;
  widenedSources.codex = hintOf(R"({"context_window_tokens":272000,"auto_compact_token_limit_tokens":200000})");
  widenedSources.openRouter = enrichment();
  const ResolvedModelMetadata widened = resolveModelMetadata("gpt-5.6-sol", widenedSources);
  EXPECT_EQ(widened.context_window_tokens, 1'050'000.0);
  EXPECT_EQ(widened.auto_compact_token_limit_tokens, 200'000.0);
}

TEST("OpenRouter's window wins when it knows the id, so the gateway advertises the real capability") {
  // The Codex catalog understates what the endpoint accepts (916k verified), so a
  // wider third-party window is advertised and the client chooses its own budget.
  ModelMetadataSources sources;
  sources.codex = hintOf(R"({"context_window_tokens":272000,"max_context_window_tokens":872000,"supported_reasoning_levels":["low","high"]})");
  sources.codexSubscription = codexSubscriptionMetadataHint();
  OpenRouterModelMetadata wide = enrichment();
  wide.context_window_tokens = 1'050'000;
  wide.max_context_window_tokens = 1'050'000;
  sources.openRouter = wide;
  const ResolvedModelMetadata resolved = resolveModelMetadata("gpt-5.6-sol", sources);
  EXPECT_EQ(resolved.context_window_tokens, 1'050'000.0);
  EXPECT_EQ(resolved.max_context_window_tokens, 1'050'000.0);
  EXPECT_EQ(resolved.auto_compact_token_limit_tokens, 945'000.0);
  EXPECT_EQ(resolved.context_source, "openrouter");
  // Tiers still come from the upload, which stays authoritative for them.
  EXPECT_EQ(resolved.reasoning_source, "codex_upload");
}

TEST("first-party windows fill ids OpenRouter does not know, and the subscription bound fills the rest") {
  ModelMetadataSources firstPartySources;
  firstPartySources.codex = hintOf(R"({"context_window_tokens":8000})");
  firstPartySources.openRouter = nullptr;
  const ResolvedModelMetadata firstParty = resolveModelMetadata("codex-only-model", firstPartySources);
  EXPECT_EQ(firstParty.context_window_tokens, 8'000.0);
  EXPECT_EQ(firstParty.context_source, "codex_upload");

  ModelMetadataSources silentSources;
  silentSources.codexSubscription = codexSubscriptionMetadataHint();
  silentSources.openRouter = nullptr;
  const ResolvedModelMetadata silent = resolveModelMetadata("codex-only-model", silentSources);
  EXPECT_EQ(silent.context_window_tokens, CODEX_SUBSCRIPTION_CONTEXT_WINDOW_TOKENS);
  EXPECT_EQ(silent.max_context_window_tokens, CODEX_SUBSCRIPTION_MAX_CONTEXT_WINDOW_TOKENS);
  EXPECT_EQ(silent.context_source, "codex_subscription");
}

TEST("a narrower third-party entry never shrinks a first-party window") {
  ModelMetadataSources sources;
  sources.codex = hintOf(R"({"context_window_tokens":400000,"max_context_window_tokens":400000})");
  sources.codexSubscription = codexSubscriptionMetadataHint();
  OpenRouterModelMetadata narrow = enrichment();
  narrow.context_window_tokens = 300'000;
  narrow.max_context_window_tokens = 300'000;
  sources.openRouter = narrow;
  const ResolvedModelMetadata resolved = resolveModelMetadata("gpt-5.6-sol", sources);
  EXPECT_EQ(resolved.context_window_tokens, 400'000.0);
  EXPECT_EQ(resolved.max_context_window_tokens, 400'000.0);
  EXPECT_EQ(resolved.context_source, "codex_upload");
}

TEST("OpenRouter breaks ties, keeping capability provenance when widths agree") {
  ModelMetadataSources sources;
  sources.codex = hintOf(R"({"context_window_tokens":1050000})");
  OpenRouterModelMetadata tiedEnrichment = enrichment();
  tiedEnrichment.context_window_tokens = 1'050'000;
  tiedEnrichment.max_context_window_tokens = 1'050'000;
  sources.openRouter = tiedEnrichment;
  const ResolvedModelMetadata tied = resolveModelMetadata("gpt-5.6-sol", sources);
  EXPECT_EQ(tied.context_window_tokens, 1'050'000.0);
  EXPECT_EQ(tied.context_source, "openrouter");
}

TEST("ids Codex does not serve keep their provider or enrichment window") {
  ModelMetadataSources sources;
  OpenRouterModelMetadata glm = enrichment();
  glm.id = "z-ai/glm-5.3";
  glm.context_window_tokens = 1'310'720;
  glm.max_context_window_tokens = 1'310'720;
  sources.openRouter = glm;
  const ResolvedModelMetadata resolved = resolveModelMetadata("glm-5.3", sources);
  EXPECT_EQ(resolved.context_window_tokens, 1'310'720.0);
  EXPECT_EQ(resolved.context_source, "openrouter");
}

TEST("a mandatory-reasoning model does not gain a none tier it does not advertise") {
  ModelMetadataSources sources;
  OpenRouterModelMetadata glm = enrichment();
  glm.id = "z-ai/glm-5.3";
  glm.reasoning = OpenRouterReasoning{.supported_efforts = {"max", "high", "low"}, .default_effort = "max", .mandatory = true};
  sources.openRouter = glm;
  const ResolvedModelMetadata resolved = resolveModelMetadata("glm-5.3", sources);
  EXPECT_EQ(resolved.supported_reasoning_levels, (Levels{{"max", "high", "low"}}));
  EXPECT_EQ(resolved.default_reasoning_effort, "max");
}

TEST("every advertised tier survives, including ones the gateway does not know") {
  ModelMetadataSources sources;
  OpenRouterModelMetadata qwen = enrichment();
  qwen.id = "qwen/qwen3.8";
  qwen.reasoning = OpenRouterReasoning{.supported_efforts = {"none", "thinking", "hyper"}, .default_effort = "thinking", .mandatory = false};
  sources.openRouter = qwen;
  const ResolvedModelMetadata resolved = resolveModelMetadata("qwen3.8", sources);
  EXPECT_EQ(resolved.supported_reasoning_levels, (Levels{{"none", "thinking", "hyper"}}));
}

TEST("the Codex snapshot adapter maps explicit null defaults to none") {
  EXPECT(codexSnapshotMetadataHint(nullptr) == std::nullopt);
  const json::object record = rt::jsonParse(
                                R"({"context_window":400000,"auto_compact_token_limit":null,"supported_reasoning_levels":[null,"low",{"effort":"high"},{"effort":""}],"default_reasoning_level":null})")
                                .as_object();
  const std::optional<ModelMetadataHint> hint = codexSnapshotMetadataHint(record);
  ASSERT(hint.has_value());  // the adapter returns a hint for a real record
  EXPECT(hint->context_window_tokens == 400'000.0);
  EXPECT(hint->auto_compact_token_limit_tokens == nullptr);
  EXPECT(hint->default_reasoning_effort == json::value("none"));
  ModelMetadataSources sources;
  sources.codex = *hint;
  sources.openRouter = nullptr;
  const ResolvedModelMetadata resolved = resolveModelMetadata("any-model", sources);
  EXPECT_EQ(resolved.supported_reasoning_levels, (Levels{{"none", "low", "high"}}));
  EXPECT_EQ(resolved.default_reasoning_effort, "none");
  EXPECT_EQ(resolved.context_source, "codex_upload");
}

TEST("a Codex record without an explicit default keeps no default effort") {
  const json::object record = rt::jsonParse(R"({"context_window":100000,"supported_reasoning_levels":["low","high"]})").as_object();
  const std::optional<ModelMetadataHint> hint = codexSnapshotMetadataHint(record);
  ASSERT(hint.has_value());  // the adapter returns a hint for a real record
  EXPECT(hint->default_reasoning_effort == std::nullopt);
  ModelMetadataSources sources;
  sources.codex = *hint;
  sources.openRouter = nullptr;
  const ResolvedModelMetadata resolved = resolveModelMetadata("any-model", sources);
  EXPECT_EQ(resolved.default_reasoning_effort, std::nullopt);
  EXPECT_EQ(resolved.supported_reasoning_levels, (Levels{{"low", "high"}}));
}

TEST("a default-only source advertises only its own effort and retains precedence") {
  ModelMetadataSources codexSources;
  codexSources.codex = hintOf(R"({"default_reasoning_effort":"high"})");
  codexSources.provider = hintOf(R"({"supported_reasoning_levels":["low"],"default_reasoning_effort":"low"})");
  codexSources.openRouter = enrichment();
  const ResolvedModelMetadata codex = resolveModelMetadata("default-only-model", codexSources);
  EXPECT_EQ(codex.supported_reasoning_levels, (Levels{{"high"}}));
  EXPECT_EQ(codex.default_reasoning_effort, "high");
  EXPECT_EQ(codex.reasoning_source, "codex_upload");

  ModelMetadataSources providerSources;
  providerSources.codex = hintOf(R"({"default_reasoning_effort":" "})");
  providerSources.provider = hintOf(R"({"supported_reasoning_levels":[],"default_reasoning_effort":"thinking"})");
  providerSources.openRouter = enrichment();
  const ResolvedModelMetadata provider = resolveModelMetadata("default-only-model", providerSources);
  EXPECT_EQ(provider.supported_reasoning_levels, (Levels{{"thinking"}}));
  EXPECT_EQ(provider.default_reasoning_effort, "thinking");
  EXPECT_EQ(provider.reasoning_source, "provider_discovery");

  ModelMetadataSources openRouterSources;
  OpenRouterModelMetadata hyper = enrichment();
  hyper.reasoning = OpenRouterReasoning{.supported_efforts = {}, .default_effort = "hyper", .mandatory = false};
  openRouterSources.openRouter = hyper;
  const ResolvedModelMetadata openRouter = resolveModelMetadata("default-only-model", openRouterSources);
  EXPECT_EQ(openRouter.supported_reasoning_levels, (Levels{{"hyper"}}));
  EXPECT_EQ(openRouter.default_reasoning_effort, "hyper");
  EXPECT_EQ(openRouter.reasoning_source, "openrouter");
}

TEST("a default-only Codex null remains explicit none while an omitted default stays unknown") {
  ModelMetadataSources explicitSources;
  explicitSources.codex = rt::orNull(codexSnapshotMetadataHint(rt::jsonParse(R"({"default_reasoning_level":null})").as_object()));
  explicitSources.provider = hintOf(R"({"supported_reasoning_levels":["high"]})");
  explicitSources.openRouter = nullptr;
  const ResolvedModelMetadata explicitNone = resolveModelMetadata("default-only-model", explicitSources);
  EXPECT_EQ(explicitNone.supported_reasoning_levels, (Levels{{"none"}}));
  EXPECT_EQ(explicitNone.default_reasoning_effort, "none");
  EXPECT_EQ(explicitNone.reasoning_source, "codex_upload");

  for (const char* record : {R"({})", R"({"default_reasoning_level":""})", R"({"default_reasoning_level":42})"}) {
    ModelMetadataSources sources;
    sources.codex = rt::orNull(codexSnapshotMetadataHint(rt::jsonParse(record).as_object()));
    sources.openRouter = nullptr;
    const ResolvedModelMetadata unknown = resolveModelMetadata("default-only-model", sources);
    EXPECT_EQ(unknown.supported_reasoning_levels, std::nullopt);
    EXPECT_EQ(unknown.default_reasoning_effort, std::nullopt);
    EXPECT_EQ(unknown.reasoning_source, "unknown");
  }
}

ASYNC_TEST("catalogs do not publish none for a source with only low and high tiers") {
  rt::env::ScopedEnvironment env(std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c4-metadata"}, {"OPENROUTER_API_KEY", "synthetic-catalog-key"}});
  const auto saved = uos::config::config.codexAuthJsonB64; uos::config::config.codexAuthJsonB64.clear(); auto store = std::make_shared<rt::MemoryKv>(rt::dateNow); uos::kv::setKvForTest(store); uos::codex::auth::resetCodexAuthCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::models::openrouter_models::resetOpenRouterModelsCacheForTest();
  auto cleanup = rt::scopeExit([saved] { uos::provider::metered::setMeteredModelsFetchForTest({}); uos::provider::metered::resetMeteredModelsCacheForTest(); uos::models::openrouter_models::resetOpenRouterModelsCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::codex::auth::resetCodexAuthCacheForTest(); uos::kv::setKvForTest(nullptr); uos::config::config.codexAuthJsonB64 = saved; });
  const json::array rows{json::object{{"id", "synthetic/default-only"}, {"reasoning", json::object{{"default_effort", "high"}}}}, json::object{{"id", "synthetic/tiers-only"}, {"reasoning", json::object{{"supported_efforts", json::array{"low", "high"}}}}}, json::object{{"id", "synthetic/paid-tiers-only"}, {"reasoning", json::object{{"supported_efforts", json::array{"low", "high"}}}}}, json::object{{"id", "synthetic/unknown"}}};
  uos::models::openrouter_models::FetchOpenRouterModelsOptions options; options.force = true; options.fetcher = [rows](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"data", rows}}); }; auto fetching = uos::models::openrouter_models::fetchOpenRouterModels(options); const auto snapshot = co_await std::move(fetching); ASSERT(snapshot);
  auto pending = uos::models::catalog::handleModels(rt::Request("https://ai.ubq.fi/v1/models?client_version=0.200.0")); const auto response = co_await std::move(pending); EXPECT_EQ(response.status(), 200); const auto payload = co_await response.json();
  for (const auto& row : payload.at("models").as_array()) {
    const auto id = rt::getString(row, "slug");
    if (id == "synthetic/default-only") { EXPECT_EQ(rt::getString(row, "default_reasoning_level"), "high"); EXPECT(rt::deepEqual(row.at("supported_reasoning_levels"), json::array{json::object{{"effort", "high"}, {"description", "Reasoning effort: high"}}})); }
    if (id == "synthetic/tiers-only") { EXPECT(!rt::get(row, "default_reasoning_level")); EXPECT(rt::deepEqual(row.at("supported_reasoning_levels"), json::array{json::object{{"effort", "low"}, {"description", "Reasoning effort: low"}}, json::object{{"effort", "high"}, {"description", "Reasoning effort: high"}}})); }
    if (id == "synthetic/unknown") { EXPECT_EQ(rt::getString(row, "default_reasoning_level"), "none"); EXPECT(rt::deepEqual(row.at("supported_reasoning_levels"), json::array{json::object{{"effort", "none"}, {"description", "No reasoning"}}})); }
  }
  EXPECT_EQ(resolveModelMetadata("synthetic/default-only").reasoning_source, "openrouter"); EXPECT_EQ(resolveModelMetadata("synthetic/unknown").reasoning_source, "unknown");
  rt::env::set("METERED_API_KEY", "synthetic-metered-key"); uos::provider::metered::resetMeteredModelsCacheForTest();
  uos::provider::metered::setMeteredModelsFetchForTest(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"data", json::array{json::object{{"id", "synthetic/paid-tiers-only"}, {"created", 0}, {"owned_by", "openlux"}, {"supported_endpoint_types", json::array{"openai-response"}}}}}}); }));
  const auto capabilities = co_await uos::models::catalog::handleModelCapabilities(); EXPECT_EQ(capabilities.status(), 200); const auto body = co_await capabilities.json(); std::size_t found = 0;
  for (const auto& row : body.at("data").as_array()) if (rt::getString(row, "id") == "synthetic/tiers-only" || rt::getString(row, "id") == "synthetic/paid-tiers-only") { ++found; EXPECT(rt::deepEqual(row.at("supported_reasoning_levels"), json::array{"low", "high"})); EXPECT(!rt::get(row, "default_reasoning_effort")); }
  EXPECT_EQ(found, 2u);
}
