// Port of tests/serve-delivery-http.test.ts through the real createServeHandler and HTTP completion receipt.
// Additional cases are bounded Bun-recorded terminal-log branch observations.
// Links: tests/serve_delivery_http_test.cpp
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
//   tests/helpers/counting_kv.cpp tests/helpers/loopback_http.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_upgrade.cpp
#include "tests/helpers/terminal_log_harness.hpp"
#include "tests/helpers/loopback_http.hpp"
#include "uos/runtime/http.hpp"
#include "uos/runtime/http_tasks.hpp"
#include "uos/handler/serve_handler.hpp"
#include "uos/rt/limits.hpp"

using namespace uos;
namespace fixture = tests::helpers::terminal_log_harness;
namespace terminal = handler::terminal_log;
namespace wire = runtime::http;
using tests::helpers::loopback_http::RawClient;
using tests::helpers::loopback_http::ScopedLoopbackFetch;

namespace {
  rt::Response streamResponse(rt::UnderlyingSource<rt::ByteChunk> source) {
    const rt::ByteStream body(std::move(source));
    const rt::ResponseInit init{.headers = rt::Headers{{"Content-Type", "text/event-stream"}}};
    return rt::Response(rt::BodyInit(body), init);
  }
  wire::ServeHttpOptions loopbackOptions() {
    wire::ServeHttpOptions options;
    options.hostname = "127.0.0.1";
    options.port = 0;
    options.onListen = [](const auto&) {};
    return options;
  }
  std::string origin(const wire::NetAddr& address) { return "http://127.0.0.1:" + rt::numberToString(address.port); }
}

ASYNC_TEST("dropping a delivery handoff releases its request signal") {
  const auto completed = rt::withResolvers<void>();
  std::weak_ptr<void> observer;
  {
    const rt::AbortController caller;
    const auto signal = caller.signal();
    observer = signal.state();
    const auto abandoned = handler::serve_handler::createRequestDeliveryLifecycle(signal, completed.promise);
    EXPECT(!abandoned.signal.aborted());
  }
  EXPECT(observer.expired());
  completed.resolve();
  co_await rt::microtask();
}

ASYNC_TEST("the gateway rejects oversized Chat uploads at the head with the normal authenticated response") {
  fixture::Isolation isolation;
  const auto savedConfig = config::config;
  const auto restore = rt::scopeExit([savedConfig] { config::config = savedConfig; });
  config::config.isDeploy = true;
  config::config.isProductionTimeline = true;
  config::config.authTokens = {"fixture-chat-size"};
  kv::setKvForTest(std::make_shared<rt::MemoryKv>(rt::dateNow));

  auto options = loopbackOptions();
  options.applicationChatBodyRejection = true;
  const auto server = wire::serveHttp(options, handler::serve_handler::createServeHandler());
  const auto address = co_await server.ready.wait();
  const std::string head = "POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
    "Expect: 100-continue\r\nContent-Length: " + std::to_string(rt::limits::kHttpBodyBytes + 1) + "\r\n";
  for (const bool authenticated : {true, false}) {
    const auto request = head + (authenticated ? "Authorization: Bearer fixture-chat-size\r\n\r\n" : "\r\n");
    const auto closed = rt::withResolvers<void>();
    const auto reply = std::make_shared<std::string>();
    const auto socket = RawClient::connect(static_cast<int>(address.port));
    socket->once("connect", [socket, request](const auto&) { socket->write(request); });
    socket->on("data", [reply](const auto& part) { *reply += part; });
    socket->once("close", [closed](const auto&) { closed.resolve(); });
    co_await closed.promise.wait();
    const auto received = rt::toLowerCase(*reply);
    EXPECT(received.starts_with(authenticated ? "http/1.1 400" : "http/1.1 401"));
    EXPECT(!rt::includes(received, "100 continue"));
    EXPECT(rt::includes(received, "connection: close"));
    EXPECT(rt::includes(received, "content-type: application/json"));
    EXPECT(rt::includes(received, "x-uos-request-id:"));
    EXPECT(rt::includes(received, authenticated ? "invalid_request_error" : "invalid_api_key"));
  }
  const auto stopped = server.shutdown();
  co_await stopped.wait();
}

