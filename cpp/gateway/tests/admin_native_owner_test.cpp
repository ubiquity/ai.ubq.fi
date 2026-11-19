// Port of all four original tests/admin-native-owner.test.ts registrations, in source order.
// Links: tests/admin_native_owner_test.cpp src/types.cpp tests/helpers/counting_kv.cpp src/codex/auth.cpp src/agent_messages.cpp src/provider/health.cpp src/utils.cpp src/catalog/types.cpp src/codex/native_auth.cpp src/config.cpp src/models/catalog.cpp src/input_normalization.cpp src/models/codex_models.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/release.cpp src/handler/index.cpp src/provider/cerebras.cpp src/admin/claude_code.cpp src/static.cpp src/health.cpp src/deepseek/waterfall.cpp src/admin/codex.cpp src/models/openrouter_models.cpp src/admin/codex_reset.cpp src/runtime_config.cpp src/live/handler.cpp src/admin/kernel.cpp
// src/handler/http.cpp src/provider/capacity_parse.cpp src/provider/presentation.cpp src/auth/passkeys.cpp src/request.cpp src/http.cpp src/admin/api_keys.cpp src/quota_projection.cpp src/api_key_policy.cpp src/catalog/store.cpp src/provider/claude_code_accounts.cpp src/paid_fallback/ledger_state.cpp
// src/authority/native_paid_contracts.cpp src/codex/reset_settings.cpp src/codex/native_auth_transport.cpp src/provider/selection.cpp src/models/metadata.cpp src/paid_fallback/ledger_backfill.cpp src/codex/manual_reset.cpp src/metered_quota.cpp src/codex/banked_reset.cpp src/provider/claude_code_quota.cpp
// src/live/routes.cpp src/api_keys.cpp src/paid_fallback/ledger_settlement.cpp src/live/sideband.cpp src/models/codex_models_whitelist.cpp src/authority/native_paid.cpp src/kernel/quota_write.cpp src/admin/api_key_audit.cpp src/authority/native_codex_capacity.cpp src/kernel/usage.cpp src/kernel/quota_v2.cpp
// src/provider/claude_code_credentials.cpp src/provider/metered.cpp src/codex/account_routing.cpp src/authority/native_kernel.cpp src/authority/native_api_key.cpp src/authority/native_codex_mutations.cpp src/provider/surplus.cpp src/codex/routing_serial.cpp src/provider/claude_code_models.cpp
// src/codex/overage_settings.cpp src/authority/native_api_key_policy.cpp src/codex/capacity_routing.cpp src/auth/passport.cpp src/provider/lithos.cpp src/codex/banked_reset_pool.cpp src/request_policy.cpp src/auth/index.cpp src/authority/native_codex.cpp src/codex/external_auth.cpp
// src/provider/anthropic_chat_responses.cpp src/codex/routing_mutations.cpp src/kernel/quota_reservations.cpp src/codex/banked_reset_claim.cpp src/authority/native_codex_contracts.cpp src/admin/api_key_mutations.cpp src/provider/surplus_claude_request.cpp src/paid_fallback/ledger_admission.cpp
// src/auth/passport_sessions.cpp src/codex/experiment.cpp src/authority/native_paid_policy.cpp src/provider/openrouter.cpp src/codex/auth_refresh.cpp src/admin/defaults.cpp src/live/upstream.cpp src/codex/banked_reset_provider.cpp src/authority/native_codex_evaluation.cpp src/kernel/attestation.cpp
// src/models/codex_models_fetch.cpp src/provider/claude_code_access.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/index.cpp src/provider/claude_code.cpp src/codex/rate_limit_429.cpp src/codex/dispatch.cpp src/cache/kv_migration_base.cpp src/codex/routing_evaluation.cpp
// src/provider/anthropic_chat_stream.cpp src/cache/prompt_analytics_read.cpp src/debug_routing.cpp src/cache/scope_experiment_targets.cpp src/authority/native_codex_429.cpp src/cache/prompt_analytics_core.cpp src/cache/scope_experiment_model.cpp src/codex/quota.cpp src/live/calls.cpp
// src/cache/telemetry_gate.cpp src/authority/native_api_key_audit.cpp src/provider/cerebras_rate_limits.cpp src/codex/banked_reset_submission.cpp src/admin/chart_source_proxy.cpp src/kv_conversion/profile_json.cpp src/provider/capacity.cpp src/auth/relay.cpp src/provider/capacity_history.cpp src/kv.cpp
// src/provider/anthropic_responses.cpp src/provider/capacity_analytics.cpp src/authority/native_kernel_contracts.cpp src/provider/anthropic_response_wire.cpp src/handler/terminal_route.cpp src/admin/capacity_observation_sync.cpp src/codex/supervisor_brief.cpp src/codex/supervisor.cpp src/catalog/index.cpp
// src/defaults.cpp src/systemone/handlers.cpp src/kernel/policy_queue.cpp src/openai_telemetry.cpp src/provider/capacity_rollups.cpp src/codex/routing_state.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos_models.cpp src/cache/kv_migration_validation.cpp src/responses_failover_stream.cpp
// src/catalog/models.cpp src/sentinel/replay_observation.cpp src/sentinel/replay_admin.cpp src/handler/terminal_log.cpp src/recent_model_context.cpp src/auth/local_admin.cpp src/provider/surplus_claude_chat_request.cpp src/deepseek/index.cpp src/sentinel/replay_model.cpp
// src/models/codex_models_availability.cpp src/codex/supervisor_config.cpp src/responses_stream.cpp src/admin/error_log.cpp src/cache/kv_migration_paid_fallback.cpp src/sentinel/replay_read.cpp src/runtime/websocket.cpp src/admin/sentinel_incident.cpp src/chat/envelope.cpp src/codex/manual_reset_state.cpp
// src/auth/local_development_key.cpp src/cache/scope_experiment.cpp src/embeddings/jobs.cpp src/cache/scope_targets.cpp src/sentinel/replay_store.cpp src/codex/supervisor_secret.cpp src/openai.cpp src/sentinel/replay_retention.cpp src/provider/claude_code_usage_schema.cpp src/provider/capacity_events.cpp
// src/cache/kv_migration_usage.cpp src/paid_fallback/rollups.cpp src/sentinel/replay_retention_schema.cpp src/codex/supervisor_follow.cpp src/embeddings/ledger.cpp src/sentinel/upstream_capture.cpp src/sentinel/incident_outbox.cpp src/paid_fallback/routing.cpp src/paid_fallback/health.cpp
// src/embeddings/voyage.cpp src/sentinel/replay_envelope.cpp src/codex/supervisor_log.cpp src/storage/kv.cpp src/embeddings/handlers.cpp src/responses_handler.cpp src/provider/capacity_contract.cpp src/codex/supervisor_transport.cpp src/upstream_wire.cpp src/decisions/handlers.cpp
// src/provider/claude_capacity_history.cpp src/responses_attempts.cpp src/handler/admission.cpp src/provider/anthropic_replay.cpp src/analytics.cpp src/provider/claude_capacity_rollups.cpp src/bounded_response_body.cpp src/codex/supervisor_inventory.cpp src/images.cpp
// src/sentinel/replay_retention_bootstrap.cpp src/responses_native_headers.cpp src/paid_fallback/removed_provider.cpp src/openai_schema.cpp src/provider/cerebras_handlers.cpp src/deepseek/flash_aliases.cpp src/paid_fallback/claude_code_route.cpp src/sentinel/replay_limits.cpp src/chat/stream_translation.cpp
// src/provider/removed_provider_circuit.cpp src/inference_deadline.cpp src/runtime/http_tasks.cpp src/sentinel/replay_retention_metadata.cpp src/provider/cerebras_responses.cpp src/responses_request.cpp src/deepseek/responses_payload.cpp src/provider/openrouter_request_controls.cpp
// src/provider/removed_provider_telemetry.cpp src/kernel/quota_reserve.cpp src/deepseek/responses.cpp src/provider/openrouter_handlers.cpp src/catalog/promotion.cpp src/inference_admission.cpp src/chat/completions.cpp src/codex/responses_error.cpp src/runtime/http_early_sse.cpp
// src/deepseek/responses_stream.cpp src/provider/lithos_handlers.cpp src/cache/prompt_analytics_queue.cpp src/storage/native_kv.cpp src/deepseek/handlers.cpp src/codex/index.cpp src/optional_telemetry_queue.cpp src/deepseek/context_admission_failure.cpp
// src/provider/stream_relay.cpp src/storage/sqlite_readonly.cpp src/provider/lithos_streams.cpp src/lib/jev_compaction/codex_items.cpp src/responses_buffered.cpp src/deepseek/waterfall_handler.cpp src/deepseek/stream.cpp src/provider/openrouter_streams.cpp src/codex/responses_state.cpp
// src/deepseek/chat_projection.cpp src/deepseek/forwarded_payload_policy.cpp src/provider/openrouter_web_tools.cpp src/codex/responses_runners.cpp src/codex/responses_operations.cpp src/lib/jev_compaction/state.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/types.cpp
// src/lib/jev_compaction/request.cpp
#include "tests/helpers/counting_kv.hpp"
#include "uos/catalog/types.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/native_auth.hpp"
#include "uos/config.hpp"
#include "uos/handler/index.hpp"
#include "uos/kv.hpp"
#include "uos/runtime_config.hpp"
#include "uos/types.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace auth = uos::codex::auth;
  namespace native = uos::codex::native_auth;
  namespace runtime = uos::runtime_config;
  namespace counting = uos::tests::helpers::counting_kv;
  using uos::handler::index::handler;
  const std::string ADMIN_TOKEN = "synthetic-uos791-admin";
  uos::types::CodexAuthState credentials(const std::string& accountId, int generation) {
    const json::object claims{{"exp", 4000000000.0 + generation}, {"generation", generation}};
    return {"header." + rt::btoa(rt::jsonStringify(claims)) + ".signature", "synthetic-refresh-" + accountId + "-" + std::to_string(generation), accountId, 1};
  }
  class FixtureKv final : public counting::CountingKv {
  public:
    using counting::CountingKv::get;
    rt::Reason failure;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      if (failure && key == auth::CODEX_AUTH_POOL_KV_KEY) std::rethrow_exception(failure);
      return counting::CountingKv::get(std::move(key), std::move(options));
    }
  };
  json::array snapshot(const FixtureKv& kv) {
    json::array rows;
    for (const auto& [key, entry] : kv.entries) rows.push_back(json::object{{"encodedKey", key}, {"value", entry.value.toJson()}, {"versionstamp", entry.versionstamp}});
    return rows;
  }
  std::vector<std::pair<std::string, std::string>> isolatedEnvironment() {
    std::vector<std::pair<std::string, std::string>> values;
    for (const auto* key : {"HOME", "TMPDIR"}) if (const auto value = rt::env::get(key)) values.emplace_back(key, *value);
    return values;
  }
  struct Fixture {
    uos::config::Config previous = uos::config::config;
    rt::env::ScopedEnvironment environment{isolatedEnvironment()};
    std::shared_ptr<FixtureKv> kv = std::make_shared<FixtureKv>();
    uos::types::CodexAuthPoolState pool;
    runtime::RuntimeConfigV2 runtimeConfig;
    json::array before;
    int modelReads = 0;
    int forbiddenCalls = 0;
    int nativeReads = 0;
    int nativeRefreshes = 0;
    std::optional<rt::GlobalFetchOverride> fetch;
    Fixture() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.adminTokens.insert(ADMIN_TOKEN);
      uos::kv::setKvForTest(kv);
      auth::resetCodexAuthCacheForTest();
      runtime::resetRuntimeConfigCacheForTest();
      fetch.emplace(rt::Fetch([this](rt::RequestInfo input, rt::RequestInit) -> rt::Response {
        const rt::Request req = std::holds_alternative<rt::Request>(input) ? std::get<rt::Request>(input) : rt::Request(std::holds_alternative<std::string>(input) ? std::get<std::string>(input) : std::get<rt::URL>(input).href());
        if (!rt::endsWith(rt::URL(req.url()).pathname(), "/models")) { ++forbiddenCalls; throw rt::JsError("OAuth and other provider calls are forbidden"); }
        ++modelReads;
        return rt::Response::json(json::object{{"models", json::array{json::object{{"slug", "gpt-6.1-sol"}, {"display_name", "Validated model"}}}}});
      }));
      native::NativeAuthHooks hooks;
      hooks.codexHome = "/synthetic/uos791";
      hooks.readAuth = [this]() -> rt::Task<json::value> { ++nativeReads; throw rt::JsError("Native credential reads are forbidden"); co_return nullptr; };
      hooks.refresh = [this](std::string, std::string, std::optional<std::string>) -> rt::Task<void> { ++nativeRefreshes; throw rt::JsError("Native refreshes are forbidden"); co_return; };
      native::setNativeCodexAuthHooksForTest(std::move(hooks));
    }
    ~Fixture() {
      fetch.reset(); native::setNativeCodexAuthHooksForTest(std::nullopt); uos::kv::setKvForTest(nullptr);
      auth::resetCodexAuthCacheForTest(); runtime::resetRuntimeConfigCacheForTest(); uos::config::config = previous;
    }
    rt::Task<void> initialize(bool malformedOwner) {
      auto first = credentials("native-account", 1);
      AWAIT(generation, native::nativeCodexCredentialGeneration(first));
      first.native_owner = uos::types::CodexAuthState::NativeOwner{.codex_home = "/synthetic/uos791", .generation_hash = malformedOwner ? "malformed" : generation};
      pool = {std::vector<uos::types::CodexAuthState>{first, credentials("sibling-account", 2)}, 1};
      const json::object catalog{{"source", "chatgpt_codex"}, {"updated_at_ms", 1}, {"client_version", "0.126.0"}, {"models", json::array{json::object{{"slug", "gpt-5.6-sol"}, {"display_name", "Stored model"}}}}};
      runtimeConfig = runtime::buildRuntimeConfig(catalog);
      kv->seed(auth::CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(pool)));
      kv->seed(auth::CODEX_MODELS_KV_KEY, rt::KvValue(catalog));
      kv->seed(runtime::RUNTIME_CONFIG_V2_KEY, rt::KvValue(json::value_from(runtimeConfig)));
      kv->seed(uos::catalog::types::CODEX_CATALOG_AUTH_GENERATION_KEY, rt::KvValue("synthetic-existing-generation"));
      before = snapshot(*kv);
      auth::cacheCodexAuthPool(pool); runtime::cacheRuntimeConfig(runtimeConfig);
    }
    rt::Task<rt::Response> upload(uos::types::CodexAuthState replacement, bool authenticated = true) {
      rt::Headers headers{{"Content-Type", "application/json"}};
      if (authenticated) headers.set("Authorization", "Bearer " + ADMIN_TOKEN);
      const json::object body{{"auth", json::object{{"tokens", json::value_from(replacement)}}}, {"models", json::object{{"client_version", "0.126.0"}}}};
      return handler(rt::Request("https://gateway.example.test/admin/codex/auth", {.method = "POST", .headers = headers, .body = rt::BodyInit(rt::jsonStringify(body))}));
    }
    rt::Task<void> verify() {
      EXPECT_EQ(forbiddenCalls, 0); EXPECT_EQ(nativeReads, 0); EXPECT_EQ(nativeRefreshes, 0);
      const bool readOnly = std::all_of(kv->commands.begin(), kv->commands.end(), [](const auto& command) { return command.command == "get" || command.command == "getMany" || command.command == "list"; });
      if (readOnly) {
        EXPECT(rt::deepEqual(snapshot(*kv), before));
        AWAIT(entry, auth::getAuthPoolEntry()); EXPECT_EQ(entry.pool, pool);
        AWAIT(cachedRuntime, runtime::loadRuntimeConfig()); ASSERT(cachedRuntime); EXPECT(rt::deepEqual(json::value_from(*cachedRuntime), json::value_from(runtimeConfig)));
      }
      EXPECT_EQ(modelReads, 1);
    }
  };
  rt::Task<void> rejectsReplacement(bool malformedOwner) {
    Fixture fixture;
    auto initializing = fixture.initialize(malformedOwner); co_await std::move(initializing);
    AWAIT(unauthorized, fixture.upload(credentials("native-account", 3), false)); EXPECT_EQ(unauthorized.status(), 401);
    if (auto body = unauthorized.body()) { auto cancelling = body->cancel(); co_await std::move(cancelling); }
    AWAIT(response, fixture.upload(credentials("native-account", 3))); EXPECT_EQ(response.status(), malformedOwner ? 503 : 409);
    AWAIT(body, response.json());
    const json::object expected{{"error", json::object{{"message", malformedOwner ? "Codex native credential ownership is malformed; nothing was replaced." : "The native Codex owner must advance this account's credentials before replacement."}, {"type", "invalid_request_error"}, {"code", malformedOwner ? "codex_auth_owner_unavailable" : "codex_auth_owner_conflict"}}}};
    EXPECT(rt::deepEqual(body, expected));
    EXPECT(!std::any_of(fixture.kv->commands.begin(), fixture.kv->commands.end(), [](const auto& command) { return command.command == "set" || command.command == "delete" || command.command == "atomic.commit"; }));
    AWAIT(stored, fixture.kv->get(auth::CODEX_AUTH_POOL_KV_KEY)); EXPECT(rt::deepEqual(stored.value.toJson(), json::value_from(fixture.pool)));
    auto verifying = fixture.verify(); co_await std::move(verifying);
  }
}

