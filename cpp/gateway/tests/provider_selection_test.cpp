// Links: tests/provider_selection_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/admin/api_key_audit.cpp src/admin/api_key_mutations.cpp src/admin/api_keys.cpp src/admin/capacity_observation_sync.cpp src/admin/chart_source_proxy.cpp src/admin/claude_code.cpp src/admin/codex.cpp src/admin/codex_reset.cpp src/admin/defaults.cpp src/admin/error_log.cpp
// src/admin/kernel.cpp src/admin/sentinel_incident.cpp src/agent_messages.cpp src/analytics.cpp src/api_key_policy.cpp src/api_keys.cpp src/auth/index.cpp src/auth/local_admin.cpp src/auth/local_development_key.cpp src/auth/passkeys.cpp
// src/auth/passport.cpp src/auth/passport_sessions.cpp src/auth/relay.cpp src/authority/native_api_key.cpp src/authority/native_api_key_audit.cpp src/authority/native_api_key_policy.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
// src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp src/cache/kv_migration_base.cpp src/cache/kv_migration_paid_fallback.cpp
// src/cache/kv_migration_usage.cpp src/cache/kv_migration_validation.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/scope_experiment.cpp src/cache/scope_experiment_model.cpp src/cache/scope_experiment_targets.cpp src/cache/scope_targets.cpp src/cache/telemetry_gate.cpp
// src/catalog/index.cpp src/catalog/models.cpp src/catalog/promotion.cpp src/catalog/store.cpp src/catalog/types.cpp src/chat/completions.cpp src/chat/envelope.cpp src/chat/stream_translation.cpp src/codex/account_routing.cpp src/codex/auth.cpp
// src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset_submission.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp
// src/codex/index.cpp src/codex/manual_reset.cpp src/codex/manual_reset_state.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/quota.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp
// src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/codex/supervisor.cpp src/codex/supervisor_brief.cpp src/codex/supervisor_config.cpp
// src/codex/supervisor_follow.cpp src/codex/supervisor_inventory.cpp src/codex/supervisor_log.cpp src/codex/supervisor_secret.cpp src/codex/supervisor_transport.cpp src/config.cpp src/debug_routing.cpp src/decisions/handlers.cpp src/deepseek/chat_projection.cpp src/deepseek/context_admission_failure.cpp
// src/deepseek/flash_aliases.cpp src/deepseek/forwarded_payload_policy.cpp src/deepseek/handlers.cpp src/deepseek/index.cpp src/deepseek/responses.cpp src/deepseek/responses_payload.cpp src/deepseek/responses_stream.cpp src/deepseek/stream.cpp src/deepseek/waterfall.cpp src/deepseek/waterfall_handler.cpp
// src/defaults.cpp src/embeddings/handlers.cpp src/embeddings/jobs.cpp src/embeddings/ledger.cpp src/embeddings/voyage.cpp src/handler/admission.cpp src/handler/http.cpp src/handler/index.cpp src/handler/terminal_log.cpp src/handler/terminal_route.cpp
// src/health.cpp src/http.cpp src/images.cpp src/inference_admission.cpp src/inference_deadline.cpp src/input_normalization.cpp src/kernel/attestation.cpp src/kernel/policy_queue.cpp src/kernel/quota_reservations.cpp src/kernel/quota_reserve.cpp
// src/kernel/quota_v2.cpp src/kernel/quota_write.cpp src/kernel/usage.cpp src/kv.cpp src/kv_conversion/profile_json.cpp src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp src/lib/jev_compaction/state.cpp src/lib/jev_compaction/types.cpp
// src/live/calls.cpp src/live/handler.cpp src/live/routes.cpp src/live/sideband.cpp src/live/upstream.cpp src/metered_quota.cpp src/models/catalog.cpp src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp
// src/models/codex_models_whitelist.cpp src/models/metadata.cpp src/models/openrouter_models.cpp src/openai.cpp src/openai_schema.cpp src/openai_telemetry.cpp src/optional_telemetry_queue.cpp src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp
// src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/removed_provider.cpp src/paid_fallback/rollups.cpp src/paid_fallback/routing.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp
// src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
// src/provider/cerebras.cpp src/provider/cerebras_handlers.cpp src/provider/cerebras_rate_limits.cpp src/provider/cerebras_responses.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp src/provider/claude_code_access.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
// src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_handlers.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos_streams.cpp src/provider/metered.cpp
// src/provider/openrouter.cpp src/provider/openrouter_handlers.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter_streams.cpp src/provider/openrouter_web_tools.cpp src/provider/presentation.cpp src/provider/removed_provider_circuit.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/stream_relay.cpp
// src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/quota_projection.cpp src/recent_model_context.cpp src/release.cpp src/request.cpp src/request_policy.cpp src/responses_attempts.cpp src/responses_buffered.cpp
// src/responses_failover_stream.cpp src/responses_handler.cpp src/responses_native_headers.cpp src/responses_request.cpp src/responses_stream.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/websocket.cpp src/runtime_config.cpp src/sentinel/incident_outbox.cpp
// src/sentinel/replay_admin.cpp src/sentinel/replay_envelope.cpp src/sentinel/replay_limits.cpp src/sentinel/replay_model.cpp src/sentinel/replay_observation.cpp src/sentinel/replay_read.cpp src/sentinel/replay_retention.cpp src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_retention_schema.cpp
// src/sentinel/replay_store.cpp src/sentinel/upstream_capture.cpp src/static.cpp src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/systemone/handlers.cpp src/types.cpp src/upstream_wire.cpp
// src/utils.cpp tests/helpers/codex_diff_kv.cpp
// Source: tests/provider-selection.test.ts; all 21 original registrations retain source order.
// Picker authentication uses real handler/index; the switched-off Lithos case uses real handleResponses.
// Originals: isolated Bun1.4.0, synthetic auth, mocked fetch/KV, 21/21 passed; no handler stand-ins.
#include "uos/responses_handler.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include <array>
#include "uos/handler/index.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/test_async.hpp"

