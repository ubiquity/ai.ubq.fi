// Links: tests/usage_optimization_measurement_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/openai_schema.cpp src/agent_messages.cpp src/request.cpp src/auth/index.cpp src/kernel/quota_reservations.cpp src/authority/native_kernel_contracts.cpp src/openai.cpp src/utils.cpp
// src/authority/native_kernel.cpp src/provider/metered.cpp src/api_key_policy.cpp src/models/codex_models.cpp src/authority/native_api_key.cpp src/kv.cpp src/auth/passport.cpp src/inference_deadline.cpp
// src/provider/surplus_claude_request.cpp src/kernel/quota_v2.cpp src/kernel/attestation.cpp src/api_keys.cpp src/authority/native_api_key_policy.cpp src/models/catalog.cpp src/config.cpp src/http.cpp
// src/storage/kv.cpp src/defaults.cpp src/kernel/policy_queue.cpp src/input_normalization.cpp src/provider/lithos.cpp src/request_policy.cpp src/provider/claude_code_models.cpp src/storage/native_kv.cpp
// src/models/metadata.cpp src/models/codex_models_fetch.cpp src/provider/surplus.cpp src/responses_request.cpp src/handler/index.cpp src/bounded_response_body.cpp src/types.cpp src/provider/cerebras.cpp
// src/auth/passkeys.cpp src/admin/kernel.cpp src/codex/auth_refresh.cpp src/admin/api_keys.cpp src/admin/api_key_audit.cpp src/models/openrouter_models.cpp src/codex/dispatch.cpp
// src/admin/codex.cpp src/auth/local_admin.cpp src/handler/terminal_route.cpp src/admin/claude_code.cpp src/release.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_backfill.cpp src/provider/selection.cpp
// src/storage/sqlite_readonly.cpp src/provider/capacity_parse.cpp src/codex/reset_settings.cpp src/kernel/usage.cpp src/codex/auth.cpp src/codex/banked_reset_pool.cpp src/authority/native_paid_contracts.cpp src/handler/admission.cpp
// src/admin/api_key_mutations.cpp src/codex/banked_reset_claim.cpp src/paid_fallback/ledger_stats.cpp src/provider/claude_code_credentials.cpp src/codex/supervisor_brief.cpp src/paid_fallback/ledger_settlement.cpp src/provider/claude_code.cpp src/codex/banked_reset.cpp
// src/authority/native_paid.cpp src/provider/capacity_events.cpp src/codex/overage_settings.cpp src/codex/banked_reset_provider.cpp src/provider/capacity_contract.cpp src/sentinel/replay_observation.cpp src/health.cpp src/kernel/quota_write.cpp
// src/provider/claude_code_quota.cpp src/provider/capacity.cpp src/auth/relay.cpp src/openai_telemetry.cpp src/inference_admission.cpp src/paid_fallback/health.cpp src/provider/capacity_history.cpp src/provider/lithos_models.cpp
// src/responses_failover_stream.cpp src/metered_quota.cpp src/handler/terminal_log.cpp src/provider/claude_code_accounts.cpp src/systemone/handlers.cpp src/deepseek/flash_aliases.cpp src/admin/codex_reset.cpp src/codex/routing_mutations.cpp
// src/cache/prompt_analytics_read.cpp src/codex/capacity_routing.cpp src/static.cpp src/handler/http.cpp src/decisions/handlers.cpp src/responses_native_headers.cpp src/cache/prompt_analytics_queue.cpp src/catalog/store.cpp
// src/provider/openrouter.cpp src/admin/error_log.cpp src/sentinel/replay_store.cpp src/runtime_config.cpp src/authority/native_codex_capacity.cpp src/sentinel/replay_read.cpp src/sentinel/replay_envelope.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/claude_code_access.cpp src/provider/capacity_rollups.cpp src/admin/defaults.cpp src/cache/scope_experiment_targets.cpp src/authority/native_codex_evaluation.cpp src/provider/health.cpp src/codex/external_auth.cpp src/sentinel/replay_retention.cpp
// src/models/codex_models_availability.cpp src/authority/native_codex_mutations.cpp src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_model.cpp src/sentinel/incident_outbox.cpp src/codex/supervisor_config.cpp src/quota_projection.cpp src/cache/kv_migration_validation.cpp
// src/models/codex_models_whitelist.cpp src/cache/prompt_analytics_core.cpp src/provider/anthropic_replay.cpp src/embeddings/jobs.cpp src/auth/passport_sessions.cpp src/paid_fallback/index.cpp src/codex/supervisor.cpp src/sentinel/replay_retention_metadata.cpp
// src/authority/native_paid_policy.cpp src/embeddings/handlers.cpp src/authority/native_codex_contracts.cpp src/sentinel/upstream_capture.cpp src/codex/banked_reset_submission.cpp src/cache/kv_migration_usage.cpp src/catalog/types.cpp src/paid_fallback/rollups.cpp
// src/sentinel/replay_retention_schema.cpp src/cache/kv_migration_paid_fallback.cpp src/provider/cerebras_rate_limits.cpp src/embeddings/ledger.cpp src/cache/scope_experiment_model.cpp src/optional_telemetry_queue.cpp src/live/handler.cpp src/analytics.cpp
// src/provider/anthropic_chat_responses.cpp src/codex/experiment.cpp src/codex/supervisor_secret.cpp src/authority/native_api_key_audit.cpp src/upstream_wire.cpp src/catalog/index.cpp src/codex/account_routing.cpp src/codex/routing_state.cpp
// src/embeddings/voyage.cpp src/sentinel/replay_admin.cpp src/codex/manual_reset_state.cpp src/provider/claude_capacity_history.cpp src/admin/sentinel_incident.cpp src/runtime/http_tasks.cpp src/auth/local_development_key.cpp src/provider/anthropic_response_wire.cpp
// src/provider/claude_capacity_rollups.cpp src/cache/kv_migration_base.cpp src/debug_routing.cpp src/responses_stream.cpp src/codex/native_auth.cpp src/codex/manual_reset.cpp src/catalog/models.cpp src/provider/claude_code_usage_schema.cpp
// src/deepseek/index.cpp src/deepseek/waterfall.cpp src/kernel/quota_reserve.cpp src/codex/routing_evaluation.cpp src/codex/rate_limit_429.cpp src/codex/supervisor_follow.cpp src/codex/routing_serial.cpp src/provider/removed_provider_telemetry.cpp
// src/provider/presentation.cpp src/live/sideband.cpp src/provider/capacity_analytics.cpp src/paid_fallback/ledger_admission.cpp src/kv_conversion/profile_json.cpp src/sentinel/replay_limits.cpp src/images.cpp src/live/routes.cpp
// src/recent_model_context.cpp src/responses_handler.cpp src/authority/native_codex.cpp src/codex/supervisor_inventory.cpp src/provider/lithos_rate_limits.cpp src/paid_fallback/removed_provider.cpp src/provider/cerebras_responses.cpp src/cache/scope_targets.cpp
// src/responses_attempts.cpp src/cache/scope_experiment.cpp src/admin/capacity_observation_sync.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_responses.cpp src/provider/openrouter_request_controls.cpp src/responses_buffered.cpp src/live/upstream.cpp
// src/cache/telemetry_gate.cpp src/deepseek/context_admission_failure.cpp src/admin/chart_source_proxy.cpp src/runtime/http_early_sse.cpp src/codex/supervisor_transport.cpp src/provider/removed_provider_circuit.cpp src/codex/responses_error.cpp src/codex/native_auth_transport.cpp
// src/provider/cerebras_handlers.cpp src/provider/openrouter_handlers.cpp src/authority/native_codex_429.cpp src/codex/supervisor_log.cpp src/deepseek/waterfall_handler.cpp src/chat/envelope.cpp src/provider/lithos_handlers.cpp src/catalog/promotion.cpp
// src/provider/lithos_streams.cpp src/chat/stream_translation.cpp src/codex/quota.cpp src/runtime/websocket.cpp src/chat/completions.cpp src/deepseek/chat_projection.cpp src/paid_fallback/routing.cpp src/provider/stream_relay.cpp
// src/live/calls.cpp src/deepseek/responses.cpp src/deepseek/forwarded_payload_policy.cpp src/provider/openrouter_streams.cpp src/deepseek/handlers.cpp src/paid_fallback/claude_code_route.cpp src/deepseek/responses_stream.cpp src/lib/jev_compaction/state.cpp
// src/deepseek/responses_payload.cpp src/provider/openrouter_web_tools.cpp src/deepseek/stream.cpp src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/types.cpp src/codex/index.cpp src/lib/jev_compaction/compact.cpp src/codex/responses_operations.cpp
// src/lib/jev_compaction/request.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp tests/helpers/kv_budget_harness.cpp tests/helpers/counting_kv.cpp src/handler/serve_handler.cpp
// Port of tests/usage-optimization-measurement.test.ts; synthetic observations only.
#include "tests/helpers/counting_kv.hpp"
#include "tests/helpers/kv_budget_harness.hpp"
#include "uos/codex/index.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <deque>

