// Links: tests/sentinel_upstream_providers_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
//        src/utils.cpp src/provider/cerebras.cpp src/handler/index.cpp src/api_keys.cpp src/sentinel/upstream_capture.cpp src/admin/codex.cpp src/auth/index.cpp src/auth/passkeys.cpp src/admin/kernel.cpp src/codex/supervisor.cpp src/codex/banked_reset_provider.cpp src/admin/api_keys.cpp src/sentinel/replay_admin.cpp src/quota_projection.cpp
//        src/types.cpp src/provider/openrouter.cpp src/admin/api_key_mutations.cpp src/authority/native_api_key_policy.cpp src/config.cpp src/kernel/policy_queue.cpp src/metered_quota.cpp src/release.cpp src/api_key_policy.cpp src/codex/auth.cpp src/codex/overage_settings.cpp src/sentinel/replay_observation.cpp src/cache/scope_experiment.cpp
//        src/authority/native_api_key.cpp src/http.cpp src/kernel/attestation.cpp src/codex/supervisor_config.cpp src/codex/external_auth.cpp src/provider/capacity.cpp src/models/codex_models_fetch.cpp src/kernel/usage.cpp src/admin/claude_code.cpp src/paid_fallback/ledger_backfill.cpp src/cache/scope_experiment_targets.cpp
//        src/authority/native_paid_contracts.cpp src/cache/scope_experiment_model.cpp src/authority/native_paid.cpp src/authority/native_paid_policy.cpp src/codex/index.cpp src/provider/capacity_contract.cpp src/provider/cerebras_rate_limits.cpp src/paid_fallback/ledger_state.cpp src/provider/claude_code_accounts.cpp
//        src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics_core.cpp src/sentinel/replay_read.cpp src/health.cpp src/cache/telemetry_gate.cpp src/debug_routing.cpp src/codex/capacity_routing.cpp src/auth/local_development_key.cpp src/admin/codex_reset.cpp src/sentinel/replay_model.cpp src/admin/api_key_audit.cpp src/auth/passport.cpp
//        src/authority/native_codex_capacity.cpp src/models/codex_models.cpp tests/helpers/sentinel_recorded_upstream.cpp src/provider/health.cpp src/authority/native_codex_contracts.cpp src/runtime_config.cpp src/provider/selection.cpp src/admin/defaults.cpp src/provider/capacity_parse.cpp src/auth/passport_sessions.cpp
//        src/provider/capacity_history.cpp src/provider/surplus.cpp src/codex/account_routing.cpp src/codex/routing_state.cpp src/request.cpp src/analytics.cpp src/codex/reset_settings.cpp src/paid_fallback/rollups.cpp src/agent_messages.cpp src/provider/capacity_analytics.cpp src/provider/claude_code_credentials.cpp
//        src/authority/native_kernel.cpp src/provider/claude_code_access.cpp src/provider/claude_code_models.cpp src/codex/routing_evaluation.cpp src/sentinel/replay_envelope.cpp src/provider/capacity_rollups.cpp src/models/codex_models_availability.cpp src/provider/presentation.cpp src/authority/native_codex_mutations.cpp src/codex/dispatch.cpp
//        src/kernel/quota_reservations.cpp src/provider/metered.cpp src/cache/kv_migration_base.cpp src/sentinel/incident_outbox.cpp src/openai_telemetry.cpp src/catalog/types.cpp src/provider/surplus_claude_request.cpp src/authority/native_api_key_audit.cpp src/kernel/quota_v2.cpp src/paid_fallback/ledger_admission.cpp src/codex/experiment.cpp
//        src/kernel/quota_write.cpp src/cache/prompt_analytics_queue.cpp src/provider/anthropic_replay.cpp src/handler/terminal_route.cpp src/cache/kv_migration_validation.cpp src/codex/manual_reset_state.cpp src/cache/kv_migration_usage.cpp src/models/codex_models_whitelist.cpp src/codex/supervisor_follow.cpp src/codex/auth_refresh.cpp
//        src/defaults.cpp src/cache/kv_migration_paid_fallback.cpp src/kv_conversion/profile_json.cpp src/codex/manual_reset.cpp src/paid_fallback/ledger_stats.cpp src/handler/terminal_log.cpp src/sentinel/replay_store.cpp src/models/openrouter_models.cpp src/codex/routing_mutations.cpp src/static.cpp src/models/catalog.cpp
//        src/provider/claude_code_quota.cpp src/codex/native_auth.cpp src/models/metadata.cpp src/codex/routing_serial.cpp src/optional_telemetry_queue.cpp src/catalog/index.cpp src/input_normalization.cpp src/authority/native_codex_evaluation.cpp src/catalog/store.cpp src/provider/lithos.cpp src/catalog/models.cpp
//        src/sentinel/replay_retention.cpp src/kv.cpp src/provider/lithos_models.cpp src/sentinel/replay_retention_schema.cpp src/images.cpp src/authority/native_codex.cpp src/provider/capacity_events.cpp src/responses_handler.cpp src/paid_fallback/ledger_settlement.cpp src/kernel/quota_reserve.cpp src/openai.cpp src/responses_stream.cpp
//        src/codex/banked_reset.cpp src/paid_fallback/health.cpp src/inference_deadline.cpp src/responses_request.cpp src/provider/removed_provider_circuit.cpp src/embeddings/handlers.cpp src/sentinel/replay_limits.cpp src/admin/sentinel_incident.cpp src/paid_fallback/index.cpp src/catalog/promotion.cpp src/request_policy.cpp
//        src/admin/error_log.cpp src/paid_fallback/removed_provider.cpp src/codex/banked_reset_pool.cpp src/responses_native_headers.cpp src/provider/removed_provider_telemetry.cpp src/embeddings/ledger.cpp src/provider/anthropic_responses.cpp src/codex/supervisor_brief.cpp src/admin/chart_source_proxy.cpp
//        src/authority/native_kernel_contracts.cpp src/handler/http.cpp src/live/handler.cpp src/deepseek/flash_aliases.cpp src/provider/openrouter_handlers.cpp src/sentinel/replay_retention_bootstrap.cpp src/storage/kv.cpp src/provider/claude_code_usage_schema.cpp src/bounded_response_body.cpp src/live/sideband.cpp
//        src/storage/sqlite_readonly.cpp src/codex/responses_operations.cpp src/provider/lithos_handlers.cpp src/provider/claude_capacity_rollups.cpp src/chat/envelope.cpp src/provider/lithos_rate_limits.cpp src/codex/rate_limit_429.cpp src/sentinel/replay_retention_metadata.cpp src/provider/surplus_claude_chat_request.cpp
//        src/codex/banked_reset_submission.cpp src/provider/claude_capacity_history.cpp src/provider/claude_code.cpp src/embeddings/jobs.cpp src/chat/stream_translation.cpp src/deepseek/responses_payload.cpp src/upstream_wire.cpp src/admin/capacity_observation_sync.cpp src/codex/banked_reset_claim.cpp src/decisions/handlers.cpp
//        src/deepseek/responses.cpp src/systemone/handlers.cpp src/embeddings/voyage.cpp src/provider/cerebras_responses.cpp src/provider/lithos_streams.cpp src/codex/supervisor_secret.cpp src/deepseek/index.cpp src/auth/relay.cpp src/live/upstream.cpp src/responses_attempts.cpp src/provider/cerebras_handlers.cpp src/handler/admission.cpp
//        src/auth/local_admin.cpp src/responses_failover_stream.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_response_wire.cpp src/deepseek/waterfall_handler.cpp src/provider/stream_relay.cpp src/cache/scope_targets.cpp src/deepseek/waterfall.cpp src/paid_fallback/routing.cpp src/codex/supervisor_inventory.cpp
//        src/codex/responses_runners.cpp src/deepseek/handlers.cpp src/openai_schema.cpp src/codex/responses_state.cpp src/codex/quota.cpp src/live/calls.cpp src/provider/openrouter_streams.cpp src/provider/openrouter_request_controls.cpp src/storage/native_kv.cpp src/runtime/http_tasks.cpp src/chat/completions.cpp
//        src/codex/supervisor_transport.cpp src/provider/anthropic_chat_stream.cpp src/deepseek/forwarded_payload_policy.cpp src/lib/jev_compaction/state.cpp src/inference_admission.cpp src/codex/supervisor_log.cpp src/deepseek/context_admission_failure.cpp src/live/routes.cpp src/responses_buffered.cpp src/recent_model_context.cpp
//        src/lib/jev_compaction/codex_items.cpp src/codex/native_auth_transport.cpp src/authority/native_codex_429.cpp src/deepseek/chat_projection.cpp src/codex/responses_error.cpp src/paid_fallback/claude_code_route.cpp src/deepseek/responses_stream.cpp src/deepseek/stream.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/types.cpp
//        src/runtime/http_early_sse.cpp src/provider/openrouter_web_tools.cpp src/runtime/websocket.cpp src/lib/jev_compaction/request.cpp
// Port of tests/sentinel-upstream-providers.test.ts: the nine recorded-transport cases, seven direct provider/
// Codex cases, and the current-source assertions of the historical framing case. The historical TS archive
// subprocess comparison is deferred; it needs its original TypeScript runtime and sandboxed historical harness.
// Both original final-handler capture/export and trusted recorded-replay cases are included below.
// REPLAY_ROUTES uses the real provider constants; the quota-denial case also covers both OpenRouter text wires.
#include "uos/api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/handler/index.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/runtime_config.hpp"
#include "uos/cache/prompt_analytics_queue.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/index.hpp"
#include "uos/config.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/kv.hpp"
#include "uos/utils.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/claude_code.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/sentinel/replay_capture.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/string.hpp"
#include <deque>
#include <exception>
#include <memory>
#include <type_traits>
#include "tests/helpers/sentinel_recorded_upstream.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::sentinel::upstream_capture;
  using namespace uos::tests::helpers::sentinel_recorded_upstream;

  // assert.throws(fn, /pattern/): node:assert matches the pattern against String(error).

  std::string showSnapshot(const RecordedUpstreamReplaySnapshot& snapshot) {
    return "{ attemptsDispatched: " + std::to_string(snapshot.attemptsDispatched) + ", attemptsCompleted: " + std::to_string(snapshot.attemptsCompleted) +
           ", chunksConsumed: " + std::to_string(snapshot.chunksConsumed) + ", chunksTotal: " + std::to_string(snapshot.chunksTotal) +
           ", failed: " + (snapshot.failed ? "true" : "false") + " }";
  }

  void expectSnapshot(const RecordedUpstreamReplaySnapshot& actual,
                      std::int64_t attemptsDispatched,
                      std::int64_t attemptsCompleted,
                      std::int64_t chunksConsumed,
                      std::int64_t chunksTotal,
                      bool failed,
                      int line) {
    RecordedUpstreamReplaySnapshot expected;
    expected.attemptsDispatched = attemptsDispatched;
    expected.attemptsCompleted = attemptsCompleted;
    expected.chunksConsumed = chunksConsumed;
    expected.chunksTotal = chunksTotal;
    expected.failed = failed;
    if (!(actual == expected)) rt::test::report(__FILE__, line, "snapshot\n      actual:   " + showSnapshot(actual) + "\n      expected: " + showSnapshot(expected), false);
  }
