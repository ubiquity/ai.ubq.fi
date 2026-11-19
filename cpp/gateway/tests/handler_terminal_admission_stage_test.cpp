// Port of tests/handler-terminal-admission-stage.test.ts: rejection-wrapper source case.
// Full original producer ingress is ported; the separately named ops reader boundary remains.
// The ops VPS journal-reader assertions are outside the runtime module manifest.
// Links: tests/handler_terminal_admission_stage_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/handler/terminal_route.cpp src/handler/http.cpp src/http.cpp src/chat/envelope.cpp src/sentinel/replay_observation.cpp src/openai_telemetry.cpp src/embeddings/jobs.cpp src/decisions/handlers.cpp
// src/provider/selection.cpp src/codex/auth.cpp src/request_policy.cpp src/chat/stream_translation.cpp src/provider/openrouter.cpp src/openai.cpp src/responses_stream.cpp src/embeddings/voyage.cpp
// src/request.cpp src/handler/admission.cpp src/responses_failover_stream.cpp src/handler/terminal_log.cpp src/kv.cpp src/runtime_config.cpp src/api_key_policy.cpp src/types.cpp
// src/sentinel/replay_store.cpp src/embeddings/ledger.cpp src/codex/routing_mutations.cpp src/sentinel/replay_envelope.cpp src/sentinel/upstream_capture.cpp src/models/codex_models.cpp src/openai_schema.cpp src/inference_deadline.cpp
// src/sentinel/replay_retention.cpp src/codex/capacity_routing.cpp src/sentinel/replay_retention_metadata.cpp src/admin/error_log.cpp src/codex/quota.cpp src/codex/routing_state.cpp src/provider/claude_code_models.cpp src/sentinel/replay_retention_schema.cpp
// src/authority/native_api_key_policy.cpp src/provider/lithos.cpp src/authority/native_paid_policy.cpp src/sentinel/incident_outbox.cpp src/authority/native_codex_capacity.cpp src/codex/responses_error.cpp src/cache/telemetry_gate.cpp src/systemone/handlers.cpp
// src/sentinel/replay_model.cpp src/provider/surplus_claude_request.cpp src/models/codex_models_fetch.cpp src/inference_admission.cpp src/utils.cpp src/authority/native_codex_mutations.cpp src/authority/native_api_key.cpp src/images.cpp
// src/codex/dispatch.cpp src/authority/native_codex_contracts.cpp src/provider/claude_code_accounts.cpp src/upstream_wire.cpp src/models/openrouter_models.cpp src/sentinel/replay_retention_bootstrap.cpp src/codex/external_auth.cpp src/codex/auth_refresh.cpp
// src/provider/cerebras.cpp src/api_keys.cpp src/cache/prompt_analytics_queue.cpp src/config.cpp src/deepseek/index.cpp src/deepseek/handlers.cpp src/optional_telemetry_queue.cpp src/cache/prompt_analytics_core.cpp
// src/input_normalization.cpp src/release.cpp src/paid_fallback/routing.cpp src/provider/openrouter_handlers.cpp src/provider/health.cpp src/auth/relay.cpp src/defaults.cpp src/provider/openrouter_request_controls.cpp
// src/paid_fallback/index.cpp src/paid_fallback/health.cpp src/provider/capacity_events.cpp src/deepseek/responses_payload.cpp src/models/codex_models_availability.cpp src/paid_fallback/ledger_admission.cpp src/authority/native_codex_evaluation.cpp src/embeddings/handlers.cpp
// src/provider/capacity.cpp src/provider/capacity_history.cpp src/provider/claude_code_quota.cpp src/sentinel/replay_read.cpp src/provider/cerebras_rate_limits.cpp src/provider/capacity_parse.cpp src/provider/lithos_handlers.cpp src/provider/claude_code_credentials.cpp
// src/provider/capacity_rollups.cpp src/provider/lithos_rate_limits.cpp src/provider/surplus.cpp src/authority/native_paid.cpp src/paid_fallback/ledger_state.cpp src/bounded_response_body.cpp src/codex/rate_limit_429.cpp src/provider/claude_code.cpp
// src/codex/experiment.cpp src/chat/completions.cpp src/provider/lithos_streams.cpp src/deepseek/responses.cpp src/runtime/http_tasks.cpp src/provider/claude_capacity_rollups.cpp src/paid_fallback/rollups.cpp src/storage/kv.cpp
// src/provider/surplus_claude_chat_request.cpp src/paid_fallback/ledger_backfill.cpp src/models/catalog.cpp src/provider/anthropic_responses.cpp src/provider/capacity_contract.cpp src/authority/native_paid_contracts.cpp src/sentinel/replay_limits.cpp src/models/metadata.cpp
// src/provider/capacity_analytics.cpp src/paid_fallback/ledger_settlement.cpp src/provider/metered.cpp src/responses_native_headers.cpp src/responses_handler.cpp src/provider/openrouter_streams.cpp src/paid_fallback/removed_provider.cpp src/deepseek/context_admission_failure.cpp
// src/provider/cerebras_handlers.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_response_wire.cpp src/auth/index.cpp src/deepseek/waterfall.cpp src/kernel/attestation.cpp src/provider/anthropic_replay.cpp src/responses_attempts.cpp
// src/responses_buffered.cpp src/kernel/quota_reserve.cpp src/debug_routing.cpp src/codex/native_auth.cpp src/deepseek/chat_projection.cpp src/auth/passport.cpp src/provider/removed_provider_telemetry.cpp src/codex/account_routing.cpp
// src/provider/removed_provider_circuit.cpp src/paid_fallback/ledger_stats.cpp src/auth/passkeys.cpp src/provider/stream_relay.cpp src/codex/banked_reset.cpp src/provider/claude_capacity_history.cpp src/auth/passport_sessions.cpp src/provider/cerebras_responses.cpp
// src/authority/native_codex_429.cpp src/models/codex_models_whitelist.cpp src/codex/routing_serial.cpp src/paid_fallback/claude_code_route.cpp src/deepseek/stream.cpp src/authority/native_kernel.cpp src/codex/routing_evaluation.cpp src/storage/native_kv.cpp
// src/provider/claude_code_usage_schema.cpp src/storage/sqlite_readonly.cpp src/runtime/http_early_sse.cpp src/metered_quota.cpp src/responses_request.cpp src/lib/jev_compaction/codex_items.cpp src/catalog/index.cpp src/deepseek/forwarded_payload_policy.cpp
// src/authority/native_codex.cpp src/catalog/models.cpp src/kernel/quota_reservations.cpp src/kernel/quota_v2.cpp src/codex/index.cpp src/provider/claude_code_access.cpp src/recent_model_context.cpp src/codex/responses_operations.cpp
// src/provider/openrouter_web_tools.cpp src/catalog/store.cpp src/lib/jev_compaction/compact.cpp src/deepseek/flash_aliases.cpp src/codex/native_auth_transport.cpp src/kernel/policy_queue.cpp src/authority/native_kernel_contracts.cpp src/cache/prompt_analytics_read.cpp
// src/deepseek/responses_stream.cpp src/codex/responses_runners.cpp src/lib/jev_compaction/state.cpp src/catalog/types.cpp src/deepseek/waterfall_handler.cpp src/auth/local_development_key.cpp src/lib/jev_compaction/types.cpp src/codex/banked_reset_pool.cpp
// src/codex/banked_reset_claim.cpp src/provider/anthropic_chat_stream.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset_submission.cpp src/codex/responses_state.cpp src/auth/local_admin.cpp src/analytics.cpp
// src/lib/jev_compaction/request.cpp src/codex/reset_settings.cpp src/codex/overage_settings.cpp src/runtime/websocket.cpp src/provider/lithos_models.cpp
#include "tests/helpers/terminal_log_harness.hpp"
#include "uos/handler/terminal_route.hpp"
#include "uos/handler/admission.hpp"
#include "uos/rt/fetch.hpp"