namespace {
  struct T2PublicAssets {
    std::vector<std::string> createdPaths;
    T2PublicAssets() {
      const auto temporary = uos::rt::env::get("TMPDIR");
      const auto directory = uos::rt::process::cwd();
      if (!temporary || !directory.starts_with(uos::rt::path::dirname(*temporary) + "/"))
        throw uos::rt::JsError("T2 public assets require the wrapper task workdir");
      const auto root = uos::rt::path::resolve(uos::rt::path::dirname(__FILE__), "../../..");
      const std::array<std::string, 3> names{"static", "favicon.png", "favicon-32.png"};
      for (const auto& name : names) {
        const auto target = uos::rt::path::join(root, name);
        const auto assetPath = uos::rt::path::join(directory, name);
        if (uos::rt::fs::existsSync(assetPath)) {
          if (uos::rt::fs::realpathSync(assetPath) != uos::rt::fs::realpathSync(target))
            throw uos::rt::JsError("T2 public asset fixture refuses an unexpected existing path");
        } else {
          uos::rt::fs::symlinkSync(target, assetPath);
          createdPaths.push_back(assetPath);
        }
      }
    }
    ~T2PublicAssets() {
      for (const auto& assetPath : createdPaths) uos::rt::fs::rmSync(assetPath);
    }
  };
  T2PublicAssets& t2PublicAssets() {
    static T2PublicAssets assets;
    return assets;
  }
  struct T2IngressScope {
    uos::config::Config savedConfig = uos::config::config;
    uos::rt::env::ScopedOverride credentials{{"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    uos::rt::GlobalFetchOverride blocked{uos::rt::Fetch([](uos::rt::RequestInfo, uos::rt::RequestInit) -> uos::rt::Response {
      throw uos::rt::JsError("T2 ingress fixture forbids an unstubbed upstream call");
    })};
    explicit T2IngressScope(std::shared_ptr<uos::rt::Kv> database = nullptr) {
      (void)t2PublicAssets();
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.authTokens.clear();
      uos::config::config.adminTokens.clear();
      if (!database) database = std::make_shared<uos::rt::MemoryKv>(uos::rt::dateNow);
      uos::kv::setKvForTest(std::move(database));
    }
    ~T2IngressScope() {
      uos::kv::setKvForTest(nullptr);
      uos::config::config = savedConfig;
    }
  };
}
#include "uos/admin/codex.hpp"
#include "uos/catalog/store.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/health.hpp"
#include "uos/kv.hpp"
#include "uos/models/catalog.hpp"
#include "uos/models/codex_models_whitelist.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/provider/presentation.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "tests/helpers/codex_diff_kv.hpp"
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace sel = uos::provider::selection;
namespace presentation = uos::provider::presentation;
namespace admin = uos::admin::codex;
namespace catalog = uos::models::catalog;
namespace diff = uos::tests::helpers::codex_diff_kv;
namespace codexAuth = uos::codex::auth;
namespace openRouterModels = uos::models::openrouter_models;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  void expectJson(const json::value& actual, const json::value& expected, const std::string& label, const char* file, int line) {
    if (!rt::deepEqual(actual, expected)) rt::test::report(file, line, label + "\n      actual: " + rt::jsonStringify(actual) + "\n      expected: " + rt::jsonStringify(expected), false);
  }
