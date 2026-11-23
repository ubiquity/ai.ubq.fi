// Port of all four original tests/handler-replay-background.test.ts cases.
// Links: tests/handler_replay_background_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
//       src/kv.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/config.cpp src/handler/http.cpp src/api_key_policy.cpp src/release.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_core.cpp src/authority/native_api_key.cpp src/utils.cpp src/http.cpp src/authority/native_api_key_policy.cpp
//       src/authority/native_paid_policy.cpp src/optional_telemetry_queue.cpp src/api_keys.cpp src/types.cpp src/codex/quota.cpp src/handler/terminal_log.cpp src/handler/terminal_route.cpp src/handler/admission.cpp src/sentinel/replay_observation.cpp src/storage/native_kv.cpp src/auth/index.cpp src/kernel/attestation.cpp
//       src/sentinel/replay_model.cpp src/kernel/quota_reserve.cpp src/kernel/quota_reservations.cpp src/authority/native_kernel.cpp src/inference_admission.cpp src/kernel/quota_v2.cpp src/auth/passport.cpp src/sentinel/replay_store.cpp src/kernel/policy_queue.cpp src/sentinel/incident_outbox.cpp src/paid_fallback/rollups.cpp
//       src/runtime/http_tasks.cpp src/responses_native_headers.cpp src/provider/cerebras_rate_limits.cpp src/chat/envelope.cpp src/openai_telemetry.cpp src/authority/native_kernel_contracts.cpp src/sentinel/replay_envelope.cpp src/request_policy.cpp src/upstream_wire.cpp src/openai.cpp src/paid_fallback/routing.cpp
//       src/paid_fallback/claude_code_route.cpp src/provider/surplus.cpp src/decisions/handlers.cpp src/openai_schema.cpp src/provider/metered.cpp src/provider/lithos.cpp src/sentinel/replay_read.cpp src/authority/native_paid_contracts.cpp src/auth/local_development_key.cpp src/provider/openrouter.cpp src/request.cpp
//       src/provider/openrouter_request_controls.cpp src/provider/claude_code_models.cpp src/responses_stream.cpp src/provider/surplus_claude_request.cpp src/models/openrouter_models.cpp src/provider/claude_code_accounts.cpp src/paid_fallback/health.cpp src/cache/telemetry_gate.cpp src/paid_fallback/index.cpp src/bounded_response_body.cpp
//       src/runtime/http_early_sse.cpp src/provider/claude_code_credentials.cpp src/embeddings/handlers.cpp src/models/codex_models_fetch.cpp src/sentinel/replay_retention_schema.cpp src/provider/selection.cpp src/provider/claude_code_quota.cpp src/provider/claude_code.cpp src/auth/passport_sessions.cpp src/authority/native_paid.cpp
//       src/codex/auth.cpp src/codex/dispatch.cpp src/models/catalog.cpp src/provider/health.cpp src/deepseek/flash_aliases.cpp src/provider/cerebras.cpp src/embeddings/ledger.cpp src/embeddings/voyage.cpp src/sentinel/replay_retention.cpp src/input_normalization.cpp src/provider/openrouter_handlers.cpp src/auth/local_admin.cpp
//       src/admin/error_log.cpp src/embeddings/jobs.cpp src/sentinel/replay_limits.cpp src/catalog/index.cpp src/defaults.cpp src/models/metadata.cpp src/catalog/models.cpp src/catalog/store.cpp src/models/codex_models.cpp src/provider/capacity_events.cpp src/provider/surplus_claude_chat_request.cpp src/codex/auth_refresh.cpp
//       src/sentinel/replay_retention_metadata.cpp src/chat/stream_translation.cpp src/deepseek/context_admission_failure.cpp src/debug_routing.cpp src/sentinel/upstream_capture.cpp src/codex/external_auth.cpp src/recent_model_context.cpp src/catalog/types.cpp src/paid_fallback/ledger_backfill.cpp src/deepseek/waterfall.cpp src/runtime_config.cpp
//       src/provider/claude_code_access.cpp src/provider/cerebras_handlers.cpp src/codex/capacity_routing.cpp src/codex/routing_mutations.cpp src/sentinel/replay_retention_bootstrap.cpp src/auth/relay.cpp src/provider/anthropic_replay.cpp src/provider/lithos_rate_limits.cpp src/authority/native_codex_mutations.cpp
//       src/provider/anthropic_chat_responses.cpp src/provider/lithos_handlers.cpp src/provider/openrouter_streams.cpp src/authority/native_codex_capacity.cpp src/inference_deadline.cpp src/lib/jev_compaction/codex_items.cpp src/responses_handler.cpp src/provider/anthropic_chat_stream.cpp src/auth/passkeys.cpp
//       src/authority/native_codex_contracts.cpp src/provider/removed_provider_circuit.cpp src/codex/native_auth.cpp src/provider/anthropic_responses.cpp src/deepseek/responses.cpp src/systemone/handlers.cpp src/analytics.cpp src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_settlement.cpp src/models/codex_models_availability.cpp
//       src/provider/lithos_models.cpp src/provider/removed_provider_telemetry.cpp src/deepseek/forwarded_payload_policy.cpp src/codex/native_auth_transport.cpp src/deepseek/responses_payload.cpp src/paid_fallback/ledger_state.cpp src/codex/rate_limit_429.cpp src/provider/anthropic_response_wire.cpp src/deepseek/chat_projection.cpp
//       src/responses_failover_stream.cpp src/provider/openrouter_web_tools.cpp src/responses_request.cpp src/images.cpp src/deepseek/index.cpp src/responses_attempts.cpp src/authority/native_codex_429.cpp src/provider/lithos_streams.cpp src/chat/completions.cpp src/paid_fallback/removed_provider.cpp src/models/codex_models_whitelist.cpp
//       src/codex/index.cpp src/deepseek/handlers.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/responses_buffered.cpp src/provider/capacity_history.cpp src/codex/banked_reset.cpp src/provider/capacity_parse.cpp src/provider/claude_capacity_history.cpp src/codex/responses_state.cpp src/lib/jev_compaction/state.cpp
//       src/runtime/websocket.cpp src/provider/claude_capacity_rollups.cpp src/codex/routing_state.cpp src/authority/native_codex_evaluation.cpp src/deepseek/stream.cpp src/provider/claude_code_usage_schema.cpp src/provider/capacity_analytics.cpp src/provider/capacity_rollups.cpp src/provider/capacity_contract.cpp
//       src/cache/prompt_analytics_read.cpp src/codex/responses_error.cpp src/codex/responses_operations.cpp src/codex/banked_reset_provider.cpp src/provider/stream_relay.cpp src/deepseek/waterfall_handler.cpp src/lib/jev_compaction/compact.cpp src/metered_quota.cpp src/codex/account_routing.cpp src/codex/routing_evaluation.cpp
//       src/codex/routing_serial.cpp src/codex/reset_settings.cpp src/authority/native_codex.cpp src/lib/jev_compaction/types.cpp src/provider/cerebras_responses.cpp src/deepseek/responses_stream.cpp src/codex/banked_reset_pool.cpp src/codex/experiment.cpp src/codex/overage_settings.cpp src/codex/banked_reset_claim.cpp
//       src/codex/responses_runners.cpp src/codex/banked_reset_submission.cpp src/lib/jev_compaction/request.cpp
#include "tests/helpers/terminal_log_harness.hpp"
#include "uos/handler/terminal_route.hpp"

