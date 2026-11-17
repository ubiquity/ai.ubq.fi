// Port of tests/oss-telemetry-overhead.test.ts: all fifteen source cases. Existing queue cases preserved.
// Time is the ASYNC_TEST fake clock: `performance.now()` deltas are simulated time.
// Links the sources of tests/handler_terminal_usage_rollup_test.cpp.
#include "tests/helpers/terminal_log_harness.hpp"
#include "uos/optional_telemetry_queue.hpp"
#include "tests/helpers/counting_kv.hpp"
#include "uos/cache/prompt_analytics.hpp"
#include "uos/kv.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace uos;
using optional_telemetry_queue::createOptionalTelemetryQueue;
using optional_telemetry_queue::OptionalTelemetryQueue;
using optional_telemetry_queue::OptionalTelemetryQueueOptions;
using optional_telemetry_queue::OptionalTelemetryQueuePartialBounds;
using optional_telemetry_queue::OptionalTelemetryQueueSnapshot;
using cache::prompt_analytics::closeOptionalPromptCacheAnalytics;
using cache::prompt_analytics::enqueuePromptCacheAnalytics;
using cache::prompt_analytics::flushOptionalPromptCacheAnalytics;
using cache::prompt_analytics::optionalPromptCacheAnalyticsSnapshot;
using cache::prompt_analytics::PromptCacheAnalyticsRecordResult;
using cache::prompt_analytics::recordPromptCacheAnalytics;
using cache::prompt_analytics::resetOptionalPromptCacheAnalyticsForTest;
using cache::prompt_analytics_core::PromptCacheAnalyticsEvent;
using cache::prompt_analytics_core::PromptCacheAnalyticsOptions;
using cache::prompt_analytics_queue::PromptCacheAnalyticsEnqueueOptions;
using tests::helpers::counting_kv::CountingKv;
using tests::helpers::counting_kv::KvCommandRecord;

namespace {
  constexpr std::string_view RELEASE = "0123456789abcdef0123456789abcdef01234567";
  constexpr double NOW_MS = 1800000000000;

  // `const tick = () => new Promise((resolve) => setTimeout(resolve, 1))`
  rt::Task<void> tick() { co_await rt::sleep(1); }
  // `const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms))`
  rt::Task<void> delay(double milliseconds) { co_await rt::sleep(milliseconds); }

  OptionalTelemetryQueuePartialBounds bounds(double maxEntries, double maxBytes, double maxAgeMs, double maxConcurrentWrites,
                                             std::optional<double> maxDrainWaitMs = std::nullopt) {
    OptionalTelemetryQueuePartialBounds partial;
    partial.maxEntries = maxEntries;
    partial.maxBytes = maxBytes;
    partial.maxAgeMs = maxAgeMs;
    partial.maxConcurrentWrites = maxConcurrentWrites;
    partial.maxDrainWaitMs = maxDrainWaitMs;
    return partial;
  }

  struct WriterGauge {
    int inFlight = 0;
    int peakInFlight = 0;
    std::vector<rt::Resolve<void>> releases;
  };

  // The async writer of the concurrency test: holds until the test releases it.
  rt::Task<bool> heldWrite(std::shared_ptr<WriterGauge> gauge) {
    gauge->inFlight += 1;
    gauge->peakInFlight = std::max(gauge->peakInFlight, gauge->inFlight);
    const rt::Promise<void> released = rt::newPromise<void>([gauge](rt::Resolve<void> resolve, rt::Reject) { gauge->releases.push_back(resolve); });
    co_await released.wait();
    gauge->inFlight -= 1;
    co_return true;
  }

  rt::Task<bool> delayedWrite() {
    co_await delay(40);
    co_return true;
  }

  PromptCacheAnalyticsEvent event() {
    PromptCacheAnalyticsEvent value{
      .provider = "chatgpt_codex",
      .model = "gpt-telemetry-overhead-model",
      .route = "responses",
      .status = 200,
      .completed = true,
      .usageTelemetryStatus = "reported",
      .inputTokens = 1'000,
      .cachedInputTokens = 400,
      .cacheWriteInputTokens = 100,
      .promptCacheKeyPresent = true,
      .promptCacheMode = "explicit",
      .fallbackReason = nullptr,
    };
    return value;
  }

  PromptCacheAnalyticsOptions options(const std::shared_ptr<CountingKv>& kv) {
    PromptCacheAnalyticsOptions value;
    value.kv = kv;
    value.release = std::string(RELEASE);
    value.now = [] { return NOW_MS; };
    return value;
  }

  // `{ release: RELEASE, now: () => NOW_MS }`: the module queue resolves the environment KV.
  PromptCacheAnalyticsEnqueueOptions moduleOptions() {
    PromptCacheAnalyticsEnqueueOptions value;
    value.release = std::string(RELEASE);
    value.now = [] { return NOW_MS; };
    return value;
  }

  /** The exact event the handler's terminal log produces for `providerResponse`. */
  PromptCacheAnalyticsEvent handlerTerminalEvent() {
    PromptCacheAnalyticsEvent value = event();
    value.provider = "chatgpt_codex";
    value.model = nullptr;
    value.promptCacheKeyPresent = false;
    value.promptCacheMode = "unspecified";
    value.fallbackReason = nullptr;
    // `handlerCompletionEvidence`
    value.completed = true;
    value.usageTelemetryStatus = "reported";
    value.inputTokens = 1'000;
    value.cachedInputTokens = 400;
    value.cacheWriteInputTokens = 100;
    return value;
  }
}