ASYNC_TEST("serve.ts classifies delivered and client-cancelled SSE responses from real HTTP completion") {
  fixture::Isolation isolation;
  ScopedLoopbackFetch network;
  rt::console::Capture logs({rt::console::Method::info});
  const auto cancelled = rt::withResolvers<void>();
  const auto pending = rt::withResolvers<void>();
  int sourceCancelled = 0;
  std::optional<rt::AbortSignal> deliveredSignal;
  std::optional<rt::AbortSignal> interruptedSignal;
  handler::serve_handler::DeliveryAwareHandler requestHandler = [&sourceCancelled, &deliveredSignal, &interruptedSignal, cancelled, pending](rt::Request request, handler::http::RequestDeliveryInfo delivery) -> rt::Task<rt::Response> {
    const bool delivered = rt::URL(request.url()).pathname() == "/delivered";
    if (delivered) deliveredSignal = delivery.downstreamSignal; else interruptedSignal = delivery.downstreamSignal;
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [delivered](auto controller) {
      controller.enqueue(rt::encodeUtf8(delivered ? "data: delivered\n\n" : "data: interrupted\n\n"));
      if (delivered) controller.close();
    };
    if (!delivered) {
      source.pull = [pending](auto) -> rt::Task<void> { co_await pending.promise.wait(); };
      source.cancel = [&sourceCancelled, cancelled](rt::Reason) { ++sourceCancelled; cancelled.resolve(); };
    }
    auto options = fixture::terminalOptions(delivered ? "served-delivered" : "served-interrupted");
    options.deliveryCompleted = delivery.completed;
    options.deliverySignal = delivery.downstreamSignal;
    auto wrapped = terminal::withTerminalRequestLog(streamResponse(source), options);
    auto response = co_await wrapped.wait();
    co_return response;
  };
  const auto server = wire::serveHttp(loopbackOptions(), handler::serve_handler::createServeHandler(requestHandler));
  const auto address = co_await server.ready.wait();
  auto fetching = rt::fetch(origin(address) + "/delivered");
  auto delivered = co_await std::move(fetching);
  auto text = co_await delivered.text();
  EXPECT_EQ(text, std::string("data: delivered\n\n"));
  const auto closed = rt::withResolvers<void>();
  auto socket = RawClient::connect(static_cast<int>(address.port));
  socket->once("connect", [socket](const auto&) { socket->write("GET /interrupted HTTP/1.1\r\nHost: localhost\r\n\r\n"); });
  bool receivedInterrupted = false;
  socket->on("data", [socket, &receivedInterrupted](const std::string& data) { if (rt::includes(data, "data: interrupted")) { receivedInterrupted = true; socket->destroy(); } });
  socket->once("error", [](const auto&) {});
  socket->once("close", [closed](const auto&) { closed.resolve(); });
  co_await closed.promise.wait();
  EXPECT(receivedInterrupted);
  co_await cancelled.promise.wait();
  pending.resolve();
  const auto stopped = server.shutdown();
  co_await stopped.wait();
  const auto deliveredLogs = fixture::terminalPayloads(logs, "served-delivered");
  const auto interruptedLogs = fixture::terminalPayloads(logs, "served-interrupted");
  ASSERT(deliveredLogs.size() == 1);
  ASSERT(interruptedLogs.size() == 1);
  EXPECT_EQ(rt::getString(deliveredLogs[0], "delivery_outcome"), std::optional<std::string>("delivered"));
  EXPECT_EQ(rt::getString(interruptedLogs[0], "delivery_outcome"), std::optional<std::string>("interrupted"));
  ASSERT(deliveredSignal && interruptedSignal);
  EXPECT(!deliveredSignal->aborted());
  EXPECT(interruptedSignal->aborted());
  EXPECT_EQ(sourceCancelled, 1);
}

ASYNC_TEST("buffered terminal delivery releases before durable writes and the real request drain awaits them") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  const auto delivered = rt::withResolvers<void>();
  const auto sink = rt::withResolvers<cache::telemetry_gate::PromptCacheTelemetryRecordResult>();
  const auto started = rt::withResolvers<void>();
  const auto drain = runtime::http_tasks::createRequestTaskDrain(delivered.promise);
  int released = 0;
  auto options = fixture::terminalOptions("buffered-delivery-drain");
  options.deliveryCompleted = delivered.promise;
  options.onSettled = [&released] { ++released; };
  options.recordTelemetry = [sink, started, &released](auto) {
    EXPECT_EQ(released, 1); started.resolve(); return sink.promise;
  };
  auto wrapped = terminal::withTerminalRequestLog(fixture::jsonResponse("complete"), options);
  auto response = co_await wrapped.wait();
  EXPECT_EQ(released, 0);
  EXPECT(fixture::terminalPayloads(logs).empty());
  delivered.resolve();
  co_await started.promise.wait();
  EXPECT_EQ(released, 1);
  auto drained = drain();
  bool done = false;
  const auto observed = drained.then([&done] { done = true; });
  co_await rt::microtask();
  EXPECT(!done);
  const cache::telemetry_gate::PromptCacheTelemetryRecordResult ignored{"ignored", "unknown_release", std::nullopt, std::nullopt, std::nullopt, std::nullopt};
  sink.resolve(ignored);
  co_await observed.wait();
  EXPECT(done);
  auto text = co_await response.text();
  EXPECT_EQ(text, std::string("complete"));
  const auto events = fixture::terminalPayloads(logs);
  ASSERT(events.size() == 1);
  EXPECT_EQ(rt::getString(events[0], "delivery_outcome"), std::optional<std::string>("delivered"));
  EXPECT_EQ(released, 1);
}