using namespace uos;
namespace m = uos::tests::helpers::counting_kv;
namespace h = uos::tests::helpers::kv_budget_harness;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  constexpr std::string_view MODEL = "gpt-5-usage-measurement";
  constexpr std::string_view UOS_ALLOWLIST_TOKEN = "uos-usage-measurement-token";
  constexpr std::string_view ADMIN_ALLOWLIST_TOKEN = "admin-usage-measurement-token";
  struct UpstreamReply {
    rt::BodyInit body;
    double responseBytes;
    int status = 200;
    rt::Headers headers{{"Content-Type", "text/event-stream"}};
    std::optional<rt::Promise<void>> waitFor{};
  };
  std::string completedSse(const std::string& label) {
    const json::object content{{"type", "output_text"}, {"text", "measurement " + label}};
    const json::object message{{"type", "message"}, {"role", "assistant"}, {"content", json::array{content}}};
    const json::object response{{"id", "resp-" + label}, {"model", std::string(MODEL)}, {"output", json::array{message}},
      {"usage", json::object{{"input_tokens", 1}, {"output_tokens", 1}, {"total_tokens", 2}}}};
    const json::object payload{{"type", "response.completed"}, {"response", response}};
    return "data: " + rt::jsonStringify(payload) + "\n\n";
  }
  UpstreamReply sseReply(const std::string& label, std::optional<rt::Promise<void>> gate = std::nullopt) {
    const auto body = completedSse(label);
    return {body, static_cast<double>(rt::utf8ByteLength(body)), 200, rt::Headers{{"Content-Type", "text/event-stream"}}, gate};
  }
  UpstreamReply errorReply(int status, const std::string& type = "server_error", const std::optional<std::string>& retryAfter = std::nullopt) {
    const auto body = rt::jsonStringify(json::object{{"error", json::object{{"message", "measurement upstream failure"}, {"type", type}}}});
    rt::Headers headers{{"Content-Type", "application/json"}};
    if (retryAfter) headers.set("Retry-After", *retryAfter);
    return {body, static_cast<double>(rt::utf8ByteLength(body)), status, headers};
  }
  UpstreamReply cancellableSseReply() {
    const auto firstEvent = std::string("data: {\"type\":\"response.output_text.delta\",\"delta\":\"partial\"}\n\n");
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [firstEvent](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { controller.enqueue(rt::encodeUtf8(firstEvent)); };
    return {rt::ByteStream(source), static_cast<double>(rt::utf8ByteLength(firstEvent))};
  }
  rt::Request inferenceRequest(const std::string& token, bool stream = false) {
    json::object body{{"model", std::string(MODEL)}, {"input", "measure the exact wire body"}};
    if (stream) rt::objectDefine(body, "stream", true);
    rt::RequestInit init;
    init.method = "POST";
    init.headers = rt::Headers{{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}};
    init.body = rt::jsonStringify(body);
    return rt::Request("https://ai.ubq.fi/v1/responses", init);
  }
  rt::Task<void> settleBackgroundWork(std::shared_ptr<m::CountingKv> kv) {
    auto previous = kv->commands.size();
    unsigned stableTurns = 0;
    for (unsigned turn = 0; turn < 8; ++turn) {
      co_await rt::microtask();
      co_await rt::sleep(5);
      const auto current = kv->commands.size();
      stableTurns = current == previous ? stableTurns + 1 : 0;
      if (stableTurns >= 2) co_return;
      previous = current;
    }
    throw rt::JsError("background KV work did not settle before the measurement window closed");
  }
  rt::Task<rt::Response> runScenario(std::shared_ptr<m::CountingKv> kv, m::KvMeasurementContext context, std::function<rt::Task<rt::Response>()> run) {
    const auto finish = kv->beginMeasurement(context);
    const auto startedAt = rt::performanceNow();
    std::optional<rt::Response> result;
    rt::Reason failure;
    try { AWAIT(response, run()); result = response; } catch (...) { failure = std::current_exception(); }
    const auto latency = rt::performanceNow() - startedAt;
    co_await settleBackgroundWork(kv);
    kv->setLatency(latency);
    finish();
    if (failure) std::rethrow_exception(failure);
    co_return *result;
  }
  const m::KvOperationBudget& budgetFor(const std::vector<m::KvOperationBudget>& budgets, const std::string& scenario) {
    const auto found = std::find_if(budgets.begin(), budgets.end(), [&](const auto& row) { return row.scenario == scenario; });
    if (found == budgets.end()) throw rt::JsError("missing measurement budget for " + scenario);
    return *found;
  }
  bool keyAt(const rt::KvKey& key, std::size_t index, const std::string& value) {
    return index < key.size() && rt::kvKeyPartStrictEquals(key[index], rt::KvKeyPart(value));
  }
  bool usageRollupKey(const rt::KvKey& key) { return keyAt(key, 0, "uos_ai") && keyAt(key, 1, "paid_fallback") && keyAt(key, 3, "usage_rollup"); }
}

