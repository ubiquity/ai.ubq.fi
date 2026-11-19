// Links: tests/claude_code_models_test.cpp
// src/health.cpp src/provider/claude_code_access.cpp
// src/admin/codex.cpp src/http.cpp src/provider/presentation.cpp src/request.cpp
// src/cache/prompt_analytics_read.cpp src/cache/scope_experiment_targets.cpp tests/helpers/prompt_cache_scope_experiment_harness.cpp tests/helpers/codex_diff_kv.cpp
// src/cache/prompt_analytics_core.cpp src/debug_routing.cpp src/cache/scope_experiment_model.cpp src/deepseek/waterfall.cpp
// src/codex/auth.cpp src/config.cpp src/provider/selection.cpp src/codex/account_routing.cpp
// src/codex/routing_state.cpp src/models/catalog.cpp src/auth/relay.cpp src/models/metadata.cpp
// src/authority/native_codex_mutations.cpp src/input_normalization.cpp src/runtime_config.cpp src/codex/experiment.cpp
// src/codex/dispatch.cpp src/types.cpp src/authority/native_codex.cpp src/provider/cerebras.cpp
// src/authority/native_codex_capacity.cpp src/catalog/types.cpp src/utils.cpp src/request_policy.cpp
// src/provider/capacity_events.cpp src/codex/routing_evaluation.cpp src/models/codex_models.cpp src/authority/native_codex_contracts.cpp
// src/provider/lithos_models.cpp src/provider/metered.cpp src/provider/claude_code_models.cpp src/provider/surplus.cpp
// src/cache/telemetry_gate.cpp src/recent_model_context.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
// src/authority/native_codex_evaluation.cpp src/codex/external_auth.cpp src/catalog/store.cpp src/provider/surplus_claude_request.cpp
// src/codex/capacity_routing.cpp src/kv.cpp src/deepseek/index.cpp src/catalog/index.cpp
// src/models/openrouter_models.cpp src/bounded_response_body.cpp src/provider/lithos.cpp src/cache/scope_targets.cpp
// src/provider/claude_code.cpp src/codex/overage_settings.cpp src/provider/claude_code_accounts.cpp src/provider/capacity_parse.cpp
// src/catalog/models.cpp src/responses_stream.cpp src/models/codex_models_whitelist.cpp tests/helpers/admin_auth_harness.cpp
// src/defaults.cpp src/provider/cerebras_rate_limits.cpp src/provider/anthropic_replay.cpp src/provider/claude_code_credentials.cpp
// src/codex/auth_refresh.cpp src/provider/openrouter.cpp src/cache/scope_experiment.cpp src/models/codex_models_fetch.cpp
// src/provider/capacity.cpp src/provider/capacity_rollups.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_quota.cpp
// src/provider/capacity_history.cpp src/provider/lithos_rate_limits.cpp src/admin/api_keys.cpp src/models/codex_models_availability.cpp
// src/codex/banked_reset.cpp src/openai_telemetry.cpp src/api_key_policy.cpp src/provider/health.cpp
// src/provider/capacity_contract.cpp src/api_keys.cpp src/provider/claude_code_usage_schema.cpp src/release.cpp
// src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/provider/surplus_claude_chat_request.cpp src/admin/api_key_audit.cpp
// src/codex/native_auth.cpp src/provider/claude_capacity_history.cpp src/codex/reset_settings.cpp src/paid_fallback/ledger_state.cpp
// src/authority/native_api_key_audit.cpp src/authority/native_paid_contracts.cpp src/provider/anthropic_chat_responses.cpp src/codex/banked_reset_pool.cpp
// src/provider/capacity_analytics.cpp src/metered_quota.cpp src/codex/rate_limit_429.cpp src/paid_fallback/ledger_stats.cpp
// src/paid_fallback/index.cpp src/codex/banked_reset_claim.cpp src/storage/kv.cpp src/paid_fallback/ledger_admission.cpp
// src/authority/native_paid.cpp src/provider/anthropic_responses.cpp src/authority/native_paid_policy.cpp src/paid_fallback/ledger_backfill.cpp
// src/provider/anthropic_response_wire.cpp src/responses_failover_stream.cpp src/catalog/promotion.cpp src/storage/sqlite_readonly.cpp
// src/codex/banked_reset_submission.cpp src/paid_fallback/ledger_settlement.cpp src/authority/native_codex_429.cpp
// src/provider/removed_provider_telemetry.cpp src/codex/banked_reset_provider.cpp src/codex/native_auth_transport.cpp src/provider/anthropic_chat_stream.cpp
// src/storage/native_kv.cpp src/codex/responses_error.cpp src/runtime/websocket.cpp src/admin/claude_code.cpp
// Remaining catalog/admin integration cases of tests/claude-code-models.test.ts; direct discovery,
// stale/cohort/late-generation and cursor cases remain covered by claude_code_models_differential_test.cpp.
#include "uos/admin/claude_code.hpp"
#include "uos/admin/codex.hpp"
#include "uos/catalog/index.hpp"
#include "uos/catalog/models.hpp"
#include "uos/catalog/types.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/models/catalog.hpp"
#include "uos/models/codex_models_whitelist.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/claude_code.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/test_async.hpp"
namespace rt = uos::rt;
namespace json = uos::json;
namespace cc = uos::provider::claude_code_models;
namespace credentials = uos::provider::claude_code_credentials;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c4-claude-models"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<rt::Kv> store = std::make_shared<rt::MemoryKv>(rt::dateNow);
    Fixture() { uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(store); cc::resetClaudeCodeModelsCacheForTest(); credentials::resetClaudeCodeCredentialCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::models::openrouter_models::resetOpenRouterModelsCacheForTest(); }
    ~Fixture() { uos::kv::setKvForTest(nullptr); cc::resetClaudeCodeModelsCacheForTest(); credentials::resetClaudeCodeCredentialCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::models::openrouter_models::resetOpenRouterModelsCacheForTest(); uos::config::config.codexAuthJsonB64 = saved; }
  };
  json::object record(const std::string& id) { return {{"id", id}, {"type", "model"}, {"display_name", "Observed " + id}, {"created_at", "2026-10-01T00:00:00Z"}}; }
  rt::Response page(const std::vector<std::string>& ids) { json::array rows; for (const auto& id : ids) rows.push_back(record(id)); return rt::Response::json(json::object{{"data", rows}, {"has_more", false}, {"last_id", ids.empty() ? json::value(nullptr) : json::value(ids.back())}}); }
}
ASYNC_TEST("observed reasoning, context and output capabilities are preserved without model-name tier guesses") {
  Fixture fixture; auto source = record("claude-capability-fixture"); source["max_input_tokens"] = 123456; source["max_tokens"] = 8192;
  source["capabilities"] = json::object{{"effort", json::object{{"supported", true}, {"low", json::object{{"supported", true}}}, {"high", json::object{{"supported", false}}}, {"future_native", json::object{{"supported", true}}}, {"none", json::object{{"supported", true}}}}}, {"thinking", json::object{{"types", json::object{{"adaptive", json::object{{"supported", true}}}, {"disabled", json::object{{"supported", false}}}}}}}, {"image_input", json::object{{"supported", true}}}, {"pdf_input", json::object{{"supported", false}}}};
  cc::DiscoveryOptions options; options.fetcher = [source](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"data", json::array{source}}, {"has_more", false}}); }; const uos::provider::claude_code::ClaudeCodeAuthHeaders credential{"oauth", "fixture-model-token"}; AWAIT(snapshot, cc::discoverClaudeCodeModels(credential, std::nullopt, options)); ASSERT_EQ(snapshot.models.size(), 1u); const auto& model = snapshot.models[0]; EXPECT_EQ(cc::claudeCodeReasoningLevels(model), (std::vector<std::string>{"low", "future_native"})); EXPECT_EQ(model.max_output_tokens, 8192); EXPECT_EQ(model.supports_image_input, true); EXPECT_EQ(model.supports_pdf_input, false);
  const auto native = uos::catalog::models::claudeCodeCodexModels(snapshot.models); EXPECT(!rt::get(native[0], "default_reasoning_level")); EXPECT(rt::deepEqual(native[0].at("supported_reasoning_levels"), json::array{json::object{{"effort", "low"}, {"description", "low"}}, json::object{{"effort", "future_native"}, {"description", "future_native"}}}));
  json::object body{{"model", model.id}, {"input", "fixture"}, {"reasoning", json::object{{"effort", "future_native"}}}, {"max_output_tokens", 321}}; EXPECT(!uos::provider::claude_code::buildClaudeCodeRequest(body)); uos::provider::claude_code::BuildClaudeCodeRequestOptions nativeOptions; nativeOptions.nativeReasoningEfforts = model.supported_reasoning_levels; const auto built = uos::provider::claude_code::buildClaudeCodeRequest(body, nativeOptions); ASSERT(built); EXPECT(rt::deepEqual(built->body.at("output_config"), json::object{{"effort", "future_native"}})); EXPECT_EQ(rt::getNumber(built->body, "max_tokens"), 321); body["reasoning"] = json::object{{"effort", "high"}}; EXPECT(!uos::provider::claude_code::buildClaudeCodeRequest(body, nativeOptions));
}
ASYNC_TEST("Successful admin credential validation seeds the full discovered catalog without a second fetch") {
  Fixture fixture; std::size_t calls = 0; const rt::Fetch fetcher([&calls](rt::RequestInfo, rt::RequestInit) { ++calls; return page({"claude-upload-observed"}); }); const rt::GlobalFetchOverride fetch(fetcher); rt::RequestInit init; init.method = "POST"; const auto synthetic = std::string("sk") + "-ant-oat-fixture-catalog"; init.body = rt::jsonStringify(json::object{{"credential", synthetic}}); rt::Request req("https://gateway.test/admin/providers/claude-code/auth", init); AWAIT(response, uos::admin::claude_code::handleAdminClaudeCodeAuthSet(req)); EXPECT_EQ(response.status(), 200); cc::CatalogOptions options; options.kv = fixture.store; AWAIT(snapshot, cc::fetchClaudeCodeModels(options)); ASSERT(snapshot); EXPECT_EQ(snapshot->models[0].id, "claude-upload-observed"); EXPECT_EQ(calls, 1u);
}
ASYNC_TEST("Observed Claude IDs feed admin counts, whitelist and both client model contracts") {
  Fixture fixture; credentials::StoreClaudeCodeStoredCredentialInput credential{.secret = "fixture-model-token"}; AWAIT(stored, credentials::storeClaudeCodeStoredCredential(fixture.store, credential)); ASSERT(stored); const rt::KvValue selection(json::object{{"provider_ids", json::array{"claude_code"}}, {"updated_at_ms", 1}}); co_await fixture.store->set(uos::provider::selection::PROVIDER_SELECTION_KV_KEY, selection); const rt::KvValue generation("catalog-test-generation"); co_await fixture.store->set(uos::catalog::types::CODEX_CATALOG_AUTH_GENERATION_KEY, generation);
  const rt::Fetch fetcher([](rt::RequestInfo input, rt::RequestInit) { std::string url; if (const auto* text = std::get_if<std::string>(&input)) url = *text; else if (const auto* parsed = std::get_if<rt::URL>(&input)) url = parsed->href(); else url = std::get<rt::Request>(input).url(); if (rt::startsWith(url, "https://api.anthropic.com/v1/models")) return page({"claude-discovered-a", "claude-discovered-b"}); return rt::Response(nullptr, rt::ResponseInit{.status = 503}); }); const rt::GlobalFetchOverride fetch(fetcher);
  AWAIT(catalog, uos::models::catalog::buildModelCatalogSnapshot()); EXPECT_EQ(catalog.sources.get("claude_code")->count, 2); EXPECT_EQ(catalog.sources.get("claude_code")->status, "available");
  AWAIT(admin, uos::admin::codex::handleAdminModelsCatalogGet()); AWAIT(adminBody, admin.json()); bool observed = false; for (const auto& row : adminBody.at("data").at("models").as_array()) if (rt::getString(row, "id") == "claude-discovered-a") observed = true; EXPECT(observed);
  AWAIT(providers, uos::admin::codex::handleAdminProviderSelectionGet()); AWAIT(providerBody, providers.json()); for (const auto& row : providerBody.at("data").at("providers").as_array()) if (rt::getString(row, "id") == "claude_code") EXPECT_EQ(rt::getNumber(row, "model_count"), 2);
  const rt::KvValue whitelist(json::object{{"model_ids", json::array{"claude-discovered-b"}}, {"updated_at_ms", 1}}); co_await fixture.store->set(uos::models::codex_models_whitelist::CODEX_MODELS_WHITELIST_KV_KEY, whitelist); AWAIT(standard, uos::models::catalog::handleModels()); AWAIT(standardBody, standard.json()); EXPECT_EQ(rt::getString(standardBody, "object"), "list"); const auto& rows = standardBody.at("data").as_array(); ASSERT_EQ(rows.size(), 1u); EXPECT_EQ(rt::getString(rows[0], "id"), "claude-discovered-b"); auto keys = rt::objectKeys(rows[0].as_object()); rt::sort(keys); EXPECT_EQ(keys, (std::vector<std::string>{"created", "id", "object", "owned_by"}));
  rt::Request request("https://gateway.test/v1/models?client_version=0.130.0"); AWAIT(native, uos::catalog::index::handleCodexCatalogModels(request, "0.130.0")); EXPECT_EQ(native.status(), 200); AWAIT(nativeBody, native.json()); const auto& models = nativeBody.at("models").as_array(); ASSERT_EQ(models.size(), 1u); EXPECT_EQ(rt::getString(models[0], "slug"), "claude-discovered-b"); EXPECT(!rt::get(models[0], "context_window"));
  AWAIT(capabilities, uos::models::catalog::handleModelCapabilities()); AWAIT(capabilityBody, capabilities.json()); EXPECT_EQ(rt::getString(capabilityBody.at("data").as_array()[0], "context_source"), "unknown");
  const rt::Fetch fail([](rt::RequestInfo, rt::RequestInit) { return rt::Response(nullptr, rt::ResponseInit{.status = 503}); }); const rt::GlobalFetchOverride failed(fail); cc::CatalogOptions options; options.force = true; AWAIT(refreshed, cc::fetchClaudeCodeModels(options)); static_cast<void>(refreshed); AWAIT(stale, uos::models::catalog::buildModelCatalogSnapshot()); EXPECT_EQ(stale.sources.get("claude_code")->status, "unavailable"); EXPECT_EQ(stale.sources.get("claude_code")->stale, true); EXPECT_EQ(stale.sources.get("claude_code")->count, 2);
}