#define EXPECT_SNAPSHOT(actual, dispatched, completed, consumed, total, failed) expectSnapshot((actual), dispatched, completed, consumed, total, failed, __LINE__)

  /**
   * Trusted fixture route map: distinct exact HTTPS endpoint strings per provider
   * enum. Only synthetic public test data is committed; nothing here is a secret.
   */
  json::object REPLAY_ROUTES() {
    return json::object{
      {"chatgpt_codex", uos::config::config.codexBaseUrl + "/responses"},
      {"claude_code", std::string(uos::provider::claude_code::CLAUDE_CODE_BASE_URL) + "/v1/messages"},
      {"surplus", std::string(uos::provider::surplus::SURPLUS_BASE_URL) + "/v1/responses"},
      {"metered", std::string(uos::provider::metered::METERED_BASE_URL) + "/v1/responses"},
      {"cerebras", std::string(uos::provider::cerebras::CEREBRAS_CHAT_COMPLETIONS_URL)},
      {"deepseek", std::string(uos::deepseek::index::DEEPSEEK_CHAT_COMPLETIONS_URL)},
      {"lithos", std::string(uos::provider::lithos::LITHOS_CHAT_COMPLETIONS_URL)},
      {"openrouter", std::string(uos::provider::openrouter::OPENROUTER_CHAT_COMPLETIONS_URL)},
    };
  }

  std::string route(std::string_view provider) { return std::string(rt::getString(REPLAY_ROUTES(), provider).value()); }

  SentinelUpstreamAttempt replayAttempt(std::string provider,
                                        std::optional<std::int64_t> status,
                                        std::optional<std::string> contentType,
                                        std::string terminal,
                                        const std::vector<std::string>& chunksText = {}) {
    SentinelUpstreamAttempt attempt;
    attempt.provider = std::move(provider);
    attempt.status = status;
    attempt.content_type = std::move(contentType);
    for (const std::string& text : chunksText) attempt.chunks_base64.push_back(rt::btoa(text));
    attempt.terminal = std::move(terminal);
    return attempt;
  }

  struct ReplayFlags {
    bool attempts_truncated = false;
    bool bytes_truncated = false;
    bool chunks_truncated = false;
  };

  SentinelUpstreamTrace replayTrace(std::vector<SentinelUpstreamAttempt> attempts, ReplayFlags flags = {}) {
    SentinelUpstreamTrace trace;
    trace.version = 1;
    trace.attempts = std::move(attempts);
    trace.attempts_truncated = flags.attempts_truncated;
    trace.bytes_truncated = flags.bytes_truncated;
    trace.chunks_truncated = flags.chunks_truncated;
    return trace;
  }

  ReplayFlags truncatedFlag(int which) {
    ReplayFlags flags;
    if (which == 0) flags.attempts_truncated = true;
    if (which == 1) flags.bytes_truncated = true;
    if (which == 2) flags.chunks_truncated = true;
    return flags;
  }

  rt::ReadableStreamDefaultReader<rt::ByteChunk> bodyReader(const rt::Response& response) {
    std::optional<rt::ByteStream> body = response.body();
    ASSERT(body.has_value());  // the recorded replay must expose a body stream
    return body->getReader();
  }
}

ASYNC_TEST("recorded upstream replay preserves exact chunk order and boundaries at EOF") {
  const rt::Fetch originalFetch = rt::globalFetch();
  const SentinelUpstreamTrace inputTrace = replayTrace({replayAttempt("metered", 200, "application/json", "eof", {"abc", "def", "ghi"})});
  const std::string inputBefore = rt::jsonStringify(json::value_from(inputTrace));
  const RecordedUpstreamReplay replay = createRecordedUpstreamReplay(inputTrace, REPLAY_ROUTES());
  auto call = replay.fetch(route("metered"));
  const rt::Response response = co_await std::move(call);
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("content-type"), std::optional<std::string>("application/json"));
  const auto reader = bodyReader(response);
  std::vector<std::string> chunks;
  for (;;) {
    const auto step = co_await reader.read();
    if (step.done) break;
    chunks.push_back(rt::decodeUtf8(*step.value));
  }
  EXPECT_EQ(chunks, std::vector<std::string>({"abc", "def", "ghi"}));  // each original chunk must be a separate exact boundary
  replay.assertComplete();
  EXPECT_SNAPSHOT(replay.snapshot(), 1, 1, 3, 3, false);
  EXPECT_EQ(rt::jsonStringify(json::value_from(inputTrace)), inputBefore);  // the input trace must stay unchanged
  EXPECT(rt::globalFetch() == originalFetch);                                // the helper must never replace or invoke global fetch
}

ASYNC_TEST("recorded upstream read_error delivers the prefix then errors on the next pull") {
  const RecordedUpstreamReplay replay = createRecordedUpstreamReplay(replayTrace({replayAttempt("surplus", 200, "text/event-stream", "read_error", {"x"})}), REPLAY_ROUTES());
  auto call = replay.fetch(route("surplus"));
  const rt::Response response = co_await std::move(call);
  EXPECT_EQ(response.headers().get("content-type"), std::optional<std::string>("text/event-stream"));
  const auto reader = bodyReader(response);
  const auto first = co_await reader.read();
  EXPECT_EQ(first.done, false);
  EXPECT_EQ(rt::decodeUtf8(first.value.value()), "x");
  EXPECT_REJECTS(reader.read(), rt::TypeError);
  replay.assertComplete();
  EXPECT_SNAPSHOT(replay.snapshot(), 1, 1, 1, 1, false);
}

ASYNC_TEST("recorded upstream fetch_error rejects with no headers or chunks and completes the attempt") {
  const RecordedUpstreamReplay replay = createRecordedUpstreamReplay(replayTrace({replayAttempt("cerebras", std::nullopt, std::nullopt, "fetch_error")}), REPLAY_ROUTES());
  auto call = replay.fetch(route("cerebras"));
  EXPECT_REJECTS(std::move(call), rt::TypeError);
  replay.assertComplete();
  EXPECT_SNAPSHOT(replay.snapshot(), 1, 1, 0, 0, false);
}

ASYNC_TEST("recorded upstream cancelled completes only when the consumer cancels after the full prefix") {
  const RecordedUpstreamReplay replay =
    createRecordedUpstreamReplay(replayTrace({replayAttempt("surplus", 200, "text/event-stream", "cancelled", {"data:", "x\n"})}), REPLAY_ROUTES());
  auto call = replay.fetch(route("surplus"));
  const rt::Response response = co_await std::move(call);
  const auto reader = bodyReader(response);
  const auto first = co_await reader.read();
  EXPECT_EQ(rt::decodeUtf8(first.value.value()), "data:");
  const auto second = co_await reader.read();
  EXPECT_EQ(rt::decodeUtf8(second.value.value()), "x\n");
  EXPECT_SNAPSHOT(replay.snapshot(), 1, 0, 2, 2, false);
  co_await reader.cancel();
  replay.assertComplete();
  EXPECT_SNAPSHOT(replay.snapshot(), 1, 1, 2, 2, false);
}