using namespace uos;
namespace fixture = tests::helpers::terminal_log_harness;
namespace terminal = handler::terminal_log;

ASYNC_TEST("terminal logging reports admission busy failures to the admin error ledger") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  std::vector<admin::error_log::AdminErrorInput> recorded;
  auto options = fixture::terminalOptions("admission-busy-admin-error");
  options.recordAdminError = [&recorded](admin::error_log::AdminErrorInput error) {
    recorded.push_back(std::move(error)); return rt::Promise<void>::resolve();
  };
  auto wrapped = terminal::withTerminalRequestLog(fixture::jsonResponse(
    R"({"error":{"message":"All Codex accounts are busy","type":"server_error","code":"codex_admission_busy"}})", 503), options);
  auto response = co_await wrapped.wait();
  EXPECT_EQ(response.status(), 503);
  ASSERT(recorded.size() == 1);
  EXPECT_EQ(recorded[0].request_id, std::string("admission-busy-admin-error"));
  EXPECT_EQ(recorded[0].status, 503.0);
  EXPECT_EQ(recorded[0].terminal_type, std::optional<std::string>("http.error"));
  EXPECT_EQ(recorded[0].failure_kind, std::optional<std::string>("codex_admission_busy"));
}

ASYNC_TEST("EdgeRuntime replay registration returns failure responses before deferred persistence settles") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  const auto previous = handler::http::globalEdgeRuntime();
  const auto restore = rt::scopeExit([previous] { handler::http::globalEdgeRuntime() = previous; });
  std::vector<rt::Promise<void>> registered;
  const auto persistence = rt::withResolvers<sentinel::replay_observation::SentinelReplayPersistResult>();
  const auto started = rt::withResolvers<void>();
  bool persistenceSettled = false;
  std::optional<sentinel::replay_model::AcceptedSentinelReplayInput> snapshot;
  auto capture = fixture::acceptedInput();
  const auto originalBytes = *capture.body;
  handler::http::globalEdgeRuntime() = handler::http::SentinelBackgroundRuntime{
    .waitUntil = [&registered](rt::Promise<void> task) { registered.push_back(task); }};
  auto options = fixture::terminalOptions("handler-replay-background");
  options.sentinelReplayInput = capture;
  options.persistSentinelReplay = [&snapshot, persistence, started](auto input, auto, auto) {
    snapshot = std::move(input); started.resolve(); return persistence.promise;
  };
  auto wrapped = terminal::withTerminalRequestLog(fixture::jsonResponse(R"({"error":{"code":"provider_transport"}})", 502), options);
  auto response = co_await wrapped.wait();
  EXPECT_EQ(response.status(), 502);
  co_await started.promise.wait();
  ASSERT(registered.size() == 1);
  ASSERT(snapshot.has_value());
  EXPECT(snapshot->body != capture.body);
  EXPECT_EQ(*snapshot->body, originalBytes);
  EXPECT(fixture::zeroed(capture.body));
  EXPECT(!persistenceSettled);
  persistenceSettled = true;
  persistence.reject(rt::makeReason(rt::JsError("fixture persistence failure")));
  co_await registered[0].wait();
  EXPECT_EQ(response.status(), 502);
  EXPECT(fixture::zeroed(capture.body));
  EXPECT(fixture::zeroed(snapshot->body));
}

