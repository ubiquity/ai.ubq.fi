// Links: tests/health_test.cpp
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
// src/utils.cpp tests/helpers/counting_kv.cpp
// Original handler ingress cases use the real router; existing direct health cases remain component evidence.
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include <array>
#include "uos/handler/index.hpp"
#include "uos/config.hpp"
#include "uos/inference_admission.hpp"
#include "uos/release.hpp"
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
#include "uos/health.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/release.hpp"
#include "uos/kv.hpp"
#include "uos/metered_quota.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_access.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/utils.hpp"
#include "helpers/counting_kv.hpp"
#include "helpers/live_allocations.hpp"
namespace {
  namespace rt = uos::rt; namespace json = uos::json;
  namespace h = uos::health; namespace ph = uos::provider::health; namespace ca = uos::codex::auth;
  using CountingKv = uos::tests::helpers::counting_kv::CountingKv;
  struct Scope {
    rt::env::ScopedEnvironment environment{{{"HOME", "/nonexistent/w06c7-health"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<CountingKv> store = std::make_shared<CountingKv>();
    rt::GlobalFetchOverride blocked{rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("health fixture must not make an unstubbed upstream call"); })};
    Scope() { uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(store); ca::resetCodexAuthCacheForTest(); ph::resetProviderHealthThrottleForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::provider::claude_code_credentials::resetClaudeCodeCredentialCacheForTest(); }
    ~Scope() { h::setActiveUpstreamHealthTimeoutMsForTest(std::nullopt); uos::kv::setKvForTest(nullptr); ca::resetCodexAuthCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::provider::claude_code_credentials::resetClaudeCodeCredentialCacheForTest(); uos::config::config.codexAuthJsonB64 = saved; }
  };
  std::string jwt(std::optional<double> expires) {
    const auto header = rt::toBase64Url(rt::encodeUtf8(R"({"alg":"none","typ":"JWT"})"));
    json::object payload; if (expires) rt::objectSet(payload, "exp", rt::makeNumber(*expires));
    return header + "." + rt::toBase64Url(rt::encodeUtf8(rt::jsonStringify(payload))) + ".";
  }
  json::object authEntry(std::string account = "acct") { return {{"access_token", jwt(rt::Math::floor(rt::dateNow() / 1000) + 3600)}, {"refresh_token", "refresh"}, {"account_id", account}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}}; }
  void seed(Scope& scope, json::array accounts) { scope.store->seed(ca::CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::object{{"accounts", std::move(accounts)}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}})); }
  std::size_t commits(const Scope& scope) { return static_cast<std::size_t>(std::count_if(scope.store->commands.begin(), scope.store->commands.end(), [](const auto& command) { return command.command == "atomic.commit"; })); }
  const json::object& object(const json::value& value, std::string_view key) { const auto* result = rt::getObject(value, key); if (!result) throw rt::JsError("missing object " + std::string(key)); return *result; }
  const json::object& object(const json::object& value, std::string_view key) { const auto* result = rt::getObject(value, key); if (!result) throw rt::JsError("missing object " + std::string(key)); return *result; }
  rt::Task<json::value> passive(bool quota = false) { h::PassiveProviderHealthOptions options{quota}; auto task = h::handleHealthProviders(options); auto response = co_await std::move(task); EXPECT_EQ(response.status(), 200); const auto result = co_await response.json(); co_return result; }
  rt::Task<rt::Response> waitForAbort(std::optional<rt::AbortSignal> signal) {
    if (!signal) throw rt::JsError("health probe did not pass an abort signal");
    auto pending = rt::newPromise<rt::Response>([signal](rt::Resolve<rt::Response>, rt::Reject reject) {
      const auto rejectWithReason = [signal, reject] { reject(rt::makeReason(rt::withCause(rt::JsError("health probe fetch was aborted"), signal->reason()))); };
      if (signal->aborted()) rejectWithReason(); else signal->addEventListener("abort", rt::EventListener(rejectWithReason), {.once = true});
    });
    const auto result = co_await pending.wait(); co_return result;
  }
  std::string urlOf(const rt::RequestInfo& input) { return std::visit([](const auto& value) -> std::string { using T = std::decay_t<decltype(value)>; if constexpr (std::is_same_v<T, std::string>) return value; else if constexpr (std::is_same_v<T, rt::URL>) return value.href(); else return value.url(); }, input); }
}
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
TEST("getJwtExpMs ignores non-string, empty, and malformed tokens") {
  const json::array values{nullptr, 42, json::object{}, "", "not-a-jwt", "header.%%%.", "header.e30.signature.extra"};
  EXPECT(!ca::getJwtExpMs(static_cast<const json::value*>(nullptr)));
  for (const auto& value : values) EXPECT(!ca::getJwtExpMs(value));
}
ASYNC_TEST("passive provider health returns every Codex slot without contacting upstream") {
  Scope scope; seed(scope, {authEntry("private-account-a"), authEntry("private-account-b")});
  auto first = ph::recordCodexProviderHealth("private-account-a", "quota_exhausted", 429, [] { return 1000.0; }); co_await first.wait();
  auto second = ph::recordCodexProviderHealth("private-account-b", "refresh_failed", 401, [] { return 2000.0; }); co_await second.wait();
  auto third = ph::recordMeteredProviderHealth("success", 200, [] { return 3000.0; }); co_await third.wait();
  AWAIT(payload, passive()); EXPECT_EQ(rt::getString(payload, "mode"), std::optional<std::string_view>("passive"));
  const auto& codex = object(payload, "codex"); EXPECT_EQ(rt::getNumber(codex, "account_count"), std::optional(2.0)); EXPECT_EQ(rt::getString(codex, "state"), std::optional<std::string_view>("degraded"));
  const auto* rows = rt::getArray(codex, "accounts"); ASSERT(rows); ASSERT_EQ(rows->size(), 2u);
  EXPECT_EQ(rt::getNumber((*rows)[0], "slot"), std::optional(1.0)); EXPECT_EQ(rt::getString(object((*rows)[0], "health"), "state"), std::optional<std::string_view>("exhausted")); EXPECT_EQ(rt::getNumber(object((*rows)[0], "health"), "last_status"), std::optional(429.0));
  EXPECT_EQ(rt::getNumber((*rows)[1], "slot"), std::optional(2.0)); EXPECT_EQ(rt::getString(object((*rows)[1], "health"), "state"), std::optional<std::string_view>("invalid")); EXPECT_EQ(rt::getNumber(object((*rows)[1], "health"), "last_status"), std::optional(401.0));
  EXPECT_EQ(rt::getString(object(object(payload, "metered"), "health"), "state"), std::optional<std::string_view>("healthy"));
  EXPECT(!rt::hasOwn(object(object(payload, "metered"), "quota"), "balance_credits")); const auto text = rt::jsonStringify(payload); EXPECT(!rt::includes(text, "private-account-a")); EXPECT(!rt::includes(text, "private-account-b"));
}
ASYNC_TEST("passive provider health reports configured Cerebras without probing it") {
  Scope scope; rt::env::set("CEREBRAS_API_KEY", "cerebras-test-key"); auto record = ph::recordCerebrasProviderHealth("success", 200, [] { return 4000.0; }); co_await record.wait();
  AWAIT(payload, passive()); const auto& provider = object(payload, "cerebras"); EXPECT_EQ(rt::getBool(provider, "configured"), std::optional(true)); EXPECT_EQ(rt::getString(object(provider, "health"), "state"), std::optional<std::string_view>("healthy")); EXPECT_EQ(rt::getNumber(object(provider, "health"), "last_status"), std::optional(200.0));
  const std::function<double()> now = [] { return 4001.0; }; AWAIT(health, ph::getCerebrasProviderHealth(now)); EXPECT_EQ(health.state, "healthy");
}
ASYNC_TEST("passive provider health reports configured DeepSeek official without probing it") {
  Scope scope; rt::env::set("DEEPSEEK_API_KEY", "deepseek-test-key"); auto record = ph::recordDeepSeekProviderHealth("success", 200, [] { return 5000.0; }); co_await record.wait();
  AWAIT(payload, passive()); const auto& provider = object(payload, "deepseek"); EXPECT_EQ(rt::getBool(provider, "configured"), std::optional(true)); EXPECT_EQ(rt::getString(object(provider, "health"), "state"), std::optional<std::string_view>("healthy")); EXPECT_EQ(rt::getNumber(object(provider, "health"), "last_status"), std::optional(200.0));
  const std::function<double()> now = [] { return 5001.0; }; AWAIT(health, ph::getDeepSeekProviderHealth(now)); EXPECT_EQ(health.state, "healthy");
}
ASYNC_TEST("passive provider health reports DeepSeek as unconfigured without the official credential") {
  Scope scope; AWAIT(payload, passive()); EXPECT_EQ(rt::getBool(object(payload, "deepseek"), "configured"), std::optional(false)); EXPECT_EQ(rt::getString(object(object(payload, "deepseek"), "health"), "state"), std::optional<std::string_view>("unknown"));
}
ASYNC_TEST("admin provider health includes cached quota fields without an active refresh") {
  Scope scope; seed(scope, {authEntry()}); AWAIT(payload, passive(true)); const auto& quota = object(object(payload, "metered"), "quota"); EXPECT(rt::getBool(quota, "available").has_value()); EXPECT(rt::hasOwn(quota, "balance_credits")); EXPECT(!rt::hasOwn(payload.as_object(), "removed_provider"));
}
ASYNC_TEST("admin provider health exposes Surplus identity and honest quota availability") {
  Scope scope; rt::env::set("SURPLUS_API_KEY", "surplus-test-key"); auto record = ph::recordSurplusProviderHealth("success", 200, [] { return 4000.0; }); co_await record.wait();
  AWAIT(payload, passive(true)); const auto& provider = object(payload, "surplus"); EXPECT_EQ(rt::getBool(provider, "configured"), std::optional(true)); EXPECT_EQ(rt::getBool(provider, "quota_monitoring_configured"), std::optional(false)); EXPECT_EQ(rt::getString(object(provider, "health"), "state"), std::optional<std::string_view>("healthy")); EXPECT_EQ(rt::getBool(object(provider, "quota"), "available"), std::optional(false)); EXPECT_EQ(rt::getString(object(provider, "quota"), "source"), std::optional<std::string_view>("not_reported"));
}
ASYNC_TEST("provider health persists a recovery immediately while retaining the last 429") {
  Scope scope;
  auto a = ph::recordCodexProviderHealth("recovering-account", "success", 200, [] { return 1000.0; }); co_await a.wait();
  auto b = ph::recordCodexProviderHealth("recovering-account", "quota_exhausted", 429, [] { return 2000.0; }); co_await b.wait();
  auto c = ph::recordCodexProviderHealth("recovering-account", "success", 200, [] { return 3000.0; }); co_await c.wait();
  const std::function<double()> now = [] { return 3001.0; }; AWAIT(health, ph::getCodexProviderHealth("recovering-account", now)); EXPECT_EQ(health.state, "healthy"); EXPECT_EQ(health.last_429_at_ms, std::optional(2000.0)); EXPECT_EQ(health.last_success_at_ms, std::optional(3000.0));
}
ASYNC_TEST("provider reachability and token refresh stay degraded until inference succeeds") {
  Scope scope;
  auto a = ph::recordCodexProviderHealth("reachable-only-account", "reachable", 200, [] { return 500.0; }); co_await a.wait();
  const std::function<double()> now501 = [] { return 501.0; }; AWAIT(reachable, ph::getCodexProviderHealth("reachable-only-account", now501)); EXPECT_EQ(reachable.state, "degraded"); EXPECT_EQ(reachable.last_event, std::optional<std::string>("reachable")); EXPECT(!reachable.last_success_at_ms);
  auto b = ph::recordCodexProviderHealth("evidence-account", "success", 200, [] { return 1000.0; }); co_await b.wait();
  auto c = ph::recordCodexProviderHealth("evidence-account", "reachable", 200, [] { return 2000.0; }); co_await c.wait();
  auto d = ph::recordCodexProviderHealth("evidence-account", "refresh_success", 200, [] { return 3000.0; }); co_await d.wait();
  const std::function<double()> now3001 = [] { return 3001.0; }; AWAIT(refreshed, ph::getCodexProviderHealth("evidence-account", now3001)); EXPECT_EQ(refreshed.state, "degraded"); EXPECT_EQ(refreshed.last_event, std::optional<std::string>("refresh_success")); EXPECT_EQ(refreshed.last_success_at_ms, std::optional(1000.0)); EXPECT_EQ(refreshed.last_refresh_at_ms, std::optional(3000.0)); EXPECT_EQ(refreshed.last_refresh_succeeded, std::optional(true));
  auto e = ph::recordCodexProviderHealth("evidence-account", "success", 200, [] { return 4000.0; }); co_await e.wait();
  const std::function<double()> now4001 = [] { return 4001.0; }; AWAIT(recovered, ph::getCodexProviderHealth("evidence-account", now4001)); EXPECT_EQ(recovered.state, "healthy"); EXPECT_EQ(recovered.last_success_at_ms, std::optional(4000.0));
}
ASYNC_TEST("provider request IDs are bounded, normalized, and sampled without per-request writes") {
  Scope scope;
  auto a = ph::recordCodexProviderHealth("request-id-account", "reachable", 200, [] { return 1000.0; }); co_await a.wait(); EXPECT_EQ(commits(scope), 1u);
  auto b = ph::recordCodexProviderHealth("request-id-account", "reachable", 200, [] { return 2000.0; }, "  first-id  "); co_await b.wait(); EXPECT_EQ(commits(scope), 2u);
  auto c = ph::recordCodexProviderHealth("request-id-account", "reachable", 200, [] { return 3000.0; }, "second-id"); co_await c.wait(); EXPECT_EQ(commits(scope), 2u);
  const std::function<double()> now3001 = [] { return 3001.0; }; AWAIT(sampled, ph::getCodexProviderHealth("request-id-account", now3001)); EXPECT_EQ(sampled.last_provider_request_id, std::optional<std::string>("first-id")); EXPECT_EQ(sampled.last_provider_request_id_at_ms, std::optional(2000.0));
  auto d = ph::recordCodexProviderHealth("control-id-account", "reachable", 200, [] { return 4000.0; }, "bad\nrequest-id"); co_await d.wait(); const std::function<double()> now4001 = [] { return 4001.0; }; AWAIT(control, ph::getCodexProviderHealth("control-id-account", now4001)); EXPECT(!control.last_provider_request_id);
  auto e = ph::recordCodexProviderHealth("oversized-id-account", "reachable", 200, [] { return 5000.0; }, std::string(257, 'x')); co_await e.wait(); const std::function<double()> now5001 = [] { return 5001.0; }; AWAIT(oversized, ph::getCodexProviderHealth("oversized-id-account", now5001)); EXPECT(!oversized.last_provider_request_id);
}
ASYNC_TEST("provider health coalesces identical quota observations without hiding a later transition") {
  Scope scope; std::vector<rt::Promise<void>> pending;
  for (int index = 0; index < 100; ++index) pending.push_back(ph::recordCodexProviderHealth("coalesced-account", "quota_exhausted", 429, [] { return 1000.0; }));
  co_await rt::all(std::move(pending)).wait();
  const std::function<double()> now1001 = [] { return 1001.0; }; AWAIT(coalesced, ph::getCodexProviderHealth("coalesced-account", now1001)); EXPECT_EQ(coalesced.state, "exhausted"); EXPECT_EQ(coalesced.last_observed_at_ms, std::optional(1000.0)); EXPECT_EQ(commits(scope), 1u);
  auto record = ph::recordCodexProviderHealth("coalesced-account", "success", 200, [] { return 2000.0; }); co_await record.wait();
  const std::function<double()> now2001 = [] { return 2001.0; }; AWAIT(recovered, ph::getCodexProviderHealth("coalesced-account", now2001)); EXPECT_EQ(recovered.state, "healthy"); EXPECT_EQ(recovered.last_observed_at_ms, std::optional(2000.0)); EXPECT_EQ(recovered.last_429_at_ms, std::optional(1000.0));
}
ASYNC_TEST("provider health serializes success to 429 to success transitions") {
  Scope scope; std::vector<rt::Promise<void>> pending;
  pending.push_back(ph::recordCodexProviderHealth("ordered-account", "success", 200, [] { return 1000.0; })); pending.push_back(ph::recordCodexProviderHealth("ordered-account", "quota_exhausted", 429, [] { return 2000.0; })); pending.push_back(ph::recordCodexProviderHealth("ordered-account", "success", 200, [] { return 3000.0; })); co_await rt::all(std::move(pending)).wait();
  const std::function<double()> now = [] { return 3001.0; }; AWAIT(health, ph::getCodexProviderHealth("ordered-account", now)); EXPECT_EQ(health.state, "healthy"); EXPECT_EQ(health.last_observed_at_ms, std::optional(3000.0)); EXPECT_EQ(health.last_success_at_ms, std::optional(3000.0)); EXPECT_EQ(health.last_429_at_ms, std::optional(2000.0)); EXPECT_EQ(commits(scope), 3u);
}
ASYNC_TEST("public health reports release and admission with zero upstream and KV work") {
  Scope scope;
  const auto snapshot = uos::inference_admission::inferenceAdmissionSnapshot();
  const auto response = h::handleHealth();
  const auto payload = co_await response.json();
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(rt::getString(payload, "status"), std::optional<std::string_view>("available"));
  const auto& release = object(payload, "release");
  EXPECT_EQ(rt::getString(release, "git_sha"), std::optional<std::string_view>(uos::config::runtimeGitSha()));
#ifdef UOS_PACKAGE_GIT_COMMIT
  EXPECT_EQ(rt::getString(release, "git_sha"), std::optional<std::string_view>(UOS_PACKAGE_GIT_COMMIT));
  EXPECT(rt::getString(release, "git_sha") != std::optional<std::string_view>("unknown"));
#endif
  EXPECT(rt::getString(release, "deployment_id").has_value());
  const auto& admission = object(payload, "admission");
  EXPECT_EQ(rt::getNumber(admission, "active"), std::optional(snapshot.active));
  EXPECT_EQ(rt::getNumber(admission, "waiting"), std::optional(snapshot.waiting));
  EXPECT_EQ(rt::getNumber(admission, "max_active"), std::optional(snapshot.maxActive));
  EXPECT_EQ(rt::getNumber(admission, "max_waiting"), std::optional(snapshot.maxWaiting));
  EXPECT_EQ(rt::getString(admission, "limiting_resource"), std::optional<std::string_view>(snapshot.limitingResource));
  EXPECT_EQ(rt::getNumber(admission, "event_loop_lag_ms"), std::optional(snapshot.eventLoopLagMs));
  const auto* memory = rt::get(admission, "memory_headroom_bytes");
  const auto* descriptors = rt::get(admission, "descriptor_headroom");
  ASSERT(memory && descriptors);
  EXPECT_EQ(*memory, snapshot.memoryHeadroomBytes ? json::value(*snapshot.memoryHeadroomBytes) : json::value(nullptr));
  EXPECT_EQ(*descriptors, snapshot.descriptorHeadroom ? json::value(*snapshot.descriptorHeadroom) : json::value(nullptr));
  EXPECT(scope.store->commands.empty());
}
ASYNC_TEST("public health reports the confinement mode recorded at native startup") {
  T2IngressScope ingress;
  const auto reset = rt::scopeExit([] { h::setConfinementMode({}); });
  for (std::string_view mode : {"landlock", "landlock-reduced", "service-manager"}) {
    h::setConfinementMode(mode);
    const rt::Request request("https://ai.ubq.fi/health");
    AWAIT(response, uos::handler::index::handler(request));
    EXPECT_EQ(response.status(), 200);
    AWAIT(payload, response.json());
    EXPECT_EQ(rt::getString(payload, "confinement"), mode);
  }
}
ASYNC_TEST("public health HEAD is release liveness without a response body") {
  T2IngressScope ingress;
  rt::RequestInit init; init.method = "HEAD";
  const rt::Request request("https://ai.ubq.fi/health", init);
  AWAIT(response, uos::handler::index::handler(request));
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("content-type"), "application/json");
  EXPECT(response.headers().get("x-uos-git-sha").has_value());
  EXPECT(response.headers().get("x-uos-deployment-id").has_value());
  AWAIT(text, response.text()); EXPECT_EQ(text, "");
}
ASYNC_TEST("obsolete auth health route is not exposed") {
  T2IngressScope ingress;
  const rt::Request request("https://ai.ubq.fi/health/auth");
  AWAIT(response, uos::handler::index::handler(request));
  EXPECT_EQ(response.status(), 404);
}
ASYNC_TEST("detailed provider health and recheck routes require admin authentication") {
  T2IngressScope ingress;
  const std::vector<std::pair<std::string, std::string>> routes{
    {"GET", "/health/providers"}, {"GET", "/health/upstream"}, {"GET", "/admin/providers"},
    {"POST", "/admin/providers/codex/1/recheck"},
    {"GET", "/admin/providers/codex/cache-scope-experiment"},
    {"POST", "/admin/providers/codex/cache-scope-experiment"},
  };
  for (const auto& [method, path] : routes) {
    rt::RequestInit init; init.method = method;
    const rt::Request request("https://ai.ubq.fi" + path, init);
    AWAIT(response, uos::handler::index::handler(request));
    EXPECT_EQ(response.status(), 401);
  }
}
ASYNC_TEST("authenticated admin provider route is private and not cacheable") {
  const auto database = std::make_shared<CountingKv>();
  T2IngressScope ingress(database);
  const std::string token = "health-route-admin-token";
  uos::config::config.adminTokens.insert(token);
  rt::RequestInit init; init.headers = rt::Headers{{"Authorization", "Bearer " + token}};
  const rt::Request request("https://ai.ubq.fi/admin/providers", init);
  AWAIT(response, uos::handler::index::handler(request));
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("Cache-Control"), "no-store");
  AWAIT(payload, response.json()); EXPECT_EQ(rt::getString(payload, "mode"), "passive");
}