ASYNC_TEST("recorded upstream cancelled prefix rejects when read beyond it, never inventing EOF") {
  const RecordedUpstreamReplay replay = createRecordedUpstreamReplay(replayTrace({replayAttempt("surplus", 200, "text/event-stream", "cancelled", {"data:"})}), REPLAY_ROUTES());
  auto call = replay.fetch(route("surplus"));
  const rt::Response response = co_await std::move(call);
  const auto reader = bodyReader(response);
  const auto first = co_await reader.read();
  EXPECT_EQ(rt::decodeUtf8(first.value.value()), "data:");
  EXPECT_REJECTS(reader.read(), rt::TypeError);
  EXPECT_THROWS_MATCHING(replay.assertComplete(), "replay failed");
  EXPECT_SNAPSHOT(replay.snapshot(), 1, 0, 1, 1, true);
  auto again = replay.fetch(route("surplus"));
  EXPECT_REJECTS(std::move(again), rt::TypeError);
}

ASYNC_TEST("recorded upstream cancelled prefix refuses early consumer cancellation") {
  const RecordedUpstreamReplay replay =
    createRecordedUpstreamReplay(replayTrace({replayAttempt("surplus", 200, "text/event-stream", "cancelled", {"data:", "x\n"})}), REPLAY_ROUTES());
  auto call = replay.fetch(route("surplus"));
  const rt::Response response = co_await std::move(call);
  const auto reader = bodyReader(response);
  co_await reader.read();
  co_await reader.cancel();
  EXPECT_THROWS_MATCHING(replay.assertComplete(), "replay failed");
  EXPECT_SNAPSHOT(replay.snapshot(), 1, 0, 1, 2, true);
}

ASYNC_TEST("recorded upstream enforces exact ordered provider routes and permanent failure") {
  // Two attempts, each recording exactly one chunk: every total below is two.
  const SentinelUpstreamTrace twoAttempts = replayTrace({
    replayAttempt("chatgpt_codex", 200, "application/json", "eof", {"{}"}),
    replayAttempt("surplus", 200, "text/event-stream", "eof", {"{}"}),
  });
  // Unexpected URL entirely outside the route map.
  const RecordedUpstreamReplay unexpected = createRecordedUpstreamReplay(twoAttempts, REPLAY_ROUTES());
  auto unmatched = unexpected.fetch(std::string("https://unmatched.example/nope"));
  EXPECT_REJECTS(std::move(unmatched), rt::TypeError);
  EXPECT_THROWS_MATCHING(unexpected.assertComplete(), "replay failed");
  EXPECT_SNAPSHOT(unexpected.snapshot(), 0, 0, 0, 2, true);
  // Wrong provider route while codex is the next recorded attempt.
  const RecordedUpstreamReplay wrongProvider = createRecordedUpstreamReplay(twoAttempts, REPLAY_ROUTES());
  auto wrongRoute = wrongProvider.fetch(route("surplus"));
  EXPECT_REJECTS(std::move(wrongRoute), rt::TypeError);
  EXPECT_SNAPSHOT(wrongProvider.snapshot(), 0, 0, 0, 2, true);
  // Order mismatch: the second codex call cannot satisfy the surplus attempt.
  const RecordedUpstreamReplay wrongOrder = createRecordedUpstreamReplay(twoAttempts, REPLAY_ROUTES());
  auto firstCall = wrongOrder.fetch(route("chatgpt_codex"));
  const rt::Response first = co_await std::move(firstCall);
  const auto firstReader = bodyReader(first);
  co_await firstReader.read();
  co_await firstReader.read();
  auto secondCall = wrongOrder.fetch(route("chatgpt_codex"));
  EXPECT_REJECTS(std::move(secondCall), rt::TypeError);
  EXPECT_SNAPSHOT(wrongOrder.snapshot(), 1, 1, 1, 2, true);
  // Dispatched but unconsumed bodies are not completion.
  const RecordedUpstreamReplay unconsumed = createRecordedUpstreamReplay(twoAttempts, REPLAY_ROUTES());
  auto codexCall = unconsumed.fetch(route("chatgpt_codex"));
  co_await std::move(codexCall);
  auto surplusCall = unconsumed.fetch(route("surplus"));
  co_await std::move(surplusCall);
  EXPECT_SNAPSHOT(unconsumed.snapshot(), 2, 0, 0, 2, false);
  EXPECT_THROWS_MATCHING(unconsumed.assertComplete(), "replay is incomplete");
  // Extra dispatch after full completion is a permanent failure.
  const RecordedUpstreamReplay extra = createRecordedUpstreamReplay(twoAttempts, REPLAY_ROUTES());
  auto extraCodexCall = extra.fetch(route("chatgpt_codex"));
  const rt::Response extraCodex = co_await std::move(extraCodexCall);
  co_await extraCodex.text();
  auto extraSurplusCall = extra.fetch(route("surplus"));
  const rt::Response extraSurplus = co_await std::move(extraSurplusCall);
  co_await extraSurplus.text();
  extra.assertComplete();
  auto extraCall = extra.fetch(route("chatgpt_codex"));
  EXPECT_REJECTS(std::move(extraCall), rt::TypeError);
  EXPECT_THROWS_MATCHING(extra.assertComplete(), "replay failed");
  EXPECT_SNAPSHOT(extra.snapshot(), 2, 2, 2, 2, true);
}

TEST("recorded upstream refuses unavailable or non-reproducible replay evidence at creation") {
  const SentinelUpstreamAttempt usable = replayAttempt("metered", 200, "application/json", "eof", {"{}"});
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({}), REPLAY_ROUTES()), "trace has no attempts");
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({usable}, truncatedFlag(0)), REPLAY_ROUTES()), "trace is truncated");
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({usable}, truncatedFlag(1)), REPLAY_ROUTES()), "trace is truncated");
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({usable}, truncatedFlag(2)), REPLAY_ROUTES()), "trace is truncated");
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({replayAttempt("metered", 200, "application/json", "pending")}), REPLAY_ROUTES()), "attempt is pending");
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({replayAttempt("metered", std::nullopt, std::nullopt, "pending")}), REPLAY_ROUTES()), "attempt is pending");
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({replayAttempt("metered", 200, "other", "eof", {"{}"})}), REPLAY_ROUTES()), "MIME");
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({replayAttempt("metered", 101, "text/event-stream", "eof")}), REPLAY_ROUTES()), "informational");
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({replayAttempt("metered", 204, "application/json", "eof", {"{}"})}), REPLAY_ROUTES()), "bodyless");
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({replayAttempt("metered", 204, "application/json", "read_error")}), REPLAY_ROUTES()), "bodyless");
  json::object ambiguous = REPLAY_ROUTES();
  rt::objectSet(ambiguous, "surplus", json::value(route("chatgpt_codex")));
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({usable}), ambiguous), "not unique");
  json::object insecure = REPLAY_ROUTES();
  rt::objectSet(insecure, "metered", json::value("http://replay.fixture.example/metered"));
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({usable}), insecure), "exact HTTPS endpoint");
  json::object credential = REPLAY_ROUTES();
  rt::objectSet(credential, "surplus", json::value("https://user:pass@replay.fixture.example/surplus"));
  EXPECT_THROWS_MATCHING(createRecordedUpstreamReplay(replayTrace({usable}), credential), "exact HTTPS endpoint");
}

ASYNC_TEST("recorded upstream bodyless 204/205/304 replay as null body completed at dispatch") {
  const std::array<std::int64_t, 3> statuses{204, 205, 304};
  for (const std::int64_t status : statuses) {
    const RecordedUpstreamReplay replay = createRecordedUpstreamReplay(replayTrace({replayAttempt("metered", status, "other", "eof")}), REPLAY_ROUTES());
    auto call = replay.fetch(route("metered"));
    const rt::Response response = co_await std::move(call);
    EXPECT_EQ(static_cast<std::int64_t>(response.status()), status);
    EXPECT_EQ(response.body().has_value(), false);
    replay.assertComplete();
    // The bodyless fixture records no chunks: nothing is pulled or stored.
    EXPECT_SNAPSHOT(replay.snapshot(), 1, 1, 0, 0, false);
  }
}


namespace {
  namespace surplus = uos::provider::surplus;
  namespace metered = uos::provider::metered;
  namespace cerebras = uos::provider::cerebras;
  namespace deepseek = uos::deepseek::index;
  namespace codex = uos::codex::index;
  namespace auth = uos::codex::auth;
  namespace capacity = uos::codex::capacity_routing;
  namespace health = uos::provider::health;
  namespace capture = uos::sentinel::replay_capture;
  using uos::api_key_policy::ApiKeyProviderDispatch;
  using uos::api_key_policy::ApiKeyQuotaDispatchError;