#define EXPECT_JSON(actual, expected, label) expectJson((actual), (expected), (label), __FILE__, __LINE__)

  class SelectionKv : public diff::DiffKv {
  public:
    bool failReads = false;
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions = {}) override {
      if (failReads) throw rt::JsError("KV read failed");
      auto entry = entryOf(key);
      co_await rt::microtask();
      co_return entry;
    }
    void put(const rt::KvKey& key, const json::value& value) { rows.set(diff::keyOf(key), Row{key, rt::KvValue(value), "00000000000000000001"}); }
    json::value storedSelection() const { return row(sel::PROVIDER_SELECTION_KV_KEY); }
    void seedSelection(const std::vector<std::string>& providerIds, double updatedAtMs = 1'700'000'000'000) {
      put(sel::PROVIDER_SELECTION_KV_KEY, json::object{{"provider_ids", json::value_from(providerIds)}, {"updated_at_ms", updatedAtMs}});
    }
  };
  class DeferredSelectionKv final : public SelectionKv {
  public:
    rt::Resolvers<void> readStarted = rt::withResolvers<void>();
    rt::Resolvers<void> releaseRead = rt::withResolvers<void>();
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      auto task = SelectionKv::get(key, options);
      auto entry = co_await std::move(task);
      readStarted.resolve();
      co_await releaseRead.promise.wait();
      co_return entry;
    }
  };
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{
      {"HOME", rt::env::get("HOME").value_or("/nonexistent/provider-selection")},
      {"TMPDIR", rt::env::get("TMPDIR").value_or("/tmp")}}};
    std::string savedAuth = uos::config::config.codexAuthJsonB64;
    rt::Fetch noNetwork{[](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("provider-selection test forbids live fetch"); }};
    rt::GlobalFetchOverride globalFetch{noNetwork};
    explicit Fixture(std::shared_ptr<rt::Kv> database) {
      uos::config::config.codexAuthJsonB64.clear();
      reset();
      uos::kv::setKvForTest(std::move(database));
      openRouterModels::setOpenRouterModelsFetchForTest(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"data", json::array{}}}); }));
    }
    ~Fixture() {
      uos::kv::setKvForTest(nullptr);
      openRouterModels::setOpenRouterModelsFetchForTest({});
      reset();
      uos::config::config.codexAuthJsonB64 = savedAuth;
    }
    static void reset() {
      sel::resetProviderSelectionCacheForTest();
      codexAuth::resetCodexAuthCacheForTest();
      uos::runtime_config::resetRuntimeConfigCacheForTest();
      uos::catalog::store::resetCodexCatalogMemoForTest();
      openRouterModels::resetOpenRouterModelsCacheForTest();
      uos::provider::metered::resetMeteredModelsCacheForTest();
      uos::provider::surplus::resetSurplusModelsCacheForTest();
      uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest();
      uos::provider::claude_code_credentials::resetClaudeCodeCredentialCacheForTest();
      uos::provider::health::resetProviderHealthThrottleForTest();
    }
  };
  json::value selectionJson(const std::optional<sel::ProviderSelection>& selection) { return selection ? json::value_from(*selection) : json::value(nullptr); }
  json::value eligibilityJson(const sel::CodexAccountEligibility& eligibility) {
    json::object result;
    std::visit([&result](const auto& arm) { result["kind"] = std::string(arm.kind); }, eligibility);
    if (const auto* only = std::get_if<sel::CodexAccountEligibilityOnly>(&eligibility)) result["hashes"] = json::value_from(only->hashes);
    return result;
  }
  rt::Request selectionRequest(const json::value& body) {
    rt::RequestInit init;
    init.method = "POST";
    init.headers = rt::Headers{{"Content-Type", "application/json"}};
    init.body = rt::jsonStringify(body);
    return rt::Request("https://ai.ubq.fi/admin/providers/selection", init);
  }
  catalog::ModelCatalogSnapshot catalogFixture() {
    catalog::ModelCatalogSnapshot snapshot;
    snapshot.models = {
      {.id = "gpt-5.6-sol", .providers = {{"codex", "openai", {"/v1/responses"}}}},
      {.id = "gpt-5.6-sol", .providers = {{"surplus", "moonshot", {"/v1/responses"}}}},
      {.id = "kimi-k2", .providers = {{"surplus", "moonshot", {"/v1/chat/completions"}}}},
      {.id = "deepseek-flash", .providers = {{"deepseek", "deepseek", {"/v1/chat/completions", "/v1/responses"}}}}
    };
    snapshot.sources.set("claude_code", {.status = "unavailable", .count = 0, .updated_at_ms = std::nullopt, .configured = false});
    snapshot.sources.set("codex", {.status = "available", .count = 1, .updated_at_ms = 1});
    snapshot.sources.set("openlux", {.status = "unavailable", .count = 0, .updated_at_ms = std::nullopt});
    snapshot.sources.set("surplus", {.status = "available", .count = 2, .updated_at_ms = 2});
    snapshot.sources.set("deepseek", {.status = "available", .count = 1, .updated_at_ms = std::nullopt, .configured = true});
    snapshot.sources.set("cerebras", {.status = "unavailable", .count = 0, .updated_at_ms = std::nullopt, .configured = false});
    snapshot.sources.set("lithos", {.status = "unavailable", .count = 0, .updated_at_ms = std::nullopt, .configured = false});
    snapshot.sources.set("openrouter", {.status = "available", .count = 2, .updated_at_ms = 3});
    return snapshot;
  }
  admin::CatalogDependencies fixtureDependencies() {
    admin::CatalogDependencies dependencies;
    dependencies.buildCatalog = [] { return rt::fulfilledTask(catalogFixture()); };
    return dependencies;
  }
  void seedCodexSnapshot(SelectionKv& database, const std::vector<std::string>& ids) {
    json::array models;
    for (const auto& id : ids) models.push_back(json::object{{"slug", id}, {"supported_reasoning_levels", json::array{"none", "low"}}});
    const json::object snapshot{{"source", "codex_cli"}, {"client_version", "0.126.0"}, {"updated_at_ms", 1'700'000'000'000.0}, {"models", models}};
    database.put(codexAuth::CODEX_MODELS_KV_KEY, snapshot);
    database.put(uos::runtime_config::RUNTIME_CONFIG_V2_KEY, json::object{{"version", 2}, {"default_model", ids.empty() ? "gpt-5-fixture" : ids.front()}, {"default_reasoning_effort", "low"}, {"codex_models", snapshot}, {"updated_at_ms", 1'700'000'000'000.0}});
    uos::runtime_config::resetRuntimeConfigCacheForTest();
  }
  rt::Task<std::vector<std::string>> listModelIds() {
    AWAIT(response, catalog::handleModels());
    EXPECT_EQ(response.status(), 200);
    AWAIT(payload, response.json());
    std::vector<std::string> result;
    if (const auto* data = rt::getArray(payload, "data")) for (const auto& row : *data) result.push_back(std::string(rt::getString(row, "id").value_or("")));
    co_return result;
  }
  template<class T> void appendIds(std::vector<std::string>& target, const T& ids) { for (const auto& id : ids) target.push_back(std::string(id)); }
  struct CatalogProvider { std::string id; friend bool operator==(const CatalogProvider&, const CatalogProvider&) = default; };
  struct CatalogEntry { std::string id; std::vector<CatalogProvider> providers; friend bool operator==(const CatalogEntry&, const CatalogEntry&) = default; };
  std::string tokenWithPayload(const json::value& payload) {
    return rt::toBase64Url(rt::encodeUtf8(rt::jsonStringify(json::object{{"alg", "none"}}))) + "." + rt::toBase64Url(rt::encodeUtf8(rt::jsonStringify(payload))) + ".signature";
  }
  void seedCodexAuthPool(SelectionKv& database) {
    const auto firstToken = tokenWithPayload(json::object{{"https://api.openai.com/profile", json::object{{"email", "first@example.com"}}}});
    database.put(codexAuth::CODEX_AUTH_POOL_KV_KEY, json::object{{"accounts", json::array{
      json::object{{"access_token", firstToken}, {"refresh_token", "refresh-one"}, {"account_id", "account-one"}, {"updated_at_ms", 1}},
      json::object{{"access_token", "not-a-jwt"}, {"refresh_token", "refresh-two"}, {"account_id", "account-two"}, {"updated_at_ms", 1}}}}, {"updated_at_ms", 1}});
    codexAuth::resetCodexAuthCacheForTest();
  }
}

