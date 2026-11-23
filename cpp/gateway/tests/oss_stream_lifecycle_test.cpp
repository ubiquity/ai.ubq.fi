// Port of tests/oss-stream-lifecycle.test.ts. All eighteen source cases are callable; source398 uses both real public relays.
// Links: tests/oss_stream_lifecycle_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/responses_failover_stream.cpp src/provider/stream_relay.cpp src/chat/stream_translation.cpp src/openai_telemetry.cpp src/upstream_wire.cpp
// src/deepseek/index.cpp src/provider/lithos.cpp src/provider/cerebras_rate_limits.cpp src/utils.cpp src/provider/removed_provider_telemetry.cpp
// src/codex/responses_error.cpp src/deepseek/stream.cpp src/deepseek/responses.cpp src/responses_stream.cpp src/config.cpp
// src/bounded_response_body.cpp src/deepseek/responses_stream.cpp src/provider/lithos_rate_limits.cpp src/deepseek/responses_payload.cpp src/http.cpp
// src/provider/cerebras.cpp src/release.cpp src/auth/relay.cpp src/request_policy.cpp src/provider/claude_code_models.cpp
// src/provider/surplus.cpp src/models/codex_models.cpp src/provider/surplus_claude_request.cpp src/provider/anthropic_chat_responses.cpp src/kv.cpp
// src/provider/claude_code_accounts.cpp src/provider/selection.cpp src/provider/claude_code_credentials.cpp src/defaults.cpp src/models/codex_models_fetch.cpp
// src/provider/claude_code.cpp src/provider/anthropic_response_wire.cpp src/types.cpp src/models/codex_models_availability.cpp src/storage/kv.cpp
// src/storage/sqlite_readonly.cpp src/codex/dispatch.cpp src/provider/claude_code_quota.cpp src/provider/anthropic_responses.cpp src/codex/auth.cpp
// src/provider/capacity_events.cpp src/provider/claude_code_usage_schema.cpp src/codex/external_auth.cpp src/provider/anthropic_chat_stream.cpp src/provider/metered.cpp
// src/provider/surplus_claude_chat_request.cpp src/provider/anthropic_replay.cpp src/codex/native_auth.cpp src/provider/capacity_contract.cpp src/codex/routing_mutations.cpp
// src/codex/banked_reset.cpp src/authority/native_codex_mutations.cpp src/codex/capacity_routing.cpp src/authority/native_codex_capacity.cpp src/codex/auth_refresh.cpp
// src/codex/reset_settings.cpp src/runtime_config.cpp src/authority/native_codex_contracts.cpp src/codex/banked_reset_provider.cpp
// src/storage/native_kv.cpp src/provider/capacity.cpp src/cache/prompt_analytics_read.cpp src/provider/health.cpp src/cache/prompt_analytics_core.cpp
// src/codex/native_auth_transport.cpp src/provider/capacity_history.cpp src/provider/claude_capacity_history.cpp src/codex/experiment.cpp src/provider/capacity_parse.cpp
// src/provider/claude_capacity_rollups.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity_analytics.cpp src/provider/capacity_rollups.cpp src/codex/account_routing.cpp
// src/codex/routing_serial.cpp src/codex/routing_evaluation.cpp src/authority/native_codex_evaluation.cpp src/codex/routing_state.cpp src/authority/native_codex.cpp
// src/codex/overage_settings.cpp src/metered_quota.cpp src/codex/rate_limit_429.cpp src/runtime/websocket.cpp src/authority/native_codex_429.cpp
// src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp src/lib/jev_compaction/state.cpp src/api_key_policy.cpp
// src/api_keys.cpp src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/authority/native_paid_policy.cpp src/authority/native_paid.cpp
// src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics.cpp src/cache/telemetry_gate.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp
// src/codex/banked_reset_submission.cpp src/codex/index.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp
// src/debug_routing.cpp src/deepseek/chat_projection.cpp src/deepseek/context_admission_failure.cpp src/deepseek/flash_aliases.cpp src/deepseek/forwarded_payload_policy.cpp
// src/deepseek/handlers.cpp src/deepseek/waterfall_handler.cpp src/deepseek/waterfall.cpp src/inference_deadline.cpp src/input_normalization.cpp
// src/models/metadata.cpp src/models/openrouter_models.cpp src/openai_schema.cpp src/openai.cpp src/optional_telemetry_queue.cpp
// src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp
// src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/removed_provider.cpp src/paid_fallback/rollups.cpp src/paid_fallback/routing.cpp
// src/provider/cerebras_handlers.cpp src/provider/cerebras_responses.cpp src/provider/claude_code_access.cpp src/provider/lithos_handlers.cpp src/provider/lithos_models.cpp
// src/provider/lithos_streams.cpp src/provider/openrouter_handlers.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter_streams.cpp src/provider/openrouter_web_tools.cpp
// src/provider/openrouter.cpp src/provider/presentation.cpp src/provider/removed_provider_circuit.cpp src/recent_model_context.cpp src/request.cpp
// src/responses_attempts.cpp src/responses_buffered.cpp src/responses_handler.cpp src/responses_native_headers.cpp src/responses_request.cpp
// src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/http.cpp
// src/sentinel/upstream_capture.cpp src/storage/key_compatibility.cpp src/storage/value_compatibility.cpp src/authority/native_paid_contracts.cpp src/lib/jev_compaction/types.cpp
// src/chat/envelope.cpp src/chat/completions.cpp tests/helpers/openai_compat_harness.cpp
// src/agent_messages.cpp src/models/catalog.cpp src/auth/index.cpp src/auth/local_development_key.cpp src/analytics.cpp
// src/auth/passport.cpp src/auth/local_admin.cpp src/kernel/attestation.cpp src/auth/passkeys.cpp src/models/codex_models_whitelist.cpp
// src/auth/passport_sessions.cpp src/catalog/index.cpp src/catalog/models.cpp src/kernel/policy_queue.cpp src/catalog/store.cpp
// src/kernel/quota_reservations.cpp src/catalog/types.cpp src/kernel/quota_v2.cpp src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp
#include "uos/responses_failover_stream.hpp"
#include "uos/chat/envelope.hpp"
#include "uos/responses_handler.hpp"
#include "uos/provider/lithos_rate_limits.hpp"
#include "tests/helpers/openai_compat_harness.hpp"
#include "uos/responses_stream.hpp"
#include "uos/provider/stream_relay.hpp"
#include "uos/deepseek/stream.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/config.hpp"
#include "uos/rt/env.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using uos::responses_failover_stream::appendResponsesPrecommitEvent;
using uos::responses_failover_stream::createOwnedResponsesStream;
using uos::responses_failover_stream::MAX_RESPONSES_PRECOMMIT_CHARS;
using uos::responses_failover_stream::MAX_RESPONSES_PRECOMMIT_EVENTS;
using uos::responses_failover_stream::OwnedResponsesStreamOptions;
using uos::responses_failover_stream::PreparedResponsesStream;
using uos::responses_failover_stream::prepareResponsesStreamForCommit;
using uos::responses_failover_stream::responseEventFromValue;
using uos::responses_stream::PreflightedResponsesStream;
using uos::responses_stream::preflightResponsesStream;
using uos::responses_stream::proxyResponsesStream;
using uos::responses_stream::readResponsesStream;
using uos::responses_stream::responsesStreamErrorOf;
using uos::responses_stream::ResponsesStreamEvent;
using uos::responses_stream::ResponsesStreamIterator;
using uos::responses_stream::WithSseKeepaliveOptions;
using uos::responses_stream::withSseKeepalive;