using namespace uos;
namespace fixture = tests::helpers::terminal_log_harness;
namespace terminal = handler::terminal_log;

ASYNC_TEST("terminal admission: actual authentication refusal is excluded with complete journal coverage") {
  fixture::Isolation isolation;const bool deploy=config::config.isDeploy;config::config.isDeploy=true;const auto restore=rt::scopeExit([deploy]{config::config.isDeploy=deploy;});rt::console::Capture logs({rt::console::Method::info});
  const rt::Request request("https://fixture.invalid/v1/responses",rt::RequestInit{.method="POST",.body="{}"});auto handling=handler::terminal_route::handleTerminalRoute(request,"/v1/responses",std::nullopt,"terminal-auth-refusal",1'800'000'000'000,rt::performanceNow());auto response=co_await std::move(handling);const auto body=co_await response.text();(void)body;EXPECT_EQ(response.status(),401);const auto events=fixture::terminalPayloads(logs);ASSERT_EQ(events.size(),1U);EXPECT_EQ(rt::getString(events[0],"admission_stage"),"pre_admission");EXPECT(rt::hasOwn(events[0].as_object(),"rate_limit_wait_ms"));EXPECT(rt::hasOwn(events[0].as_object(),"rate_limit_failover_model"));
  // The original event kind is preserved by its exact producer label; sampleEvents/ops reader is outside this runtime manifest.
  std::vector<std::string> kinds;for(const auto& call:logs.calls())if(!call.args.empty()&&(call.args[0]=="[ai.ubq.fi] request_accepted"||call.args[0]=="[ai.ubq.fi] request_terminal"))kinds.push_back(call.args[0]=="[ai.ubq.fi] request_accepted"?"accepted":"terminal");ASSERT_EQ(kinds.size(),1U);EXPECT_EQ(kinds[0],"terminal");
}