ASYNC_TEST("rejected SSE headers cancel the original body and settle its terminal once") {
  fixture::Isolation isolation;
  ScopedLoopbackFetch network;
  rt::console::Capture logs({rt::console::Method::info});
  struct Observation {
    int cancels = 0;
    int terminals = 0;
    int releases = 0;
  };
  const auto observed = std::make_shared<Observation>();
  const auto server = wire::serveHttp(loopbackOptions(), [observed](rt::Request req, wire::ServeHandlerInfo info) -> rt::Task<rt::Response> {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.cancel = [observed](rt::Reason reason) {
      ++observed->cancels;
      EXPECT_EQ(rt::errorMessage(reason), "Invalid character in header content [\"x-fixture\"]");
    };
    auto response = streamResponse(std::move(source));
    // Fetch Headers accepts DEL in a ByteString. Native HTTP rejects it.
    response.headers().set("x-fixture", "\x7f");
    auto options = fixture::terminalOptions("rejected-sse-headers");
    options.deliveryCompleted = info.completed;
    options.deliverySignal = req.signal();
    options.onTerminal = [observed](auto outcome, auto reason) {
      ++observed->terminals;
      EXPECT_EQ(outcome, "incomplete");
      EXPECT_EQ(reason, "downstream_cancelled");
      return rt::Promise<void>::resolve();
    };
    options.onSettled = [observed] { ++observed->releases; };
    const auto wrapped = terminal::withTerminalRequestLog(response, options);
    auto result = co_await wrapped.wait();
    co_return result;
  });
  const auto address = co_await server.ready.wait();
  auto fetching = rt::fetch(origin(address) + "/rejected-headers");
  const auto response = co_await std::move(fetching);
  EXPECT_EQ(response.status(), 500);
  EXPECT_EQ(response.headers().get("content-type"), "text/plain; charset=utf-8");
  EXPECT(!response.headers().has("x-fixture"));
  const auto body = co_await response.text();
  EXPECT_EQ(body, "Internal Server Error");
  const auto stopped = server.shutdown();
  ASSERT_EQ(observed->cancels, 1);
  co_await stopped.wait();
  EXPECT_EQ(observed->cancels, 1);
  EXPECT_EQ(observed->terminals, 1);
  EXPECT_EQ(observed->releases, 1);
  const auto events = fixture::terminalPayloads(logs, "rejected-sse-headers");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(rt::getString(events[0], "delivery_outcome"), "interrupted");
}

ASYNC_TEST("semantic completion sends ready SSE bytes while one accounting settlement is pending") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  const auto accounting = rt::withResolvers<void>();
  int attempts = 0;
  int releases = 0;
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.start = [](auto controller) {
    controller.enqueue(rt::encodeUtf8("event: response.completed\ndata: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n"));
    controller.close();
  };
  auto original = openai_telemetry::setResponseCompletionTelemetry(streamResponse(source), true);
  auto options = fixture::terminalOptions("pending-completed-accounting");
  options.onTerminal = [accounting, &attempts](std::string outcome, std::optional<std::string> reason) {
    ++attempts; EXPECT_EQ(outcome, std::string("completed")); EXPECT(!reason); return accounting.promise;
  };
  options.onSettled = [&releases] { ++releases; };
  auto wrapped = terminal::withTerminalRequestLog(original, options);
  auto response = co_await wrapped.wait();
  auto reader = response.body()->getReader();
  auto first = co_await reader.read();
  ASSERT(!first.done && first.value);
  EXPECT(rt::includes(rt::decodeUtf8(*first.value), "response.completed"));
  EXPECT_EQ(attempts, 1);
  EXPECT_EQ(releases, 0);
  accounting.resolve();
  auto end = co_await reader.read();
  EXPECT(end.done);
  EXPECT_EQ(attempts, 1);
  // EOF closes the wrapper before logging finishes; let its terminal task settle.
  co_await rt::sleep(1);
  EXPECT_EQ(releases, 1);
  EXPECT_EQ(fixture::terminalPayloads(logs).size(), std::size_t{1});
}