  // All credentials are synthetic; imported auth code cannot discover the host's native auth or provider keys.
  struct CredentialIsolation {
    std::string fixtureHome = rt::env::get("HOME").value_or("/tmp/claude-1002/wave/w10c1/t3_sentinel/empty-home");
    std::string fixtureTmp = rt::env::get("TMPDIR").value_or("/tmp/claude-1002/wave/w10c1/t3_sentinel");
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", fixtureHome}, {"TMPDIR", fixtureTmp}}};
    std::string seed = uos::config::config.codexAuthJsonB64;
    CredentialIsolation() { uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialIsolation() { uos::config::config.codexAuthJsonB64 = seed; }
  };

  std::string decodeChunks(const SentinelUpstreamTrace& trace) {
    std::string text;
    for (const SentinelUpstreamAttempt& attempt : trace.attempts) {
      for (const std::string& chunk : attempt.chunks_base64) text += rt::decodeUtf8(uos::utils::base64UrlDecode(chunk));
    }
    return text;
  }

  rt::Fetch failingFetcher(std::string message) {
    return rt::Fetch([message = std::move(message)](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
      throw rt::TypeError(message);
      co_return rt::Response();
    });
  }

  rt::Fetch fixedFetcher(std::string body, rt::Headers headers) {
    return rt::Fetch([body = std::move(body), headers = std::move(headers)](rt::RequestInfo, rt::RequestInit) {
      rt::ResponseInit init;
      init.status = 200;
      init.headers = headers;
      return rt::Response(rt::BodyInit(body), init);
    });
  }

  void expectAttempt(const SentinelUpstreamTrace& trace, const std::string& provider, const std::string& mime, const std::string& terminal) {
    ASSERT_EQ(trace.attempts.size(), std::size_t{1});
    const SentinelUpstreamAttempt& attempt = trace.attempts[0];
    EXPECT_EQ(attempt.provider, provider);
    EXPECT_EQ(attempt.status, std::optional<std::int64_t>(200));
    EXPECT_EQ(attempt.content_type, std::optional<std::string>(mime));
    EXPECT_EQ(attempt.terminal, terminal);
  }