ASYNC_TEST("terminal admission: rejection wrapper preserves explicit stages and generic logger remains unknown") {
  fixture::Isolation isolation;
  const std::vector<std::string> stages{"pre_admission", "accepted"};
  for (const auto& stage : stages) {
    rt::console::Capture logs({rt::console::Method::info});
    terminal::RejectionTerminalLogInput options{.requestId = "terminal-wrapper-" + stage, .admissionStage = stage,
      .startedAtMonotonicMs = rt::performanceNow()};
    auto wrapped = terminal::withRejectionTerminalLog(fixture::jsonResponse("fixture rejection", 401), "responses", options);
    auto response = co_await wrapped.wait();
    auto text = co_await response.text();
    EXPECT_EQ(text, std::string("fixture rejection"));
    const auto events = fixture::terminalPayloads(logs);
    ASSERT(events.size() == 1);
    EXPECT_EQ(rt::getString(events[0], "admission_stage"), std::optional<std::string>(stage));
    EXPECT(rt::hasOwn(events[0].as_object(), "rate_limit_wait_ms"));
    EXPECT(rt::hasOwn(events[0].as_object(), "rate_limit_failover_model"));
  }
  rt::console::Capture logs({rt::console::Method::info});
  auto options = fixture::terminalOptions("terminal-legacy-unknown");
  auto wrapped = terminal::withTerminalRequestLog(fixture::jsonResponse("fixture success"), options);
  auto response = co_await wrapped.wait();
  auto text = co_await response.text();
  EXPECT_EQ(text, std::string("fixture success"));
  const auto events = fixture::terminalPayloads(logs);
  ASSERT(events.size() == 1);
  EXPECT(rt::get(events[0], "admission_stage")->is_null());
}

ASYNC_TEST("terminal admission: accepted cancellation remains in the real producer-reader cohort without dispatch") {
  fixture::Isolation isolation;const std::string token="terminal-admission-fixture";EXPECT(!config::config.authTokens.contains(token));const inference_admission::PartialInferenceAdmissionLimits limits{.maxActive=1,.maxWaiting=0};auto controller=inference_admission::createInferenceAdmissionController(limits);const rt::AbortController caller;int dispatched=0;config::config.authTokens.insert(token);handler::admission::setInferenceAdmissionControllerForTest(controller);const auto cleanup=rt::scopeExit([token]{handler::admission::setInferenceAdmissionControllerForTest(std::nullopt);config::config.authTokens.erase(token);});
  rt::GlobalFetchOverride fetch(rt::Fetch([&dispatched](rt::RequestInfo,rt::RequestInit)->rt::Response{++dispatched;throw rt::JsError("fixture must never dispatch");}));rt::console::Capture logs({rt::console::Method::info});logs.setObserver([caller](const rt::console::Call& call){if(!call.args.empty()&&call.args[0]=="[ai.ubq.fi] request_accepted")caller.abort(rt::makeReason(rt::DOMException("fixture accepted cancellation","AbortError")));});
  const rt::Request request("https://fixture.invalid/v1/responses",rt::RequestInit{.method="POST",.headers=rt::Headers{{"Authorization","Bearer "+token}},.body="{}",.signal=caller.signal()});auto handling=handler::terminal_route::handleTerminalRoute(request,"/v1/responses",std::nullopt,"terminal-accepted-cancellation",1'800'000'000'000,rt::performanceNow());auto response=co_await std::move(handling);const auto text=co_await response.text();(void)text;EXPECT_EQ(response.status(),499);EXPECT_EQ(dispatched,0);std::vector<std::string> kinds;std::vector<json::value> events;for(const auto& call:logs.calls())if(call.args.size()>1&&(call.args[0]=="[ai.ubq.fi] request_accepted"||call.args[0]=="[ai.ubq.fi] request_terminal")){kinds.push_back(call.args[0]=="[ai.ubq.fi] request_accepted"?"accepted":"terminal");events.push_back(rt::jsonParse(call.args[1].string()));}const std::vector<std::string> expected{"accepted","terminal"};EXPECT_EQ(kinds,expected);ASSERT_EQ(events.size(),2U);EXPECT_EQ(rt::getString(events[1],"admission_stage"),"accepted");EXPECT_EQ(controller.snapshot().active,0);EXPECT_EQ(controller.snapshot().waiting,0);
  // Actual producer fields/events above are preserved; the original sampleEvents reader/coverage assertions require the out-of-manifest ops reader.
}

ASYNC_TEST("route-less rejections release the resource once and emit no terminal record") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  int releases = 0;
  terminal::RejectionTerminalLogInput options{.requestId = "no-route", .admissionStage = "pre_admission", .startedAtMonotonicMs = 0};
  options.onSettled = [&releases] { ++releases; };
  auto original = fixture::jsonResponse("not terminal", 404);
  auto wrapped = terminal::withRejectionTerminalLog(original, std::nullopt, options);
  EXPECT_EQ(releases, 1);
  auto response = co_await wrapped.wait();
  EXPECT(response == original);
  EXPECT(fixture::terminalPayloads(logs).empty());
}