ASYNC_TEST("provider read failure persists client failure evidence once and preserves the downstream error") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  const auto capture = fixture::acceptedInput();
  int persistenceCalls = 0;
  std::optional<sentinel::replay_model::SentinelFailureObservation> internal;
  std::optional<sentinel::replay_model::SentinelClientFailureObservation> client;
  std::vector<std::string> settlements;
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.pull = [](auto) { throw rt::JsError("provider failed fixture"); };
  auto options = fixture::terminalOptions("provider-read-failure");
  options.sentinelReplayInput = capture;
  options.onTerminal = [&settlements](auto outcome, auto reason) {
    settlements.push_back(outcome + ":" + reason.value_or("")); return rt::Promise<void>::resolve();
  };
  options.persistSentinelReplay = [&persistenceCalls, &internal, &client](auto, auto observation, auto clientObservation) {
    ++persistenceCalls; internal = observation; client = clientObservation;
    sentinel::replay_observation::SentinelReplayPersistResult result = sentinel::replay_observation::SentinelReplayPersistResultDisabled{.reason = "kv_unavailable"};
    return rt::Promise<sentinel::replay_observation::SentinelReplayPersistResult>::resolve(result);
  };
  auto wrapped = terminal::withTerminalRequestLog(streamResponse(source), options);
  auto response = co_await wrapped.wait();
  auto reading = response.text();
  EXPECT_REJECTS_MESSAGE(std::move(reading), rt::JsError, "provider failed fixture");
  EXPECT_EQ(persistenceCalls, 1);
  EXPECT_EQ(settlements, std::vector<std::string>{"incomplete:stream_read_error"});
  ASSERT(internal && client);
  EXPECT_EQ(internal->failure_kind, std::optional<std::string>("gateway_stream_read_error"));
  EXPECT_EQ(internal->terminal_type, std::optional<std::string>("error"));
  EXPECT(internal->stream == true && !internal->completed);
  EXPECT(!client->completed && !client->framing_valid);
  EXPECT(fixture::zeroed(capture.body));
  const auto events = fixture::terminalPayloads(logs);
  ASSERT(events.size() == 1);
  // Fresh Bun branch output; normalize only elapsed latency and this fixture's request id.
  auto normalized = events[0];
  normalized.as_object()["latency_ms"] = 0;
  const auto expectedLog = rt::jsonParse(R"GOLDEN({"request_id":"provider-read-failure","route":"responses","status":200,"provider":"gateway","latency_ms":0,"admission_wait_ms":null,"admission_stage":null,"first_provider_dispatch_ms":null,"first_provider_headers_ms":null,"first_codex_dispatch_ms":null,"first_codex_headers_ms":null,"first_upstream_sse_event_ms":null,"first_semantic_commitment_ms":null,"stream_terminal_ms":null,"downstream_drain_ms":null,"delivery_outcome":"interrupted","model":null,"reasoning":null,"output_token_allowance":null,"rate_limit_wait_ms":null,"rate_limit_failover_model":null,"provider_request_id":null,"input_tokens":null,"cached_input_tokens":null,"cache_write_input_tokens":null,"output_tokens":null,"total_tokens":null,"usage_observed":false,"usage_telemetry_status":"missing","prompt_cache_key_present":false,"prompt_cache_mode":"unspecified","explicit_breakpoint_count":0,"account_slot":null,"account_cohort_id":null,"active_generation":null,"active_transition_reason":null,"fallback_reason":null,"semantic_output_observed":null,"upstream_event_kinds":[],"in_stream_retries":0,"upstream_failure_events":[],"stream":true,"stream_terminal_type":"error","failure_kind":"gateway_stream_read_error","response_created_observed":false,"synthetic_terminal_type":null,"attempted_providers":[],"removed_provider_trigger_class":null,"removed_provider_circuit_transition":null,"removed_provider_selected_model":null,"removed_provider_task_type":null,"removed_provider_latency_ms":null,"removed_provider_terminal_status":null,"removed_provider_semantic_commitment":null,"git_sha":"unknown","deno_revision":"unknown","router_revision":null})GOLDEN");
  EXPECT_EQ(rt::jsonStringify(normalized), rt::jsonStringify(expectedLog));
  EXPECT_EQ(rt::getString(events[0], "failure_kind"), std::optional<std::string>("gateway_stream_read_error"));
  EXPECT_EQ(rt::getString(events[0], "delivery_outcome"), std::optional<std::string>("interrupted"));
}