  std::string syntheticJwt(const std::string& label, double nowMs) {
    const std::string header = rt::toBase64Url(rt::encodeUtf8(R"({"alg":"none"})"));
    const json::object payload{{"exp", rt::makeNumber((nowMs + 60 * 60'000) / 1'000)}};
    return header + "." + rt::toBase64Url(rt::encodeUtf8(rt::jsonStringify(payload))) + "." + label;
  }

  std::string sha256Hex(const rt::Bytes& bytes) { return rt::toHex(rt::crypto::subtle::digest("SHA-256", bytes)); }

  void assertClientStreamSemantics(const std::string& text) {
    if (!rt::endsWith(text, "\n\n")) throw rt::JsError("every client frame must end on its blank-line terminator");
    std::vector<std::string> events;
    std::vector<json::value> values;
    for (const std::string& frame : rt::split(text, "\n\n")) {
      if (frame.empty()) continue;
      const std::vector<std::string> lines = rt::split(frame, "\n");
      ASSERT_EQ(lines.size(), std::size_t{2});
      EXPECT(rt::startsWith(lines[0], "event: "));
      EXPECT(rt::startsWith(lines[1], "data: "));
      events.push_back(rt::slice(lines[0], 7));
      values.push_back(rt::jsonParse(rt::slice(lines[1], 6)));
    }
    EXPECT_EQ(events, std::vector<std::string>({"response.created", "response.output_text.delta", "response.output_text.done", "response.output_item.done", "response.completed"}));
    ASSERT_EQ(values.size(), std::size_t{5});
    EXPECT_EQ(rt::getString(values[1], "delta"), std::optional<std::string>("fixture text"));
    EXPECT_EQ(rt::getString(values[2], "text"), std::optional<std::string>("fixture text"));
    EXPECT_EQ(rt::getString(rt::get(rt::get(values[3], "item"), "type")), std::optional<std::string_view>("message"));
    EXPECT_EQ(rt::getString(rt::get(rt::get(values[4], "response"), "status")), std::optional<std::string_view>("completed"));
  }
}

ASYNC_TEST("actual Surplus fetch captures pre-normalization SSE bytes and stays lazy until the consumer reads") {
  const CredentialIsolation isolation;
  const SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const std::vector<std::string> rawChunks{
    "data: " + rt::jsonStringify(json::object{{"type", "response.created"}, {"response", json::object{{"id", "resp_raw"}, {"created_at", 0}}}}) + "\n\n",
    "data: " + rt::jsonStringify(json::object{{"type", "response.output_text.delta"}, {"output_index", 0}, {"content_index", 0}, {"delta", "hello"}}) + "\n\n",
    "data: " + rt::jsonStringify(json::object{{"type", "response.completed"}, {"response", json::object{{"id", "resp_raw"}, {"model", "surplus-fixture"}, {"output", json::array{}}}}}) + "\n\n",
  };
  struct PullState { std::deque<std::string> pending; std::size_t pulls = 0; };
  const auto state = std::make_shared<PullState>();
  state->pending.assign(rawChunks.begin(), rawChunks.end());
  surplus::SurplusAuthenticatedFetchOptions options;
  options.apiKey = "surplus-fixture-key";
  options.sentinelUpstreamRecorder = recorder;
  options.fetcher = rt::Fetch([state](rt::RequestInfo, rt::RequestInit) {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [state](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) -> rt::Task<void> {
      state->pulls += 1;
      if (state->pending.empty()) controller.close();
      else {
        controller.enqueue(rt::encodeUtf8(state->pending.front()));
        state->pending.pop_front();
      }
      co_return;
    };
    const rt::ByteStream stream(source, {.highWaterMark = 0});
    const rt::ResponseInit init{.status = 200, .headers = rt::Headers{{"Content-Type", "text/event-stream"}, {"X-Request-Id", "surplus-raw-1"}}};
    return rt::Response(rt::BodyInit(stream), init);
  });
  const json::value body = json::object{{"model", "surplus-fixture"}, {"input", "hello"}};
  auto fetching = surplus::fetchSurplusResponses(body, options);
  const surplus::SurplusResponsesResult result = co_await std::move(fetching);
  EXPECT_EQ(result.request_id, std::optional<std::string>("surplus-raw-1"));
  EXPECT_EQ(result.response.status(), 200);
  EXPECT(state->pulls <= 1);
  auto reading = result.response.text();
  const std::string normalized = co_await std::move(reading);
  EXPECT_EQ(state->pulls, rawChunks.size());
  const SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
  expectAttempt(trace, "surplus", "text/event-stream", "cancelled");
  EXPECT_EQ(decodeChunks(trace), rt::join(rawChunks, ""));
  EXPECT(rt::startsWith(decodeChunks(trace), "data: "));
  EXPECT(rt::startsWith(normalized, "event: response.created"));
  EXPECT(rt::includes(normalized, "output_text.done"));
  recorder.dispose();
}

ASYNC_TEST("actual Surplus no-header fetch failure records fetch_error without inventing headers") {
  const CredentialIsolation isolation;
  const SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  surplus::SurplusAuthenticatedFetchOptions options;
  options.apiKey = "surplus-fixture-key";
  options.fetcher = failingFetcher("surplus socket closed");
  options.sentinelUpstreamRecorder = recorder;
  const json::value body = json::object{{"model", "surplus-fixture"}, {"input", "hello"}};
  std::optional<std::string> failureCode;
  try {
    auto fetching = surplus::fetchSurplusResponses(body, options);
    co_await std::move(fetching);
  } catch (const surplus::SurplusError& error) { failureCode = error.code; }
  EXPECT_EQ(failureCode, std::optional<std::string>("surplus_upstream_unreachable"));
  const SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
  ASSERT_EQ(trace.attempts.size(), std::size_t{1});
  EXPECT_EQ(trace.attempts[0].status, std::nullopt);
  EXPECT_EQ(trace.attempts[0].content_type, std::nullopt);
  EXPECT_EQ(trace.attempts[0].terminal, "fetch_error");
  EXPECT_EQ(trace.attempts[0].chunks_base64.size(), std::size_t{0});
  recorder.dispose();
}

ASYNC_TEST("quota denial never opens a dispatch slot on any paid provider") {
  const CredentialIsolation isolation;
  uos::api_key_policy::ApiKeyQuotaDispatchErrorOptions errorOptions;
  errorOptions.status = 429;
  errorOptions.code = "quota_denied";
  errorOptions.errorType = "server_error";
  const rt::Reason quota = rt::makeReason(ApiKeyQuotaDispatchError("quota denied", errorOptions));
  const auto beforeDispatch = [quota] { return rt::Promise<std::optional<ApiKeyProviderDispatch>>::reject(quota).wait(); };
  std::size_t dispatchAttempts = 0;
  const rt::Fetch fetcher([&dispatchAttempts](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
    dispatchAttempts += 1;
    throw rt::JsError("must not dispatch");
    co_return rt::Response();
  });
  const json::value responseBody = json::object{{"model", "surplus-fixture"}, {"input", "hello"}};
  const SentinelUpstreamRecorder surplusRecorder = createSentinelUpstreamRecorder();
  surplus::SurplusAuthenticatedFetchOptions surplusOptions;
  surplusOptions.apiKey = "surplus-fixture-key";
  surplusOptions.beforeDispatch = beforeDispatch;
  surplusOptions.fetcher = fetcher;
  surplusOptions.sentinelUpstreamRecorder = surplusRecorder;
  auto surplusFetching = surplus::fetchSurplusResponses(responseBody, surplusOptions);
  EXPECT_REJECTS(std::move(surplusFetching), ApiKeyQuotaDispatchError);
  EXPECT_EQ(surplusRecorder.snapshotAndSeal().attempts.size(), std::size_t{0});
  const SentinelUpstreamRecorder meteredRecorder = createSentinelUpstreamRecorder();
  metered::MeteredAuthenticatedFetchOptions meteredOptions;
  meteredOptions.apiKey = "metered-fixture-key";
  meteredOptions.beforeDispatch = beforeDispatch;
  meteredOptions.fetcher = fetcher;
  meteredOptions.sentinelUpstreamRecorder = meteredRecorder;
  const json::value meteredBody = json::object{{"model", "metered-fixture"}, {"input", "hello"}};
  auto meteredFetching = metered::fetchMeteredResponses(meteredBody, meteredOptions);
  EXPECT_REJECTS(std::move(meteredFetching), ApiKeyQuotaDispatchError);
  EXPECT_EQ(meteredRecorder.snapshotAndSeal().attempts.size(), std::size_t{0});
  const SentinelUpstreamRecorder cerebrasRecorder = createSentinelUpstreamRecorder();
  cerebras::CerebrasChatCompletionsOptions cerebrasOptions;
  cerebrasOptions.apiKey = "cerebras-fixture-key";
  cerebrasOptions.beforeDispatch = beforeDispatch;
  cerebrasOptions.fetcher = fetcher;
  cerebrasOptions.sentinelUpstreamRecorder = cerebrasRecorder;
  const json::object chatBody{{"model", "gpt-oss-120b"}, {"messages", json::array{json::object{{"role", "user"}, {"content", "hello"}}}}};
  auto cerebrasFetching = cerebras::fetchCerebrasChatCompletions(chatBody, cerebrasOptions);
  EXPECT_REJECTS(std::move(cerebrasFetching), ApiKeyQuotaDispatchError);
  EXPECT_EQ(cerebrasRecorder.snapshotAndSeal().attempts.size(), std::size_t{0});
  rt::env::set("OPENROUTER_API_KEY", "openrouter-fixture-key");
  const rt::GlobalFetchOverride replaceFetch(fetcher);
  for (int wire = 0; wire < 2; ++wire) {
    const SentinelUpstreamRecorder openRouterRecorder = createSentinelUpstreamRecorder();
    uos::provider::openrouter::OpenRouterDispatchHooks hooks;
    hooks.beforeDispatch = beforeDispatch;
    hooks.sentinelUpstreamRecorder = openRouterRecorder;
    const uos::provider::openrouter::OpenRouterDispatchInit init{rt::AbortController().signal(), hooks};
    auto fetching = wire == 0 ? uos::provider::openrouter::fetchOpenRouterChatCompletions(chatBody, init)
      : uos::provider::openrouter::fetchOpenRouterResponses(chatBody, init);
    EXPECT_REJECTS(std::move(fetching), ApiKeyQuotaDispatchError);
    EXPECT_EQ(openRouterRecorder.snapshotAndSeal().attempts.size(), std::size_t{0});
    openRouterRecorder.dispose();
  }
  EXPECT_EQ(dispatchAttempts, std::size_t{0});
}

ASYNC_TEST("actual Metered dispatch returns the exact upstream body and bounded headers") {
  const CredentialIsolation isolation;
  const SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const std::string raw = "data: " + rt::jsonStringify(json::object{{"type", "response.created"}, {"response", json::object{{"id", "resp_metered"}, {"created_at", 0}}}}) + "\n\n" +
    "data: " + rt::jsonStringify(json::object{{"type", "response.completed"}, {"response", json::object{{"id", "resp_metered"}}}}) + "\n\n";
  metered::MeteredAuthenticatedFetchOptions options;
  options.apiKey = "metered-fixture-key";
  options.fetcher = fixedFetcher(raw, rt::Headers{{"Content-Type", "text/event-stream"}, {"X-Oneapi-Request-Id", "metered-id-1"}});
  options.sentinelUpstreamRecorder = recorder;
  const json::value body = json::object{{"model", "metered-fixture"}, {"input", "hello"}};
  auto fetching = metered::fetchMeteredResponses(body, options);
  const metered::MeteredResponsesResult result = co_await std::move(fetching);
  EXPECT_EQ(result.request_id, std::optional<std::string>("metered-id-1"));
  EXPECT_EQ(result.response.status(), 200);
  auto reading = result.response.text();
  const std::string text = co_await std::move(reading);
  EXPECT_EQ(text, raw);
  expectAttempt(recorder.snapshotAndSeal(), "metered", "text/event-stream", "eof");
  recorder.dispose();
  const SentinelUpstreamRecorder noHeader = createSentinelUpstreamRecorder();
  options.fetcher = failingFetcher("metered socket closed");
  options.sentinelUpstreamRecorder = noHeader;
  auto failing = metered::fetchMeteredResponses(body, options);
  EXPECT_REJECTS(std::move(failing), metered::MeteredError);
  const SentinelUpstreamTrace failed = noHeader.snapshotAndSeal();
  ASSERT_EQ(failed.attempts.size(), std::size_t{1});
  EXPECT_EQ(failed.attempts[0].terminal, "fetch_error");
  noHeader.dispose();
}

ASYNC_TEST("actual Cerebras dispatch returns the exact upstream body and records the attempt") {
  const CredentialIsolation isolation;
  const SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const std::string raw = rt::jsonStringify(json::object{{"id", "cerebras-1"}, {"object", "chat.completion"}, {"choices", json::array{json::object{{"index", 0}}}}});
  cerebras::CerebrasChatCompletionsOptions options;
  options.apiKey = "cerebras-fixture-key";
  options.fetcher = fixedFetcher(raw, rt::Headers{{"Content-Type", "application/json"}});
  options.sentinelUpstreamRecorder = recorder;
  const json::object body{{"model", "gpt-oss-120b"}, {"messages", json::array{json::object{{"role", "user"}, {"content", "hello"}}}}};
  auto fetching = cerebras::fetchCerebrasChatCompletions(body, options);
  const rt::Response response = co_await std::move(fetching);
  EXPECT_EQ(response.status(), 200);
  auto reading = response.text();
  const std::string text = co_await std::move(reading);
  EXPECT_EQ(text, raw);
  expectAttempt(recorder.snapshotAndSeal(), "cerebras", "application/json", "eof");
  recorder.dispose();
  const SentinelUpstreamRecorder noHeader = createSentinelUpstreamRecorder();
  options.fetcher = failingFetcher("cerebras socket closed");
  options.sentinelUpstreamRecorder = noHeader;
  auto failing = cerebras::fetchCerebrasChatCompletions(body, options);
  EXPECT_REJECTS(std::move(failing), cerebras::CerebrasError);
  const SentinelUpstreamTrace failed = noHeader.snapshotAndSeal();
  ASSERT_EQ(failed.attempts.size(), std::size_t{1});
  EXPECT_EQ(failed.attempts[0].terminal, "fetch_error");
  noHeader.dispose();
}

ASYNC_TEST("actual DeepSeek dispatch returns the exact upstream body and records the attempt") {
  const CredentialIsolation isolation;
  const SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const std::string raw = rt::jsonStringify(json::object{{"id", "deepseek-1"}, {"object", "chat.completion"}, {"choices", json::array{json::object{{"index", 0}}}}});
  std::optional<json::value> dispatchedBody;
  deepseek::DeepSeekChatCompletionsOptions options;
  options.apiKey = "deepseek-fixture-key";
  options.sentinelUpstreamRecorder = recorder;
  options.fetcher = rt::Fetch([&dispatchedBody, raw](rt::RequestInfo input, rt::RequestInit init) {
    const std::string url = std::visit([](const auto& value) -> std::string {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::string>) return value;
      else if constexpr (std::is_same_v<T, rt::URL>) return value.href();
      else return value.url();
    }, input);
    EXPECT_EQ(url, std::string(deepseek::DEEPSEEK_CHAT_COMPLETIONS_URL));
    ASSERT(init.body.has_value());
    const std::string* text = std::get_if<std::string>(&*init.body);
    if (!text) throw rt::JsError("DeepSeek sentinel fixture expected a string request body");
    dispatchedBody = rt::jsonParse(*text);
    const rt::ResponseInit responseInit{.status = 200, .headers = rt::Headers{{"Content-Type", "application/json"}}};
    return rt::Response(rt::BodyInit(raw), responseInit);
  });
  const json::object body{{"model", "deepseek-v4-flash"}, {"messages", json::array{json::object{{"role", "user"}, {"content", "hello"}}}}, {"max_completion_tokens", 512}};
  const std::string model = "deepseek-v4-flash";
  auto fetching = deepseek::fetchDeepSeekChatCompletions(body, model, options);
  const rt::Response response = co_await std::move(fetching);
  EXPECT_EQ(response.status(), 200);
  auto reading = response.text();
  const std::string text = co_await std::move(reading);
  EXPECT_EQ(text, raw);
  const json::value expected = json::object{{"model", "deepseek-flash"}, {"messages", json::array{json::object{{"role", "user"}, {"content", "hello"}}}}, {"max_tokens", 512}};
  ASSERT(dispatchedBody.has_value());
  EXPECT(rt::deepEqual(*dispatchedBody, expected));
  expectAttempt(recorder.snapshotAndSeal(), "deepseek", "application/json", "eof");
  recorder.dispose();
  const SentinelUpstreamRecorder noHeader = createSentinelUpstreamRecorder();
  options.fetcher = failingFetcher("deepseek socket closed");
  options.sentinelUpstreamRecorder = noHeader;
  const json::object failureBody{{"model", "deepseek-flash"}, {"messages", json::array{json::object{{"role", "user"}, {"content", "hello"}}}}};
  const std::string failureModel = "deepseek-flash";
  auto failing = deepseek::fetchDeepSeekChatCompletions(failureBody, failureModel, options);
  EXPECT_REJECTS(std::move(failing), deepseek::DeepSeekError);
  const SentinelUpstreamTrace failed = noHeader.snapshotAndSeal();
  ASSERT_EQ(failed.attempts.size(), std::size_t{1});
  EXPECT_EQ(failed.attempts[0].terminal, "fetch_error");
  noHeader.dispose();
}

ASYNC_TEST("actual Codex intermediate retries each count as an attempt and the wrapper is the WeakMap identity") {
  const CredentialIsolation isolation;
  const auto store = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  uos::kv::setKvForTest(store);
  auto cleanup = rt::scopeExit([store] {
    auth::resetCodexAuthCacheForTest();
    capacity::resetCodexAccountRoutingForTest();
    health::resetProviderHealthThrottleForTest();
    store->close();
    uos::kv::setKvForTest(nullptr);
  });
  const double nowMs = 1'700'000'000'000;
  rt::globalDateNow() = [nowMs] { return nowMs; };
  const auto authFor = [nowMs](const std::string& label) {
    return uos::types::CodexAuthState{.access_token = syntheticJwt(label, nowMs), .refresh_token = "refresh-" + label, .account_id = "account-" + label, .updated_at_ms = nowMs};
  };
  const uos::types::CodexAuthPoolState pool{.accounts = {authFor("one"), authFor("two")}, .updated_at_ms = nowMs};
  const rt::KvValue value(json::value_from(pool));
  auto seeding = store->set(auth::CODEX_AUTH_POOL_KV_KEY, value);
  co_await std::move(seeding);
  auth::resetCodexAuthCacheForTest();
  capacity::resetCodexAccountRoutingForTest();
  health::resetProviderHealthThrottleForTest();
  bool shouldQuotaFailOne = false;
  const rt::Fetch fetcher([&shouldQuotaFailOne, nowMs](rt::RequestInfo input, rt::RequestInit init) {
    const rt::Request request = std::visit([&init](const auto& argument) { return rt::Request(argument, init); }, input);
    if (request.url() == "https://auth.openai.com/oauth/token") return rt::Response(rt::BodyInit(R"({"access_token":"refreshed-one"})"), {.status = 200});
    if (shouldQuotaFailOne && request.headers().get("ChatGPT-Account-ID").value_or("") == "account-one") {
      const rt::ResponseInit responseInit{.status = 429, .headers = rt::Headers{{"Content-Type", "application/json"}, {"Retry-After", rt::Date(nowMs + 60'000).toUTCString()}}};
      return rt::Response(rt::BodyInit(R"({"error":{"type":"usage_limit_reached"}})"), responseInit);
    }
    return rt::Response(rt::BodyInit("{}"), {.status = 200});
  });
  const rt::GlobalFetchOverride replacement(fetcher);
  const json::value body = json::object{{"model", "gpt-5.6-luna"}, {"prompt_cache_key", "cache-recorder-weakmap"}, {"input", "fixture"}};
  const SentinelUpstreamRecorder firstRecorder = createSentinelUpstreamRecorder();
  uos::codex::dispatch::FetchCodexResponsesOptions options;
  options.cacheScope = "api-key:recorder-weakmap";
  options.sentinelUpstreamRecorder = firstRecorder;
  auto firstFetching = codex::fetchCodexResponses(body, options);
  const rt::Response first = co_await std::move(firstFetching);
  EXPECT_EQ(auth::getCodexResponseSlot(first), std::optional<double>(1));
  auto completing = auth::markCodexResponseCompleted(first);
  co_await std::move(completing);
  firstRecorder.dispose();
  EXPECT_EQ(firstRecorder.snapshotAndSeal().attempts.size(), std::size_t{1});
  shouldQuotaFailOne = true;
  const SentinelUpstreamRecorder remapRecorder = createSentinelUpstreamRecorder();
  options.sentinelUpstreamRecorder = remapRecorder;
  auto remapFetching = codex::fetchCodexResponses(body, options);
  const rt::Response remapped = co_await std::move(remapFetching);
  shouldQuotaFailOne = false;
  EXPECT_EQ(remapped.status(), 200);
  EXPECT_EQ(auth::getCodexResponseSlot(remapped), std::optional<double>(2));
  const auth::CodexResponseActiveTelemetry telemetry = auth::getCodexResponseActiveTelemetry(remapped);
  EXPECT_EQ(telemetry.activeGeneration, std::optional<double>(2));
  EXPECT_EQ(telemetry.activeTransitionReason, std::optional<std::string>("quota_exhausted"));
  auto reading = remapped.text();
  co_await std::move(reading);
  const SentinelUpstreamTrace trace = remapRecorder.snapshotAndSeal();
  ASSERT_EQ(trace.attempts.size(), std::size_t{2});
  EXPECT_EQ(trace.attempts[0].provider, "chatgpt_codex");
  EXPECT_EQ(trace.attempts[0].status, std::optional<std::int64_t>(429));
  EXPECT_EQ(trace.attempts[0].terminal, "eof");
  EXPECT_EQ(trace.attempts[1].provider, "chatgpt_codex");
  EXPECT_EQ(trace.attempts[1].status, std::optional<std::int64_t>(200));
  EXPECT_EQ(trace.attempts[1].terminal, "eof");
  EXPECT(!trace.attempts[0].chunks_base64.empty());
  remapRecorder.dispose();
}


namespace {
  const std::string SUPER_ADMIN_TOKEN = "sentinel-upstream-provider-super-admin";
  const std::string MARKER = "sentinel-upstream-handler-marker-71c3e59a";
  const std::string RAW_UPSTREAM_BODY = R"({"error":{"message":"Codex fixture upstream failure","type":"server_error","code":"fixture_500","param":null}})";
  const rt::Bytes CAPTURE_BODY = rt::encodeUtf8(R"({"model":"gpt-5.6-sol","stream":false,"input":"sentinel-upstream-handler-marker-71c3e59a"})");

  const json::value& field(const json::value& value, std::string_view key) {
    const auto* member = rt::get(value, key);
    if (!member) throw rt::JsError("missing synthetic response field");
    return *member;
  }

  void resetHandlerCaptureCaches() {
    auth::resetCodexAuthCacheForTest();
    capacity::resetCodexAccountRoutingForTest();
    uos::runtime_config::resetRuntimeConfigCacheForTest();
    health::resetProviderHealthThrottleForTest();
  }


  struct T3StaticAssets {
    std::vector<std::string> createdPaths;
    T3StaticAssets() {
      const auto temporary = uos::rt::env::get("TMPDIR");
      const auto directory = uos::rt::process::cwd();
      if (!temporary || !directory.starts_with(uos::rt::path::dirname(*temporary) + "/"))
        throw uos::rt::JsError("T3 static assets require the wrapper task workdir");
      const auto repository = uos::rt::path::resolve(uos::rt::path::dirname(__FILE__), "../../..");
      const std::vector<std::string> names{"static", "favicon.png", "favicon-32.png"};
      for (const auto& name : names) {
        const auto target = uos::rt::path::join(repository, name);
        const auto assetPath = uos::rt::path::join(directory, name);
        if (uos::rt::fs::existsSync(assetPath)) {
          if (uos::rt::fs::realpathSync(assetPath) != uos::rt::fs::realpathSync(target))
            throw uos::rt::JsError("T3 static asset fixture refuses an unexpected existing path");
        } else {
          uos::rt::fs::symlinkSync(target, assetPath);
          createdPaths.push_back(assetPath);
        }
      }
    }
    ~T3StaticAssets() {
      for (const auto& path : createdPaths) uos::rt::fs::rmSync(path);
    }
  };

  void prepareT3StaticAssets() {
    static T3StaticAssets assets;
  }

  struct HandlerCaptureStore {
    CredentialIsolation isolation;
    std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
    std::function<double()> originalNow = rt::globalDateNow();
    double nowMs = 1'700'000'000'000;
    HandlerCaptureStore() {
      prepareT3StaticAssets();
      uos::kv::setKvForTest(kv);
      uos::config::config.adminTokens.insert(SUPER_ADMIN_TOKEN);
      rt::globalDateNow() = [now = nowMs] { return now; };
      resetHandlerCaptureCaches();
    }
    ~HandlerCaptureStore() {
      uos::cache::prompt_analytics_queue::resetOptionalPromptCacheAnalyticsForTest();
      resetHandlerCaptureCaches();
      uos::config::config.adminTokens.erase(SUPER_ADMIN_TOKEN);
      rt::globalDateNow() = originalNow;
      kv->close();
      uos::kv::setKvForTest(nullptr);
    }
  };

  rt::Task<std::string> seedHandlerCapturePool(std::shared_ptr<rt::Kv> kv, double nowMs) {
    const std::string accessToken = syntheticJwt("handler-fixture", nowMs);
    const uos::types::CodexAuthState account{.access_token = accessToken, .refresh_token = "refresh-handler-fixture", .account_id = "account-handler-fixture", .updated_at_ms = nowMs};
    const uos::types::CodexAuthPoolState pool{.accounts = {account}, .updated_at_ms = nowMs};
    const rt::KvValue poolValue(json::value_from(pool));
    const json::object model{{"slug", "gpt-5.6-sol"}, {"default_reasoning_level", "low"}, {"supported_reasoning_levels", json::array{"none", "low", "medium", "high"}}};
    const json::object models{{"source", "chatgpt_codex"}, {"client_version", "0.100.0"}, {"updated_at_ms", nowMs}, {"models", json::array{model}}};
    const rt::KvValue runtime(json::object{{"version", 2}, {"default_model", "gpt-5.6-sol"}, {"default_reasoning_effort", "low"}, {"codex_models", models}, {"updated_at_ms", nowMs}});
    std::vector<rt::Promise<rt::KvCommitResult>> writes;
    writes.emplace_back(kv->set(auth::CODEX_AUTH_POOL_KV_KEY, poolValue));
    writes.emplace_back(kv->set(uos::runtime_config::RUNTIME_CONFIG_V2_KEY, runtime));
    const auto settled = rt::all(std::move(writes));
    co_await settled.wait();
    resetHandlerCaptureCaches();
    co_return accessToken;
  }

  rt::Request gatewayRequest(const std::string& token) {
    const rt::RequestInit init{.method = "POST", .headers = rt::Headers{{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}}, .body = CAPTURE_BODY};
    return rt::Request("https://ai.ubq.fi/v1/responses", init);
  }

  rt::Request exportRequest(double nowMs) {
    const rt::RequestInit init{.headers = rt::Headers{{"Authorization", "Bearer " + SUPER_ADMIN_TOKEN}}};
    return rt::Request("https://ai.ubq.fi/admin/sentinel/replay-captures?after_ms=0&before_ms=" + rt::numberToString(nowMs + 2'000), init);
  }

  rt::Fetch handlerFailureFetch(std::string accessToken, int& dispatches) {
    return rt::Fetch([accessToken = std::move(accessToken), &dispatches](rt::RequestInfo input, rt::RequestInit init) {
      const auto request = std::visit([&init](const auto& argument) { return rt::Request(argument, init); }, input);
      if (request.url() == "https://auth.openai.com/oauth/token") {
        const json::object payload{{"access_token", accessToken}};
        return rt::Response::json(payload);
      }
      if (request.url() == route("chatgpt_codex")) {
        ++dispatches;
        const rt::ResponseInit responseInit{.status = 500, .headers = rt::Headers{{"Content-Type", "application/json"}}};
        return rt::Response(rt::BodyInit(RAW_UPSTREAM_BODY), responseInit);
      }
      if (rt::includes(request.url(), "openai.com")) return rt::Response(rt::BodyInit(R"({"error":"fixture"})"), {.status = 400});
      throw rt::JsError("Unexpected handler test fetch");
    });
  }
  rt::Task<void> seedAuthenticatedKey(std::shared_ptr<rt::Kv> kv, std::string token) {
    const double now = rt::dateNow();
    auto hashing = uos::utils::sha256Base64Url(token);
    const std::string hash = co_await std::move(hashing);
    const std::string id = "upstream-provider-" + hash.substr(0, 12);
    json::object policyBase{{"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", 100}, {"usage_requests", 0},
      {"usage_reset_at_ms", now + 60'000}, {"window_ms", 60'000}, {"usage_quota_version", 3}, {"paid_fallback_enabled", true},
      {"paid_fallback_limit_microcredits", 1'000'000}, {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0},
      {"paid_fallback_reservation_request_id", nullptr}};
    json::object hashed{{"id", id}};
    rt::objectAssign(hashed, policyBase);
    auto writingHash = kv->set(uos::api_keys::apiKeyHashKey(hash), rt::KvValue(hashed));
    co_await std::move(writingHash);
    json::object record{{"id", id}, {"name", "sentinel upstream provider test key"}, {"prefix", token.substr(0, 12)}, {"hash", hash}, {"created_at_ms", now}};
    rt::objectAssign(record, policyBase);
    record["paid_fallback_model_ids"] = json::array{"gpt-5.6-sol"};
    record["paid_fallback_quota_per_credit"] = 500'000;
    record["paid_fallback_max_exposure_microcredits"] = json::object{{"gpt-5.6-sol", 250'000}};
    record["paid_fallback_pricing_checked_at_ms"] = now;
    auto writingRecord = kv->set(uos::api_keys::apiKeyIdKey(id), rt::KvValue(record));
    co_await std::move(writingRecord);
  }

}


ASYNC_TEST("real handler failure captures exact raw Codex upstream bytes through encrypted export and decrypt") {
  const HandlerCaptureStore fixture;
  const auto keyBytes = rt::crypto::randomBytes(32);
  const rt::env::ScopedOverride replayKey({{"SENTINEL_REPLAY_KEY", uos::utils::base64UrlEncode(keyBytes)}});
  const std::string token = uos::api_keys::generateApiKeyToken();
  auto preparing = seedHandlerCapturePool(fixture.kv, fixture.nowMs);
  const auto accessToken = co_await std::move(preparing);
  int upstreamDispatches = 0;
  const rt::GlobalFetchOverride replacement(handlerFailureFetch(accessToken, upstreamDispatches));
  auto seeding = seedAuthenticatedKey(fixture.kv, token);
  co_await std::move(seeding);
  auto handling = uos::handler::index::handler(gatewayRequest(token));
  auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 500);
  EXPECT_EQ(upstreamDispatches, 1);
  const auto responseText = co_await response.text();
  EXPECT(!rt::includes(responseText, MARKER));
  auto exporting = uos::handler::index::handler(exportRequest(rt::dateNow()));
  auto page = co_await std::move(exporting);
  EXPECT_EQ(page.status(), 200);
  const auto exported = co_await page.json();
  const auto& captures = field(exported, "data").as_array();
  ASSERT_EQ(captures.size(), std::size_t{1});
  const auto encrypted = json::value_to<uos::sentinel::replay_model::ExportedSentinelReplayCapture>(captures[0]);
  auto decrypting = uos::sentinel::replay_read::decryptExportedSentinelReplay(encrypted, keyBytes);
  const auto plaintext = co_await std::move(decrypting);
  EXPECT_EQ(plaintext.version, 3.0);
  EXPECT_EQ(plaintext.endpoint, std::string("/v1/responses"));
  EXPECT_EQ(plaintext.method, std::string("POST"));
  ASSERT(plaintext.body);
  EXPECT_EQ(*plaintext.body, CAPTURE_BODY);
  EXPECT_EQ(rt::decodeUtf8(*plaintext.body), rt::decodeUtf8(CAPTURE_BODY));
  EXPECT_EQ(plaintext.observation.status, 500.0);
  ASSERT_EQ(plaintext.upstream.attempts.size(), std::size_t{1});
  const auto& attempt = plaintext.upstream.attempts[0];
  EXPECT_EQ(attempt.provider, std::string("chatgpt_codex"));
  EXPECT_EQ(attempt.status, std::optional<std::int64_t>(500));
  EXPECT_EQ(attempt.content_type, std::optional<std::string>("application/json"));
  EXPECT_EQ(attempt.terminal, std::string("eof"));
  EXPECT_EQ(decodeChunks(plaintext.upstream), RAW_UPSTREAM_BODY);
  const auto exportedText = rt::jsonStringify(exported);
  EXPECT(!rt::includes(exportedText, "account-handler-fixture"));
  EXPECT(!rt::includes(exportedText, RAW_UPSTREAM_BODY));
}

ASYNC_TEST("real handler replays the decrypted recorded Codex raw failure through the trusted replay transport") {
  const HandlerCaptureStore fixture;
  const auto keyBytes = rt::crypto::randomBytes(32);
  const rt::env::ScopedOverride replayKey({{"SENTINEL_REPLAY_KEY", uos::utils::base64UrlEncode(keyBytes)}});
  const std::string token = uos::api_keys::generateApiKeyToken();
  auto preparing = seedHandlerCapturePool(fixture.kv, fixture.nowMs);
  const auto accessToken = co_await std::move(preparing);
  int rawDispatches = 0;
  const rt::GlobalFetchOverride replacement(handlerFailureFetch(accessToken, rawDispatches));
  auto seeding = seedAuthenticatedKey(fixture.kv, token);
  co_await std::move(seeding);
  auto handling = uos::handler::index::handler(gatewayRequest(token));
  auto originalResponse = co_await std::move(handling);
  EXPECT_EQ(originalResponse.status(), 500);
  EXPECT_EQ(rawDispatches, 1);
  const auto originalText = co_await originalResponse.text();
  EXPECT(!rt::includes(originalText, MARKER));
  auto exporting = uos::handler::index::handler(exportRequest(rt::dateNow()));
  auto page = co_await std::move(exporting);
  EXPECT_EQ(page.status(), 200);
  const auto exported = co_await page.json();
  const auto& captures = field(exported, "data").as_array();
  ASSERT_EQ(captures.size(), std::size_t{1});
  const auto encrypted = json::value_to<uos::sentinel::replay_model::ExportedSentinelReplayCapture>(captures[0]);
  auto decrypting = uos::sentinel::replay_read::decryptExportedSentinelReplay(encrypted, keyBytes);
  const auto plaintext = co_await std::move(decrypting);
  EXPECT_EQ(plaintext.endpoint, std::string("/v1/responses"));
  EXPECT_EQ(plaintext.method, std::string("POST"));
  ASSERT(plaintext.body);
  EXPECT_EQ(*plaintext.body, CAPTURE_BODY);
  const auto replay = createRecordedUpstreamReplay(plaintext.upstream, REPLAY_ROUTES());
  const auto upstreamBefore = rt::jsonStringify(json::value_from(plaintext.upstream));
  resetHandlerCaptureCaches();
  int replayedDispatches = 0;
  const rt::Fetch replayFetch([accessToken, replay, &replayedDispatches](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
    const auto request = std::visit([&init](const auto& argument) { return rt::Request(argument, init); }, input);
    if (request.url() == "https://auth.openai.com/oauth/token") {
      const json::object payload{{"access_token", accessToken}};
      co_return rt::Response::json(payload);
    }
    ++replayedDispatches;
    auto dispatching = replay.fetch(std::move(input), std::move(init));
    auto response = co_await std::move(dispatching);
    co_return response;
  });
  rt::setGlobalFetch(replayFetch);
  rt::Headers headers;
  for (const auto& member : plaintext.compatibility_headers) headers.set(std::string(member.key()), std::string(member.value().as_string()));
  headers.set("Authorization", "Bearer " + token);
  if (plaintext.content_type) headers.set("Content-Type", *plaintext.content_type);
  const rt::RequestInit init{.method = plaintext.method, .headers = headers, .body = *plaintext.body};
  const rt::Request replayRequest("https://ai.ubq.fi" + plaintext.endpoint, init);
  auto replaying = uos::handler::index::handler(replayRequest);
  auto replayedResponse = co_await std::move(replaying);
  EXPECT_EQ(replayedResponse.status(), originalResponse.status());
  EXPECT_EQ(replayedResponse.headers().get("x-uos-upstream"), originalResponse.headers().get("x-uos-upstream"));
  const auto replayedText = co_await replayedResponse.text();
  EXPECT(!rt::includes(replayedText, MARKER));
  EXPECT(rt::deepEqual(rt::jsonParse(replayedText), rt::jsonParse(originalText)));
  EXPECT_EQ(replayedDispatches, 1);
  replay.assertComplete();
  EXPECT_SNAPSHOT(replay.snapshot(), 1, 1, 1, 1, false);
  EXPECT_EQ(rt::jsonStringify(json::value_from(plaintext.upstream)), upstreamBefore);
  auto reExporting = uos::handler::index::handler(exportRequest(rt::dateNow()));
  auto rePage = co_await std::move(reExporting);
  const auto reExported = co_await rePage.json();
  ASSERT(!field(reExported, "data").as_array().empty());
  EXPECT(rt::deepEqual(field(reExported, "data").as_array()[0], captures[0]));
}

ASYNC_TEST("historical framing fixture replays through the real Surplus fetch without the unterminated suffix") {
  const CredentialIsolation isolation;
  const std::string path = rt::path::join(rt::path::dirname(__FILE__), "fixtures/sentinel-historical-framing.json");
  const rt::Bytes fixtureBytes = rt::fs::readFileSync(path);
  const json::value fixture = rt::jsonParse(rt::decodeUtf8(fixtureBytes));
  EXPECT_EQ(rt::getNumber(fixture, "version"), std::optional<double>(1));
  const json::value* request = rt::get(fixture, "request");
  ASSERT(request != nullptr);
  EXPECT_EQ(rt::getString(rt::get(request, "endpoint")), std::optional<std::string_view>("/v1/responses"));
  EXPECT_EQ(rt::getString(rt::get(request, "method")), std::optional<std::string_view>("POST"));
  const json::value* upstream = rt::get(fixture, "upstream");
  ASSERT(upstream != nullptr);
  const SentinelUpstreamTrace trace = parseSentinelUpstreamTrace(*upstream);
  EXPECT_EQ(trace.attempts_truncated, false);
  EXPECT_EQ(trace.bytes_truncated, false);
  EXPECT_EQ(trace.chunks_truncated, false);
  expectAttempt(trace, "surplus", "text/event-stream", "eof");
  const std::string raw = decodeChunks(trace);
  EXPECT(rt::includes(raw, "response.completed"));
  EXPECT_EQ(rt::endsWith(raw, "\n\n"), false);
  const auto rawObservation = capture::inspectSentinelSse(rt::encodeUtf8(raw));
  ASSERT(rawObservation.terminal_body_base64.hasValue());
  EXPECT(rt::includes(rt::decodeUtf8(rt::fromBase64(*rawObservation.terminal_body_base64)), "response.completed"));
  EXPECT_EQ(rawObservation.stream, true);
  EXPECT_EQ(rawObservation.completed, true);
  EXPECT_EQ(rawObservation.terminal_type, std::optional<std::string>("response.completed"));
  EXPECT_EQ(rawObservation.failure_kind, std::optional<std::string>("invalid_sse_framing"));
  EXPECT_EQ(rawObservation.framing_valid, false);
  EXPECT(rawObservation.error_code == nullptr);
  EXPECT(rawObservation.error_param == nullptr);
  EXPECT_EQ(rawObservation.terminal_body_truncated, false);
  const uos::sentinel::replay_model::SentinelFailureObservation observation{
    .status = 200, .stream = true, .completed = true, .terminal_type = "response.completed", .failure_kind = std::nullopt,
    .synthetic_terminal_type = std::nullopt, .provider_route = "surplus",
  };
  const auto clientObservation = capture::resolveSentinelClientFailureObservation(observation, rawObservation);
  EXPECT_EQ(clientObservation.framing_valid, false);
  EXPECT_EQ(clientObservation.failure_kind, std::optional<std::string>("invalid_sse_framing"));
  EXPECT(capture::shouldPersistSentinelReplay(observation, clientObservation));
  const RecordedUpstreamReplay replay = createRecordedUpstreamReplay(trace, REPLAY_ROUTES());
  const rt::Fetch originalFetch = rt::globalFetch();
  surplus::SurplusAuthenticatedFetchOptions options;
  options.apiKey = "test-only-fixture-key";
  options.fetcher = replay.fetch;
  const std::optional<std::string_view> bodyText = rt::getString(rt::get(request, "body"));
  ASSERT(bodyText.has_value());
  const json::value body = rt::jsonParse(*bodyText);
  auto fetching = surplus::fetchSurplusResponses(body, options);
  const surplus::SurplusResponsesResult result = co_await std::move(fetching);
  EXPECT_EQ(result.response.status(), 200);
  EXPECT(rt::globalFetch() == originalFetch);
  auto reading = result.response.text();
  const std::string normalized = co_await std::move(reading);
  assertClientStreamSemantics(normalized);
  EXPECT_SNAPSHOT(replay.snapshot(), 1, 0, 4, 4, false);
  EXPECT_EQ(sha256Hex(rt::encodeUtf8(raw)), "d81a61c2ede75351b9d9e51bb3157dbe51f4baec92c67f6ffcfa5a910b771bad");
  EXPECT_EQ(sha256Hex(rt::encodeUtf8(normalized)), "e4b23d2ea5338058f67a28fb6b02f526e38166e7c50715dd23bcc23333c0145a");
  EXPECT_THROWS_MATCHING(assertClientStreamSemantics(raw), "blank-line terminator");
  EXPECT_EQ(rt::includes(raw, "event: response.output_text.done"), false);
  EXPECT_EQ(sha256Hex(fixtureBytes), "33938442d369c52213bdde02ea5e51b9848050400302f5e9dde4fcf6b84cacaf");
}