ASYNC_TEST("usage optimization fixture records per-auth KV commands, atomic commits, and Codex wire bytes") {
  const auto tmp = rt::env::get("TMPDIR").value_or("/tmp");
  rt::env::ScopedEnvironment environment(std::vector<std::pair<std::string, std::string>>{
    {"HOME", tmp + "/h"}, {"TMPDIR", tmp}, {"UOS_AI_TOKEN", std::string(UOS_ALLOWLIST_TOKEN)}, {"DENO_DEPLOY_TOKEN", std::string(ADMIN_ALLOWLIST_TOKEN)}});
  const auto savedConfig = config::config;
  config::config.codexAuthJsonB64.clear();
  config::config.authTokens = {std::string(UOS_ALLOWLIST_TOKEN)}; config::config.adminTokens = {std::string(ADMIN_ALLOWLIST_TOKEN)}; config::config.isDeploy = true;
  h::installHarness();
  const auto kv = std::make_shared<m::CountingKv>();
  uos::kv::setKvForTest(kv);
  const auto cleanup = rt::scopeExit([savedConfig] { h::cleanupHarness(); config::config = savedConfig; });
  rt::console::Capture capture({rt::console::Method::info, rt::console::Method::warn, rt::console::Method::error});
  std::deque<UpstreamReply> queuedReplies;
  unsigned fetchCalls = 0;
  std::function<void()> onFetch;
  const rt::Fetch fetcher([&](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
    const auto url = h::fetchInputUrl(input);
    EXPECT(rt::endsWith(url, "/responses"));
    ASSERT(init.body && std::holds_alternative<std::string>(*init.body));
    kv->recordSerializedRequestBytes(static_cast<double>(rt::utf8ByteLength(std::get<std::string>(*init.body))));
    ASSERT(!queuedReplies.empty());
    auto reply = queuedReplies.front(); queuedReplies.pop_front();
    ++fetchCalls;
    if (onFetch) onFetch();
    if (reply.waitFor) co_await reply.waitFor->wait();
    kv->recordUpstreamResponseBytes(reply.responseBytes);
    const rt::ResponseInit responseInit{.status = reply.status, .headers = reply.headers};
    co_return rt::Response(reply.body, responseInit);
  });
  const rt::GlobalFetchOverride fetch(fetcher);
  const auto prepare = [&](std::optional<std::tuple<std::string, std::string, double>> key = std::nullopt) -> rt::Task<void> {
    kv->clearData(); h::resetApiKeyPolicyCacheForTest(); h::resetRuntimeConfigCacheForTest(); h::resetCodexAuthCacheForTest(); h::resetProviderHealthThrottleForTest();
    auto runtime = h::runtime();
    runtime["default_model"] = std::string(MODEL);
    runtime["codex_models"] = json::object{{"source", "chatgpt_codex"}, {"client_version", "0.150.0"}, {"updated_at_ms", rt::dateNow()},
      {"models", json::array{json::object{{"slug", std::string(MODEL)}, {"default_reasoning_level", "medium"}, {"supported_reasoning_levels", json::array{"none", "low", "medium", "high"}}}}}};
    kv->seed(h::RUNTIME_CONFIG_V2_KEY, runtime);
    const json::object auth{{"accounts", json::array{json::object{{"access_token", "measurement-access-token"}, {"refresh_token", "measurement-refresh-token"},
      {"account_id", "measurement-account"}, {"updated_at_ms", rt::dateNow()}}}}, {"updated_at_ms", rt::dateNow()}};
    kv->seed(h::CODEX_AUTH_POOL_KV_KEY, auth);
    if (key) {
      const auto& [token, id, limit] = *key;
      AWAIT(hash, h::sha256Base64Url(token));
      const json::object record{{"id", id}, {"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", limit}, {"usage_requests", 0},
        {"usage_reset_at_ms", rt::dateNow() + 60000}, {"window_ms", 60000}, {"usage_quota_version", 3}, {"paid_fallback_enabled", false},
        {"paid_fallback_limit_microcredits", 0}, {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr}};
      kv->seed(rt::KvKey{"ubq_ai", "api_keys", "hash", hash}, record);
    }
    co_return;
  };
  const auto queue = [&](std::initializer_list<UpstreamReply> replies) { queuedReplies.assign(replies); fetchCalls = 0; onFetch = {}; };
  const auto runHandler = [&](const std::string& token, const std::string& authKind, const std::string& outcome) {
    const m::KvMeasurementContext context{authKind, outcome};
    const std::function<rt::Task<rt::Response>()> run = [token] { return h::handler(inferenceRequest(token)); };
    return runScenario(kv, context, run);
  };
  EXPECT_EQ(m::classifyKvKey(rt::KvKey{"uos_ai", "api_key_usage", "v3", "key"}), "mandatory_correctness");
  EXPECT_EQ(m::classifyKvKey(rt::KvKey{"ubq_ai", "prompt_cache_telemetry", "v1"}), "optional_telemetry");
  const auto boundedToken = "u_" + std::string(64, '1');
  auto preparation0 = prepare(std::tuple{boundedToken, std::string("bounded-measurement"), 2.0}); co_await std::move(preparation0); queue({sseReply("bounded")});
  AWAIT(bounded, runHandler(boundedToken, "bounded_api_key", "success")); EXPECT_EQ(bounded.status(), 200); EXPECT_EQ(fetchCalls, 1U);
  const auto unlimitedToken = "u_" + std::string(64, '2');
  auto preparation1 = prepare(std::tuple{unlimitedToken, std::string("unlimited-measurement"), -1.0}); co_await std::move(preparation1); queue({sseReply("unlimited")});
  AWAIT(unlimited, runHandler(unlimitedToken, "unlimited_api_key", "success")); EXPECT_EQ(unlimited.status(), 200); EXPECT_EQ(fetchCalls, 1U);
  auto preparation2 = prepare(); co_await std::move(preparation2); queue({sseReply("uos")}); AWAIT(uosResponse, runHandler(std::string(UOS_ALLOWLIST_TOKEN), "uos_allowlist", "success")); EXPECT_EQ(uosResponse.status(), 200); EXPECT_EQ(fetchCalls, 1U);
  auto preparation3 = prepare(); co_await std::move(preparation3); queue({sseReply("admin")}); AWAIT(admin, runHandler(std::string(ADMIN_ALLOWLIST_TOKEN), "admin_allowlist", "success")); EXPECT_EQ(admin.status(), 200); EXPECT_EQ(fetchCalls, 1U);
  const auto failureToken = "u_" + std::string(64, '3');
  auto preparation4 = prepare(std::tuple{failureToken, std::string("upstream-failure-measurement"), 2.0}); co_await std::move(preparation4); queue({errorReply(503)});
  AWAIT(upstreamFailure, runHandler(failureToken, "bounded_api_key", "upstream_failure")); EXPECT_EQ(upstreamFailure.status(), 503); EXPECT_EQ(fetchCalls, 1U);
  auto preparation5 = prepare(); co_await std::move(preparation5);
  const json::object retryBody{{"model", std::string(MODEL)}, {"input", "retry must preserve the same serialized body"}};
  queue({errorReply(429, "rate_limit_error", "1"), sseReply("retry")});
  codex::dispatch::FetchCodexResponsesOptions retryOptions; retryOptions.retrySleep = [](double) { return rt::fulfilledTask(); };
  const std::function<rt::Task<rt::Response>()> runRetry = [retryBody, retryOptions] { return codex::index::fetchCodexResponses(retryBody, retryOptions); };
  const m::KvMeasurementContext retryContext{"codex_auth_pool", "retry"};
  AWAIT(retried, runScenario(kv, retryContext, runRetry)); EXPECT_EQ(retried.status(), 200); EXPECT_EQ(fetchCalls, 2U);
  const auto disconnectToken = "u_" + std::string(64, '4');
  auto preparation6 = prepare(std::tuple{disconnectToken, std::string("disconnect-measurement"), 2.0}); co_await std::move(preparation6); queue({cancellableSseReply()});
  const auto finishDisconnect = kv->beginMeasurement({"bounded_api_key", "client_disconnect"}); const auto disconnectStart = rt::performanceNow();
  AWAIT(disconnectResponse, h::handler(inferenceRequest(disconnectToken, true))); EXPECT_EQ(disconnectResponse.status(), 200); ASSERT(disconnectResponse.body());
  auto reader = disconnectResponse.body()->getReader(); AWAIT(first, reader.read()); EXPECT(!first.done);
  const auto disconnectReason = rt::thrown("measurement client disconnected"); co_await reader.cancel(disconnectReason);
  const auto disconnectLatency = rt::performanceNow() - disconnectStart; co_await settleBackgroundWork(kv); kv->setLatency(disconnectLatency); finishDisconnect(); EXPECT_EQ(fetchCalls, 1U);
  const auto concurrentToken = "u_" + std::string(64, '5');
  auto preparation7 = prepare(std::tuple{concurrentToken, std::string("concurrent-measurement"), 1.0}); co_await std::move(preparation7);
  const auto upstreamGate = h::deferred(); queue({sseReply("concurrent", upstreamGate.promise)});
  const auto firstFetch = h::deferred(); onFetch = firstFetch.resolve;
  const auto finishConcurrent = kv->beginMeasurement({"bounded_api_key", "concurrent_admission"}); const auto concurrentStart = rt::performanceNow();
  std::vector<rt::Promise<rt::Response>> pending;
  for (unsigned i = 0; i < 8; ++i) pending.emplace_back(h::handler(inferenceRequest(concurrentToken)));
  co_await firstFetch.promise.wait(); upstreamGate.resolve(); AWAIT(responses, rt::all(std::move(pending)).wait());
  const auto concurrentLatency = rt::performanceNow() - concurrentStart; co_await settleBackgroundWork(kv); kv->setLatency(concurrentLatency); finishConcurrent();
  EXPECT_EQ(std::count_if(responses.begin(), responses.end(), [](const auto& response) { return response.status() == 200; }), 1);
  EXPECT_EQ(std::count_if(responses.begin(), responses.end(), [](const auto& response) { return response.status() == 429; }), 7); EXPECT_EQ(fetchCalls, 1U);
  EXPECT(queuedReplies.empty()); const auto budgets = kv->budgets();
  EXPECT(std::none_of(kv->commands.begin(), kv->commands.end(), [](const auto& row) { return !row.scenario; }));
  const std::vector<std::string> expected{"admin_allowlist:success", "bounded_api_key:client_disconnect", "bounded_api_key:concurrent_admission", "bounded_api_key:success", "bounded_api_key:upstream_failure", "codex_auth_pool:retry", "unlimited_api_key:success", "uos_allowlist:success"};
  std::vector<std::string> actual;
  for (const auto& budget : budgets) { actual.push_back(budget.scenario); EXPECT(budget.commands > 0); EXPECT(budget.serialized_request_bytes > 0); EXPECT(budget.upstream_response_bytes > 0); EXPECT(budget.latency_ms); }
  EXPECT_EQ(actual, expected);
  EXPECT(budgetFor(budgets, "bounded_api_key:success").atomic_commits >= 2); EXPECT(budgetFor(budgets, "unlimited_api_key:success").atomic_commits >= 2);
  EXPECT(budgetFor(budgets, "uos_allowlist:success").commands > 0); EXPECT(budgetFor(budgets, "admin_allowlist:success").commands > 0);
  EXPECT_EQ(budgetFor(budgets, "codex_auth_pool:retry").serialized_request_bytes, static_cast<double>(rt::utf8ByteLength(rt::jsonStringify(retryBody)) * 2));
  unsigned rollupReads = 0, rollupCommits = 0, disconnectRollups = 0, disconnectV3Commits = 0, disconnectReplayCommits = 0;
  bool allowlistLedger = false, committed = false;
  for (const auto& row : kv->commands) {
    const bool rollup = std::any_of(row.keys.begin(), row.keys.end(), usageRollupKey);
    if (row.scenario == "bounded_api_key:success" && rollup) { if (row.command == "get") ++rollupReads; if (row.command == "atomic.commit" && row.atomicResult == "committed") ++rollupCommits; }
    if (row.scenario == "bounded_api_key:client_disconnect") {
      if (rollup) ++disconnectRollups;
      if (row.command == "atomic.commit" && row.atomicResult == "committed" && std::any_of(row.keys.begin(), row.keys.end(), [](const auto& key) { return keyAt(key, 0, "uos_ai") && keyAt(key, 1, "api_key_usage") && keyAt(key, 2, "v3"); })) ++disconnectV3Commits;
      if (row.command == "atomic.commit" && row.atomicResult == "committed" && std::any_of(row.keys.begin(), row.keys.end(), [](const auto& key) { return keyAt(key, 0, "uos_ai") && keyAt(key, 1, "sentinel_replay"); })) ++disconnectReplayCommits;
    }
    if ((row.scenario == "uos_allowlist:success" || row.scenario == "admin_allowlist:success") && std::any_of(row.keys.begin(), row.keys.end(), [](const auto& key) { return keyAt(key, 0, "uos_ai") && keyAt(key, 1, "api_key_usage"); })) allowlistLedger = true;
    if (row.command == "atomic.commit" && row.atomicResult == "committed") committed = true;
  }
  EXPECT_EQ(rollupReads, 1U); EXPECT_EQ(rollupCommits, 1U); EXPECT_EQ(budgetFor(budgets, "bounded_api_key:client_disconnect").atomic_commits, 5);
  EXPECT_EQ(disconnectRollups, 0U); EXPECT_EQ(disconnectV3Commits, 2U); EXPECT_EQ(disconnectReplayCommits, 2U);
  EXPECT(budgetFor(budgets, "bounded_api_key:concurrent_admission").atomic_commits >= 2); EXPECT(!allowlistLedger); EXPECT(committed);
  json::array observations;
  for (const auto& budget : budgets) observations.push_back(json::object{{"scenario", budget.scenario}, {"reads", budget.read_commands}, {"writes", budget.write_mutations}, {"atomic_commits", budget.atomic_commits},
    {"mandatory_commands", budget.mandatory_correctness_commands}, {"optional_commands", budget.optional_telemetry_commands}, {"request_bytes", budget.serialized_request_bytes}, {"response_bytes", budget.upstream_response_bytes}, {"latency_ms", budget.latency_ms.value_or(0)}});
  rt::console::info("[usage-optimization] operation-byte-budget", rt::jsonStringify(observations));
}