ASYNC_TEST("cancelled downstream drops capture ownership and preserves completed semantic settlement") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  const auto capture = fixture::acceptedInput();
  const auto pending = rt::withResolvers<void>();
  int cancelled = 0;
  int persisted = 0;
  std::vector<std::string> settlements;
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.start = [](auto controller) { controller.enqueue(rt::encodeUtf8("data: ready\n\n")); };
  source.pull = [pending](auto) -> rt::Task<void> { co_await pending.promise.wait(); };
  source.cancel = [&cancelled](auto) { ++cancelled; };
  auto response = openai_telemetry::setResponseCompletionTelemetry(streamResponse(source), true);
  auto options = fixture::terminalOptions("cancelled-completed");
  options.sentinelReplayInput = capture;
  options.onTerminal = [&settlements](auto outcome, auto) { settlements.push_back(outcome); return rt::Promise<void>::resolve(); };
  options.persistSentinelReplay = [&persisted](auto, auto, auto) {
    ++persisted;
    sentinel::replay_observation::SentinelReplayPersistResult result = sentinel::replay_observation::SentinelReplayPersistResultDisabled{.reason = "kv_unavailable"};
    return rt::Promise<sentinel::replay_observation::SentinelReplayPersistResult>::resolve(result);
  };
  auto wrapped = terminal::withTerminalRequestLog(response, options);
  auto output = co_await wrapped.wait();
  auto reader = output.body()->getReader();
  auto first = co_await reader.read();
  EXPECT(!first.done);
  const auto cancellation = reader.cancelPromise(rt::makeReason(rt::JsError("fixture cancel")));
  co_await cancellation.wait();
  pending.resolve();
  co_await rt::sleep(1);
  EXPECT_EQ(cancelled, 1);
  EXPECT_EQ(persisted, 0);
  EXPECT_EQ(settlements, std::vector<std::string>{"completed"});
  EXPECT(fixture::zeroed(capture.body));
  const auto events = fixture::terminalPayloads(logs);
  ASSERT(events.size() == 1);
  EXPECT_EQ(rt::getString(events[0], "delivery_outcome"), std::optional<std::string>("interrupted"));
}

ASYNC_TEST("accounting retries three times and a redacted failure never replaces the buffered response") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info, rt::console::Method::warn});
  int calls = 0;
  int releases = 0;
  auto options = fixture::terminalOptions("retries");
  options.onSettled = [&releases] { ++releases; };
  options.onTerminal = [&calls](auto outcome, auto reason) {
    ++calls;
    EXPECT_EQ(outcome, std::string("incomplete"));
    EXPECT_EQ(reason, std::optional<std::string>("response_incomplete"));
    return rt::Promise<void>::reject(rt::makeReason(rt::TypeError("private")));
  };
  auto wrapped = terminal::withTerminalRequestLog(fixture::jsonResponse("complete"), options);
  auto response = co_await wrapped.wait();
  auto text = co_await response.text();
  EXPECT_EQ(text, std::string("complete"));
  EXPECT_EQ(calls, 3);
  EXPECT_EQ(releases, 1);
  std::vector<json::value> warnings;
  for (const auto& call : logs.calls()) {
    if (call.args.size() == 2 && call.args[0] == "[ai.ubq.fi] quota_accounting_failed") warnings.push_back(rt::jsonParse(call.args[1].string()));
  }
  ASSERT(warnings.size() == 1);
  EXPECT(rt::deepEqual(warnings[0], rt::jsonParse(R"({"request_id":"retries","route":"responses","errors":[{"class":"TypeError"}]})")));
  EXPECT(!rt::includes(rt::jsonStringify(warnings[0]), "private"));
}

TEST("aggregate accounting warnings preserve source error classes without messages") {
  rt::console::Capture warnings({rt::console::Method::warn});
  const std::vector<rt::Reason> errors{rt::makeReason(rt::TypeError("private")), rt::thrown(nullptr), rt::thrown(4), rt::thrown("private")};
  terminal::warnQuotaAccountingFailure({"responses", "aggregate"}, rt::makeReason(rt::AggregateError(errors, "private")));
  ASSERT(warnings.calls().size() == 1);
  const auto observed = rt::jsonParse(warnings.calls()[0].args[1].string());
  EXPECT(rt::deepEqual(observed, rt::jsonParse(R"({"request_id":"aggregate","route":"responses","errors":[{"class":"TypeError"},{"class":"object"},{"class":"number"},{"class":"string"}]})")));
}