TEST("provider selection normalizes to the roster order without duplicates") {
  EXPECT_EQ(sel::normalizeSelectedProviderIds(json::array{"openlux", "codex", "openlux"}), (std::vector<std::string>{"codex", "openlux"}));
  EXPECT_EQ(sel::normalizeSelectedProviderIds(json::array{" codex "}), (std::vector<std::string>{"codex"}));
  EXPECT_EQ(sel::normalizeSelectedProviderIds(json::array{"unknown", "surplus"}), (std::vector<std::string>{"surplus"}));
  EXPECT(sel::normalizeSelectedProviderIds(json::array{1, nullptr, json::object{}}).empty());
}
TEST("a stored provider selection keeps its known ids and drops retired ones") {
  EXPECT_JSON(selectionJson(sel::normalizeProviderSelection(json::object{{"provider_ids", json::array{"surplus", "retired"}}, {"updated_at_ms", 5}})), (json::object{{"provider_ids", json::array{"surplus"}}, {"updated_at_ms", 5}}), "known ids survive");
  EXPECT(!sel::normalizeProviderSelection(json::object{{"provider_ids", "codex"}, {"updated_at_ms", 5}}));
  EXPECT(!sel::normalizeProviderSelection(json::object{{"provider_ids", json::array{7}}, {"updated_at_ms", 5}}));
  EXPECT(!sel::normalizeProviderSelection(json::object{{"provider_ids", json::array{}}, {"updated_at_ms", 0}}));
  EXPECT(!sel::normalizeProviderSelection(rt::KvValue(nullptr)));
}
TEST("an empty or absent selection keeps every provider eligible") {
  for (const auto provider : sel::SELECTABLE_PROVIDER_IDS) {
    EXPECT(sel::isProviderEnabled(provider, std::nullopt));
    EXPECT(sel::isProviderEnabled(provider, sel::ProviderSelection{{}, 1}));
  }
  const sel::ProviderSelection narrowed{{"surplus"}, 1};
  EXPECT(sel::isProviderEnabled("surplus", narrowed));
  EXPECT(!sel::isProviderEnabled("codex", narrowed));
  EXPECT(sel::providerSelectionIsActive(narrowed));
  EXPECT(!sel::providerSelectionIsActive(std::nullopt));
  EXPECT(!sel::providerSelectionIsActive(sel::ProviderSelection{{}, 1}));
}
ASYNC_TEST("a subscription selection narrows the Codex tier instead of switching it off") {
  const std::string firstAccount = "account-one", secondAccount = "account-two";
  AWAIT(firstHash, sel::codexSubscriptionHash(firstAccount));
  AWAIT(secondHash, sel::codexSubscriptionHash(secondAccount));
  const auto first = sel::codexSubscriptionSelectionId(firstHash), second = sel::codexSubscriptionSelectionId(secondHash);
  EXPECT(first != second);
  AWAIT(memoizedHash, sel::codexSubscriptionHash(firstAccount));
  EXPECT_EQ(first, "codex:" + memoizedHash);
  EXPECT(sel::isProviderSelectionId(std::string_view(first)));
  EXPECT(!sel::isProviderSelectionId(std::string_view("codex:not-a-hash")));
  EXPECT(!sel::isProviderSelectionId(std::string_view("codex:")));
  const sel::ProviderSelection onlySecond{{second}, 1};
  EXPECT(sel::isProviderEnabled("codex", onlySecond));
  EXPECT(!sel::isProviderEnabled("surplus", onlySecond));
  EXPECT_JSON(eligibilityJson(sel::codexAccountEligibility(onlySecond)), (json::object{{"kind", "only"}, {"hashes", json::array{secondHash}}}), "subscription eligibility");
  EXPECT(sel::isCodexSubscriptionEnabled(secondHash, onlySecond));
  EXPECT(!sel::isCodexSubscriptionEnabled(firstHash, onlySecond));
  EXPECT_JSON(eligibilityJson(sel::codexAccountEligibility(std::nullopt)), (json::object{{"kind", "all"}}), "absent selection");
  EXPECT_JSON(eligibilityJson(sel::codexAccountEligibility(sel::ProviderSelection{{}, 1})), (json::object{{"kind", "all"}}), "empty selection");
  EXPECT_JSON(eligibilityJson(sel::codexAccountEligibility(sel::ProviderSelection{{"codex"}, 1})), (json::object{{"kind", "all"}}), "umbrella");
  EXPECT_JSON(eligibilityJson(sel::codexAccountEligibility(sel::ProviderSelection{{"surplus"}, 1})), (json::object{{"kind", "none"}}), "tier disabled");
  EXPECT(!sel::isProviderEnabled("codex", sel::ProviderSelection{{"surplus"}, 1}));
}
TEST("the codex umbrella absorbs subscription ids so storage stays unambiguous") {
  const auto subscription = sel::codexSubscriptionSelectionId(std::string(64, 'a')), other = sel::codexSubscriptionSelectionId(std::string(64, 'b'));
  EXPECT_EQ(sel::normalizeSelectedProviderIds(json::array{other, "codex", subscription}), (std::vector<std::string>{"codex"}));
  EXPECT_EQ(sel::normalizeSelectedProviderIds(json::array{other, "surplus", subscription}), (std::vector<std::string>{"surplus", subscription, other}));
  EXPECT_EQ(sel::normalizeSelectedProviderIds(json::array{"codex:not-a-hash", "codex"}), (std::vector<std::string>{"codex"}));
  EXPECT(sel::normalizeSelectedProviderIds(json::array{"codex:not-a-hash"}).empty());
  EXPECT_JSON(selectionJson(sel::normalizeProviderSelection(json::object{{"provider_ids", json::array{subscription, "codex"}}, {"updated_at_ms", 5}})), (json::object{{"provider_ids", json::array{"codex"}}, {"updated_at_ms", 5}}), "umbrella storage");
}
TEST("a catalog row keeps only the providers that are still active") {
  const std::vector<CatalogEntry> entries{{"codex-only", {{"codex"}}}, {"shared", {{"codex"}, {"surplus"}}}, {"openlux-only", {{"openlux"}}}};
  EXPECT_EQ(sel::filterCatalogEntriesByProviderSelection(entries, std::nullopt), entries);
  EXPECT_EQ(sel::filterCatalogEntriesByProviderSelection(entries, sel::ProviderSelection{{"surplus"}, 1}), (std::vector<CatalogEntry>{{"shared", {{"surplus"}}}}));
  const sel::ProviderSelection subscriptionOnly{{sel::codexSubscriptionSelectionId(std::string(64, 'c'))}, 1};
  EXPECT_EQ(sel::filterCatalogEntriesByProviderSelection(entries, subscriptionOnly), (std::vector<CatalogEntry>{{"codex-only", {{"codex"}}}, {"shared", {{"codex"}}}}));
}
ASYNC_TEST("the routing selection read is cached, primed by a save, and fails open") {
  const auto database = std::make_shared<SelectionKv>();
  {
    Fixture fixture(database);
    AWAIT(absent, sel::loadProviderSelection(database)); EXPECT(!absent);
    const std::vector<std::string> submitted{"openlux"};
    AWAIT(stored, sel::storeProviderSelection(database, submitted)); EXPECT(stored.has_value());
    EXPECT_EQ(stored->provider_ids, submitted);
    EXPECT_JSON(database->storedSelection(), json::value_from(*stored), "saved row");
    AWAIT(primed, sel::loadProviderSelectionCached()); EXPECT_EQ(primed, stored);
    database->seedSelection({});
    AWAIT(cached, sel::loadProviderSelectionCached()); EXPECT_EQ(cached->provider_ids, submitted);
    AWAIT(expired, sel::loadProviderSelectionCached(rt::dateNow() + sel::PROVIDER_SELECTION_CACHE_TTL_MS + 1)); EXPECT(expired->provider_ids.empty());
    database->failReads = true;
    AWAIT(failed, sel::loadProviderSelectionCached(rt::dateNow() + 2 * sel::PROVIDER_SELECTION_CACHE_TTL_MS)); EXPECT(failed->provider_ids.empty());
  }
  const auto unavailable = std::make_shared<SelectionKv>(); unavailable->failReads = true;
  Fixture fixture(unavailable);
  AWAIT(nothing, sel::loadProviderSelectionCached()); EXPECT(!nothing);
}
ASYNC_TEST("an in-flight selection read cannot replace a cache primed by an admin save") {
  const auto database = std::make_shared<DeferredSelectionKv>(); database->seedSelection({"openlux"}); Fixture fixture(database);
  rt::Promise<std::optional<sel::ProviderSelection>> inFlightRead(sel::loadProviderSelectionCached());
  co_await database->readStarted.promise.wait();
  const auto request = selectionRequest(json::object{{"provider_ids", json::array{"cerebras"}}});
  AWAIT(saved, admin::handleAdminProviderSelectionSet(request)); EXPECT_EQ(saved.status(), 200);
  AWAIT(savedBody, saved.json()); EXPECT_JSON(savedBody.at("provider_ids"), (json::array{"cerebras"}), "saved selection");
  const json::object expected{{"provider_ids", json::array{"cerebras"}}, {"updated_at_ms", savedBody.at("updated_at_ms")}};
  AWAIT(primed, sel::loadProviderSelectionCached()); EXPECT_JSON(selectionJson(primed), expected, "admin primes cache");
  database->releaseRead.resolve();
  const auto old = co_await inFlightRead.wait(); EXPECT_JSON(selectionJson(old), (json::object{{"provider_ids", json::array{"openlux"}}, {"updated_at_ms", 1'700'000'000'000.0}}), "original read result");
  AWAIT(current, sel::loadProviderSelectionCached()); EXPECT_JSON(selectionJson(current), expected, "stale read cannot overwrite save");
}
TEST("a new loop can load provider selection after the shared read was abandoned") {
  const auto database = std::make_shared<DeferredSelectionKv>();
  Fixture fixture(database);
  {
    rt::FakeClock clock;
    rt::Loop first(clock);
    const rt::LoopScope scope(first);
    const rt::Promise<std::optional<sel::ProviderSelection>> pending(sel::loadProviderSelectionCached(1));
    rt::blockOn(first, database->readStarted.promise.wait());
    EXPECT(pending.pending());
  }
  const auto replacement = std::make_shared<SelectionKv>();
  replacement->seedSelection({"cerebras"});
  uos::kv::setKvForTest(replacement);
  rt::FakeClock clock;
  rt::Loop second(clock);
  const auto selected = rt::blockOn(second, sel::loadProviderSelectionCached(2));
  ASSERT(selected);
  EXPECT_EQ(selected->provider_ids, std::vector<std::string>{"cerebras"});
}
ASYNC_TEST("admin provider picker reports the roster, catalog counts, and the saved selection") {
  const auto database = std::make_shared<SelectionKv>(); database->seedSelection({"surplus"}); Fixture fixture(database);
  const auto dependencies = fixtureDependencies(); AWAIT(response, admin::handleAdminProviderSelectionGet(dependencies));
  EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("cache-control"), "no-store"); AWAIT(body, response.json());
  json::array rows{
    json::object{{"id", "codex"}, {"model_count", 1}, {"status", "available"}, {"configured", true}, {"subscriptions", json::array{}}},
    json::object{{"id", "claude_code"}, {"model_count", 0}, {"status", "unavailable"}, {"configured", false}},
    json::object{{"id", "surplus"}, {"model_count", 2}, {"status", "available"}, {"configured", true}},
    json::object{{"id", "openlux"}, {"model_count", 0}, {"status", "unavailable"}, {"configured", false}},
    json::object{{"id", "deepseek"}, {"model_count", 1}, {"status", "available"}, {"configured", true}},
    json::object{{"id", "cerebras"}, {"model_count", 0}, {"status", "unavailable"}, {"configured", false}},
    json::object{{"id", "lithos"}, {"model_count", 0}, {"status", "unavailable"}, {"configured", false}}
  };
  const bool routerReady = uos::provider::openrouter::readOpenRouterApiKey().has_value(), gatewayReady = uos::deepseek::waterfall::deepSeekWaterfallCatalogEnabled(std::nullopt);
  rows.push_back(json::object{{"id", "openrouter"}, {"model_count", 0}, {"status", routerReady ? "available" : "unavailable"}, {"configured", routerReady}});
  rows.push_back(json::object{{"id", std::string(presentation::GATEWAY_PROVIDER_ID)}, {"model_count", 0}, {"status", gatewayReady ? "available" : "unavailable"}, {"configured", gatewayReady}});
  json::array expected;
  for (const auto& row : rows) {
    const auto id = std::string(*rt::getString(row, "id"));
    const auto copy = id == presentation::GATEWAY_PROVIDER_ID ? presentation::GATEWAY_PROVIDER_PRESENTATION : presentation::providerPresentation(id);
    auto complete = json::value_from(copy).as_object();
    for (const auto& tier : presentation::PROVIDER_TIERS) if (tier.id == copy.tier) complete["tier_label"] = std::string(tier.label);
    rt::objectAssign(complete, row.as_object()); expected.push_back(complete);
  }
  EXPECT_JSON(body.at("data").at("providers"), expected, "fixed roster, catalog counts, complete presentation");
  json::array tiers; for (const auto& tier : presentation::PROVIDER_TIERS) tiers.push_back(json::object{{"id", std::string(tier.id)}, {"label", std::string(tier.label)}});
  EXPECT_JSON(body.at("data").at("tiers"), tiers, "waterfall tier order");
  EXPECT_JSON(body.at("data").at("selection").at("provider_ids"), (json::array{"surplus"}), "saved selection");
  EXPECT_EQ(rt::getBool(body.at("data"), "filter_active"), true);
  const auto unavailable = selectionRequest(json::object{{"provider_ids", json::array{"gemini"}}});
  AWAIT(refused, admin::handleAdminProviderSelectionSet(unavailable));
  EXPECT_EQ(refused.status(), 400);
}
ASYNC_TEST("every selectable provider carries a complete presentation and a health key the view reports") {
  const auto database = std::make_shared<SelectionKv>(); Fixture fixture(database); rt::env::set("DEEPSEEK_API_KEY", "fixture-deepseek-key");
  const std::string home = rt::fs::mkdtempSync(rt::os::tmpdir() + "/gemini-picker-");
  const auto cleanup = rt::scopeExit([home] { rt::fs::rmSync(home, {.recursive = true, .force = true}); });
  const rt::env::ScopedOverride antigravity{{"HOME", home}, {"UOS_GATEWAY_ENABLE_ANTIGRAVITY", "1"}};
  rt::fs::mkdirSync(home + "/.local/bin", {.recursive = true, .mode = 0700});
  rt::fs::mkdirSync(home + "/.gemini/antigravity-cli", {.recursive = true, .mode = 0700});
  rt::fs::writeFileSync(home + "/.local/bin/agy", "fixture", {.flag = "wx", .mode = 0700});
  rt::fs::writeFileSync(home + "/.gemini/antigravity-cli/antigravity-oauth-token", "", {.flag = "wx", .mode = 0600});
  AWAIT(snapshot, catalog::buildModelCatalogSnapshot());
  admin::CatalogDependencies dependencies; dependencies.buildCatalog = [snapshot] { return rt::fulfilledTask(snapshot); };
  AWAIT(response, admin::handleAdminProviderSelectionGet(dependencies)); AWAIT(body, response.json());
  const auto& providers = body.at("data").at("providers").as_array(); std::vector<std::string> ids;
  for (const auto& row : providers) ids.push_back(std::string(*rt::getString(row, "id")));
  std::vector<std::string> roster; appendIds(roster, sel::SELECTABLE_PROVIDER_IDS); EXPECT_EQ(ids, roster);
  const auto gemini = std::find_if(providers.begin(), providers.end(), [](const auto& row) { return rt::getString(row, "id") == "gemini"; });
  ASSERT(gemini != providers.end());
  EXPECT_EQ(rt::getNumber(*gemini, "model_count"), std::optional<double>(3));
  EXPECT_EQ(rt::getBool(*gemini, "configured"), true);
  const auto gateway = std::find_if(providers.begin(), providers.end(), [](const auto& row) { return rt::getString(row, "id") == presentation::GATEWAY_PROVIDER_ID; });
  EXPECT(gateway != providers.end()); EXPECT(rt::getBool(*gateway, "selectable") != false);
  std::vector<std::string> actualTiers, expectedTiers;
  for (const auto& tier : body.at("data").at("tiers").as_array()) actualTiers.push_back(std::string(*rt::getString(tier, "id")));
  for (const auto& tier : presentation::PROVIDER_TIERS) expectedTiers.push_back(std::string(tier.id));
  EXPECT_EQ(actualTiers, expectedTiers);
  AWAIT(healthResponse, uos::health::handleHealthProviders()); AWAIT(healthBody, healthResponse.json());
  for (const auto& row : providers) {
    const auto id = std::string(*rt::getString(row, "id"));
    const auto* defined = id == presentation::GATEWAY_PROVIDER_ID ? &presentation::GATEWAY_PROVIDER_PRESENTATION : presentation::PROVIDER_PRESENTATION.get(id);
    EXPECT(defined != nullptr);
    const auto& copy = *defined;
    EXPECT_EQ(rt::getString(row, "label"), copy.label); EXPECT_EQ(rt::getString(row, "tier"), copy.tier);
    for (const auto& tier : presentation::PROVIDER_TIERS) if (tier.id == copy.tier) EXPECT_EQ(rt::getString(row, "tier_label"), tier.label);
    EXPECT_EQ(rt::getString(row, "detail"), copy.detail); EXPECT(!copy.detail.empty());
    EXPECT_JSON(row.at("endpoints"), json::value_from(copy.endpoints), id + " endpoints");
    EXPECT_EQ(rt::getString(row, "health_key"), copy.health_key); EXPECT(healthBody.as_object().contains(copy.health_key));
    EXPECT(std::find(uos::provider::health::RECORD_PROVIDER_IDS.begin(), uos::provider::health::RECORD_PROVIDER_IDS.end(), copy.health_key) != uos::provider::health::RECORD_PROVIDER_IDS.end());
  }
  const auto deepseek = std::find_if(providers.begin(), providers.end(), [](const auto& row) { return rt::getString(row, "id") == "deepseek"; }); EXPECT(deepseek != providers.end());
  EXPECT_JSON(deepseek->at("endpoints"), (json::array{"/v1/chat/completions", "/v1/responses"}), "DeepSeek endpoints");
  EXPECT(rt::includes(rt::getString(*deepseek, "detail").value_or(""), "Chat Completions upstream and Responses through the gateway's translation"));
  for (const auto id : uos::deepseek::index::DEEPSEEK_OFFICIAL_MODEL_IDS) {
    const auto model = std::find_if(snapshot.models.begin(), snapshot.models.end(), [id](const auto& row) { return row.id == id; }); EXPECT(model != snapshot.models.end());
    const auto provider = std::find_if(model->providers.begin(), model->providers.end(), [](const auto& row) { return row.id == "deepseek"; }); EXPECT(provider != model->providers.end());
    EXPECT_JSON(deepseek->at("endpoints"), json::value_from(provider->supported_endpoints), std::string(id) + " catalog and roster endpoints");
  }
  const auto lithos = std::find_if(providers.begin(), providers.end(), [](const auto& row) { return rt::getString(row, "id") == "lithos"; }); EXPECT(lithos != providers.end());
  EXPECT_EQ(rt::getString(*lithos, "label"), "LithosAI"); EXPECT_EQ(rt::getString(*lithos, "health_key"), "lithos");
  const auto fallback = presentation::providerPresentation("future-provider"); EXPECT_EQ(fallback.label, "future-provider"); EXPECT_EQ(fallback.tier, "direct");
  EXPECT(rt::includes(fallback.detail, "no presentation entry yet")); EXPECT(fallback.endpoints.empty()); EXPECT_EQ(fallback.health_key, "future-provider");
}
ASYNC_TEST("admin provider picker lists each configured Codex subscription under its opaque id") {
  const auto database = std::make_shared<SelectionKv>(); Fixture fixture(database); seedCodexAuthPool(*database);
  const auto dependencies = fixtureDependencies(); AWAIT(response, admin::handleAdminProviderSelectionGet(dependencies)); AWAIT(body, response.json());
  const auto& providers = body.at("data").at("providers").as_array();
  const auto codex = std::find_if(providers.begin(), providers.end(), [](const auto& row) { return rt::getString(row, "id") == "codex"; }); EXPECT(codex != providers.end());
  const std::string firstAccount = "account-one", secondAccount = "account-two";
  AWAIT(firstHash, sel::codexSubscriptionHash(firstAccount)); AWAIT(secondHash, sel::codexSubscriptionHash(secondAccount));
  EXPECT_JSON(codex->at("subscriptions"), (json::array{
    json::object{{"id", sel::codexSubscriptionSelectionId(firstHash)}, {"label", "first@example.com"}, {"slot", 1}},
    json::object{{"id", sel::codexSubscriptionSelectionId(secondHash)}, {"label", "Codex account 2"}, {"slot", 2}}}), "opaque subscription roster");
  for (const auto& provider : providers) {
    EXPECT(!rt::get(provider, "subscriptions") || rt::getString(provider, "id") == "codex");
    EXPECT(!rt::includes(rt::jsonStringify(provider), "account-one"));
  }
  const auto secondId = std::string(*rt::getString(codex->at("subscriptions").as_array()[1], "id"));
  const auto request = selectionRequest(json::object{{"provider_ids", json::array{secondId, "surplus"}}});
  AWAIT(saved, admin::handleAdminProviderSelectionSet(request)); EXPECT_EQ(saved.status(), 200); AWAIT(savedBody, saved.json());
  EXPECT_JSON(savedBody.at("provider_ids"), (json::array{"surplus", secondId}), "subscription write ordering");
  AWAIT(reloaded, admin::handleAdminProviderSelectionGet(dependencies)); AWAIT(reloadedBody, reloaded.json()); EXPECT_EQ(rt::getBool(reloadedBody.at("data"), "filter_active"), true);
}
ASYNC_TEST("an empty saved provider selection is reported as no filter") {
  const auto database = std::make_shared<SelectionKv>(); Fixture fixture(database); const auto dependencies = fixtureDependencies();
  AWAIT(response, admin::handleAdminProviderSelectionGet(dependencies)); AWAIT(body, response.json());
  EXPECT_JSON(body.at("data").at("selection"), (json::object{{"provider_ids", json::array{}}, {"updated_at_ms", 0}}), "no saved filter"); EXPECT_EQ(rt::getBool(body.at("data"), "filter_active"), false);
}
ASYNC_TEST("the provider picker refuses to render without KV") {
  Fixture fixture(nullptr); admin::CatalogDependencies dependencies;
  dependencies.buildCatalog = []() -> rt::Task<catalog::ModelCatalogSnapshot> { throw rt::JsError("the catalog must not be built without KV"); co_return catalog::ModelCatalogSnapshot{}; };
  AWAIT(response, admin::handleAdminProviderSelectionGet(dependencies)); EXPECT_EQ(response.status(), 500);
}
ASYNC_TEST("saving a provider selection canonicalizes it and rejects unknown ids") {
  const auto database = std::make_shared<SelectionKv>(); Fixture fixture(database);
  const auto request = selectionRequest(json::object{{"provider_ids", json::array{"cerebras", "codex", "cerebras"}}});
  AWAIT(response, admin::handleAdminProviderSelectionSet(request)); EXPECT_EQ(response.status(), 200); AWAIT(body, response.json());
  EXPECT_JSON(body.at("provider_ids"), (json::array{"codex", "cerebras"}), "canonical wire order"); EXPECT_EQ(rt::getBool(body, "stored"), true);
  const json::object expected{{"provider_ids", json::array{"codex", "cerebras"}}, {"updated_at_ms", body.at("updated_at_ms")}};
  EXPECT_JSON(database->storedSelection(), expected, "persisted canonical row");
  const std::vector<json::value> invalidPayloads{json::object{{"provider_ids", json::array{"codex", "nope"}}}, json::object{{"provider_ids", json::array{42}}}, json::object{{"provider_ids", "codex"}}, json::object{}};
  for (const auto& payload : invalidPayloads) { const auto invalidRequest = selectionRequest(payload); AWAIT(invalid, admin::handleAdminProviderSelectionSet(invalidRequest)); EXPECT_EQ(invalid.status(), 400); }
  EXPECT_JSON(database->storedSelection(), expected, "rejected writes change nothing");
  const auto emptyRequest = selectionRequest(json::object{{"provider_ids", json::array{}}}); AWAIT(empty, admin::handleAdminProviderSelectionSet(emptyRequest)); EXPECT_EQ(empty.status(), 200); AWAIT(emptyBody, empty.json());
  EXPECT_JSON(database->storedSelection(), (json::object{{"provider_ids", json::array{}}, {"updated_at_ms", emptyBody.at("updated_at_ms")}}), "empty list clears filter");
}
ASYNC_TEST("the provider picker routes are registered and stay behind admin auth") {
  const auto database = std::make_shared<SelectionKv>(); Fixture fixture(database);
  T2IngressScope ingress(database);
  const rt::Request selectionRequest("https://ai.ubq.fi/admin/providers/selection");
  AWAIT(unauthenticated, uos::handler::index::handler(selectionRequest));
  EXPECT_EQ(unauthenticated.status(), 401);
  const rt::Request unknownRequest("https://ai.ubq.fi/admin/providers/selection-unknown");
  AWAIT(unknown, uos::handler::index::handler(unknownRequest));
  EXPECT_EQ(unknown.status(), 404);
}

