// Links: tests/admin_codex_reset_test.cpp
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
// src/utils.cpp tests/helpers/codex_banked_reset_harness.cpp tests/helpers/manual_reset_harness.cpp
// Port of tests/admin-codex-reset.test.ts; direct cases and the real registered authentication gate.
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
#include "tests/helpers/manual_reset_harness.hpp"
#include "uos/admin/codex_reset.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/test_async.hpp"
namespace rt = uos::rt;
namespace json = uos::json;
namespace harness = uos::tests::helpers::manual_reset_harness;
using uos::admin::codex_reset::handleAdminCodexResetConsume;
namespace {
  rt::Request request(const json::value& body) {
    rt::RequestInit init; init.method = "POST"; init.headers = rt::Headers{{"Content-Type", "application/json"}}; init.body = rt::jsonStringify(body);
    return rt::Request("http://localhost/admin/providers/codex/banked-resets/consume", init);
  }
}
ASYNC_TEST("admin consume accepts only stable account identity") {
  harness::CredentialScope scope;
  const std::vector<json::value> values{json::object{}, json::object{{"account_id_hash", "bad"}}, json::object{{"account_id_hash", std::string(64, 'a')}, {"access_token", "forbidden"}}, json::object{{"account_id_hash", std::string(64, 'a')}, {"slot", 1}}, json::object{{"account_id_hash", std::string(rt::limits::kRegexSubjectBytes + 1, 'a')}}};
  for (const auto& value : values) { auto handling = handleAdminCodexResetConsume(request(value)); const auto response = co_await std::move(handling); EXPECT_EQ(response.status(), 400); }
}
ASYNC_TEST("admin consume reports verified matching account only after promotion") {
  harness::CredentialScope scope; auto creating = harness::manualFixture(); const auto fixture = co_await std::move(creating);
  const json::value body(json::object{{"account_id_hash", fixture->targetHash}}); auto handling = handleAdminCodexResetConsume(request(body), fixture->dependencies); auto response = co_await std::move(handling); EXPECT_EQ(response.status(), 200); const auto result = co_await response.json();
  const json::value expected(json::object{{"reset_status", "verified"}, {"active_account_id_hash", fixture->targetHash}}); EXPECT(rt::deepEqual(result, expected)); EXPECT_EQ(response.headers().get("Cache-Control"), std::optional<std::string>("no-store"));
}
ASYNC_TEST("the registered consume route retains ordinary admin authentication") {
  T2IngressScope ingress;
  const json::value body(json::object{{"account_id_hash", std::string(64, 'a')}});
  auto handling = uos::handler::index::handler(request(body));
  auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 401);
}

ASYNC_TEST("duplicate API response cannot claim current priority after a later active transition") {
  harness::CredentialScope scope; auto creating = harness::manualFixture(); const auto fixture = co_await std::move(creating);
  const json::value body(json::object{{"account_id_hash", fixture->targetHash}}); auto firstTask = handleAdminCodexResetConsume(request(body), fixture->dependencies); const auto first = co_await std::move(firstTask); EXPECT_EQ(first.status(), 200);
  auto active = fixture->active; active["generation"] = 20; const rt::KvValue later(active); co_await fixture->kv->set(uos::codex::routing_state::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, later);
  auto duplicateTask = handleAdminCodexResetConsume(request(body), fixture->dependencies); auto duplicate = co_await std::move(duplicateTask); EXPECT_EQ(duplicate.status(), 409); const auto result = co_await duplicate.json(); EXPECT_EQ(rt::getString(result.as_object().at("error"), "code"), std::optional<std::string>("codex_reset_already_completed")); EXPECT(!rt::get(result, "active_account_id_hash")); EXPECT_EQ(fixture->provider->redeemInputs.size(), 1u);
}