ASYNC_TEST("active upstream health retains detailed failure diagnostics") {
  Scope scope; seed(scope, {authEntry()});
  rt::GlobalFetchOverride failure{rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response("temporary error", {.status = 503, .headers = rt::Headers{{"Content-Type", "text/plain"}}}); })};
  AWAIT(response, h::handleHealthUpstream()); const auto payload = co_await response.json(); EXPECT_EQ(response.status(), 503); EXPECT_EQ(rt::getNumber(payload, "status"), std::optional(503.0));
  const auto* rows = rt::getArray(object(payload, "auth"), "accounts"); ASSERT(rows); ASSERT_EQ(rows->size(), 1u); const std::string refresh = "refresh"; const auto hash = co_await uos::utils::sha256Hex(refresh); EXPECT_EQ(uos::utils::getString(rt::get((*rows)[0], "refresh_sha256")), std::optional(hash)); EXPECT(rt::get((*rows)[0], "native_owner_codex_home")->is_null()); EXPECT_EQ(rt::getString(object(object(payload, "probes"), "codex"), "provider"), std::optional<std::string_view>("chatgpt_codex")); EXPECT_EQ(rt::getNumber(object(object(payload, "probes"), "codex"), "status"), std::optional(503.0));
}
ASYNC_TEST("active upstream health bounds malformed Codex auth metadata") {
  const json::array invalid{json::object{{"tokens", json::object{{"access_token", "legacy-access-token"}, {"refresh_token", "legacy-refresh-token"}}}}, json::object{{"accounts", json::array{json::object{{"access_token", "partial-access-token"}, {"refresh_token", "partial-refresh-token"}, {"account_id", "partial-account"}}}}, {"updated_at_ms", 1}}};
  for (const auto& record : invalid) { Scope scope; scope.store->seed(ca::CODEX_AUTH_POOL_KV_KEY, rt::KvValue(record)); AWAIT(response, h::handleHealthUpstream()); const auto text = co_await response.text(); const auto payload = rt::jsonParse(text); EXPECT_EQ(response.status(), 503); EXPECT_EQ(rt::getNumber(payload, "status"), std::optional(503.0)); EXPECT_EQ(rt::getNumber(object(object(payload, "probes"), "codex"), "status"), std::optional(503.0)); EXPECT(rt::length(text) < 2048); }
}
ASYNC_TEST("completed health probes release their timeout promises on the running loop") {
  Scope scope;
  seed(scope, {authEntry()});
  {
    const rt::GlobalFetchOverride mock{rt::Fetch([](rt::RequestInfo, rt::RequestInit) {
      return rt::Response::json(json::object{{"models", json::array{json::object{{"slug", "gpt-health"}}}}});
    })};
    const auto configured = co_await h::handleHealthUpstream();
    EXPECT_EQ(configured.status(), 200);
  }
  // Exercise the same completed probe race without accumulating the separate
  // Codex auth race's allocations across the samples.
  uos::kv::setKvForTest(std::make_shared<rt::MemoryKv>(rt::dateNow));
  const auto unconfigured = co_await h::handleHealthUpstream();
  const auto payload = co_await unconfigured.json();
  const auto& codex = object(object(payload, "probes"), "codex");
  EXPECT_EQ(rt::getString(codex, "error"), "No Codex auth configured (CODEX_AUTH_JSON_B64 or KV entry missing).");
  const auto probe = []() -> rt::Task<void> {
    const auto response = co_await h::handleHealthUpstream();
    EXPECT_EQ(response.status(), 503);
  };
  auto checking = uos::tests::helpers::expectStableLiveAllocations(probe, 64);
  co_await std::move(checking);
}
ASYNC_TEST("active upstream health preserves the provider that finishes before the shared deadline") {
  for (int stalled = 0; stalled < 2; ++stalled) {
    Scope scope; seed(scope, {authEntry()}); rt::env::set("METERED_API_KEY", "metered-api-key"); h::setActiveUpstreamHealthTimeoutMsForTest(20);
    rt::GlobalFetchOverride mock{rt::Fetch([stalled](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
      const rt::URL url(urlOf(input)); const bool codex = url.hostname() == "chatgpt.com";
      if ((stalled == 0 && codex) || (stalled == 1 && !codex)) return waitForAbort(init.signal);
      if (codex) return rt::fulfilledTask(rt::Response::json(json::object{{"models", json::array{json::object{{"slug", "gpt-health"}}}}}));
      if (url.pathname() == "/api/usage/token/") return rt::fulfilledTask(rt::Response::json(json::object{{"success", true}, {"data", json::object{{"total_available", 1'000'000}, {"total_granted", 1'000'000}, {"total_used", 10}, {"unlimited_quota", false}}}}));
      throw rt::JsError("Unexpected Metered URL: " + url.href());
    })};
    AWAIT(response, h::handleHealthUpstream()); const auto payload = co_await response.json(); EXPECT_EQ(response.status(), 503); const auto& probes = object(payload, "probes"); const auto& codex = object(probes, "codex"); const auto& metered = object(probes, "metered_quota");
    EXPECT_EQ(rt::getNumber(codex, "status"), std::optional(stalled == 0 ? 503.0 : 200.0)); EXPECT_EQ(rt::getNumber(metered, "status"), std::optional(stalled == 0 ? 200.0 : 503.0));
    EXPECT_EQ(rt::getString(stalled == 0 ? codex : metered, "error"), std::optional<std::string_view>(stalled == 0 ? "Codex models probe timed out." : "Metered quota probe timed out."));
    co_await rt::microtask(); co_await rt::microtask();
  }
}
ASYNC_TEST("Claude health status carries account workspace and allowlist provenance without secrets") {
  Scope scope;
  namespace credentials = uos::provider::claude_code_credentials;
  namespace accounts = uos::provider::claude_code_accounts;
  const credentials::StoreClaudeCodeStoredCredentialInput input{.secret = "fixture-claude-secret", .label = "fixture-label"};
  AWAIT(stored, credentials::storeClaudeCodeStoredCredential(scope.store, input)); ASSERT(stored);
  const credentials::ClaudeCodeWorkspaceState workspace{"workspace-fixture", "Fixture workspace", "kv", 10, 20, std::nullopt};
  AWAIT(workspaceStored, credentials::storeClaudeCodeWorkspaceState(scope.store, workspace)); EXPECT(workspaceStored);
  AWAIT(account, accounts::resolveClaudeCodeAccount(scope.store)); ASSERT(account);
  const std::vector<std::string> ids{"fixture-principal"}; AWAIT(access, uos::provider::claude_code_access::storeClaudeCodeAccessPolicy(*scope.store, ids)); EXPECT_EQ(access.allowed_key_ids, ids);
  AWAIT(snapshot, h::getClaudeSubscriptionStatusSnapshot());
  EXPECT_EQ(rt::getBool(snapshot, "configured"), std::optional(true)); EXPECT_EQ(uos::utils::getString(rt::get(snapshot, "account_id")), std::optional(account->account_id)); EXPECT_EQ(uos::utils::getString(rt::get(snapshot, "account_cohort_id")), std::optional(account->account_cohort_id));
  EXPECT_EQ(rt::getString(object(snapshot, "workspace"), "id"), std::optional<std::string_view>("workspace-fixture")); EXPECT_EQ(rt::getString(object(snapshot, "workspace"), "name"), std::optional<std::string_view>("Fixture workspace")); EXPECT_EQ(rt::getNumber(object(snapshot, "workspace"), "verified_at_ms"), std::optional(20.0));
  EXPECT_EQ(rt::getBool(snapshot, "principal_allowlist_configured"), std::optional(true)); EXPECT_EQ(rt::getString(object(snapshot, "credential"), "label"), std::optional<std::string_view>("fixture-label")); EXPECT_EQ(rt::getBool(object(snapshot, "credential"), "refresh_material_present"), std::optional(false));
  EXPECT(!rt::includes(rt::jsonStringify(snapshot), "fixture-claude-secret"));
}
ASYNC_TEST("Claude probe refusal reports independently from successful Codex health") {
  Scope scope; seed(scope, {authEntry()});
  namespace credentials = uos::provider::claude_code_credentials;
  const credentials::StoreClaudeCodeStoredCredentialInput input{.secret = "fixture-claude-probe-secret"}; AWAIT(stored, credentials::storeClaudeCodeStoredCredential(scope.store, input)); ASSERT(stored);
  rt::GlobalFetchOverride mock{rt::Fetch([](rt::RequestInfo info, rt::RequestInit) {
    const rt::URL url(urlOf(info));
    return url.hostname() == "api.anthropic.com" ? rt::Response("workspace refused", {.status = 403, .headers = rt::Headers{{"Content-Type", "text/plain"}}}) : rt::Response::json(json::object{{"models", json::array{json::object{{"slug", "gpt-health"}}}}});
  })};
  AWAIT(response, h::handleHealthUpstream()); const auto payload = co_await response.json();
  EXPECT_EQ(response.status(), 200); const auto& probes = object(payload, "probes"); EXPECT_EQ(rt::getNumber(object(probes, "codex"), "status"), std::optional(200.0)); EXPECT_EQ(rt::getNumber(object(probes, "claude_subscription"), "status"), std::optional(503.0)); EXPECT_EQ(rt::getString(object(probes, "claude_subscription"), "error"), std::optional<std::string_view>("workspace refused"));
}