namespace {
  using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;
  using ReadResult = rt::ReadableStreamReadResult<rt::ByteChunk>;

  rt::ByteChunk bytes(std::string_view value) { return rt::encodeUtf8(value); }
  std::string decode(const rt::ByteChunk& value) { return rt::decodeUtf8(value); }
  std::string sseFrame(const json::object& value) { return "data: " + rt::jsonStringify(json::value(value)) + "\n\n"; }
  std::string deltaFrame(std::string_view text) {
    json::object value;
    rt::objectDefine(value, "type", json::string("response.output_text.delta"));
    rt::objectDefine(value, "delta", json::string(text));
    return sseFrame(value);
  }
  std::string completedFrame() { return sseFrame(rt::jsonParse(R"({"type":"response.completed","response":{"status":"completed"}})").as_object()); }

  /** Races `settled` against a bounded wait; the timer is cleared whichever side wins. */
  template <class T> rt::Task<bool> settlesWithin(rt::Promise<T> settled, double timeoutMs) {
    rt::Timeout timer;
    const rt::Promise<bool> timeout = rt::newPromise<bool>([&timer, timeoutMs](const rt::Resolve<bool>& resolve, const rt::Reject&) {
      timer = rt::setTimeout([resolve] { resolve(false); }, timeoutMs);
    });
    const rt::Promise<bool> done = settled.then([](const auto&...) { return true; }, [](rt::Reason) { return true; });
    std::vector<rt::Promise<bool>> racers{done, timeout};
    const rt::Promise<bool> race = rt::race(std::move(racers));
    const bool result = co_await race.wait();
    rt::clearTimeout(timer);
    co_return result;
  }

  /** Bounded poll for state a stream does not expose as a promise. */
  rt::Task<bool> becomesTrue(std::function<bool()> condition, double timeoutMs) {
    const double deadlineAt = rt::performanceNow() + timeoutMs;
    while (!condition()) {
      if (rt::performanceNow() >= deadlineAt) co_return false;
      co_await rt::sleep(5);
    }
    co_return true;
  }