ASYNC_TEST("/v1/models hides the models of a switched-off provider") {
  const auto database = std::make_shared<SelectionKv>(); Fixture fixture(database);
  rt::env::set("DEEPSEEK_API_KEY", "fixture-deepseek-key"); rt::env::set("CEREBRAS_API_KEY", "fixture-cerebras-key"); rt::env::set("LITHOSAI_API_KEY", "fixture-lithos-key"); rt::env::set("OPENROUTER_API_KEY", "fixture-openrouter-key");
  seedCodexSnapshot(*database, {"gpt-5.6-sol"});
  std::vector<std::string> every{"gpt-5.6-sol", "gpt-oss-120b", "qwen-3.8-27b"}; appendIds(every, uos::deepseek::index::DEEPSEEK_OFFICIAL_MODEL_IDS); appendIds(every, uos::provider::lithos::LITHOS_MODEL_IDS); every.push_back(std::string(uos::deepseek::waterfall::DEEPSEEK_WATERFALL_MODEL_ID)); every.push_back("typesafe/jev-latest");
  AWAIT(unfiltered, listModelIds()); EXPECT_EQ(unfiltered, every);
  database->seedSelection({"deepseek", "cerebras"}); sel::resetProviderSelectionCacheForTest();
  std::vector<std::string> direct{"gpt-oss-120b", "qwen-3.8-27b"}; appendIds(direct, uos::deepseek::index::DEEPSEEK_OFFICIAL_MODEL_IDS);
  AWAIT(narrowed, listModelIds()); EXPECT_EQ(narrowed, direct);
  database->seedSelection({"deepseek", "cerebras", "ubiquity"}); sel::resetProviderSelectionCacheForTest(); direct.push_back(std::string(uos::deepseek::waterfall::DEEPSEEK_WATERFALL_MODEL_ID));
  AWAIT(gateway, listModelIds()); EXPECT_EQ(gateway, direct);
  database->seedSelection({"codex"}); sel::resetProviderSelectionCacheForTest(); AWAIT(codexOnly, listModelIds()); EXPECT_EQ(codexOnly, (std::vector<std::string>{"gpt-5.6-sol"}));
}
ASYNC_TEST("a switched-off Lithos provider leaves an explicit Responses id to availability checks") {
  const auto database = std::make_shared<SelectionKv>(); Fixture fixture(database);
  rt::env::set("LITHOSAI_API_KEY", "lith_sk_fixture");
  int lithosUpstreamCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&lithosUpstreamCalls](rt::RequestInfo input, rt::RequestInit) {
    const auto url = std::visit([](const auto& value) -> std::string {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::string>) return value;
      else if constexpr (std::is_same_v<T, rt::URL>) return value.href();
      else return value.url();
    }, input);
    if (url == uos::provider::lithos::LITHOS_CHAT_COMPLETIONS_URL) ++lithosUpstreamCalls;
    return rt::Response::json(json::object{});
  }));
  seedCodexSnapshot(*database, {"gpt-5.6-sol"});
  database->seedSelection({"codex"});
  rt::RequestInit init; init.method = "POST"; init.headers = rt::Headers{{"Content-Type", "application/json"}};
  const json::object input{{"model", std::string(uos::provider::lithos::LITHOS_MODEL_IDS[4])}, {"input", "hi"}, {"stream", false}};
  init.body = rt::jsonStringify(input);
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  AWAIT(response, uos::responses_handler::handleResponses(request));
  EXPECT_EQ(lithosUpstreamCalls, 0);
  EXPECT_EQ(response.status(), 404);
  AWAIT(payload, response.json()); EXPECT_EQ(rt::getString(payload.at("error"), "code"), "model_not_found");
}

