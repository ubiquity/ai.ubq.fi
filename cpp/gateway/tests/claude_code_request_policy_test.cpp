// Links: tests/claude_code_request_policy_test.cpp
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
// Direct metadata, catalog and credential-cohort cases of tests/claude-code-request-policy.test.ts.
// The full Responses/Chat handler, principal/quota admission, in-flight rotation and workspace-retry
// assertions await src/handler and the final request assembly; native dispatch cases also run in
// claude_code_route_test.cpp. No handler substitute or provider transport is installed here.
// The three disabled-support cases and failed-refresh case port their metadata/catalog portions here; their final
// handleResponses refusal/body/native-dispatch assertions remain for the handler owner.
#include "uos/catalog/models.hpp"
#include "uos/config.hpp"
#include "uos/codex/auth.hpp"
#include "uos/kv.hpp"
#include "uos/models/catalog.hpp"
#include "uos/models/codex_models_whitelist.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/selection.hpp"
#include "uos/request_policy.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/test_async.hpp"
namespace rt = uos::rt;
namespace json = uos::json;
namespace cc = uos::provider::claude_code_models;
namespace credentials = uos::provider::claude_code_credentials;
namespace policy = uos::request_policy;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  const std::string MODEL = "claude-haiku-5-5", SECRET = "fixture-request-metadata";
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c4-policy"}, {"METERED_API_KEY", "fixture-metered-key"}, {"CLAUDE_CODE_ALLOWED_KEY_IDS", "metadata-owner"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<rt::Kv> store = std::make_shared<rt::MemoryKv>(rt::dateNow);
    std::vector<std::string> ids{MODEL};
    std::optional<json::value> disabled = json::value(true);
    bool failed = false; std::size_t discovery = 0, other = 0;
    Fixture() { uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(store); cc::resetClaudeCodeModelsCacheForTest(); credentials::resetClaudeCodeCredentialCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::provider::metered::resetMeteredModelsCacheForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); }
    ~Fixture() { uos::kv::setKvForTest(nullptr); cc::resetClaudeCodeModelsCacheForTest(); credentials::resetClaudeCodeCredentialCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::provider::metered::resetMeteredModelsCacheForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); uos::config::config.codexAuthJsonB64 = saved; }
    rt::Fetch fetcher() { return rt::Fetch([this](rt::RequestInfo input, rt::RequestInit) {
      std::string url; if (const auto* text = std::get_if<std::string>(&input)) url = *text; else if (const auto* parsed = std::get_if<rt::URL>(&input)) url = parsed->href(); else url = std::get<rt::Request>(input).url();
      if (rt::startsWith(url, "https://api.anthropic.com/v1/models")) { ++discovery; if (failed) return rt::Response(nullptr, rt::ResponseInit{.status = 503}); json::array rows; for (const auto& id : ids) { json::object row{{"id", id}}; if (disabled) row["capabilities"] = json::object{{"thinking", json::object{{"types", json::object{{"disabled", json::object{{"supported", *disabled}}}}}}}}; rows.push_back(row); } return rt::Response::json(json::object{{"data", rows}, {"has_more", false}}); }
      ++other; return rt::Response::json(json::object{{"error", json::object{{"message", "Unexpected fixture transport"}}}}, rt::ResponseInit{.status = 500});
    }); }
  };
  rt::Task<void> seedPaid(std::vector<std::string> ids) { json::array rows; for (const auto& id : ids) rows.push_back(json::object{{"id", id}, {"supported_endpoint_types", json::array{"openai"}}}); uos::provider::metered::FetchMeteredModelsOptions options; options.force = true; options.fetcher = [rows](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"data", rows}}); }; auto task = uos::provider::metered::fetchMeteredModels(options); const auto snapshot = co_await std::move(task); static_cast<void>(snapshot); }
  rt::Task<void> seed(Fixture& fixture) {
    credentials::StoreClaudeCodeStoredCredentialInput credential{.secret = SECRET}; auto storeTask = credentials::storeClaudeCodeStoredCredential(fixture.store, credential); const auto stored = co_await std::move(storeTask); static_cast<void>(stored);
    const rt::KvValue selection(json::object{{"provider_ids", json::array{"claude_code", "openlux"}}, {"updated_at_ms", 1}}); co_await fixture.store->set(uos::provider::selection::PROVIDER_SELECTION_KV_KEY, selection); std::vector<std::string> models{MODEL}; auto task = seedPaid(models); co_await std::move(task);
  }
  rt::Task<std::optional<rt::Response>> availability() { auto task = policy::getCodexModelMetadata(MODEL, "responses"); const auto metadata = co_await std::move(task); co_return policy::validateCodexModelAvailable(MODEL, "responses", metadata); }
  rt::Task<void> rotate(Fixture& fixture, bool workspace) {
    if (!workspace) { credentials::StoreClaudeCodeStoredCredentialInput credential{.secret = "fixture-replacement"}; auto task = credentials::storeClaudeCodeStoredCredential(fixture.store, credential); const auto stored = co_await std::move(task); static_cast<void>(stored); }
    else { const json::object raw{{"id", "wrkspc_replacement"}, {"name", nullptr}, {"source", "kv"}, {"discovered_at_ms", nullptr}, {"verified_at_ms", nullptr}, {"last_error", nullptr}}; const auto state = json::value_to<credentials::ClaudeCodeWorkspaceState>(json::value(raw)); auto task = credentials::storeClaudeCodeWorkspaceState(fixture.store, state); const auto stored = co_await std::move(task); static_cast<void>(stored); }
  }
}
ASYNC_TEST("observed Claude-only IDs resolve with conservative metadata without a paid catalog row") {
  Fixture fixture; co_await seed(fixture); std::vector<std::string> empty; co_await seedPaid(empty); const rt::GlobalFetchOverride fetch(fixture.fetcher()); AWAIT(metadata, policy::getCodexModelMetadata(MODEL, "responses")); EXPECT(!policy::validateCodexModelAvailable(MODEL, "responses", metadata)); EXPECT_EQ(metadata.supportedEndpoints, (std::optional(std::vector<std::string>{"openai", "openai-response"}))); EXPECT_EQ(metadata.reasoning.levels, (std::vector<std::string>{"none"})); EXPECT_EQ(metadata.reasoning.defaultLevel, "none"); ASSERT(metadata.record); EXPECT_EQ(rt::getString(*metadata.record, "slug"), MODEL); EXPECT(!rt::get(*metadata.record, "context_window")); EXPECT_EQ(fixture.other, 0u);
}
ASYNC_TEST("an existing Codex record retains precedence and its exact reasoning metadata") {
  Fixture fixture; co_await seed(fixture); const json::object snapshot{{"source", "fixture"}, {"updated_at_ms", rt::dateNow()}, {"models", json::array{json::object{{"slug", MODEL}, {"supported_reasoning_levels", json::array{"max"}}, {"default_reasoning_level", "max"}}}}}; const auto runtime = uos::runtime_config::buildRuntimeConfig(snapshot); const rt::KvValue stored(uos::runtime_config::toJson(runtime)); co_await fixture.store->set(uos::runtime_config::RUNTIME_CONFIG_V2_KEY, stored); uos::runtime_config::cacheRuntimeConfig(runtime); const rt::GlobalFetchOverride fetch(fixture.fetcher()); AWAIT(metadata, policy::getCodexModelMetadata(MODEL, "responses")); EXPECT(!metadata.supportedEndpoints); EXPECT_EQ(metadata.reasoning.levels, (std::vector<std::string>{"max"})); EXPECT_EQ(fixture.discovery, 0u);
}
ASYNC_TEST("unrelated non-Codex paid metadata does not invoke Claude discovery") {
  Fixture fixture; co_await seed(fixture); const std::vector<std::string> ids{"deepseek-fixture", "unrelated-paid-fixture"}; co_await seedPaid(ids); const rt::GlobalFetchOverride fetch(fixture.fetcher()); const std::vector<std::string> routes{"chat.completions", "responses"}; for (const auto& id : ids) for (const auto& route : routes) { AWAIT(metadata, policy::getCodexModelMetadata(id, route)); ASSERT(metadata.record); EXPECT_EQ(rt::getString(*metadata.record, "slug"), id); EXPECT_EQ(metadata.supportedEndpoints, std::optional(std::vector<std::string>{"openai"})); EXPECT_EQ(metadata.reasoning.levels, (std::vector<std::string>{"none"})); const auto refusal = policy::validateCodexModelAvailable(id, route, metadata); EXPECT_EQ(refusal ? refusal->status() : 200, route == "responses" ? 404 : 200); } EXPECT_EQ(fixture.discovery, 0u); EXPECT_EQ(fixture.other, 0u);
}
ASYNC_TEST("a disabled Claude provider cannot add Responses support to a chat-only paid row") {
  Fixture fixture; co_await seed(fixture); const rt::KvValue selection(json::object{{"provider_ids", json::array{"openlux"}}, {"updated_at_ms", 2}}); co_await fixture.store->set(uos::provider::selection::PROVIDER_SELECTION_KV_KEY, selection); uos::provider::selection::resetProviderSelectionCacheForTest(); const rt::GlobalFetchOverride fetch(fixture.fetcher()); AWAIT(refusal, availability()); ASSERT(refusal); EXPECT_EQ(refusal->status(), 404); EXPECT_EQ(fixture.discovery, 0u); EXPECT_EQ(fixture.other, 0u);
}
ASYNC_TEST("a missing credential cannot add Responses support or cause discovery") {
  Fixture fixture; co_await seed(fixture); AWAIT(cleared, credentials::clearClaudeCodeStoredCredential(fixture.store)); static_cast<void>(cleared); const rt::GlobalFetchOverride fetch(fixture.fetcher()); AWAIT(refusal, availability()); ASSERT(refusal); EXPECT_EQ(refusal->status(), 404); EXPECT_EQ(fixture.discovery, 0u); EXPECT_EQ(fixture.other, 0u);
}
ASYNC_TEST("failed discovery does not fabricate support for a Claude-prefixed paid row") {
  Fixture fixture; co_await seed(fixture); fixture.failed = true; const rt::GlobalFetchOverride fetch(fixture.fetcher()); AWAIT(refusal, availability()); ASSERT(refusal); EXPECT_EQ(refusal->status(), 404); EXPECT_EQ(fixture.discovery, 1u); EXPECT_EQ(fixture.other, 0u);
}
namespace {
  rt::Task<void> rotationCase(bool workspace) { Fixture fixture; co_await seed(fixture); const rt::GlobalFetchOverride fetch(fixture.fetcher()); AWAIT(initial, availability()); EXPECT(!initial); co_await rotate(fixture, workspace); fixture.ids = {"claude-new-generation"}; AWAIT(refusal, availability()); ASSERT(refusal); EXPECT_EQ(refusal->status(), 404); EXPECT_EQ(fixture.discovery, 2u); EXPECT_EQ(fixture.other, 0u); }
  rt::Task<void> disabledCase(std::optional<json::value> proof) { Fixture fixture; co_await seed(fixture); fixture.disabled = proof; const rt::GlobalFetchOverride fetch(fixture.fetcher()); AWAIT(metadata, policy::getCodexModelMetadata(MODEL, "responses")); EXPECT(metadata.reasoning.levels.empty()); EXPECT(!metadata.reasoning.defaultLevel); cc::CatalogOptions options; options.kv = fixture.store; AWAIT(models, cc::fetchClaudeCodeModels(options)); ASSERT(models); const auto native = uos::catalog::models::claudeCodeCodexModels(models->models); EXPECT(native[0].at("supported_reasoning_levels").as_array().empty()); EXPECT(!rt::get(native[0], "default_reasoning_level")); AWAIT(capabilities, uos::models::catalog::handleModelCapabilities()); AWAIT(body, capabilities.json()); for (const auto& row : body.at("data").as_array()) if (rt::getString(row, "id") == MODEL) { EXPECT(row.at("supported_reasoning_levels").as_array().empty()); EXPECT(!rt::get(row, "default_reasoning_effort")); } EXPECT_EQ(fixture.other, 0u); }
  rt::Task<void> mismatchedCase(bool workspace) { Fixture fixture; co_await seed(fixture); const rt::GlobalFetchOverride fetch(fixture.fetcher()); cc::CatalogOptions options; options.kv = fixture.store; options.expectedBinding = cc::CatalogOptions::ExpectedBinding{{"api_key", SECRET}, std::nullopt}; AWAIT(initial, cc::fetchClaudeCodeModels(options)); ASSERT(initial); EXPECT_EQ(initial->models[0].supports_disabled_thinking, true); co_await rotate(fixture, workspace); AWAIT(stale, cc::fetchClaudeCodeModels(options)); EXPECT(!stale); EXPECT_EQ(fixture.discovery, 1u); EXPECT_EQ(fixture.other, 0u); }
}
ASYNC_TEST("request metadata cannot reuse IDs after credential rotation") { auto task = rotationCase(false); co_await std::move(task); }
ASYNC_TEST("request metadata cannot reuse IDs after workspace rotation") { auto task = rotationCase(true); co_await std::move(task); }
ASYNC_TEST("disabled support false neither advertises none nor permits its native dispatch") { auto task = disabledCase(json::value(false)); co_await std::move(task); }
ASYNC_TEST("disabled support undefined neither advertises none nor permits its native dispatch") { auto task = disabledCase(std::nullopt); co_await std::move(task); }
ASYNC_TEST("disabled support true neither advertises none nor permits its native dispatch") { auto task = disabledCase(json::value("true")); co_await std::move(task); }
ASYNC_TEST("disabled proof cannot cross a mismatched expected credential binding") { auto task = mismatchedCase(false); co_await std::move(task); }
ASYNC_TEST("disabled proof cannot cross a mismatched expected workspace binding") { auto task = mismatchedCase(true); co_await std::move(task); }
ASYNC_TEST("discovery retains independent disabled booleans, never truthy values or adaptive-effort inference") {
  Fixture fixture; const std::vector<std::optional<json::value>> flags{json::value(true), json::value(false), std::nullopt, json::value("true")}; json::array records; for (std::size_t i = 0; i < flags.size(); ++i) { json::object supported; if (flags[i]) supported["supported"] = *flags[i]; records.push_back(json::object{{"id", "claude-observed-capability-" + std::to_string(i)}, {"capabilities", json::object{{"effort", json::object{{"supported", false}}}, {"thinking", json::object{{"types", json::object{{"disabled", supported}}}}}}}}); } cc::DiscoveryOptions options; options.fetcher = [records](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"data", records}, {"has_more", false}}); }; const uos::provider::claude_code::ClaudeCodeAuthHeaders credential{"oauth", SECRET}; AWAIT(discovered, cc::discoverClaudeCodeModels(credential, std::nullopt, options)); ASSERT_EQ(discovered.models.size(), 4u); EXPECT_EQ(discovered.models[0].supports_disabled_thinking, true); EXPECT_EQ(discovered.models[1].supports_disabled_thinking, false); EXPECT(!discovered.models[2].supports_disabled_thinking); EXPECT(!discovered.models[3].supports_disabled_thinking); const auto native = uos::catalog::models::claudeCodeCodexModels(discovered.models); EXPECT(rt::deepEqual(native[0].at("supported_reasoning_levels"), json::array{json::object{{"effort", "none"}, {"description", "No reasoning"}}}));
}
ASYNC_TEST("a failed discovery refresh keeps stale IDs but withdraws disabled-thinking permission") {
  Fixture fixture; co_await seed(fixture); const rt::GlobalFetchOverride fetch(fixture.fetcher()); cc::CatalogOptions options; options.kv = fixture.store; AWAIT(initial, cc::fetchClaudeCodeModels(options)); ASSERT(initial); EXPECT_EQ(initial->models[0].supports_disabled_thinking, true); fixture.failed = true; options.force = true; AWAIT(stale, cc::fetchClaudeCodeModels(options)); ASSERT(stale); EXPECT_EQ(stale->stale, true); EXPECT_EQ(stale->models[0].id, MODEL); EXPECT(!stale->models[0].supports_disabled_thinking); EXPECT_EQ(fixture.other, 0u);
}