ASYNC_TEST("prompt-cache analytics sink fixture records on the counting KV") {
  const auto kv = std::make_shared<CountingKv>();
  auto recording = recordPromptCacheAnalytics(handlerTerminalEvent(), options(kv));
  const PromptCacheAnalyticsRecordResult recorded = co_await std::move(recording);
  EXPECT_EQ(recorded.status, std::string("recorded"));  // the fixture sink disposition must be recorded
  EXPECT_EQ(recorded.reason, std::string("recorded"));
  EXPECT_EQ(recorded.bucket_start_at_ms, std::optional<double>(rt::Math::floor(NOW_MS / (15 * 60'000)) * (15 * 60'000)));
  EXPECT(kv->entries.size() > 0);  // the durable aggregate, cohort and cardinality counters must land
  std::vector<KvCommandRecord> commits;
  for (const KvCommandRecord& command : kv->commands)
    if (command.command == "atomic.commit") commits.push_back(command);
  EXPECT_EQ(commits.size(), std::size_t{1});  // one recorded sample is one atomic CAS plus sum commit
  ASSERT(!commits.empty());
  EXPECT_EQ(commits[0].atomicResult, std::optional<std::string>("committed"));  // the fixture KV must commit the sink's atomic CAS plus sum mutations
}

ASYNC_TEST("optional telemetry queue charges entries and UTF-8 bytes until each write settles") {
  const rt::Resolvers<bool> held = rt::withResolvers<bool>();
  const rt::Promise<bool> heldPromise = held.promise;

  OptionalTelemetryQueueOptions<double> entriesOptions;
  entriesOptions.write = [heldPromise](double) { return heldPromise; };
  entriesOptions.bounds = bounds(2, 4096, 60000, 1);
  const OptionalTelemetryQueue<double> entriesQueue = createOptionalTelemetryQueue(entriesOptions);
  std::vector<std::string> outcomes;
  for (const double value : {0.0, 1.0, 2.0}) outcomes.push_back(entriesQueue.enqueue(value));
  EXPECT_EQ(outcomes, (std::vector<std::string>{"enqueued", "enqueued", "dropped_capacity"}));
  const OptionalTelemetryQueueSnapshot entriesSnapshot = entriesQueue.snapshot();
  EXPECT_EQ(entriesSnapshot.retained_entries, std::int64_t(2));
  EXPECT_EQ(entriesSnapshot.queued_entries, std::int64_t(1));
  EXPECT_EQ(entriesSnapshot.writes_in_flight, std::int64_t(1));
  EXPECT_EQ(entriesSnapshot.retained_bytes, 2.0);
  EXPECT_EQ(entriesSnapshot.dropped_by_entries, std::int64_t(1));
  EXPECT_EQ(entriesSnapshot.dropped_by_bytes, std::int64_t(0));

  OptionalTelemetryQueueOptions<double> bytesOptions;
  bytesOptions.write = [heldPromise](double) { return heldPromise; };
  bytesOptions.bounds = bounds(8, 300, 60000, 1);
  bytesOptions.measure = [](const double&) { return 200.0; };
  const OptionalTelemetryQueue<double> bytesQueue = createOptionalTelemetryQueue(bytesOptions);
  outcomes.clear();
  for (const double value : {0.0, 1.0, 2.0}) outcomes.push_back(bytesQueue.enqueue(value));
  EXPECT_EQ(outcomes, (std::vector<std::string>{"enqueued", "dropped_bytes", "dropped_bytes"}));
  const OptionalTelemetryQueueSnapshot bytesSnapshot = bytesQueue.snapshot();
  EXPECT_EQ(bytesSnapshot.retained_bytes, 200.0);
  EXPECT_EQ(bytesSnapshot.retained_entries, std::int64_t(1));
  EXPECT_EQ(bytesSnapshot.dropped_by_bytes, std::int64_t(2));
  EXPECT_EQ(bytesSnapshot.dropped_by_entries, std::int64_t(0));

  OptionalTelemetryQueueOptions<std::string> utf8Options;
  utf8Options.write = [](std::string) { return rt::Promise<bool>::resolve(true); };
  const OptionalTelemetryQueue<std::string> utf8Queue = createOptionalTelemetryQueue(utf8Options);
  EXPECT_EQ(utf8Queue.enqueue("é"), std::string("enqueued"));
  EXPECT_EQ(utf8Queue.snapshot().retained_bytes, 4.0);  // a two-byte character plus its JSON quotes
  EXPECT_EQ(utf8Queue.enqueue("😀"), std::string("enqueued"));
  EXPECT_EQ(utf8Queue.snapshot().retained_bytes, 10.0);  // a four-byte character is measured in UTF-8 bytes, not UTF-16 code units
  const rt::Promise<void> utf8Flush = utf8Queue.flush();
  co_await utf8Flush.wait();

  // `malformedQueue.enqueue(1n)`: a BigInt has no json::value form, so the second
  // unserializable payload is a nested NaN (refused by the same replacer).
  OptionalTelemetryQueueOptions<json::value> malformedOptions;
  malformedOptions.write = [](json::value) { return rt::Promise<bool>::resolve(true); };
  const OptionalTelemetryQueue<json::value> malformedQueue = createOptionalTelemetryQueue(malformedOptions);
  EXPECT_EQ(malformedQueue.enqueue(json::value(rt::Number::POSITIVE_INFINITY)), std::string("dropped_bytes"));
  json::object nestedNaN;
  nestedNaN["a"] = json::array{json::value(rt::Number::NaN)};
  EXPECT_EQ(malformedQueue.enqueue(json::value(nestedNaN)), std::string("dropped_bytes"));
  EXPECT_EQ(malformedQueue.snapshot().dropped_by_bytes, std::int64_t(2));
  EXPECT_EQ(malformedQueue.snapshot().retained_entries, std::int64_t(0));

  OptionalTelemetryQueueOptions<double> invalidMeasureOptions;
  invalidMeasureOptions.write = [](double) { return rt::Promise<bool>::resolve(true); };
  invalidMeasureOptions.measure = [](const double& value) { return value == 1 ? rt::Number::NaN : -1.0; };
  const OptionalTelemetryQueue<double> invalidMeasureQueue = createOptionalTelemetryQueue(invalidMeasureOptions);
  EXPECT_EQ(invalidMeasureQueue.enqueue(1), std::string("dropped_bytes"));  // a malformed size fails closed
  EXPECT_EQ(invalidMeasureQueue.enqueue(2), std::string("dropped_bytes"));
  EXPECT_EQ(invalidMeasureQueue.snapshot().retained_entries, std::int64_t(0));

  held.resolve(true);
  const rt::Promise<void> entriesFlush = entriesQueue.flush();
  co_await entriesFlush.wait();
  const rt::Promise<void> bytesFlush = bytesQueue.flush();
  co_await bytesFlush.wait();
}

ASYNC_TEST("optional telemetry queue drops queued samples past its age ceiling") {
  auto nowMs = std::make_shared<double>(NOW_MS);
  const rt::Resolvers<bool> held = rt::withResolvers<bool>();
  const rt::Promise<bool> heldPromise = held.promise;
  OptionalTelemetryQueueOptions<std::string> options;
  options.write = [heldPromise](std::string) { return heldPromise; };
  options.bounds = bounds(4, 4096, 1000, 1);
  options.now = [nowMs] { return *nowMs; };
  const OptionalTelemetryQueue<std::string> queue = createOptionalTelemetryQueue(options);
  EXPECT_EQ(queue.enqueue("in-flight"), std::string("enqueued"));
  EXPECT_EQ(queue.enqueue("stale"), std::string("enqueued"));
  EXPECT_EQ(queue.snapshot().queued_entries, std::int64_t(1));
  EXPECT_EQ(queue.snapshot().retained_entries, std::int64_t(2));

  *nowMs += 5000;
  EXPECT_EQ(queue.enqueue("fresh"), std::string("enqueued"));
  const OptionalTelemetryQueueSnapshot snapshot = queue.snapshot();
  EXPECT_EQ(snapshot.dropped_by_age, std::int64_t(1));  // a queued sample past the age ceiling is dropped instead of written
  EXPECT_EQ(snapshot.queued_entries, std::int64_t(1));
  EXPECT_EQ(snapshot.retained_entries, std::int64_t(2));  // the unresolved in-flight write keeps its charge
  EXPECT_EQ(snapshot.oldest_retained_age_ms, std::optional<double>(5000));

  held.resolve(true);
  const rt::Promise<void> flushed = queue.flush();
  co_await flushed.wait();
}

ASYNC_TEST("optional telemetry queue bounds writer concurrency and coalesces concurrent flushes") {
  auto gauge = std::make_shared<WriterGauge>();
  OptionalTelemetryQueueOptions<double> options;
  options.write = [gauge](double) { return rt::Promise<bool>(heldWrite(gauge)); };
  options.bounds = bounds(32, 4096, 60000, 2);
  const OptionalTelemetryQueue<double> queue = createOptionalTelemetryQueue(options);
  for (int index = 0; index < 6; index += 1) EXPECT_EQ(queue.enqueue(index), std::string("enqueued"));

  const std::int64_t coalescedBefore = queue.snapshot().coalesced_flushes;
  const rt::Promise<void> first = queue.flush();
  const rt::Promise<void> second = queue.flush();
  EXPECT(first == second);  // a concurrent flush coalesces onto the drain that is already running
  EXPECT_EQ(queue.snapshot().coalesced_flushes, coalescedBefore + 2);  // each concurrent flush is reported as coalesced
  EXPECT(queue.snapshot().writes_in_flight <= 2);  // writer concurrency never exceeds its bound

  for (int round = 0; round < 50; round += 1) {
    std::vector<rt::Resolve<void>> releases;
    releases.swap(gauge->releases);
    for (const rt::Resolve<void>& release : releases) release();
    co_await tick();
    const OptionalTelemetryQueueSnapshot snapshot = queue.snapshot();
    if (snapshot.queued_entries == 0 && snapshot.writes_in_flight == 0) break;
  }
  co_await first.wait();
  const OptionalTelemetryQueueSnapshot settled = queue.snapshot();
  EXPECT_EQ(settled.enqueued, std::int64_t(6));
  EXPECT_EQ(settled.delivered, std::int64_t(6));
  EXPECT_EQ(settled.failed, std::int64_t(0));
  EXPECT_EQ(settled.drain_in_progress, false);
  EXPECT_EQ(gauge->peakInFlight, 2);
}

ASYNC_TEST("optional telemetry queue dispatches an entry accepted while a completed drain retires") {
  auto written = std::make_shared<std::vector<double>>();
  OptionalTelemetryQueueOptions<double> options;
  options.write = [written](double entry) {
    written->push_back(entry);
    return rt::Promise<bool>::resolve(true);
  };
  const OptionalTelemetryQueue<double> queue = createOptionalTelemetryQueue(options);

  // The empty flush's loop has already returned, but its retirement microtask
  // has not run yet, so `drain` is still published when the entry is accepted.
  // Coalescing that enqueue onto the completed drain would strand the entry.
  const rt::Promise<void> retiringFlush = queue.flush();
  EXPECT_EQ(queue.snapshot().drain_in_progress, true);  // the completed drain stays published until its cleanup microtask runs
  EXPECT_EQ(queue.enqueue(7), std::string("enqueued"));

  co_await retiringFlush.wait();
  EXPECT_EQ(*written, std::vector<double>{7});  // an entry accepted during drain retirement must reach the sink without another enqueue or flush

  const rt::Promise<void> flushed = queue.flush();
  co_await flushed.wait();
  const OptionalTelemetryQueueSnapshot snapshot = queue.snapshot();
  EXPECT_EQ(snapshot.enqueued, std::int64_t(1));
  EXPECT_EQ(snapshot.delivered, std::int64_t(1));
  EXPECT_EQ(snapshot.failed, std::int64_t(0));
  EXPECT_EQ(snapshot.retained_entries, std::int64_t(0));
  EXPECT_EQ(snapshot.retained_bytes, 0.0);
  EXPECT_EQ(snapshot.queued_entries, std::int64_t(0));
  EXPECT_EQ(snapshot.drain_timeouts, std::int64_t(0));
  EXPECT_EQ(snapshot.drain_in_progress, false);
}

ASYNC_TEST("optional telemetry queue reports a failing sink without throwing or stalling the drain") {
  auto attempts = std::make_shared<int>(0);
  OptionalTelemetryQueueOptions<double> options;
  options.write = [attempts](double) {
    *attempts += 1;
    return rt::Promise<bool>::reject(rt::makeReason(rt::JsError("optional telemetry sink unavailable")));
  };
  const OptionalTelemetryQueue<double> queue = createOptionalTelemetryQueue(options);
  EXPECT_EQ(queue.enqueue(1), std::string("enqueued"));
  EXPECT_EQ(queue.enqueue(2), std::string("enqueued"));
  const rt::Promise<void> flushed = queue.flush();
  co_await flushed.wait();
  const OptionalTelemetryQueueSnapshot snapshot = queue.snapshot();
  EXPECT_EQ(*attempts, 2);
  EXPECT_EQ(snapshot.delivered, std::int64_t(0));
  EXPECT_EQ(snapshot.failed, std::int64_t(2));
  EXPECT_EQ(snapshot.retained_entries, std::int64_t(0));
  EXPECT_EQ(snapshot.retained_bytes, 0.0);
  EXPECT_EQ(snapshot.last_error_class, std::optional<std::string>("Error"));
  EXPECT_EQ(snapshot.drain_in_progress, false);

  OptionalTelemetryQueueOptions<double> refusingOptions;
  refusingOptions.write = [](double) { return rt::Promise<bool>::resolve(false); };
  const OptionalTelemetryQueue<double> refusing = createOptionalTelemetryQueue(refusingOptions);
  EXPECT_EQ(refusing.enqueue(1), std::string("enqueued"));
  const rt::Promise<void> refusingFlush = refusing.flush();
  co_await refusingFlush.wait();
  EXPECT_EQ(refusing.snapshot().failed, std::int64_t(1));  // a sink that declines an entry is reported as a failure

  EXPECT_EQ(queue.enqueue(3), std::string("enqueued"));
  const rt::Promise<void> closed = queue.close();
  co_await closed.wait();
  EXPECT_EQ(queue.enqueue(4), std::string("dropped_closed"));
  const OptionalTelemetryQueueSnapshot closedSnapshot = queue.snapshot();
  EXPECT_EQ(closedSnapshot.closed, true);
  EXPECT_EQ(closedSnapshot.shutdown_incomplete, false);  // a close with no unresolved work is complete
  EXPECT_EQ(closedSnapshot.retained_entries, std::int64_t(0));
  EXPECT_EQ(closedSnapshot.dropped_after_closed, std::int64_t(1));
  EXPECT_EQ(closedSnapshot.failed, std::int64_t(3));
}

ASYNC_TEST("optional telemetry queue bounds flush and close when a sink never settles") {
  const rt::Resolvers<bool> held = rt::withResolvers<bool>();
  const rt::Promise<bool> heldPromise = held.promise;
  OptionalTelemetryQueueOptions<double> options;
  options.write = [heldPromise](double) { return heldPromise; };
  options.bounds = bounds(4, 4096, 60000, 1, 50);
  const OptionalTelemetryQueue<double> queue = createOptionalTelemetryQueue(options);
  EXPECT_EQ(queue.enqueue(1), std::string("enqueued"));
  EXPECT_EQ(queue.enqueue(2), std::string("enqueued"));

  const double flushStartedAt = rt::performanceNow();
  const rt::Promise<void> flushed = queue.flush();
  co_await flushed.wait();
  const double flushMs = rt::performanceNow() - flushStartedAt;
  EXPECT(flushMs < 1000);  // a never-settling write must bound flush
  const OptionalTelemetryQueueSnapshot stalled = queue.snapshot();
  EXPECT_EQ(stalled.drain_timeouts, std::int64_t(1));  // an operation that runs out of its wait bound reports the timeout
  EXPECT_EQ(stalled.dispatch_stalled, true);
  EXPECT_EQ(stalled.writes_in_flight, std::int64_t(1));  // the stuck write retains its capacity
  EXPECT_EQ(stalled.retained_entries, std::int64_t(2));  // the unresolved write and the queued sample both stay charged
  EXPECT_EQ(stalled.queued_entries, std::int64_t(1));  // no new dispatch accumulates behind the stuck write
  EXPECT_EQ(stalled.delivered, std::int64_t(0));
  EXPECT_EQ(stalled.drain_in_progress, false);

  // A stalled queue never waits again, and nothing is dispatched automatically.
  const double repeatStartedAt = rt::performanceNow();
  const rt::Promise<void> repeated = queue.flush();
  co_await repeated.wait();
  EXPECT(rt::performanceNow() - repeatStartedAt < 1000);  // a stalled flush settles without waiting
  EXPECT_EQ(queue.snapshot().drain_timeouts, std::int64_t(1));
  EXPECT_EQ(queue.snapshot().delivered, std::int64_t(0));

  // close() is bounded too, and a close that cannot finish is terminal: the
  // unresolved work stays charged and reported instead of being written later.
  const double closeStartedAt = rt::performanceNow();
  const rt::Promise<void> closed = queue.close();
  co_await closed.wait();
  const double closeMs = rt::performanceNow() - closeStartedAt;
  EXPECT(closeMs < 1000);  // a never-settling write must bound close
  const OptionalTelemetryQueueSnapshot closedWhileStuck = queue.snapshot();
  EXPECT_EQ(closedWhileStuck.closed, true);
  EXPECT_EQ(closedWhileStuck.dispatch_stalled, true);
  EXPECT_EQ(closedWhileStuck.shutdown_incomplete, true);  // shutdown reports that unresolved work remains
  EXPECT_EQ(closedWhileStuck.retained_entries, std::int64_t(2));
  EXPECT_EQ(closedWhileStuck.queued_entries, std::int64_t(1));
  EXPECT_EQ(closedWhileStuck.writes_in_flight, std::int64_t(1));
  EXPECT_EQ(queue.enqueue(3), std::string("dropped_closed"));

  // Once the stuck write settles, capacity returns but a timed-out close never
  // dispatches the withheld sample: it stays accounted, never silently written.
  held.resolve(true);
  co_await tick();
  const rt::Promise<void> afterFlush = queue.flush();
  co_await afterFlush.wait();
  const OptionalTelemetryQueueSnapshot afterClose = queue.snapshot();
  EXPECT_EQ(afterClose.dispatch_stalled, false);  // capacity returned once the stuck write settled
  EXPECT_EQ(afterClose.delivered, std::int64_t(1));  // only the write already in flight completed
  EXPECT_EQ(afterClose.retained_entries, std::int64_t(1));  // the withheld sample stays charged
  EXPECT_EQ(afterClose.queued_entries, std::int64_t(1));
  EXPECT_EQ(afterClose.writes_in_flight, std::int64_t(0));
  EXPECT_EQ(afterClose.shutdown_incomplete, true);
}

ASYNC_TEST("optional telemetry queue bounds an operation whose sink progresses slower than its wait bound") {
  OptionalTelemetryQueueOptions<double> options;
  options.write = [](double) { return rt::Promise<bool>(delayedWrite()); };
  options.bounds = bounds(8, 4096, 60000, 1, 150);
  const OptionalTelemetryQueue<double> queue = createOptionalTelemetryQueue(options);
  for (int index = 0; index < 6; index += 1) EXPECT_EQ(queue.enqueue(index), std::string("enqueued"));

  const double startedAt = rt::performanceNow();
  const rt::Promise<void> flushed = queue.flush();
  co_await flushed.wait();
  const double elapsedMs = rt::performanceNow() - startedAt;
  const OptionalTelemetryQueueSnapshot afterFlush = queue.snapshot();
  EXPECT_EQ(afterFlush.drain_timeouts, std::int64_t(1));  // progress does not reset the operation's absolute wait bound
  EXPECT(afterFlush.delivered < 6);  // per-item progress must not extend the operation
  EXPECT(elapsedMs < 240);  // the operation ends at its absolute bound
  EXPECT_EQ(afterFlush.retained_entries, 6 - afterFlush.delivered);

  // Settle the write that was still in flight when the operation ran out of time.
  for (int round = 0; round < 50 && queue.snapshot().writes_in_flight > 0; round += 1) co_await delay(20);
  const OptionalTelemetryQueueSnapshot settled = queue.snapshot();
  EXPECT_EQ(settled.writes_in_flight, std::int64_t(0));  // the in-flight write settles without leaving a pending timer
  EXPECT(settled.delivered < 6);  // the timed-out operation never dispatched the rest of the queue
  EXPECT_EQ(settled.retained_entries, 6 - settled.delivered);
  EXPECT_EQ(settled.queued_entries, 6 - settled.delivered);
}

ASYNC_TEST("optional telemetry shutdown drains and closes the module queue") {
  const auto kv = std::make_shared<CountingKv>();
  uos::kv::setKvForTest(kv);
  // `finally { setKvForTest(null) }`. The module queue holds promises of this case's loop, so it is
  // dropped with it (the next case resets it first anyway).
  const auto restore = rt::scopeExit([] {
    resetOptionalPromptCacheAnalyticsForTest();
    uos::kv::setKvForTest(nullptr);
  });
  EXPECT(!optionalPromptCacheAnalyticsSnapshot().has_value());  // the module queue is created lazily
  const rt::Promise<void> flushed = flushOptionalPromptCacheAnalytics();
  co_await flushed.wait();

  auto enqueuing = enqueuePromptCacheAnalytics(event(), moduleOptions());
  const PromptCacheAnalyticsRecordResult queued = co_await std::move(enqueuing);
  EXPECT_EQ(queued.status, std::string("queued"));
  const std::optional<OptionalTelemetryQueueSnapshot> retained = optionalPromptCacheAnalyticsSnapshot();
  ASSERT(retained.has_value());  // the first optional sample creates the module queue
  EXPECT_EQ(retained->enqueued, std::int64_t(1));

  auto closing = closeOptionalPromptCacheAnalytics();
  co_await std::move(closing);
  const std::optional<OptionalTelemetryQueueSnapshot> settled = optionalPromptCacheAnalyticsSnapshot();
  ASSERT(settled.has_value());
  EXPECT_EQ(settled->closed, true);
  EXPECT_EQ(settled->failed, std::int64_t(0));  // the module queue sink must not report a failed write
  EXPECT_EQ(settled->drain_timeouts, std::int64_t(0));
  EXPECT_EQ(settled->retained_entries, std::int64_t(0));
  EXPECT_EQ(settled->shutdown_incomplete, false);
  EXPECT_EQ(settled->drain_in_progress, false);
  EXPECT_EQ(settled->delivered, std::int64_t(1));  // the shutdown drain delivers the retained sample to the environment KV sink
  EXPECT(kv->entries.size() > 0);  // the drain writes the same durable counters the direct path writes
  auto refusing = enqueuePromptCacheAnalytics(event(), moduleOptions());
  const PromptCacheAnalyticsRecordResult refused = co_await std::move(refusing);
  EXPECT_EQ(refused.reason, std::string("dropped_closed"));
}

ASYNC_TEST("optional telemetry close before lazy initialization is terminal") {
  const auto kv = std::make_shared<CountingKv>();
  resetOptionalPromptCacheAnalyticsForTest();
  uos::kv::setKvForTest(kv);
  const auto restore = rt::scopeExit([] {
    resetOptionalPromptCacheAnalyticsForTest();
    uos::kv::setKvForTest(nullptr);
  });
  EXPECT(!optionalPromptCacheAnalyticsSnapshot().has_value());  // no optional sample has created the module queue

  auto closing = closeOptionalPromptCacheAnalytics();
  co_await std::move(closing);
  EXPECT(!optionalPromptCacheAnalyticsSnapshot().has_value());  // a close before the first sample must not create the module queue

  auto enqueuing = enqueuePromptCacheAnalytics(event(), moduleOptions());
  const PromptCacheAnalyticsRecordResult refused = co_await std::move(enqueuing);
  EXPECT_EQ(refused.status, std::string("dropped"));  // an eligible sample after close must be refused, not queued
  EXPECT_EQ(refused.reason, std::string("dropped_closed"));

  const rt::Promise<void> flushed = flushOptionalPromptCacheAnalytics();
  co_await flushed.wait();
  EXPECT(!optionalPromptCacheAnalyticsSnapshot().has_value());  // a refused sample never creates or opens the module queue
  EXPECT_EQ(kv->entries.size(), std::size_t{0});  // a refused sample must not write durable counters
}

ASYNC_TEST("optional telemetry close refuses a sample already resolving when close arrives") {
  resetOptionalPromptCacheAnalyticsForTest();
  const auto restore = rt::scopeExit([] { resetOptionalPromptCacheAnalyticsForTest(); });
  // The sample suspends while it resolves its cohort hash, so the close lands
  // before it reaches the queue: it must be refused there, never create the
  // queue, and never restart the writing the close promised to stop.
  const rt::Promise<PromptCacheAnalyticsRecordResult> pending(enqueuePromptCacheAnalytics(event(), moduleOptions()));
  auto closing = closeOptionalPromptCacheAnalytics();
  co_await std::move(closing);
  const PromptCacheAnalyticsRecordResult refused = co_await pending.wait();

  EXPECT_EQ(refused.status, std::string("dropped"));
  EXPECT_EQ(refused.reason, std::string("dropped_closed"));
  EXPECT(!optionalPromptCacheAnalyticsSnapshot().has_value());  // an in-flight sample must not create the module queue after close
}

ASYNC_TEST("optional cache analytics: an inline delayed sink decides non-stream response latency") {
  tests::helpers::terminal_log_harness::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  const auto store = std::make_shared<CountingKv>();
  std::vector<PromptCacheAnalyticsRecordResult> sinkResults;
  auto terminalOptions = tests::helpers::terminal_log_harness::terminalOptions("telemetry-overhead-inline-non-stream");
  terminalOptions.recordCacheAnalytics = [store, &sinkResults](PromptCacheAnalyticsEvent input) -> rt::Task<PromptCacheAnalyticsRecordResult> {
    co_await delay(250);
    input.completed = true;
    input.usageTelemetryStatus = "reported";
    input.inputTokens = 1'000;
    input.cachedInputTokens = 400;
    input.cacheWriteInputTokens = 100;
    auto writing = recordPromptCacheAnalytics(input, options(store));
    auto recorded = co_await std::move(writing);
    sinkResults.push_back(recorded);
    co_return recorded;
  };
  const double startedAt = rt::performanceNow();
  auto wrapped = handler::terminal_log::withTerminalRequestLog(tests::helpers::terminal_log_harness::jsonResponse("complete", 200, "chatgpt_codex"), terminalOptions);
  auto response = co_await wrapped.wait();
  const double elapsed = rt::performanceNow() - startedAt;
  EXPECT_EQ(response.status(), 200);
  auto text = co_await response.text();
  EXPECT_EQ(text, std::string("complete"));
  ASSERT(sinkResults.size() == 1);
  EXPECT_EQ(sinkResults[0].status, std::string("recorded"));
  EXPECT_EQ(sinkResults[0].reason, std::string("recorded"));
  EXPECT(store->entries.size() > 0);
  EXPECT(elapsed >= 225);
}

ASYNC_TEST("optional cache analytics: a failing inline sink loses the sample in one attempt") {
  tests::helpers::terminal_log_harness::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  int attempts = 0;
  auto terminalOptions = tests::helpers::terminal_log_harness::terminalOptions("telemetry-overhead-inline-failing");
  terminalOptions.recordCacheAnalytics = [&attempts](PromptCacheAnalyticsEvent) {
    ++attempts;
    return rt::Promise<PromptCacheAnalyticsRecordResult>::reject(rt::makeReason(rt::JsError("optional cache analytics sink unavailable")));
  };
  const double startedAt = rt::performanceNow();
  auto wrapped = handler::terminal_log::withTerminalRequestLog(tests::helpers::terminal_log_harness::jsonResponse("complete", 200, "chatgpt_codex"), terminalOptions);
  auto response = co_await wrapped.wait();
  const double elapsed = rt::performanceNow() - startedAt;
  EXPECT_EQ(response.status(), 200);
  auto text = co_await response.text();
  EXPECT_EQ(text, std::string("complete"));
  EXPECT_EQ(attempts, 1);
  EXPECT(elapsed < 250);
}

ASYNC_TEST("queued optional analytics leaves the non-stream response off the sink path and records the same counters") {
  tests::helpers::terminal_log_harness::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  const auto store = std::make_shared<CountingKv>();
  std::vector<PromptCacheAnalyticsRecordResult> queuedResults;
  std::vector<bool> sinkResults;
  using Entry = cache::prompt_analytics_queue::PromptCacheAnalyticsQueueEntry;
  OptionalTelemetryQueueOptions<Entry> queueOptions;
  queueOptions.write = [&sinkResults](Entry entry) -> rt::Task<bool> {
    co_await delay(250);
    auto writing = cache::prompt_analytics_queue::writePromptCacheAnalyticsQueueEntry(entry);
    const bool recorded = co_await std::move(writing);
    sinkResults.push_back(recorded);
    co_return recorded;
  };
  const auto queue = createOptionalTelemetryQueue(queueOptions);
  auto terminalOptions = tests::helpers::terminal_log_harness::terminalOptions("telemetry-overhead-queued-non-stream");
  terminalOptions.recordCacheAnalytics = [queue, store, &queuedResults](PromptCacheAnalyticsEvent input) -> rt::Task<PromptCacheAnalyticsRecordResult> {
    input.completed = true;
    input.usageTelemetryStatus = "reported";
    input.inputTokens = 1'000;
    input.cachedInputTokens = 400;
    input.cacheWriteInputTokens = 100;
    PromptCacheAnalyticsEnqueueOptions enqueueOptions;
    static_cast<PromptCacheAnalyticsOptions&>(enqueueOptions) = options(store);
    enqueueOptions.queue = queue;
    auto enqueuing = enqueuePromptCacheAnalytics(input, enqueueOptions);
    auto queued = co_await std::move(enqueuing);
    queuedResults.push_back(queued);
    co_return queued;
  };
  const double startedAt = rt::performanceNow();
  auto wrapped = handler::terminal_log::withTerminalRequestLog(tests::helpers::terminal_log_harness::jsonResponse("complete", 200, "chatgpt_codex"), terminalOptions);
  auto response = co_await wrapped.wait();
  const double elapsed = rt::performanceNow() - startedAt;
  EXPECT_EQ(response.status(), 200);
  auto text = co_await response.text();
  EXPECT_EQ(text, std::string("complete"));
  ASSERT(queuedResults.size() == 1);
  EXPECT_EQ(queuedResults[0].status, std::string("queued"));
  EXPECT_EQ(queuedResults[0].reason, std::string("queued"));
  EXPECT_EQ(queue.snapshot().enqueued, std::int64_t{1});
  EXPECT(elapsed < 125);
  const auto closing = queue.close();
  co_await closing.wait();
  const auto snapshot = queue.snapshot();
  EXPECT_EQ(sinkResults, std::vector<bool>{true});
  EXPECT_EQ(snapshot.delivered, std::int64_t{1});
  EXPECT_EQ(snapshot.failed, std::int64_t{0});
  EXPECT_EQ(snapshot.retained_entries, std::int64_t{0});
  EXPECT_EQ(snapshot.retained_bytes, 0.0);
  EXPECT_EQ(snapshot.dropped_by_age, std::int64_t{0});
  EXPECT_EQ(snapshot.drain_timeouts, std::int64_t{0});
  EXPECT(!snapshot.drain_in_progress);
  EXPECT(!snapshot.shutdown_incomplete);
  const auto reference = std::make_shared<CountingKv>();
  auto recording = recordPromptCacheAnalytics(handlerTerminalEvent(), options(reference));
  const auto recorded = co_await std::move(recording);
  EXPECT_EQ(recorded.status, std::string("recorded"));
  EXPECT_EQ(store->entries.size(), reference->entries.size());
  for (const auto& [key, entry] : store->entries) {
    const auto* expected = reference->entries.get(key);
    ASSERT(expected != nullptr);
    EXPECT(entry.value == expected->value);
  }
}

ASYNC_TEST("queued optional analytics retains only sanitized metadata and never drops required evidence") {
  tests::helpers::terminal_log_harness::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  using Entry = cache::prompt_analytics_queue::PromptCacheAnalyticsQueueEntry;
  const std::string rawModel = "gpt-telemetry-secret-model-must-not-be-retained";
  std::vector<Entry> retained;
  OptionalTelemetryQueueOptions<Entry> queueOptions;
  queueOptions.write = [&retained](Entry entry) { retained.push_back(entry); return rt::Promise<bool>::resolve(true); };
  const auto queue = createOptionalTelemetryQueue(queueOptions);
  auto enqueueOptions = moduleOptions();
  enqueueOptions.queue = queue;
  auto input = event();
  input.model = rawModel;
  const rt::Promise<PromptCacheAnalyticsRecordResult> enqueuing(enqueuePromptCacheAnalytics(input, enqueueOptions));
  const auto queued = co_await enqueuing.wait();
  EXPECT_EQ(queued.status, std::string("queued"));
  EXPECT_EQ(queued.reason, std::string("queued"));
  EXPECT_EQ(queued.bucket_start_at_ms, std::optional<double>(rt::Math::floor(NOW_MS / (15 * 60'000)) * (15 * 60'000)));
  ASSERT(retained.size() == 1);
  const auto serialized = rt::jsonStringify(json::value_from(retained[0]));
  EXPECT(!rt::includes(serialized, rawModel));
  EXPECT(rt::Regex("[a-f0-9]{64}").test(serialized));
  EXPECT(rt::length(serialized) < 1'024);
  EXPECT_EQ(retained[0].usage_kind, std::string("reported"));
  EXPECT_EQ(retained[0].deltas.input_tokens, std::optional<double>(1'000));
  EXPECT_EQ(retained[0].deltas.cached_input_tokens, std::optional<double>(400));
  input = event();
  input.usageTelemetryStatus = "missing";
  input.inputTokens.reset();
  input.cachedInputTokens.reset();
  const rt::Promise<PromptCacheAnalyticsRecordResult> missingWrite(enqueuePromptCacheAnalytics(input, enqueueOptions));
  const auto missing = co_await missingWrite.wait();
  EXPECT_EQ(missing.status, std::string("queued"));
  // Bun's native WebCrypto completion gives the first drain another task turn. With rt's
  // synchronous digest, the same source test fails under a Promise.resolve digest oracle too;
  // await the existing bounded drain before inspecting the second retained sample.
  const auto sampled = queue.flush();
  co_await sampled.wait();
  ASSERT(retained.size() == 2);
  EXPECT_EQ(retained[1].usage_kind, std::string("missing"));
  EXPECT(rt::deepEqual(json::value_from(retained[1].deltas), rt::jsonParse(R"({"sample_count":1})")));
  OptionalTelemetryQueueOptions<Entry> closedOptions;
  closedOptions.write = [](Entry) { return rt::Promise<bool>::resolve(true); };
  const auto closedQueue = createOptionalTelemetryQueue(closedOptions);
  const auto closing = closedQueue.close();
  co_await closing.wait();
  enqueueOptions.queue = closedQueue;
  auto direct = enqueuePromptCacheAnalytics(event(), enqueueOptions);
  const auto dropped = co_await std::move(direct);
  EXPECT_EQ(dropped.status, std::string("dropped"));
  EXPECT_EQ(dropped.reason, std::string("dropped_closed"));
  int adminErrors = 0;
  std::vector<PromptCacheAnalyticsRecordResult> handlerAnalytics;
  auto terminalOptions = tests::helpers::terminal_log_harness::terminalOptions("telemetry-overhead-required-evidence");
  terminalOptions.recordCacheAnalytics = [enqueueOptions, &handlerAnalytics](PromptCacheAnalyticsEvent sample) -> rt::Task<PromptCacheAnalyticsRecordResult> {
    auto task = enqueuePromptCacheAnalytics(sample, enqueueOptions);
    const auto result = co_await std::move(task);
    handlerAnalytics.push_back(result);
    co_return result;
  };
  terminalOptions.recordAdminError = [&adminErrors](auto) { ++adminErrors; return rt::Promise<void>::resolve(); };
  auto wrapped = handler::terminal_log::withTerminalRequestLog(tests::helpers::terminal_log_harness::jsonResponse("complete"), terminalOptions);
  auto response = co_await wrapped.wait();
  EXPECT_EQ(response.status(), 200);
  auto text = co_await response.text();
  EXPECT_EQ(text, std::string("complete"));
  ASSERT(handlerAnalytics.size() == 1);
  EXPECT_EQ(handlerAnalytics[0].status, std::string("ignored"));
  EXPECT_EQ(handlerAnalytics[0].reason, std::string("not_completed_2xx"));
  EXPECT_EQ(adminErrors, 1);
  EXPECT_EQ(closedQueue.snapshot().delivered, std::int64_t{0});
  EXPECT_EQ(closedQueue.snapshot().dropped_after_closed, std::int64_t{1});
  const auto flushing = queue.flush();
  co_await flushing.wait();
}