ASYNC_TEST("authenticated native-owner upload returns 409 without replacing state") {
  auto testing = rejectsReplacement(false); co_await std::move(testing);
}
ASYNC_TEST("authenticated native-owner upload returns 503 without replacing state") {
  auto testing = rejectsReplacement(true); co_await std::move(testing);
}
ASYNC_TEST("authenticated equal-token upload retains native ownership and the sibling") {
  Fixture fixture;
  auto initializing = fixture.initialize(false); co_await std::move(initializing);
  AWAIT(response, fixture.upload(fixture.pool.accounts[0])); EXPECT_EQ(response.status(), 200);
  AWAIT(body, response.json()); EXPECT_EQ(rt::getBool(body, "stored"), std::optional(true));
  AWAIT(stored, fixture.kv->get(auth::CODEX_AUTH_POOL_KV_KEY));
  const auto pool = json::value_to<uos::types::CodexAuthPoolState>(stored.value.toJson());
  ASSERT_EQ(pool.accounts.size(), std::size_t(2));
  EXPECT_EQ(pool.accounts[0].native_owner, fixture.pool.accounts[0].native_owner);
  EXPECT_EQ(pool.accounts[1], fixture.pool.accounts[1]);
  auto verifying = fixture.verify(); co_await std::move(verifying);
}
ASYNC_TEST("authenticated upload still propagates unknown persistence errors") {
  Fixture fixture;
  auto initializing = fixture.initialize(false); co_await std::move(initializing);
  const auto failure = rt::makeReason(rt::JsError("synthetic storage failure"));
  fixture.kv->failure = failure;
  rt::Reason caught;
  try { AWAIT(response, fixture.upload(credentials("native-account", 3))); (void)response; } catch (...) { caught = std::current_exception(); }
  EXPECT(caught == failure);
  auto verifying = fixture.verify(); co_await std::move(verifying);
}
