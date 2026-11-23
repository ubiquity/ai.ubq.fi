// Links: tests/serve_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
//   src/serve.cpp src/kv.cpp src/auth/local_admin.cpp src/config.cpp src/auth/local_development_key.cpp src/authority/native_paid.cpp src/types.cpp src/cache/prompt_analytics_queue.cpp
//   src/optional_telemetry_queue.cpp src/provider/metered.cpp src/utils.cpp src/authority/native_paid_policy.cpp src/api_key_policy.cpp src/authority/native_api_key_policy.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_stats.cpp
//   src/cache/prompt_analytics_core.cpp src/provider/surplus_claude_request.cpp tests/helpers/counting_kv.cpp src/authority/native_paid_contracts.cpp src/runtime/http.cpp src/authority/native_api_key.cpp src/runtime/http_body.cpp src/http.cpp
//   src/api_keys.cpp src/auth/relay.cpp src/paid_fallback/ledger_backfill.cpp src/handler/serve_handler.cpp src/codex/reset_settings.cpp src/release.cpp src/provider/anthropic_responses.cpp src/auth/index.cpp src/codex/auth.cpp
//   src/bounded_response_body.cpp src/kernel/quota_reservations.cpp src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp src/models/catalog.cpp src/provider/surplus.cpp src/input_normalization.cpp src/codex/native_auth.cpp src/models/metadata.cpp
//   src/auth/passport.cpp src/paid_fallback/ledger_state.cpp src/handler/index.cpp src/models/codex_models_fetch.cpp src/models/codex_models_whitelist.cpp src/runtime/http_early_sse.cpp src/health.cpp src/provider/capacity.cpp src/auth/passport_sessions.cpp
//   src/request_policy.cpp src/provider/capacity_rollups.cpp src/models/codex_models_availability.cpp src/metered_quota.cpp src/provider/capacity_parse.cpp src/auth/passkeys.cpp src/admin/kernel.cpp src/agent_messages.cpp src/provider/surplus_claude_chat_request.cpp
//   src/provider/capacity_history.cpp src/admin/codex.cpp src/provider/health.cpp src/admin/api_keys.cpp src/provider/selection.cpp src/codex/supervisor.cpp src/kernel/usage.cpp src/admin/sentinel_incident.cpp src/codex/auth_refresh.cpp
//   src/provider/anthropic_replay.cpp src/catalog/store.cpp src/provider/capacity_contract.cpp src/live/handler.cpp src/provider/claude_capacity_history.cpp src/runtime/http_upgrade.cpp src/models/openrouter_models.cpp src/quota_projection.cpp src/live/calls.cpp
//   src/catalog/index.cpp src/cache/scope_experiment.cpp src/cache/scope_experiment_targets.cpp src/codex/account_routing.cpp src/codex/dispatch.cpp src/live/upstream.cpp src/catalog/models.cpp src/admin/error_log.cpp src/codex/routing_serial.cpp
//   src/handler/http.cpp src/codex/native_auth_transport.cpp src/responses_stream.cpp src/authority/native_codex_mutations.cpp src/codex/routing_mutations.cpp src/provider/claude_capacity_rollups.cpp src/kernel/quota_v2.cpp src/cache/scope_experiment_model.cpp src/deepseek/waterfall.cpp
//   src/admin/claude_code.cpp src/authority/native_codex_capacity.cpp src/codex/routing_evaluation.cpp src/admin/defaults.cpp src/provider/cerebras_rate_limits.cpp src/codex/supervisor_brief.cpp src/authority/native_codex_contracts.cpp src/provider/claude_code_accounts.cpp src/analytics.cpp
//   src/authority/native_codex_evaluation.cpp src/admin/api_key_mutations.cpp src/live/sideband.cpp src/paid_fallback/ledger_admission.cpp src/provider/claude_code_credentials.cpp src/handler/terminal_route.cpp src/provider/claude_code_models.cpp src/defaults.cpp src/kernel/policy_queue.cpp
//   src/cache/kv_migration_validation.cpp src/catalog/promotion.cpp src/codex/supervisor_config.cpp src/cache/kv_migration_base.cpp src/provider/anthropic_response_wire.cpp src/codex/capacity_routing.cpp src/provider/capacity_events.cpp src/provider/openrouter.cpp src/provider/anthropic_chat_responses.cpp
//   src/models/codex_models.cpp src/provider/claude_code_access.cpp src/sentinel/replay_observation.cpp src/provider/claude_code_quota.cpp src/codex/supervisor_secret.cpp src/codex/experiment.cpp src/codex/banked_reset.cpp src/codex/external_auth.cpp src/codex/routing_state.cpp
//   src/cache/kv_migration_paid_fallback.cpp src/cache/kv_migration_usage.cpp src/provider/cerebras.cpp src/admin/api_key_audit.cpp src/authority/native_codex.cpp src/sentinel/replay_model.cpp src/static.cpp src/provider/claude_code.cpp src/provider/anthropic_chat_stream.cpp
//   src/sentinel/replay_retention_schema.cpp src/codex/supervisor_follow.cpp src/admin/codex_reset.cpp src/sentinel/replay_limits.cpp src/admin/capacity_observation_sync.cpp src/codex/banked_reset_provider.cpp src/kernel/quota_reserve.cpp src/cache/prompt_analytics_read.cpp src/kernel/attestation.cpp
//   src/codex/rate_limit_429.cpp src/systemone/handlers.cpp src/codex/supervisor_inventory.cpp src/runtime_config.cpp src/embeddings/jobs.cpp src/cache/telemetry_gate.cpp src/embeddings/voyage.cpp src/codex/overage_settings.cpp src/embeddings/ledger.cpp
//   src/kernel/quota_write.cpp src/openai_telemetry.cpp src/handler/terminal_log.cpp src/catalog/types.cpp src/embeddings/handlers.cpp src/handler/admission.cpp src/chat/envelope.cpp src/request.cpp src/provider/capacity_analytics.cpp
//   src/paid_fallback/rollups.cpp src/authority/native_api_key_audit.cpp src/runtime/http_tasks.cpp src/admin/chart_source_proxy.cpp src/cache/scope_targets.cpp src/upstream_wire.cpp src/openai.cpp src/debug_routing.cpp src/storage/kv.cpp
//   src/sentinel/replay_store.cpp src/codex/manual_reset.cpp src/provider/claude_code_usage_schema.cpp src/codex/supervisor_transport.cpp src/codex/banked_reset_pool.cpp src/provider/lithos.cpp src/sentinel/incident_outbox.cpp src/sentinel/replay_envelope.cpp src/deepseek/index.cpp
//   src/sentinel/replay_retention.cpp src/live/routes.cpp src/responses_native_headers.cpp src/codex/banked_reset_claim.cpp src/chat/completions.cpp src/sentinel/upstream_capture.cpp src/runtime/websocket.cpp src/authority/native_codex_429.cpp src/provider/openrouter_handlers.cpp
//   src/inference_deadline.cpp src/provider/openrouter_streams.cpp src/sentinel/replay_read.cpp src/codex/banked_reset_submission.cpp src/provider/presentation.cpp src/sentinel/replay_retention_bootstrap.cpp src/provider/lithos_models.cpp src/chat/stream_translation.cpp src/inference_admission.cpp
//   src/recent_model_context.cpp src/kv_conversion/profile_json.cpp src/storage/sqlite_readonly.cpp src/provider/openrouter_request_controls.cpp src/responses_failover_stream.cpp src/images.cpp src/openai_schema.cpp src/sentinel/replay_retention_metadata.cpp src/provider/removed_provider_telemetry.cpp
//   src/decisions/handlers.cpp src/deepseek/context_admission_failure.cpp src/sentinel/replay_admin.cpp src/codex/quota.cpp src/lib/jev_compaction/codex_items.cpp src/deepseek/handlers.cpp src/provider/lithos_rate_limits.cpp src/codex/supervisor_log.cpp src/responses_handler.cpp
//   src/responses_request.cpp src/codex/manual_reset_state.cpp src/provider/lithos_handlers.cpp src/responses_attempts.cpp src/deepseek/responses.cpp src/storage/native_kv.cpp src/provider/cerebras_handlers.cpp src/paid_fallback/removed_provider.cpp src/paid_fallback/health.cpp
//   src/provider/removed_provider_circuit.cpp src/paid_fallback/routing.cpp src/provider/lithos_streams.cpp src/deepseek/forwarded_payload_policy.cpp src/provider/stream_relay.cpp src/provider/openrouter_web_tools.cpp src/deepseek/responses_payload.cpp src/codex/index.cpp src/lib/jev_compaction/compact.cpp
//   src/deepseek/chat_projection.cpp src/responses_buffered.cpp src/deepseek/stream.cpp src/deepseek/flash_aliases.cpp src/paid_fallback/claude_code_route.cpp src/deepseek/responses_stream.cpp src/deepseek/waterfall_handler.cpp src/codex/responses_error.cpp src/lib/jev_compaction/state.cpp
//   src/provider/cerebras_responses.cpp src/lib/jev_compaction/types.cpp src/lib/jev_compaction/request.cpp src/codex/responses_runners.cpp src/codex/responses_operations.cpp src/codex/responses_state.cpp
// New module evidence only: serve.ts has no original direct unit registrations.
#include "uos/serve.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/cache/prompt_analytics_queue.hpp"
#include "tests/helpers/counting_kv.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test_async.hpp"
namespace rt=uos::rt;
namespace serve=uos::serve;
namespace queue=uos::cache::prompt_analytics_queue;

