// Links: tests/request_delivery_lifecycle_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
//   src/handler/serve_handler.cpp src/auth/index.cpp src/auth/passport.cpp src/api_key_policy.cpp src/utils.cpp src/auth/passkeys.cpp src/authority/native_api_key.cpp src/auth/passport_sessions.cpp src/agent_messages.cpp
//   src/auth/local_admin.cpp src/kernel/attestation.cpp src/types.cpp src/authority/native_api_key_policy.cpp src/request.cpp src/auth/local_development_key.cpp src/api_keys.cpp src/models/catalog.cpp src/authority/native_paid_policy.cpp
//   src/provider/metered.cpp src/analytics.cpp src/provider/claude_code_models.cpp src/provider/claude_code_credentials.cpp src/auth/relay.cpp src/authority/native_paid.cpp src/provider/selection.cpp src/models/codex_models_whitelist.cpp src/deepseek/index.cpp
//   src/input_normalization.cpp src/provider/openrouter.cpp src/provider/surplus_claude_request.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/request_policy.cpp src/provider/cerebras.cpp src/provider/claude_code_accounts.cpp src/models/codex_models.cpp
//   src/models/openrouter_models.cpp src/provider/surplus.cpp src/models/codex_models_fetch.cpp src/models/codex_models_availability.cpp src/deepseek/waterfall.cpp src/provider/claude_code.cpp src/provider/claude_code_quota.cpp src/models/metadata.cpp src/paid_fallback/ledger_backfill.cpp
//   src/runtime_config.cpp src/codex/dispatch.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_settlement.cpp src/authority/native_paid_contracts.cpp src/provider/claude_code_usage_schema.cpp src/responses_stream.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp
//   src/provider/lithos.cpp src/codex/routing_mutations.cpp src/authority/native_codex_mutations.cpp src/provider/surplus_claude_chat_request.cpp src/handler/index.cpp src/admin/codex.cpp src/bounded_response_body.cpp src/codex/rate_limit_429.cpp src/provider/capacity_parse.cpp
//   src/kernel/policy_queue.cpp src/authority/native_codex_capacity.cpp src/http.cpp src/codex/native_auth.cpp src/authority/native_codex_contracts.cpp src/config.cpp src/admin/api_keys.cpp src/kernel/quota_reservations.cpp src/admin/claude_code.cpp
//   src/authority/native_kernel.cpp src/codex/capacity_routing.cpp src/paid_fallback/ledger_stats.cpp src/admin/kernel.cpp src/cache/scope_experiment_targets.cpp src/provider/capacity.cpp src/recent_model_context.cpp src/admin/api_key_mutations.cpp src/provider/lithos_models.cpp
//   src/codex/experiment.cpp src/admin/capacity_observation_sync.cpp src/catalog/types.cpp src/release.cpp src/cache/scope_experiment_model.cpp src/provider/claude_capacity_history.cpp src/admin/api_key_audit.cpp src/codex/account_routing.cpp src/provider/lithos_rate_limits.cpp
//   src/provider/presentation.cpp src/codex/routing_state.cpp src/provider/claude_capacity_rollups.cpp src/health.cpp src/codex/native_auth_transport.cpp src/codex/supervisor.cpp src/metered_quota.cpp src/codex/supervisor_inventory.cpp src/kv.cpp
//   src/provider/capacity_history.cpp src/codex/supervisor_config.cpp src/authority/native_kernel_contracts.cpp src/cache/prompt_analytics_read.cpp src/authority/native_codex_429.cpp src/provider/capacity_contract.cpp src/catalog/index.cpp src/openai_telemetry.cpp src/provider/health.cpp
//   src/catalog/store.cpp src/debug_routing.cpp src/defaults.cpp src/quota_projection.cpp src/codex/routing_serial.cpp src/cache/prompt_analytics_core.cpp src/provider/capacity_analytics.cpp src/provider/cerebras_rate_limits.cpp src/codex/supervisor_follow.cpp
//   src/catalog/models.cpp src/admin/defaults.cpp src/codex/routing_evaluation.cpp src/provider/capacity_events.cpp src/codex/banked_reset_provider.cpp src/kernel/quota_write.cpp src/provider/capacity_rollups.cpp src/admin/sentinel_incident.cpp src/cache/kv_migration_base.cpp
//   src/kernel/usage.cpp src/codex/external_auth.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_response_wire.cpp src/authority/native_api_key_audit.cpp src/provider/anthropic_replay.cpp src/responses_failover_stream.cpp src/sentinel/incident_outbox.cpp src/kernel/quota_v2.cpp
//   src/codex/supervisor_brief.cpp src/provider/anthropic_chat_responses.cpp src/authority/native_codex.cpp src/provider/claude_code_access.cpp src/codex/overage_settings.cpp src/static.cpp src/admin/error_log.cpp src/sentinel/replay_retention_schema.cpp src/sentinel/replay_admin.cpp
//   src/sentinel/replay_read.cpp src/admin/codex_reset.cpp src/cache/scope_experiment.cpp src/codex/supervisor_secret.cpp src/codex/banked_reset.cpp src/codex/reset_settings.cpp src/cache/kv_migration_validation.cpp src/handler/http.cpp src/codex/supervisor_transport.cpp
//   src/admin/chart_source_proxy.cpp src/sentinel/replay_envelope.cpp src/paid_fallback/rollups.cpp src/codex/manual_reset.cpp src/handler/terminal_route.cpp src/responses_handler.cpp src/responses_attempts.cpp src/sentinel/replay_model.cpp src/provider/removed_provider_telemetry.cpp
//   src/codex/responses_error.cpp src/authority/native_codex_evaluation.cpp src/sentinel/replay_store.cpp src/cache/kv_migration_usage.cpp src/responses_request.cpp src/cache/telemetry_gate.cpp src/cache/kv_migration_paid_fallback.cpp src/paid_fallback/routing.cpp src/runtime/websocket.cpp
//   src/codex/banked_reset_pool.cpp src/live/handler.cpp src/paid_fallback/health.cpp src/provider/anthropic_chat_stream.cpp src/openai.cpp src/codex/supervisor_log.cpp src/codex/banked_reset_claim.cpp src/paid_fallback/removed_provider.cpp src/codex/banked_reset_submission.cpp
//   src/sentinel/replay_observation.cpp src/deepseek/flash_aliases.cpp src/cache/scope_targets.cpp src/decisions/handlers.cpp src/kv_conversion/profile_json.cpp src/provider/removed_provider_circuit.cpp src/sentinel/replay_retention.cpp src/upstream_wire.cpp src/inference_deadline.cpp
//   src/sentinel/replay_retention_bootstrap.cpp src/sentinel/upstream_capture.cpp src/codex/quota.cpp src/storage/kv.cpp src/provider/openrouter_handlers.cpp src/handler/terminal_log.cpp src/deepseek/waterfall_handler.cpp src/codex/manual_reset_state.cpp src/cache/prompt_analytics_queue.cpp
//   src/sentinel/replay_limits.cpp src/openai_schema.cpp src/images.cpp src/provider/openrouter_streams.cpp src/responses_native_headers.cpp src/sentinel/replay_retention_metadata.cpp src/codex/index.cpp src/storage/sqlite_readonly.cpp src/embeddings/jobs.cpp
//   src/codex/responses_state.cpp src/runtime/http_early_sse.cpp src/chat/stream_translation.cpp src/provider/lithos_handlers.cpp src/deepseek/chat_projection.cpp src/paid_fallback/claude_code_route.cpp src/live/sideband.cpp src/catalog/promotion.cpp src/deepseek/context_admission_failure.cpp
//   src/embeddings/ledger.cpp src/systemone/handlers.cpp src/deepseek/forwarded_payload_policy.cpp src/live/upstream.cpp src/handler/admission.cpp src/embeddings/voyage.cpp src/codex/responses_runners.cpp src/provider/lithos_streams.cpp src/provider/openrouter_request_controls.cpp
//   src/provider/openrouter_web_tools.cpp src/chat/envelope.cpp src/runtime/http_tasks.cpp src/deepseek/responses.cpp src/embeddings/handlers.cpp src/live/calls.cpp src/deepseek/handlers.cpp src/lib/jev_compaction/state.cpp src/provider/stream_relay.cpp
//   src/provider/cerebras_responses.cpp src/kernel/quota_reserve.cpp src/lib/jev_compaction/types.cpp src/responses_buffered.cpp src/chat/completions.cpp src/optional_telemetry_queue.cpp src/provider/cerebras_handlers.cpp src/live/routes.cpp src/storage/native_kv.cpp
// src/deepseek/responses_stream.cpp src/deepseek/responses_payload.cpp src/lib/jev_compaction/codex_items.cpp src/inference_admission.cpp src/codex/responses_operations.cpp src/lib/jev_compaction/compact.cpp src/deepseek/stream.cpp src/lib/jev_compaction/request.cpp
// Port of tests/request-delivery-lifecycle.test.ts:15; one original registration, six assertions.
#include "uos/handler/serve_handler.hpp"
#include "uos/rt/test_async.hpp"