ASYNC_TEST("thrown inference errors propagate before deferred exception replay persistence settles") {
  fixture::Isolation isolation;
  const auto previous = handler::http::globalEdgeRuntime();
  const auto restore = rt::scopeExit([previous] { handler::http::globalEdgeRuntime() = previous; });
  std::vector<rt::Promise<void>> registeredTasks;
  const auto persistence = rt::withResolvers<sentinel::replay_observation::SentinelReplayPersistResult>();
  std::vector<sentinel::replay_model::AcceptedSentinelReplayInput> persisted;
  bool persistenceSettled = false;
  auto capture = fixture::acceptedInput();
  const auto originalBytes = *capture.body;
  const rt::Reason runError = rt::makeReason(rt::JsError("fixture inference failure"));
  handler::http::globalEdgeRuntime() = handler::http::SentinelBackgroundRuntime{
    .waitUntil = [&registeredTasks](rt::Promise<void> task) { registeredTasks.push_back(task); }};
  const auto settleOnFailure = rt::scopeExit([&persistenceSettled, persistence] {
    if (!persistenceSettled) persistence.resolve(sentinel::replay_observation::SentinelReplayPersistResultDisabled{.reason = "kv_unavailable"});
  });
  handler::terminal_route::PersistInferenceExceptionReplayOptions options;
  options.persistSentinelReplay = [&persisted, persistence](auto snapshot, auto) {
    persisted.push_back(std::move(snapshot));
    return persistence.promise;
  };
  rt::Promise<void> scheduling(handler::terminal_route::persistInferenceExceptionReplay(capture, runError, options));
  const auto propagated = scheduling.then([runError] { std::rethrow_exception(runError); });
  EXPECT_REJECTS_REASON(propagated.wait(), runError);
  EXPECT(!persistenceSettled);
  ASSERT_EQ(registeredTasks.size(), std::size_t{1});
  EXPECT(fixture::zeroed(capture.body));
  ASSERT(!persisted.empty());
  const auto snapshot = persisted[0];
  EXPECT(snapshot.body != capture.body);
  EXPECT_EQ(*snapshot.body, originalBytes);
  persistenceSettled = true;
  persistence.reject(rt::makeReason(rt::JsError("fixture persistence failure")));
  co_await registeredTasks[0].wait();
  EXPECT(fixture::zeroed(snapshot.body));
}

