// Links: tests/jev_compaction_smoke_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
//   src/models/catalog.cpp src/input_normalization.cpp src/provider/selection.cpp src/provider/surplus.cpp src/models/metadata.cpp src/utils.cpp src/authority/native_api_key_policy.cpp src/provider/lithos.cpp src/handler/index.cpp
//   src/admin/codex.cpp src/authority/native_api_key.cpp src/provider/openrouter.cpp src/http.cpp src/auth/index.cpp src/provider/anthropic_chat_responses.cpp src/deepseek/index.cpp src/models/codex_models.cpp
//   src/handler/admission.cpp src/models/codex_models_fetch.cpp src/admin/kernel.cpp src/codex/dispatch.cpp src/provider/cerebras.cpp src/api_keys.cpp src/types.cpp src/codex/auth.cpp src/auth/passkeys.cpp
//   src/runtime_config.cpp src/provider/claude_code_credentials.cpp src/agent_messages.cpp src/codex/experiment.cpp src/quota_projection.cpp src/provider/claude_code_accounts.cpp src/admin/claude_code.cpp src/kernel/quota_v2.cpp src/models/openrouter_models.cpp
//   src/codex/external_auth.cpp src/provider/anthropic_response_wire.cpp src/catalog/index.cpp src/catalog/models.cpp src/provider/surplus_claude_request.cpp src/admin/api_keys.cpp src/live/handler.cpp src/cache/scope_experiment_targets.cpp
//   src/api_key_policy.cpp src/models/codex_models_whitelist.cpp src/runtime/http.cpp src/provider/capacity_events.cpp src/provider/capacity_parse.cpp src/handler/http.cpp src/runtime/http_body.cpp src/cache/scope_experiment_model.cpp src/provider/claude_code_models.cpp
//   src/paid_fallback/ledger_state.cpp src/authority/native_kernel.cpp src/config.cpp src/admin/defaults.cpp src/provider/health.cpp src/auth/relay.cpp src/admin/api_key_audit.cpp src/static.cpp src/debug_routing.cpp
//   src/catalog/store.cpp src/authority/native_paid_contracts.cpp src/provider/presentation.cpp src/provider/anthropic_chat_stream.cpp src/provider/lithos_models.cpp src/kernel/quota_write.cpp src/admin/codex_reset.cpp src/kernel/usage.cpp src/provider/claude_code_access.cpp
//   src/request_policy.cpp src/inference_admission.cpp src/live/sideband.cpp src/recent_model_context.cpp src/release.cpp src/kernel/policy_queue.cpp src/provider/claude_code_quota.cpp src/authority/native_paid.cpp src/admin/capacity_observation_sync.cpp
//   src/codex/auth_refresh.cpp src/paid_fallback/index.cpp src/codex/banked_reset_provider.cpp src/metered_quota.cpp src/kernel/quota_reservations.cpp src/kernel/attestation.cpp src/defaults.cpp src/catalog/types.cpp src/paid_fallback/ledger_stats.cpp
//   src/paid_fallback/ledger_backfill.cpp src/request.cpp src/codex/account_routing.cpp src/responses_stream.cpp src/openai_telemetry.cpp src/codex/overage_settings.cpp src/codex/routing_state.cpp src/authority/native_codex_mutations.cpp src/sentinel/replay_admin.cpp
//   src/cache/prompt_analytics_read.cpp src/models/codex_models_availability.cpp src/codex/quota.cpp src/codex/manual_reset.cpp src/health.cpp src/provider/capacity.cpp src/provider/metered.cpp src/authority/native_codex_capacity.cpp src/auth/passport.cpp
//   src/codex/banked_reset.cpp src/provider/capacity_analytics.cpp src/authority/native_paid_policy.cpp src/provider/lithos_rate_limits.cpp src/provider/capacity_history.cpp src/codex/supervisor.cpp src/paid_fallback/ledger_settlement.cpp src/cache/kv_migration_base.cpp src/authority/native_codex_contracts.cpp
//   src/live/upstream.cpp src/provider/capacity_contract.cpp src/provider/claude_capacity_history.cpp src/cache/prompt_analytics_core.cpp src/codex/routing_mutations.cpp src/live/calls.cpp src/cache/scope_experiment.cpp src/codex/capacity_routing.cpp src/admin/error_log.cpp
//   src/codex/routing_serial.cpp src/handler/serve_handler.cpp src/provider/claude_capacity_rollups.cpp src/auth/passport_sessions.cpp src/codex/manual_reset_state.cpp src/deepseek/waterfall.cpp src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp src/provider/cerebras_rate_limits.cpp
//   src/codex/supervisor_brief.cpp src/authority/native_codex_evaluation.cpp src/kv.cpp src/admin/api_key_mutations.cpp src/paid_fallback/ledger_admission.cpp src/handler/terminal_route.cpp src/cache/kv_migration_validation.cpp src/catalog/promotion.cpp src/codex/supervisor_config.cpp
//   src/provider/capacity_rollups.cpp src/authority/native_kernel_contracts.cpp src/sentinel/replay_observation.cpp src/codex/supervisor_secret.cpp src/authority/native_api_key_audit.cpp src/cache/kv_migration_paid_fallback.cpp src/cache/kv_migration_usage.cpp src/authority/native_codex.cpp src/sentinel/replay_model.cpp
//   src/provider/claude_code.cpp src/sentinel/replay_retention_schema.cpp src/codex/supervisor_follow.cpp src/sentinel/replay_read.cpp src/sentinel/replay_limits.cpp src/kernel/quota_reserve.cpp src/codex/rate_limit_429.cpp src/systemone/handlers.cpp src/bounded_response_body.cpp
//   src/codex/supervisor_inventory.cpp src/codex/native_auth.cpp src/embeddings/jobs.cpp src/cache/telemetry_gate.cpp src/embeddings/voyage.cpp src/runtime/http_early_sse.cpp src/embeddings/ledger.cpp src/provider/surplus_claude_chat_request.cpp src/admin/sentinel_incident.cpp
//   src/jev_compaction/compaction.cpp src/provider/anthropic_replay.cpp src/handler/terminal_log.cpp src/runtime/http_upgrade.cpp src/storage/kv.cpp src/sentinel/replay_envelope.cpp src/codex/banked_reset_submission.cpp src/analytics.cpp src/embeddings/handlers.cpp
//   src/codex/banked_reset_claim.cpp src/chat/envelope.cpp src/paid_fallback/rollups.cpp src/auth/local_admin.cpp src/runtime/http_tasks.cpp src/admin/chart_source_proxy.cpp src/lib/jev_compaction/codex_items.cpp src/cache/scope_targets.cpp src/upstream_wire.cpp
//   src/openai.cpp src/sentinel/replay_store.cpp src/provider/claude_code_usage_schema.cpp src/codex/supervisor_transport.cpp src/codex/banked_reset_pool.cpp src/sentinel/incident_outbox.cpp src/auth/local_development_key.cpp src/sentinel/replay_retention.cpp src/live/routes.cpp
//   src/responses_native_headers.cpp src/chat/completions.cpp src/sentinel/upstream_capture.cpp src/runtime/websocket.cpp src/authority/native_codex_429.cpp src/provider/openrouter_handlers.cpp src/inference_deadline.cpp src/provider/openrouter_streams.cpp src/sentinel/replay_retention_bootstrap.cpp
//   src/chat/stream_translation.cpp src/kv_conversion/profile_json.cpp src/provider/anthropic_responses.cpp src/storage/sqlite_readonly.cpp src/provider/openrouter_request_controls.cpp src/responses_failover_stream.cpp src/images.cpp src/openai_schema.cpp src/sentinel/replay_retention_metadata.cpp
//   src/provider/removed_provider_telemetry.cpp src/decisions/handlers.cpp src/deepseek/context_admission_failure.cpp src/lib/jev_compaction/state.cpp src/jev_compaction/hybrid.cpp src/deepseek/handlers.cpp src/codex/native_auth_transport.cpp src/codex/supervisor_log.cpp src/responses_handler.cpp
//   src/responses_request.cpp src/lib/jev_compaction/client.cpp src/provider/lithos_handlers.cpp src/responses_attempts.cpp src/deepseek/responses.cpp src/storage/native_kv.cpp src/provider/cerebras_handlers.cpp src/paid_fallback/removed_provider.cpp src/paid_fallback/health.cpp
//   src/provider/removed_provider_circuit.cpp src/lib/jev_compaction/types.cpp src/paid_fallback/routing.cpp src/provider/lithos_streams.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp src/deepseek/forwarded_payload_policy.cpp src/provider/stream_relay.cpp src/provider/openrouter_web_tools.cpp
//   src/cache/prompt_analytics_queue.cpp src/deepseek/responses_payload.cpp src/codex/index.cpp src/optional_telemetry_queue.cpp src/deepseek/chat_projection.cpp src/responses_buffered.cpp src/deepseek/stream.cpp src/deepseek/flash_aliases.cpp src/paid_fallback/claude_code_route.cpp
//   src/deepseek/responses_stream.cpp src/deepseek/waterfall_handler.cpp src/codex/responses_error.cpp src/provider/cerebras_responses.cpp src/codex/responses_runners.cpp src/codex/responses_operations.cpp src/codex/responses_state.cpp
// New bounded module differentials; full smoke entry remains uninvoked.
// The owner approved including the real private implementation in this standalone test TU.
// Its .cpp is deliberately excluded from Links; no production test-only API is exported.
// Owner-approved test-context exception: GCC12.2 warns on anonymous coroutine types only when the real .cpp is included; standalone source remains strict.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif
#include "../src/lib/jev_compaction/jev_compaction_smoke.cpp"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "uos/rt/crypto.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/fs.hpp"
namespace smoke=uos::lib::jev_compaction::jev_compaction_smoke;
namespace types=uos::lib::jev_compaction::types;
namespace rt=uos::rt;namespace json=uos::json;
namespace {
const json::value& oracle(){static const auto value=rt::jsonParse(R"oracle({"text":{"type":"message","role":"user","content":[{"type":"input_text","text":"fixture\n\ud83e\udd84"}]},"call":{"type":"function_call","call_id":"call-1","name":"shell","arguments":"{\"command\":\"printf 'fixture'\"}"},"output":{"type":"function_call_output","call_id":"call-1","output":"done"},"inputLength":15,"inputSha256":"7835f088823b4898d376d59591981e2e0348d0f8929575a8ccfcd5c9c34af882","chunks":["data: {\"id\":\"chatcmpl-jev-smoke\",\"object\":\"chat.completion.chunk\",\"created\":1780000009,\"model\":\"deepseek-flash\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"fixture\\n\ud83e\udd84\"},\"finish_reason\":null}]}\n\n","data: {\"id\":\"chatcmpl-jev-smoke\",\"object\":\"chat.completion.chunk\",\"created\":1780000009,\"model\":\"deepseek-flash\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3,\"total_tokens\":10}}\n\n"],"sse":"data: {\"id\":\"chatcmpl-jev-smoke\",\"object\":\"chat.completion.chunk\",\"created\":1780000009,\"model\":\"deepseek-flash\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"fixture\\n\ud83e\udd84\"},\"finish_reason\":null}]}\n\ndata: {\"id\":\"chatcmpl-jev-smoke\",\"object\":\"chat.completion.chunk\",\"created\":1780000009,\"model\":\"deepseek-flash\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3,\"total_tokens\":10}}\n\ndata: [DONE]\n\n","asked":{"answers":{"call_t1":{"noul":0.1},"result_t1":{"noul":0.1}}},"counters":{"jev_calls":1,"jev_questions":2}})oracle");return value;}
}
TEST("jev-compaction-smoke module links its explicit script callable without executing it"){
  auto volatile entry=&smoke::importMetaMain;EXPECT(entry!=nullptr);EXPECT(!rt::env::has("OPENROUTER_API_KEY"));EXPECT(!rt::env::has("CODEX_AUTH_JSON_B64"));EXPECT(!rt::fs::existsSync(".data"));
}
TEST("jev-compaction-smoke message call and output helpers match the inert Bun oracle"){
  EXPECT_EQ(json::value(smoke::text("user","fixture\n🦄")),*rt::get(oracle(),"text"));EXPECT_EQ(json::value(smoke::call("call-1","printf 'fixture'")),*rt::get(oracle(),"call"));EXPECT_EQ(json::value(smoke::output("call-1","done")),*rt::get(oracle(),"output"));
}
TEST("jev-compaction-smoke finite transcript preserves exact source fields and bytes"){
  const auto input=smoke::compactionInput();EXPECT_EQ(static_cast<double>(input.size()),rt::getNumber(oracle(),"inputLength").value());const auto serialized=rt::jsonStringify(input);const auto hash=rt::crypto::createHash("sha256").update(serialized).digest("hex");EXPECT_EQ(hash,std::string(rt::getString(oracle(),"inputSha256").value()));
}
TEST("jev-compaction-smoke SSE helpers preserve usage omission and terminal framing"){
  const json::object delta{{"role","assistant"},{"content","fixture\n🦄"}};const json::object usage{{"prompt_tokens",7},{"completion_tokens",3},{"total_tokens",10}};const std::vector<std::string> chunks{smoke::chatChunk(delta,std::nullopt),smoke::chatChunk({},"stop",usage)};json::array values;for(const auto& chunk:chunks)values.push_back(json::value(chunk));EXPECT_EQ(json::value(values),*rt::get(oracle(),"chunks"));EXPECT_EQ(smoke::sseBody(chunks),std::string(rt::getString(oracle(),"sse").value()));
}
ASYNC_TEST("jev-compaction-smoke finite fake asker records one call and two questions"){
  auto report=std::make_shared<smoke::Report>();const auto asker=smoke::dropEverythingAsker(report);types::JevQuestions questions;questions.set("call_t1",types::NoulQuestion{.type="noul",.instructions="keep?"});questions.set("result_t1",types::NoulQuestion{.type="noul",.instructions="keep?"});const types::JevState state=json::object{};auto asking=asker->ask(state,questions);const auto response=co_await std::move(asking);EXPECT_EQ(json::value(response),*rt::get(oracle(),"asked"));const auto* counters=rt::get(oracle(),"counters");EXPECT_EQ(report->jev_calls,rt::getNumber(*counters,"jev_calls").value());EXPECT_EQ(report->jev_questions,rt::getNumber(*counters,"jev_questions").value());
}
TEST("jev-compaction-smoke source assertion diagnostics retain the Node scalar failure text"){
  EXPECT_THROWS_MESSAGE(smoke::assertEqual(1,2,"custom"),rt::JsError,"custom\n\n1 !== 2\n");EXPECT_THROWS_MESSAGE(smoke::assert_(false,"custom"),rt::JsError,"custom");EXPECT_THROWS_MESSAGE(smoke::assertMatch("wrong"),rt::JsError,"The input did not match the regular expression /calls_dropped=3/. Input:\n\n'wrong'\n");
}