namespace {
  class DelayedMigrationStore final : public rt::ForwardingKv {
  public:
    explicit DelayedMigrationStore(std::shared_ptr<rt::Kv> store) : ForwardingKv(*store), owner(std::move(store)) {}

    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options) override {
      const bool marker = rt::kvKeyStrictEquals(key, uos::codex::reset_settings::CODEX_RESET_USAGE_MIGRATION_KEY);
      if (marker) co_await rt::sleep(1'000);
      auto pending = owner->get(std::move(key), options);
      auto entry = co_await std::move(pending);
      if (marker) readFinished = true;
      co_return entry;
    }

    void close() override {
      ++closes;
      readFinishedAtClose = readFinished;
      owner->close();
    }

    int closes = 0;
    bool readFinishedAtClose = false;

  private:
    std::shared_ptr<rt::Kv> owner;
    bool readFinished = false;
  };
}

TEST("serve module publishes inert application callables without invoking its guarded launcher") {
  EXPECT(static_cast<bool>(serve::application.fetch));
  EXPECT(static_cast<bool>(serve::application.upgrade));
  EXPECT(static_cast<bool>(serve::application.onListen));
  rt::Task<void> (*volatile entry)(const serve::ServeStartupOptions&) = &serve::importMetaMain;
  EXPECT(entry!=nullptr);
  EXPECT(!rt::fs::existsSync(".data"));
}
ASYNC_TEST("serve optional telemetry shutdown returns without a sample or shutdown log") {
  queue::resetOptionalPromptCacheAnalyticsForTest();
  rt::console::Capture logging({rt::console::Method::info});
  auto shuttingDown=serve::shutdownOptionalTelemetry();co_await std::move(shuttingDown);
  EXPECT(!queue::optionalPromptCacheAnalyticsSnapshot());
  EXPECT(logging.calls().empty());
  queue::resetOptionalPromptCacheAnalyticsForTest();
}
ASYNC_TEST("serve startup maintenance runs once and returns the same idempotent stop promise") {
  auto kv=std::make_shared<uos::tests::helpers::counting_kv::CountingKv>();
  rt::console::Capture logging;
  auto stop=serve::startMacMaintenance(kv);
  auto first=stop();auto second=stop();EXPECT(first==second);
  co_await first.wait();
  const auto commands=kv->commands.size();EXPECT(commands>0);
  co_await rt::sleep(60'000);
  auto third=stop();EXPECT(first==third);co_await third.wait();
  EXPECT_EQ(kv->commands.size(),commands);
}
ASYNC_TEST("serve loopback runtime parsing rejects invalid and hosted auth-bypass arguments before KV access") {
  const auto original=uos::config::config.isDeploy;
  const auto restore=rt::scopeExit([original]{uos::config::config.isDeploy=original;});
  uos::config::config.isDeploy=true;
  std::vector<std::string> noArguments;
  auto ordinary=serve::configureLoopbackServeRuntime(noArguments);co_await std::move(ordinary);
  std::vector<std::string> bypass{"--disable-admin-auth"};
  auto denied=serve::configureLoopbackServeRuntime(bypass);
  EXPECT_REJECTS_MESSAGE(std::move(denied),rt::JsError,"[ai.ubq.fi] --disable-admin-auth is unavailable in Deno Deploy.");
  std::vector<std::string> invalid{"--unknown"};
  auto unknown=serve::configureLoopbackServeRuntime(invalid);
  EXPECT_REJECTS(std::move(unknown),rt::JsError);
  auto resetting=serve::configureLoopbackServeRuntime(noArguments);co_await std::move(resetting);
}

ASYNC_TEST("serve migration initializes only through its explicit callable and uses injected fixture state") {
  auto kv=std::make_shared<uos::tests::helpers::counting_kv::CountingKv>();
  uos::kv::setKvForTest(kv);
  const auto restore=rt::scopeExit([]{uos::kv::setKvForTest(nullptr);});
  rt::console::Capture logging;
  EXPECT(kv->commands.empty());
  const auto initialized=serve::initializeModule();co_await initialized.wait();
  EXPECT(!kv->commands.empty());
  EXPECT(!rt::fs::existsSync(".data"));
}

ASYNC_TEST("serve startup failure waits for its migration before closing KV") {
  const auto memory = std::make_shared<rt::MemoryKv>(rt::dateNow);
  const rt::KvValue marker(true);
  co_await memory->set(uos::codex::reset_settings::CODEX_RESET_USAGE_MIGRATION_KEY, marker);
  const auto store = std::make_shared<DelayedMigrationStore>(memory);
  uos::kv::setKvForTest(store);
  const auto restore = rt::scopeExit([] { uos::kv::setKvForTest(nullptr); });
  rt::console::Capture listenerLog({rt::console::Method::log});
  listenerLog.setObserver([](const rt::console::Call&) { throw rt::JsError("controlled listener setup failure"); });
  const serve::ServeStartupOptions loopback{"127.0.0.1", 0};
  auto serving = serve::importMetaMain(loopback);
  EXPECT_REJECTS_MESSAGE(std::move(serving), rt::JsError, "controlled listener setup failure");
  EXPECT_EQ(listenerLog.calls().size(), 1u);
  EXPECT_EQ(store->closes, 1);
  EXPECT(store->readFinishedAtClose);
}