ASYNC_TEST("exception replay persistence is awaited and cleaned up without a background registrar") {
  fixture::Isolation isolation;
  const auto previous = handler::http::globalEdgeRuntime();
  const auto restore = rt::scopeExit([previous] { handler::http::globalEdgeRuntime() = previous; });
  handler::http::globalEdgeRuntime() = std::nullopt;
  std::vector<sentinel::replay_model::AcceptedSentinelReplayInput> persisted;
  bool persistenceSettled = false;
  auto capture = fixture::acceptedInput();
  const auto originalBytes = *capture.body;
  const rt::Reason runError = rt::makeReason(rt::JsError("fixture inference failure"));
  handler::terminal_route::PersistInferenceExceptionReplayOptions options;
  options.persistSentinelReplay = [&persisted, &persistenceSettled](auto snapshot, auto) {
    persisted.push_back(std::move(snapshot));
    sentinel::replay_observation::SentinelReplayPersistResult result =
      sentinel::replay_observation::SentinelReplayPersistResultDisabled{.reason = "kv_unavailable"};
    return rt::Promise<sentinel::replay_observation::SentinelReplayPersistResult>::resolve(result)
      .finally_([&persistenceSettled] { persistenceSettled = true; });
  };
  rt::Promise<void> scheduling(handler::terminal_route::persistInferenceExceptionReplay(capture, runError, options));
  EXPECT(!persistenceSettled);
  ASSERT(!persisted.empty());
  const auto snapshot = persisted[0];
  EXPECT(fixture::zeroed(capture.body));
  EXPECT_EQ(*snapshot.body, originalBytes);
  co_await scheduling.wait();
  EXPECT(persistenceSettled);
  EXPECT(fixture::zeroed(snapshot.body));
}

ASYNC_TEST("background exception replay keeps its suspended callback alive") {
  fixture::Isolation isolation;
  const auto resume = rt::withResolvers<void>();
  auto capture = std::make_shared<std::string>("background replay callback");
  const std::weak_ptr observer = capture;
  std::optional<rt::Promise<void>> registered;
  handler::terminal_route::PersistInferenceExceptionReplayOptions options;
  options.waitUntil = [&registered](rt::Promise<void> pending) { registered = std::move(pending); };
  options.persistSentinelReplay = [capture = std::move(capture), waiting = resume.promise](auto snapshot, auto)
    -> rt::Task<sentinel::replay_observation::SentinelReplayPersistResult> {
    co_await waiting.wait();
    EXPECT_EQ(*capture, "background replay callback");
    EXPECT(!fixture::zeroed(snapshot.body));
    co_return sentinel::replay_observation::SentinelReplayPersistResultDisabled{.reason = "kv_unavailable"};
  };
  auto input = fixture::acceptedInput();
  const auto reason = rt::makeReason(rt::JsError("controlled inference failure"));
  auto scheduling = handler::terminal_route::persistInferenceExceptionReplay(input, reason, std::move(options));
  co_await std::move(scheduling);
  ASSERT(registered);
  EXPECT(fixture::zeroed(input.body));
  ASSERT(!observer.expired());
  resume.resolve();
  co_await registered->wait();
  EXPECT(observer.expired());
}