  template <class T> rt::Task<rt::Reason> rejectionOf(rt::Promise<T> pending) {
    rt::Reason failure;
    try {
      co_await pending.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    if (!failure) rt::test::report(__FILE__, __LINE__, "Expected the operation to reject", true);
    co_return failure;
  }

  /**
   * A scripted upstream provider. `produce` supplies the frame for one pull, or
   * std::nullopt to hold that read open until the source is cancelled. Entering a
   * pull is the handshake these fixtures wait on instead of assuming microtask turns.
   */
  struct ProbeState {
    int started = 0;
    int pending = 0;
    int cancelCount = 0;
    std::function<void()> releaseQuiet;
    std::vector<std::pair<int, rt::Resolve<void>>> waiters;
    void notify() {
      std::vector<std::pair<int, rt::Resolve<void>>> remaining;
      for (auto& [count, resolve] : waiters) {
        if (started < count) remaining.emplace_back(count, resolve);
        else resolve();
      }
      waiters = std::move(remaining);
    }
  };

  struct UpstreamProbe {
    rt::ByteStream stream;
    std::shared_ptr<ProbeState> state;
    rt::Promise<void> cancelled;
    int started() const { return state->started; }
    int pending() const { return state->pending; }
    int cancelCount() const { return state->cancelCount; }
    /** Resolves once `count` pull callbacks have been entered. */
    rt::Promise<void> startedAtLeast(int count) const {
      if (state->started >= count) return rt::Promise<void>::resolve();
      const rt::Resolvers<void> resolvers = rt::withResolvers<void>();
      state->waiters.emplace_back(count, resolvers.resolve);
      return resolvers.promise;
    }
  };

  UpstreamProbe probeUpstream(std::function<std::optional<rt::ByteChunk>(int pullIndex)> produce = nullptr, double highWaterMark = 1) {
    auto state = std::make_shared<ProbeState>();
    const rt::Resolvers<void> cancelled = rt::withResolvers<void>();
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [state, produce](Controller controller) -> rt::Task<void> {
      state->started += 1;
      state->pending += 1;
      state->notify();
      rt::Reason failure;
      try {
        const std::optional<rt::ByteChunk> chunk = produce ? produce(state->started) : std::nullopt;
        if (!chunk) {
          const rt::Resolvers<void> quiet = rt::withResolvers<void>();
          state->releaseQuiet = [resolve = quiet.resolve] { resolve(); };
          co_await quiet.promise.wait();
        } else {
          controller.enqueue(*chunk);
        }
      } catch (...) {
        failure = std::current_exception();
      }
      state->pending -= 1;
      state->notify();
      if (failure) std::rethrow_exception(failure);
    };
    source.cancel = [state, resolveCancelled = cancelled.resolve](rt::Reason) {
      state->cancelCount += 1;
      if (state->releaseQuiet) state->releaseQuiet();
      state->releaseQuiet = nullptr;
      resolveCancelled();
    };
    return UpstreamProbe{.stream = rt::ByteStream(std::move(source), {.highWaterMark = highWaterMark}), .state = state, .cancelled = cancelled.promise};
  }

  /** Releases a probe stream that a failed assertion may have left locked. */
  rt::Task<void> releaseProbe(UpstreamProbe probe) {
    if (!probe.stream.locked()) {
      const rt::Reason reason = rt::thrown(json::value("test cleanup"));
      try {
        co_await probe.stream.cancel(reason);
      } catch (...) {
      }
    }
  }

  rt::Task<void> cancelQuietly(rt::ReadableStreamDefaultReader<rt::ByteChunk> reader, std::string reasonText) {
    const rt::Reason reason = rt::thrown(json::value(json::string(reasonText)));
    try {
      co_await reader.cancel(reason);
    } catch (...) {
    }
  }
}

ASYNC_TEST("Responses proxy releases an in-flight upstream read when its reader cancels") {
  const UpstreamProbe probe = probeUpstream();
  const rt::ByteStream proxied = proxyResponsesStream(probe.stream);
  const auto reader = proxied.getReader();
  const rt::Promise<ReadResult> abandonedRead(reader.read());
  const rt::Promise<void> startedPromise1 = probe.startedAtLeast(1);
  const bool settled1 = co_await settlesWithin(startedPromise1, 500);
  EXPECT_EQ(settled1, true);
  EXPECT_EQ(probe.stream.locked(), true);
  EXPECT_EQ(probe.pending(), 1);

  const rt::Reason reason = rt::thrown(json::value("downstream client disconnected"));
  co_await reader.cancel(reason);
  const bool settled2 = co_await settlesWithin(probe.cancelled, 500);
  EXPECT_EQ(settled2, true);
  EXPECT_EQ(probe.cancelCount(), 1);
  EXPECT_EQ(probe.started(), 1);
  const bool settled3 = co_await settlesWithin(abandonedRead, 500);
  EXPECT_EQ(settled3, true);
  const std::function<bool()> unlocked = [stream = probe.stream] { return !stream.locked(); };
  const bool settled4 = co_await becomesTrue(unlocked, 500);
  EXPECT_EQ(settled4, true);
  co_await cancelQuietly(reader, "test cleanup");
  co_await releaseProbe(probe);
}

ASYNC_TEST("Responses proxy cancels an upstream stream that was never read") {
  const UpstreamProbe probe = probeUpstream();
  const rt::ByteStream proxied = proxyResponsesStream(probe.stream);
  const auto reader = proxied.getReader();
  const rt::Reason reason = rt::thrown(json::value("downstream client disconnected before the first read"));
  co_await reader.cancel(reason);
  EXPECT_EQ(probe.cancelCount(), 1);
  EXPECT_EQ(probe.started(), 0);
  EXPECT_EQ(probe.stream.locked(), false);
  co_await cancelQuietly(reader, "test cleanup");
  co_await releaseProbe(probe);
}

ASYNC_TEST("Responses proxy stops an eager upstream producer while its reader is idle") {
  const UpstreamProbe probe = probeUpstream([](int pullIndex) { return std::optional<rt::ByteChunk>(bytes(deltaFrame("chunk-" + std::to_string(pullIndex)))); });
  const rt::ByteStream proxied = proxyResponsesStream(probe.stream);
  const auto reader = proxied.getReader();
  const ReadResult first = co_await reader.read();
  if (first.done) rt::test::report(__FILE__, __LINE__, "Expected the first proxied event.", true);
  EXPECT_EQ(decode(*first.value), deltaFrame("chunk-1"));

  co_await rt::sleep(25);
  const int settledPulls = probe.started();
  // One event queued downstream plus one upstream read-ahead chunk is the whole
  // end-to-end allowance; the caps alone do not bound what an idle reader sees.
  EXPECT(settledPulls <= 3);
  co_await rt::sleep(50);
  EXPECT_EQ(probe.started(), settledPulls);
  EXPECT(probe.pending() <= 1);
  co_await cancelQuietly(reader, "slow reader stopped");
  co_await releaseProbe(probe);
}

ASYNC_TEST("Owned Responses stream stops an eager upstream producer while its consumer is idle") {
  const UpstreamProbe probe = probeUpstream([](int pullIndex) { return std::optional<rt::ByteChunk>(bytes(deltaFrame("chunk-" + std::to_string(pullIndex)))); });
  const rt::ByteStream body = createOwnedResponsesStream(OwnedResponsesStreamOptions{
    .initial = {},
    .iterator = readResponsesStream(probe.stream),
    .responseId = "resp_lifecycle",
  });
  const auto reader = body.getReader();
  const ReadResult first = co_await reader.read();
  if (first.done) rt::test::report(__FILE__, __LINE__, "Expected the first owned event.", true);
  EXPECT_EQ(decode(*first.value), deltaFrame("chunk-1"));

  co_await rt::sleep(25);
  const int settledPulls = probe.started();
  EXPECT(settledPulls <= 3);
  co_await rt::sleep(50);
  EXPECT_EQ(probe.started(), settledPulls);
  EXPECT(probe.pending() <= 1);
  co_await cancelQuietly(reader, "slow consumer stopped");
  co_await releaseProbe(probe);
}

ASYNC_TEST("Responses proxy forwards one terminal, cancels once, and never replays its upstream") {
  const std::vector<std::string> frames{deltaFrame("committed"), completedFrame(), deltaFrame("post-terminal")};
  const UpstreamProbe probe = probeUpstream([frames](int pullIndex) -> std::optional<rt::ByteChunk> {
    if (pullIndex - 1 >= static_cast<int>(frames.size())) return std::nullopt;
    return bytes(frames[static_cast<std::size_t>(pullIndex - 1)]);
  });
  const rt::ByteStream proxied = proxyResponsesStream(probe.stream);
  const auto reader = proxied.getReader();
  const ReadResult committed = co_await reader.read();
  if (committed.done) rt::test::report(__FILE__, __LINE__, "Expected the committed text event.", true);
  EXPECT_EQ(decode(*committed.value), frames[0]);
  const ReadResult terminal = co_await reader.read();
  if (terminal.done) rt::test::report(__FILE__, __LINE__, "Expected the terminal event.", true);
  EXPECT_EQ(decode(*terminal.value), frames[1]);

  const ReadResult done = co_await reader.read();
  EXPECT_EQ(done.done, true);
  EXPECT_EQ(probe.cancelCount(), 1);
  EXPECT(probe.started() <= 3);
  const rt::Reason reason = rt::thrown(json::value("terminal already delivered"));
  co_await reader.cancel(reason);
  EXPECT_EQ(probe.cancelCount(), 1);
  co_await cancelQuietly(reader, "test cleanup");
  co_await releaseProbe(probe);
}

ASYNC_TEST("Responses preflight propagates caller cancellation into a pending first read") {
  const rt::AbortController controller;
  const UpstreamProbe probe = probeUpstream();
  const rt::Promise<PreflightedResponsesStream> preflight(preflightResponsesStream(probe.stream, controller.signal()));
  const rt::Promise<void> startedPromise5 = probe.startedAtLeast(1);
  const bool settled5 = co_await settlesWithin(startedPromise5, 500);
  EXPECT_EQ(settled5, true);
  EXPECT_EQ(probe.stream.locked(), true);
  EXPECT_EQ(probe.pending(), 1);
  controller.abort(rt::makeReason(rt::DOMException("client disconnected", "AbortError")));

  const rt::Reason error = co_await rejectionOf(preflight);
  EXPECT(rt::isError(error));
  EXPECT_EQ(rt::errorName(error), std::optional<std::string>("AbortError"));
  EXPECT_EQ(probe.cancelCount(), 1);
  EXPECT_EQ(probe.started(), 1);
  const std::function<bool()> unlocked = [stream = probe.stream] { return !stream.locked(); };
  const bool settled6 = co_await becomesTrue(unlocked, 500);
  EXPECT_EQ(settled6, true);
  controller.abort(rt::makeReason(rt::DOMException("test cleanup", "AbortError")));
  const rt::Promise<void> settled = preflight.then([](const PreflightedResponsesStream&) {}, [](rt::Reason) {});
  co_await settlesWithin(settled, 500);
  co_await releaseProbe(probe);
}

ASYNC_TEST("Owned Responses stream aborts a parked upstream read when its consumer cancels") {
  const rt::AbortController upstreamAbort;
  const UpstreamProbe probe = probeUpstream();
  OwnedResponsesStreamOptions options{
    .initial = {},
    .iterator = readResponsesStream(probe.stream, upstreamAbort.signal()),
    .responseId = "resp_lifecycle",
  };
  options.abortUpstream = [upstreamAbort](rt::Reason reason) { upstreamAbort.abort(reason); };
  const rt::ByteStream body = createOwnedResponsesStream(std::move(options));
  const auto reader = body.getReader();
  const rt::Promise<ReadResult> parked(reader.read());
  const rt::Promise<void> startedPromise7 = probe.startedAtLeast(1);
  const bool settled7 = co_await settlesWithin(startedPromise7, 500);
  EXPECT_EQ(settled7, true);
  EXPECT_EQ(probe.stream.locked(), true);
  EXPECT_EQ(probe.pending(), 1);

  const rt::Reason reason = rt::thrown(json::value("downstream client disconnected"));
  co_await reader.cancel(reason);
  const bool settled8 = co_await settlesWithin(probe.cancelled, 500);
  EXPECT_EQ(settled8, true);
  EXPECT_EQ(probe.cancelCount(), 1);
  EXPECT_EQ(probe.started(), 1);
  const bool settled9 = co_await settlesWithin(parked, 500);
  EXPECT_EQ(settled9, true);
  const std::function<bool()> unlocked = [stream = probe.stream] { return !stream.locked(); };
  const bool settled10 = co_await becomesTrue(unlocked, 500);
  EXPECT_EQ(settled10, true);
  co_await cancelQuietly(reader, "test cleanup");
  if (!upstreamAbort.signal().aborted()) upstreamAbort.abort(rt::makeReason(rt::DOMException("test cleanup", "AbortError")));
  co_await releaseProbe(probe);
}

ASYNC_TEST("SSE keepalive forwards cancellation and keeps one pending upstream read") {
  const UpstreamProbe probe = probeUpstream([](int pullIndex) { return pullIndex == 1 ? std::optional<rt::ByteChunk>(bytes(deltaFrame("first"))) : std::nullopt; });
  WithSseKeepaliveOptions options;
  options.intervalMs = 5;
  const rt::ByteStream keepalive = withSseKeepalive(probe.stream, options);
  const auto reader = keepalive.getReader();
  const ReadResult first = co_await reader.read();
  if (first.done) rt::test::report(__FILE__, __LINE__, "Expected the first upstream frame.", true);
  EXPECT_EQ(decode(*first.value), deltaFrame("first"));
  const ReadResult heartbeat = co_await reader.read();
  if (heartbeat.done) rt::test::report(__FILE__, __LINE__, "Expected a keepalive frame while the provider is quiet.", true);
  EXPECT_EQ(decode(*heartbeat.value), std::string(": keepalive\n\n"));
  EXPECT(probe.pending() <= 1);

  const int startedWhileIdle = probe.started();
  co_await rt::sleep(25);
  EXPECT_EQ(probe.started(), startedWhileIdle);

  const rt::Reason reason = rt::thrown(json::value("client disconnected"));
  co_await reader.cancel(reason);
  EXPECT_EQ(probe.cancelCount(), 1);
  co_await cancelQuietly(reader, "test cleanup");
  co_await releaseProbe(probe);
}

ASYNC_TEST("Responses precommit buffer refuses an unbounded eager producer and releases its iterator") {
  auto produced = std::make_shared<int>(0);
  auto released = std::make_shared<bool>(false);
  // A producing iterator with no asynchronous work of its own; its return() records
  // the release the precommit buffer must perform.
  const ResponsesStreamIterator eager([produced, released](rt::Yield<ResponsesStreamEvent> yield) -> rt::Task<void> {
    bool returning = false;
    try {
      for (;;) {
        *produced += 1;
        json::object value;
        rt::objectDefine(value, "type", json::string("response.in_progress"));
        rt::objectDefine(value, "sequence_number", *produced);
        ResponsesStreamEvent event = responseEventFromValue(std::move(value));
        co_await yield(std::move(event));
      }
    } catch (const rt::GeneratorReturn&) {
      returning = true;
    }
    *released = true;
    if (returning) throw rt::GeneratorReturn{};
  });
  const rt::Promise<PreparedResponsesStream> prepared(prepareResponsesStreamForCommit(eager));
  const rt::Reason error = co_await rejectionOf(prepared);
  const auto* streamError = responsesStreamErrorOf(error);
  ASSERT(streamError);
  EXPECT_EQ(streamError->kind, std::string("event_too_large"));
  EXPECT_EQ(*produced, static_cast<int>(MAX_RESPONSES_PRECOMMIT_EVENTS) + 1);
  EXPECT_EQ(*released, true);
  const rt::Promise<rt::IteratorResult<ResponsesStreamEvent>> finished(eager.return_());
  co_await settlesWithin(finished, 250);
}

TEST("Responses precommit character budget refuses events past its cap without buffering them") {
  json::object value;
  rt::objectDefine(value, "type", json::string("response.in_progress"));
  rt::objectDefine(value, "pad", json::string(std::string(1024 * 1024, 'x')));
  const ResponsesStreamEvent chunk = responseEventFromValue(std::move(value));
  std::vector<ResponsesStreamEvent> buffered;
  double chars = 0;
  std::size_t accepted = 0;
  bool refused = false;
  try {
    for (;;) {
      chars = appendResponsesPrecommitEvent(buffered, chunk, chars);
      accepted += 1;
    }
  } catch (...) {
    const auto* error = responsesStreamErrorOf(std::current_exception());
    refused = error && error->kind == "event_too_large";
  }
  EXPECT(refused);
  EXPECT_EQ(buffered.size(), accepted);
  EXPECT(chars <= MAX_RESPONSES_PRECOMMIT_CHARS);

  refused = false;
  try {
    appendResponsesPrecommitEvent(buffered, chunk, MAX_RESPONSES_PRECOMMIT_CHARS - 1);
  } catch (...) {
    const auto* error = responsesStreamErrorOf(std::current_exception());
    refused = error && error->kind == "event_too_large";
  }
  EXPECT(refused);
  EXPECT_EQ(buffered.size(), accepted);
}

namespace {
  namespace relay = uos::provider::stream_relay;
  namespace telemetry = uos::openai_telemetry;
  using Frame = relay::ProviderStreamFrame;
  using FrameIterator = relay::ProviderStreamIterator;
  struct CredentialScope {
    rt::env::ScopedEnvironment env{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c2"}}};
    std::string previous = uos::config::config.codexAuthJsonB64;
    CredentialScope() { uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialScope() { uos::config::config.codexAuthJsonB64 = previous; }
  };
  relay::ProviderStreamAdapter fixtureAdapter() {
    relay::ProviderStreamAdapter adapter;
    adapter.responseHeaders = [](std::optional<std::string>) { return rt::Headers(); };
    adapter.frames = [](rt::Response upstream, std::string model, relay::ProviderStreamFrameOptions options) -> FrameIterator {
      uos::deepseek::stream::DeepSeekStreamOptions settings; settings.signal = options.signal;
      return uos::deepseek::stream::iterateDeepSeekChatCompletionStream(upstream, std::move(model), settings);
    };
    adapter.recordResponseHealth = [](double, std::optional<std::string>) {};
    adapter.recordProviderError = [](std::optional<double>, std::optional<std::string>) {};
    adapter.recordCancellation = [](const telemetry::UsageContext*) {};
    adapter.recordIncompleteResponse = [](const telemetry::UsageContext*) {};
    adapter.recordFinishFailureKind = [](const telemetry::UsageContext*, std::optional<std::string>) {};
    adapter.recordTransportFailure = [](const telemetry::UsageContext*, rt::Reason, std::string) {};
    adapter.terminalTypeForError = [](rt::Reason, rt::AbortSignal) { return "response.failed"; };
    adapter.streamErrorCode = "deepseek_upstream_error";
    adapter.responsesProfile = uos::deepseek::responses::DEEPSEEK_RESPONSES_PROFILE;
    return adapter;
  }
  rt::Response fixtureRelay(bool chat, const relay::ProviderStreamAdapter& adapter, relay::ProviderStreamSource upstream,
    const telemetry::UsageContext* context, rt::AbortSignal downstream, rt::AbortSignal request, relay::CancelPending cancelPending = {}) {
    if (chat) return relay::relayChatCompletionStream(adapter, std::move(upstream), std::nullopt, context, downstream, request, "deepseek-flash", cancelPending);
    relay::RelayResponsesStreamOptions options{std::move(upstream), "deepseek-flash", "resp_lifecycle", 1,
      {json::array{}, "auto", true, std::nullopt}, {}, {}, std::nullopt, context, downstream, request, "deepseek-flash", std::move(cancelPending)};
    return relay::relayResponsesStream(adapter, options);
  }
  struct RelayCounts { int next = 0; int returned = 0; int finalized = 0; int postDone = 0; int health = 0; int cancelled = 0; int errors = 0; };
  struct ObservedUsage { std::optional<telemetry::UsageTokens> usage; bool completed; };
  struct RelayFixtureState {
    RelayCounts counts;
    rt::Resolvers<void> cleanupGate = rt::withResolvers<void>();
    rt::Resolvers<void> cleanupEntered = rt::withResolvers<void>();
    std::optional<FrameIterator> iterator;
    std::shared_ptr<telemetry::ResponseTelemetryState> telemetry = telemetry::createResponseTelemetryState();
    std::vector<ObservedUsage> observedUsage;
  };
  rt::Task<void> gatedFrames(std::shared_ptr<RelayFixtureState> state, rt::AsyncGenerator<Frame> frames, rt::Yield<Frame> yield) {
    rt::Reason failure;
    const auto forward = [state, yield](Frame frame) -> rt::Task<void> {
      const bool done = std::holds_alternative<uos::deepseek::stream::DeepSeekStreamFrameDone>(frame);
      co_await yield(std::move(frame));
      if (done) state->counts.postDone += 1;
    };
    try { co_await rt::forAwait(frames, forward); } catch (...) { failure = std::current_exception(); }
    state->counts.finalized += 1;
    state->cleanupEntered.resolve();
    co_await state->cleanupGate.promise.wait();
    if (failure) std::rethrow_exception(failure);
  }
  struct RelayFixture {
    rt::Response response;
    UpstreamProbe probe;
    std::shared_ptr<RelayFixtureState> state;
    rt::Task<void> cleanup() const {
      state->cleanupGate.resolve();
      if (state->iterator && state->counts.finalized == 0) co_await state->iterator->return_();
      state->iterator.reset();
      co_await releaseProbe(probe);
    }
  };
  RelayFixture providerRelayFixture(bool chat) {
    const json::object chunk = rt::jsonParse(R"({"id":"chatcmpl_lifecycle","object":"chat.completion.chunk","created":1,"model":"deepseek-flash","choices":[{"index":0,"delta":{"content":"answer"},"finish_reason":"stop"}],"usage":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5}})").as_object();
    const auto probe = probeUpstream([chunk](int index) -> std::optional<rt::ByteChunk> {
      return index == 1 ? std::optional<rt::ByteChunk>(bytes(sseFrame(chunk) + "data: [DONE]\n\n" + sseFrame(chunk))) : std::nullopt;
    });
    const auto state = std::make_shared<RelayFixtureState>();
    auto adapter = fixtureAdapter();
    adapter.frames = [state](rt::Response upstream, std::string model, relay::ProviderStreamFrameOptions options) {
      uos::deepseek::stream::DeepSeekStreamOptions settings; settings.signal = options.signal;
      const auto inner = uos::deepseek::stream::iterateDeepSeekChatCompletionStream(upstream, std::move(model), settings);
      const rt::AsyncGenerator<Frame> frames([state, inner](rt::Yield<Frame> yield) { return gatedFrames(state, inner, std::move(yield)); });
      FrameIterator counted(frames);
      const auto next = counted.next;
      counted.next = [state, next] { state->counts.next += 1; return next(); };
      const auto finish = counted.return_;
      counted.return_ = [state, finish] { state->counts.returned += 1; return finish(); };
      state->iterator = counted;
      return counted;
    };
    adapter.recordResponseHealth = [state](double, std::optional<std::string>) { state->counts.health += 1; };
    adapter.recordProviderError = [state](std::optional<double>, std::optional<std::string>) { state->counts.errors += 1; };
    adapter.recordCancellation = [state](const telemetry::UsageContext*) { state->counts.cancelled += 1; };
    telemetry::UsageContext context{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt};
    context.responseTelemetry = state->telemetry;
    context.onTerminalUsage = [state](std::optional<telemetry::UsageTokens> usage, bool completed) { state->observedUsage.push_back({std::move(usage), completed}); };
    const auto signal = rt::AbortController().signal();
    return {fixtureRelay(chat, adapter, rt::Response(probe.stream), &context, signal, signal), probe, state};
  }
  rt::Task<void> disposalCase(bool chat, bool cancel) {
    CredentialScope credentials;
    const auto fixture = providerRelayFixture(chat);
    const auto state = fixture.state;
    if (!cancel) {
      const rt::Promise<std::string> output(fixture.response.text());
      const bool entered = co_await settlesWithin(state->cleanupEntered.promise, 500); EXPECT(entered);
      EXPECT_EQ(state->counts.returned, 1); EXPECT_EQ(state->counts.finalized, 1); EXPECT_EQ(state->counts.next, 2); EXPECT_EQ(state->counts.postDone, 0);
      EXPECT_EQ(fixture.probe.cancelCount(), 1); EXPECT(!fixture.probe.stream.locked()); EXPECT(fixture.probe.started() <= 2);
      EXPECT(!state->telemetry->streamTerminalType); EXPECT(state->observedUsage.empty()); EXPECT_EQ(state->counts.health, 0);
      const bool settled = co_await settlesWithin(output, 25); EXPECT(!settled);
      state->cleanupGate.resolve();
      const auto text = co_await output.wait(); EXPECT(rt::includes(text, "answer"));
      const auto terminal = chat ? "data: [DONE]" : "event: response.completed";
      EXPECT_EQ(rt::split(text, terminal).size() - 1, 1U);
      EXPECT_EQ(state->telemetry->streamTerminalType, std::optional<std::string>("response.completed"));
      EXPECT_EQ(state->observedUsage.size(), 1U); EXPECT(state->observedUsage[0].completed); EXPECT(state->observedUsage[0].usage);
      EXPECT_EQ(state->observedUsage[0].usage->totalTokens, std::optional<double>(5));
      EXPECT_EQ(state->counts.health, 1); EXPECT_EQ(state->counts.errors, 0); EXPECT_EQ(state->counts.returned, 1); EXPECT_EQ(fixture.probe.cancelCount(), 1);
      co_await fixture.cleanup();
    } else {
      const auto reader = fixture.response.body()->getReader();
      const auto consume = [reader]() -> rt::Task<void> { for (;;) { const auto next = co_await reader.read(); if (next.done) co_return; } };
      const rt::Promise<void> output(rt::async(consume));
      const bool entered = co_await settlesWithin(state->cleanupEntered.promise, 500); EXPECT(entered); EXPECT_EQ(state->counts.returned, 1);
      const auto reason = rt::thrown(json::value("client disconnected during DONE disposal"));
      co_await reader.cancel(reason); EXPECT_EQ(state->counts.returned, 1);
      state->cleanupGate.resolve(); co_await output.wait();
      EXPECT_EQ(state->telemetry->streamTerminalType, std::optional<std::string>("cancelled"));
      EXPECT_EQ(state->counts.cancelled, 1); EXPECT_EQ(state->counts.finalized, 1); EXPECT_EQ(state->counts.next, 2); EXPECT_EQ(state->counts.postDone, 0);
      EXPECT_EQ(state->counts.health, 0); EXPECT_EQ(state->counts.errors, 0); EXPECT_EQ(state->observedUsage.size(), 1U); EXPECT(!state->observedUsage[0].completed);
      EXPECT_EQ(fixture.probe.cancelCount(), 1); EXPECT(!fixture.probe.stream.locked());
      co_await cancelQuietly(reader, "test cleanup"); co_await fixture.cleanup();
    }
  }
}

ASYNC_TEST("Provider chat relay disposes DONE before terminal and usage settlement") { co_await disposalCase(true, false); }
ASYNC_TEST("Provider chat cancellation during DONE disposal returns the iterator only once") { co_await disposalCase(true, true); }
ASYNC_TEST("Provider responses relay disposes DONE before terminal and usage settlement") { co_await disposalCase(false, false); }
ASYNC_TEST("Provider responses cancellation during DONE disposal returns the iterator only once") { co_await disposalCase(false, true); }

ASYNC_TEST("Provider relays cancel a late deferred response without reading it") {
  CredentialScope credentials;
  auto adapter = fixtureAdapter(); adapter.terminalTypeForError = [](rt::Reason, rt::AbortSignal) { return "cancelled"; };
  for (int wire = 0; wire < 2; ++wire) {
    const auto lateUpstream = probeUpstream(nullptr, 0);
    const auto pending = rt::withResolvers<rt::Response>();
    const auto signal = rt::AbortController().signal();
    const rt::AbortController pendingDispatch;
    int pendingCancellationCount = 0;
    const relay::CancelPending cancelPending = [&](rt::Reason reason) { pendingCancellationCount += 1; pendingDispatch.abort(reason); };
    const auto response = fixtureRelay(wire == 0, adapter, pending.promise, nullptr, signal, signal, cancelPending);
    const auto reader = response.body()->getReader();
    const auto pendingRead = reader.readPromise();
    const auto reason = rt::thrown(json::value("client disconnected"));
    co_await reader.cancel(reason);
    EXPECT_EQ(pendingCancellationCount, 1); EXPECT(pendingDispatch.signal().aborted());
    const rt::ResponseInit init{.status = 200, .headers = rt::Headers{{"Content-Type", "text/event-stream"}}};
    pending.resolve(rt::Response(lateUpstream.stream, init));
    const bool readSettled = co_await settlesWithin(pendingRead, 500); EXPECT(readSettled);
    const auto next = co_await pendingRead.wait(); EXPECT(next.done);
    const bool cancelled = co_await settlesWithin(lateUpstream.cancelled, 500); EXPECT(cancelled);
    EXPECT_EQ(lateUpstream.cancelCount(), 1); EXPECT_EQ(lateUpstream.started(), 0);
    co_await cancelQuietly(reader, "test cleanup"); co_await releaseProbe(lateUpstream);
  }
}

namespace {
  rt::Task<void> refusalCase(bool chat) {
    CredentialScope credentials;
    auto adapter = fixtureAdapter(); adapter.streamErrorCode = "provider_gateway_timeout";
    const auto downstream = rt::AbortController().signal(); const rt::AbortController request;
    const rt::ResponseInit init{.status = 504};
    const rt::Response refused(std::string(R"({"error":{"code":"provider_gateway_timeout","message":"provider gateway timed out"}})"), init);
    const auto response = fixtureRelay(chat, adapter, refused, nullptr, downstream, request.signal());
    const auto text = co_await response.text(); EXPECT(rt::includes(text, "provider_gateway_timeout")); EXPECT(rt::includes(text, "provider gateway timed out"));
    const rt::AbortController deadline; deadline.abort(rt::makeReason(rt::DOMException("Gateway deadline fixture", "TimeoutError")));
    const rt::Response local(std::string(R"({"error":{"code":"gateway_timeout","message":"Upstream request exceeded the gateway deadline."}})"), init);
    const auto localFailure = fixtureRelay(chat, adapter, local, nullptr, downstream, deadline.signal());
    const auto localText = co_await localFailure.text(); EXPECT_EQ(localText, "");
  }
}
ASYNC_TEST("Provider chat distinguishes an upstream 504 from a local deadline") { co_await refusalCase(true); }
ASYNC_TEST("Provider responses distinguishes an upstream 504 from a local deadline") { co_await refusalCase(false); }

ASYNC_TEST("Lithos handlers cancel deferred waits and report elapsed time through both stream relays") {
  namespace harness = uos::tests::helpers::openai_compat_harness;
  harness::Fixture fixture;
  rt::env::set("LITHOSAI_API_KEY", "lith_sk_fixture");
  for (const bool chat : std::vector<bool>{true, false}) {
    uos::provider::lithos_rate_limits::clearLithosFailoverWindows();
    int calls = 0;
    rt::GlobalFetchOverride fetch(rt::Fetch([&calls](rt::RequestInfo, rt::RequestInit) {
      ++calls;
      const json::object payload{{"error",json::object{{"message","rate limit exceeded"}}}};
      return rt::Response::json(payload,rt::ResponseInit{.status=429,.headers=rt::Headers{{"retry-after-ms","5000"}}});
    }));
    rt::AbortController controller;
    const auto cleanup=rt::scopeExit([controller] { controller.abort(rt::thrown(json::value("test cleanup"))); });
    json::object body{{"model","deepseek-ai/DeepSeek-V4.1-Flash"},{"stream",true}};
    if(chat)body["messages"]=json::array{json::object{{"role","user"},{"content","hi"}}};else body["input"]="hi";
    const auto request=chat?harness::specialProviderChatRequest(body,controller.signal()):harness::specialProviderResponsesRequest(body,controller.signal());
    uos::openai_telemetry::UsageContext context{.keyId=std::nullopt,.kernelRepo=std::nullopt,.kernelOrg=std::nullopt,.requestId=chat?"lithos-deferred-cancel-chat":"lithos-deferred-cancel-responses",.responseTelemetry=uos::openai_telemetry::createResponseTelemetryState()};
    auto task=chat?uos::chat::envelope::handleChatCompletions(request,&context):uos::responses_handler::handleResponses(request,&context);
    rt::Promise<rt::Response> pending(std::move(task));const auto response=co_await pending.wait();
    EXPECT_EQ(response.status(),200);EXPECT_EQ(calls,1);
    const auto telemetry=uos::openai_telemetry::getResponseTelemetry(response);EXPECT(telemetry);EXPECT(!telemetry->rateLimitWaitMs);
    rt::test::clock().advance(25);
    EXPECT(response.body());const auto reason=rt::thrown(json::value("client disconnected"));auto cancel=response.body()->cancel(reason);co_await std::move(cancel);
    co_await rt::sleep(0);
    const auto finalTelemetry=uos::openai_telemetry::getResponseTelemetry(response);
    EXPECT_EQ(finalTelemetry->rateLimitWaitMs,std::optional<double>(25));EXPECT_EQ(finalTelemetry->streamTerminalType,std::optional<std::string>("cancelled"));EXPECT_EQ(calls,1);
    controller.abort(rt::thrown(json::value("test cleanup")));co_await rt::microtask();
  }
  uos::provider::lithos_rate_limits::clearLithosFailoverWindows();
  co_await harness::settleHarness();
}

namespace {
  rt::Task<void> pendingCancellationLifetime(std::weak_ptr<int>* observed, bool lateAbort) {
    const auto calls = std::make_shared<int>(0);
    *observed = calls;
    const rt::AbortController cancellation;
    const auto signal = rt::AbortSignal::any({cancellation.signal()});
    auto adapter = fixtureAdapter();
    adapter.frames = [](rt::Response, std::string, relay::ProviderStreamFrameOptions) -> FrameIterator {
      const rt::AsyncGenerator<Frame> frames([](rt::Yield<Frame> yield) -> rt::Task<void> {
        Frame done = uos::deepseek::stream::DeepSeekStreamFrameDone{};
        co_await yield(std::move(done));
      });
      return frames;
    };
    const relay::CancelPending cancel = [calls, cancellation](rt::Reason reason) {
      ++*calls;
      cancellation.abort(reason);
    };
    const auto response = fixtureRelay(true, adapter, rt::Response(), nullptr, signal, signal, cancel);
    auto reading = response.text();
    const auto output = co_await std::move(reading);
    EXPECT_EQ(output, "data: [DONE]\n\n");
    EXPECT_EQ(*calls, 0);
    if (lateAbort) {
      const auto reason = rt::thrown(json::value("late downstream abort"));
      cancellation.abort(reason);
      EXPECT_EQ(*calls, 1);
      cancellation.abort(reason);
      EXPECT_EQ(*calls, 1);
    }
  }
}

TEST("provider relay releases pending cancellation signal cycles when its loop ends") {
  std::weak_ptr<int> observed;
  rt::FakeClock clock;
  {
    rt::Loop loop(clock);
    auto running = pendingCancellationLifetime(&observed, false);
    rt::blockOn(loop, std::move(running));
    EXPECT(!observed.expired());
  }
  EXPECT(observed.expired());
}

TEST("provider relay preserves exactly one late downstream cancellation after completion") {
  std::weak_ptr<int> observed;
  rt::FakeClock clock;
  {
    rt::Loop loop(clock);
    auto running = pendingCancellationLifetime(&observed, true);
    rt::blockOn(loop, std::move(running));
  }
  EXPECT(observed.expired());
}