ASYNC_TEST("/v1/models still applies the model whitelist on top of the provider selection") {
  const auto database = std::make_shared<SelectionKv>(); Fixture fixture(database); seedCodexSnapshot(*database, {"gpt-5.6-sol", "gpt-5.6-terra"});
  database->put(uos::models::codex_models_whitelist::CODEX_MODELS_WHITELIST_KV_KEY, json::object{{"model_ids", json::array{"gpt-5.6-terra"}}, {"updated_at_ms", 1'700'000'000'000.0}});
  database->seedSelection({"codex"}); sel::resetProviderSelectionCacheForTest(); AWAIT(ids, listModelIds()); EXPECT_EQ(ids, (std::vector<std::string>{"gpt-5.6-terra"}));
}
ASYNC_TEST("the gateway identity joins the selection like any provider") {
  const auto database = std::make_shared<SelectionKv>(); Fixture fixture(database);
  const auto request = selectionRequest(json::object{{"provider_ids", json::array{"ubiquity", "openrouter"}}}); AWAIT(response, admin::handleAdminProviderSelectionSet(request)); EXPECT_EQ(response.status(), 200); AWAIT(body, response.json());
  EXPECT_JSON(body.at("provider_ids"), (json::array{"openrouter", "ubiquity"}), "gateway roster order");
  EXPECT_JSON(database->storedSelection(), (json::object{{"provider_ids", json::array{"openrouter", "ubiquity"}}, {"updated_at_ms", body.at("updated_at_ms")}}), "gateway stored selection");
}
TEST("an active selection without the gateway identity switches the waterfall route off") {
  const sel::ProviderSelection withGateway{{"ubiquity", "openrouter"}, 1}, withoutGateway{{"openrouter"}, 1};
  EXPECT(sel::isProviderEnabled(presentation::GATEWAY_PROVIDER_ID, withGateway)); EXPECT(!sel::isProviderEnabled(presentation::GATEWAY_PROVIDER_ID, withoutGateway));
  EXPECT(!uos::deepseek::waterfall::deepSeekWaterfallCatalogEnabled(withoutGateway));
}
TEST("the public catalog drops the gateway route while its identity is off") {
  const std::vector<CatalogEntry> entries{{"ubiquity/deepseek-flash", {{"ubiquity"}, {"openrouter"}}}, {"some-other", {{"openrouter"}}}};
  const auto withoutGateway = sel::filterCatalogEntriesByProviderSelection(entries, sel::ProviderSelection{{"openrouter"}, 1});
  std::vector<std::string> ids; for (const auto& entry : withoutGateway) ids.push_back(entry.id); EXPECT_EQ(ids, (std::vector<std::string>{"some-other"}));
  const auto withGateway = sel::filterCatalogEntriesByProviderSelection(entries, sel::ProviderSelection{{"ubiquity"}, 1});
  std::vector<std::vector<std::string>> providers;
  for (const auto& entry : withGateway) { std::vector<std::string> row; for (const auto& provider : entry.providers) row.push_back(provider.id); providers.push_back(row); }
  EXPECT_EQ(providers, (std::vector<std::vector<std::string>>{{"ubiquity"}}));
}
