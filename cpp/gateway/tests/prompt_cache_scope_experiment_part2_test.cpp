// Links: tests/prompt_cache_scope_experiment_part2_test.cpp
// src/provider/removed_provider_telemetry.cpp src/responses_failover_stream.cpp src/config.cpp src/types.cpp
// src/kv.cpp src/utils.cpp src/codex/banked_reset.cpp src/runtime_config.cpp
// src/release.cpp src/models/codex_models.cpp src/storage/kv.cpp src/http.cpp
// src/storage/sqlite_readonly.cpp src/defaults.cpp src/storage/native_kv.cpp src/codex/banked_reset_provider.cpp
// src/codex/reset_settings.cpp src/provider/cerebras_rate_limits.cpp src/auth/relay.cpp
// src/codex/auth.cpp src/codex/capacity_routing.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_capacity.cpp
// src/codex/routing_state.cpp src/codex/routing_mutations.cpp src/authority/native_codex_mutations.cpp src/authority/native_codex_evaluation.cpp
// src/codex/native_auth.cpp src/codex/external_auth.cpp src/codex/rate_limit_429.cpp src/provider/health.cpp
// src/authority/native_codex_429.cpp src/codex/native_auth_transport.cpp src/bounded_response_body.cpp src/runtime/websocket.cpp
// src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp tests/helpers/codex_diff_kv.cpp src/authority/native_codex.cpp
// src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp
// src/codex/auth_refresh.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/overage_settings.cpp
// src/codex/routing_evaluation.cpp src/codex/routing_serial.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp
// src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp
// src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
// src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
// src/provider/selection.cpp src/catalog/promotion.cpp src/catalog/types.cpp src/models/codex_models_fetch.cpp
// src/cache/scope_experiment.cpp src/cache/scope_experiment_targets.cpp src/cache/scope_experiment_model.cpp src/cache/scope_targets.cpp
// tests/helpers/prompt_cache_scope_experiment_harness.cpp src/responses_stream.cpp src/openai_telemetry.cpp src/codex/responses_error.cpp
// Port of tests/prompt-cache-scope-experiment-part2.test.ts using the real promotion implementation.
#include "tests/helpers/prompt_cache_scope_experiment_harness.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
namespace h = uos::tests::helpers::prompt_cache_scope_experiment_harness;
namespace p = uos::catalog::promotion;
namespace rt = uos::rt;
namespace json = uos::json;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  void put(const rt::KvKey& key, const json::value& value) { h::kv->put(key, rt::KvValue(value)); }
  std::string text(const rt::KvKey& key) { return rt::jsonStringify(h::kv->row(key)); }
  rt::Task<p::PromptCacheScopePromotionInput> inputFor(std::string owner) {
    AWAIT(authStamp, h::authPoolVersionstamp()); AWAIT(binding, h::promotionBinding());
    const uos::models::codex_models::PromptCacheScope scope{"responses_implicit_input_text_keyed_cycle_isolated_v5", "account_scoped", "preserved", "independent", std::string(h::MODEL), 3, "live_probe", rt::dateNow()};
    co_return p::PromptCacheScopePromotionInput{std::string(h::MODEL), scope, {h::leaseKey(std::string(h::MODEL)), owner}, authStamp, binding.catalogVersionstamp, binding.runtimeVersionstamp};
  }
}
ASYNC_TEST("a catalog client-version drift stops before the next paid sample") {
  h::HarnessScope scope; h::seed(); h::ScopeTransport transport;
  transport.onInference = [](auto, const auto&, const auto&) { auto snapshot = h::kv->row(h::CODEX_MODELS_KV_KEY); snapshot.as_object()["client_version"] = "0.202.0"; put(h::CODEX_MODELS_KV_KEY, snapshot); return h::sseCompleted(0, 2560); };
  const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(result, h::runExperiment()); EXPECT_EQ(result.status, "inconclusive"); EXPECT_EQ(result.inconclusive_reason, std::optional<std::string>("target_catalog_drift")); EXPECT_EQ(transport.requests.size(), 1u); EXPECT_EQ(transport.refreshes, 0u); EXPECT(!rt::includes(text(h::CODEX_MODELS_KV_KEY), "\"scope\""));
}
ASYNC_TEST("scope promotion rejects unknown dimensions and a renamed target model without mutating the catalog") {
  h::HarnessScope scope; h::seed(); const std::string owner = "test-owner"; const auto key = h::leaseKey(std::string(h::MODEL));
  put(key, json::object{{"owner", owner}, {"lease_until_ms", rt::makeNumber(rt::dateNow() + 60000)}});
  AWAIT(unknownInput, inputFor(owner)); unknownInput.scope.account_slots = "unknown"; AWAIT(unknown, h::promoteCodexPromptCacheScope(h::kv, unknownInput)); EXPECT_EQ(unknown.status, "inconclusive"); EXPECT_EQ(unknown.reason, std::optional<std::string>("invalid_scope"));
  auto snapshot = h::kv->row(h::CODEX_MODELS_KV_KEY); snapshot.as_object()["models"] = json::array{json::object{{"slug", std::string(h::MODEL) + "-renamed"}}}; put(h::CODEX_MODELS_KV_KEY, snapshot);
  AWAIT(renamedInput, inputFor(owner)); AWAIT(renamed, h::promoteCodexPromptCacheScope(h::kv, renamedInput)); EXPECT_EQ(renamed.status, "inconclusive"); EXPECT_EQ(renamed.reason, std::optional<std::string>("model_drift")); EXPECT(!rt::includes(text(h::CODEX_MODELS_KV_KEY), "\"scope\""));
}
ASYNC_TEST("scope promotion publishes a non-default target and preserves the runtime default") {
  h::HarnessScope scope; h::seed(); const auto alternate = std::string(h::MODEL) + "-alternate";
  auto snapshot = h::kv->row(h::CODEX_MODELS_KV_KEY); snapshot.as_object().at("models").as_array().push_back(h::modelRecord(alternate)); put(h::CODEX_MODELS_KV_KEY, snapshot);
  auto runtime = h::kv->row(h::RUNTIME_CONFIG_V2_KEY); runtime.as_object()["codex_models"] = snapshot; put(h::RUNTIME_CONFIG_V2_KEY, runtime); runtime.as_object()["default_model"] = alternate; put(h::RUNTIME_CONFIG_V2_KEY, runtime);
  const std::string owner = "runtime-switch-owner"; const auto key = h::leaseKey(std::string(h::MODEL)); put(key, json::object{{"owner", owner}, {"lease_until_ms", rt::makeNumber(rt::dateNow() + 60000)}});
  const auto beforeSnapshot = text(h::CODEX_MODELS_KV_KEY), beforeRuntime = text(h::RUNTIME_CONFIG_V2_KEY); AWAIT(input, inputFor(owner)); AWAIT(result, h::promoteCodexPromptCacheScope(h::kv, input)); EXPECT_EQ(result.status, "promoted");
  const auto published = h::kv->row(h::CODEX_MODELS_KV_KEY); const auto& first = published.as_object().at("models").as_array()[0]; const auto& provider = first.as_object().at("prompt_cache").as_object().at("providers").as_array()[0]; EXPECT(rt::hasOwn(provider.as_object(), "scope"));
  EXPECT_EQ(rt::getString(h::kv->row(h::RUNTIME_CONFIG_V2_KEY), "default_model"), std::optional(alternate)); EXPECT(text(h::CODEX_MODELS_KV_KEY) != beforeSnapshot); EXPECT(text(h::RUNTIME_CONFIG_V2_KEY) != beforeRuntime); EXPECT(!rt::includes(beforeSnapshot, "\"scope\""));
}
ASYNC_TEST("successful scope promotion atomically extends its same-owner lease") {
  h::HarnessScope scope; h::seed(); const std::string owner = "promotion-handoff-owner"; const auto key = h::leaseKey(std::string(h::MODEL)); const auto before = rt::dateNow(), nearExpiry = before + 1000;
  put(key, json::object{{"owner", owner}, {"lease_until_ms", rt::makeNumber(nearExpiry)}}); AWAIT(input, inputFor(owner)); AWAIT(result, h::promoteCodexPromptCacheScope(h::kv, input)); EXPECT_EQ(result.status, "promoted");
  const auto lease = h::kv->row(key); EXPECT_EQ(rt::getString(lease, "owner"), std::optional(owner)); const auto until = rt::getNumber(lease, "lease_until_ms"); ASSERT(until); EXPECT(*until >= before + 90000); EXPECT(*until > nearExpiry);
}
ASYNC_TEST("scope promotion fences auth-pool drift before catalog publication") {
  h::HarnessScope scope; h::seed(); AWAIT(expectedAuth, h::authPoolVersionstamp()); const std::string owner = "auth-pool-fence-owner"; const auto key = h::leaseKey(std::string(h::MODEL)); put(key, json::object{{"owner", owner}, {"lease_until_ms", rt::makeNumber(rt::dateNow() + 60000)}});
  const auto beforeSnapshot = text(h::CODEX_MODELS_KV_KEY), beforeRuntime = text(h::RUNTIME_CONFIG_V2_KEY); auto pool = h::kv->row(h::CODEX_AUTH_POOL_KV_KEY); auto& account = pool.as_object().at("accounts").as_array()[0].as_object(); account["access_token"] = "rotated-before-promotion-access"; account["refresh_token"] = "rotated-before-promotion-refresh"; put(h::CODEX_AUTH_POOL_KV_KEY, pool);
  AWAIT(input, inputFor(owner)); input.authPoolVersionstamp = expectedAuth; AWAIT(result, h::promoteCodexPromptCacheScope(h::kv, input)); EXPECT_EQ(result.status, "inconclusive"); EXPECT_EQ(result.reason, std::optional<std::string>("auth_pool_drift")); EXPECT_EQ(text(h::CODEX_MODELS_KV_KEY), beforeSnapshot); EXPECT_EQ(text(h::RUNTIME_CONFIG_V2_KEY), beforeRuntime);
}