namespace rt = uos::rt;
namespace serve_handler = uos::handler::serve_handler;

ASYNC_TEST("request delivery lifecycle switches from request aborts to server delivery completion") {
  rt::AbortController beforeHandoffRequest;
  auto beforeHandoffCompletion = rt::withResolvers<void>();
  auto beforeHandoff = serve_handler::createRequestDeliveryLifecycle(beforeHandoffRequest.signal(), beforeHandoffCompletion.promise);
  const auto requestAbort = rt::makeReason(rt::DOMException("client left before handoff", "AbortError"));
  beforeHandoffRequest.abort(requestAbort);
  EXPECT_EQ(beforeHandoff.signal.aborted(), true);
  EXPECT_EQ(beforeHandoff.signal.reason(), requestAbort);
  beforeHandoffCompletion.resolve();

  rt::AbortController deliveredRequest;
  auto deliveredCompletion = rt::withResolvers<void>();
  auto delivered = serve_handler::createRequestDeliveryLifecycle(deliveredRequest.signal(), deliveredCompletion.promise);
  delivered.handoff();
  deliveredRequest.abort(rt::makeReason(rt::DOMException("legacy post-handoff abort", "AbortError")));
  co_await rt::microtask();
  EXPECT_EQ(delivered.signal.aborted(), false);
  deliveredCompletion.resolve();
  co_await deliveredCompletion.promise.wait();
  EXPECT_EQ(delivered.signal.aborted(), false);

  rt::AbortController interruptedRequest;
  auto interruptedCompletion = rt::withResolvers<void>();
  auto interrupted = serve_handler::createRequestDeliveryLifecycle(interruptedRequest.signal(), interruptedCompletion.promise);
  interrupted.handoff();
  const auto deliveryFailure = rt::makeReason(rt::DOMException("response delivery failed", "AbortError"));
  interruptedCompletion.reject(deliveryFailure);
  auto ignored = interruptedCompletion.promise.catch_([](rt::Reason) {});
  co_await ignored.wait();
  co_await rt::microtask();
  EXPECT_EQ(interrupted.signal.aborted(), true);
  EXPECT_EQ(interrupted.signal.reason(), deliveryFailure);
}
