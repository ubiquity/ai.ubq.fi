#pragma once
// Port of src/optional-telemetry-queue.ts.
//
// A small bounded, best-effort queue for optional telemetry writes.
//
// Optional telemetry must never decide client-visible latency or response
// truthfulness, and it must never grow without a ceiling when its sink is slow
// or unavailable. This primitive therefore owns five internal bounds - retained
// entries, retained payload bytes, entry age, concurrent writer calls and the
// time a drain waits for progress - and reports every drop, failure, timeout,
// coalesced flush and drain in its snapshot.
//
// It is deliberately not a general job framework: it has one writer, one FIFO
// of sanitized entries, and no retry policy. Required durable evidence (quota
// accounting, admin errors, Sentinel replay capture) is never routed here.
//
// The queue is generic over its entry type, as in the TS, so the implementation of
// createOptionalTelemetryQueue lives in this header (namespace detail).
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/prelude.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace uos::optional_telemetry_queue {
  struct OptionalTelemetryQueueBounds {
    // Maximum charged entries, including unresolved in-flight writes.
    double maxEntries = 0;
    // Maximum charged UTF-8 payload bytes, including unresolved in-flight writes.
    double maxBytes = 0;
    // Maximum age of a queued entry before it is dropped instead of written.
    double maxAgeMs = 0;
    // Maximum writer calls in flight at once.
    double maxConcurrentWrites = 0;
    // Absolute wait bound for one drain operation. Progress does not reset it: a
    // drain that runs out of it ends instead of hanging, stops dispatching, keeps
    // the unresolved writes charged, and reports the timeout, so `flush` and
    // `close` always settle.
    double maxDrainWaitMs = 0;
  };

  // `Partial<OptionalTelemetryQueueBounds>`
  struct OptionalTelemetryQueuePartialBounds {
    std::optional<double> maxEntries;
    std::optional<double> maxBytes;
    std::optional<double> maxAgeMs;
    std::optional<double> maxConcurrentWrites;
    std::optional<double> maxDrainWaitMs;
  };

  // Conservative internal limits. They are not operator knobs: there is no
  // environment variable, CLI flag or configuration surface for them.
  inline constexpr OptionalTelemetryQueueBounds OPTIONAL_TELEMETRY_QUEUE_BOUNDS{256, 256 * 1024, 60000, 4, 5000};

  // "enqueued" | "dropped_closed" | "dropped_capacity" | "dropped_bytes"
  using OptionalTelemetryEnqueueOutcome = std::string;

  struct OptionalTelemetryQueueSnapshot {
    OptionalTelemetryQueueBounds bounds;
    // Charged entries, including unresolved in-flight writes.
    std::int64_t retained_entries = 0;
    // Charged UTF-8 payload bytes, including unresolved in-flight writes.
    double retained_bytes = 0;
    // Charged entries waiting to be dispatched.
    std::int64_t queued_entries = 0;
    // Age of the oldest charged entry, including unresolved in-flight writes.
    std::optional<double> oldest_retained_age_ms;
    std::int64_t writes_in_flight = 0;
    bool drain_in_progress = false;
    // Drains that ended because the operation ran out of its wait bound.
    std::int64_t drain_timeouts = 0;
    // True while stuck in-flight writes retain capacity and new dispatch is stopped.
    bool dispatch_stalled = false;
    // close() ended with unresolved retained work (a timeout, or work already
    // withheld by a stall). The queue never dispatches again; the unresolved
    // charges stay reported instead of being silently written or forgotten.
    bool shutdown_incomplete = false;
    std::int64_t coalesced_flushes = 0;
    bool closed = false;
    std::int64_t enqueued = 0;
    std::int64_t delivered = 0;
    std::int64_t failed = 0;
    std::int64_t dropped_by_entries = 0;
    std::int64_t dropped_by_bytes = 0;
    std::int64_t dropped_by_age = 0;
    std::int64_t dropped_after_closed = 0;
    // Error class name only; never a message, key or payload.
    std::optional<std::string> last_error_class;
  };

  // Resolves true when the entry was recorded durably, false when it was not.
  template <class TEntry> using OptionalTelemetryWriter = std::function<rt::Promise<bool>(TEntry entry)>;

  template <class TEntry> struct OptionalTelemetryQueue {
    std::function<OptionalTelemetryEnqueueOutcome(TEntry entry)> enqueue;
    // Starts or joins one bounded drain operation. The wait bound is absolute for
    // that operation: progress does not reset it. Always settles within
    // `maxDrainWaitMs`, reporting `drain_timeouts`/`dispatch_stalled` instead of
    // hanging; a later explicit flush may dispatch once capacity is free again.
    std::function<rt::Promise<void>()> flush;
    // Stops accepting entries, then performs one bounded drain operation. If
    // unresolved work remains, the queue reports `shutdown_incomplete` and never
    // dispatches again, so shutdown can neither hang nor write after giving up.
    std::function<rt::Promise<void>()> close;
    std::function<OptionalTelemetryQueueSnapshot()> snapshot;
  };

  template <class TEntry> struct OptionalTelemetryQueueOptions {
    OptionalTelemetryWriter<TEntry> write;
    OptionalTelemetryQueuePartialBounds bounds;
    // An empty function is undefined (`Date.now`).
    std::function<double()> now;
    // Charged UTF-8 payload bytes of one entry. The default measures the JSON
    // payload, which excludes any process-local delivery handle the caller stores
    // on the entry; bounded metadata is what the byte ceiling is meant to cap. A
    // non-finite or negative result fails closed: the entry is dropped, never
    // charged as free. An empty function is undefined (the default, which needs an
    // entry type with a JSON form, `json::has_value_from`).
    std::function<double(const TEntry& entry)> measure;
  };

  // Measures one JSON payload in real UTF-8 bytes. Returns `NaN` when the payload
  // cannot be serialized faithfully (a cycle, a BigInt, or a non-finite number
  // that JSON would silently rewrite as `null`), so the caller drops instead of
  // charging a wrong size. A json::value cannot hold a cycle or a BigInt.
  double measureJsonPayloadBytes(const json::value& value);

  namespace detail {
    double boundedInteger(const std::optional<double>& value, double fallback);
    // `error instanceof Error ? error.name : typeof error`
    std::string errorClass(const rt::Reason& error);

    template <class TEntry> struct PendingEntry {
      TEntry entry;
      double bytes = 0;
      double enqueued_at_ms = 0;
    };

    template <class TEntry> struct QueueState {
      OptionalTelemetryWriter<TEntry> write;
      OptionalTelemetryQueueBounds bounds;
      std::function<double()> now;
      std::function<double(const TEntry&)> measure;
      std::deque<PendingEntry<TEntry>> queued;
      // Dispatched writes that have not settled, with the charge they still hold
      // (`Map<Promise<void>, PendingEntry>`, keyed by the write's promise).
      std::vector<std::pair<rt::Promise<void>, PendingEntry<TEntry>>> inFlight;
      struct Counters {
        std::int64_t enqueued = 0;
        std::int64_t delivered = 0;
        std::int64_t failed = 0;
        std::int64_t dropped_by_entries = 0;
        std::int64_t dropped_by_bytes = 0;
        std::int64_t dropped_by_age = 0;
        std::int64_t dropped_after_closed = 0;
        std::int64_t coalesced_flushes = 0;
        std::int64_t drain_timeouts = 0;
      } counters;
      std::int64_t retainedEntries = 0;
      double retainedBytes = 0;
      std::optional<rt::Promise<void>> drain;
      // True while the published drain's loop can still dispatch. The loop clears it
      // synchronously as it returns - before the retirement microtask clears `drain`
      // - so a call in that window starts a fresh drain instead of coalescing onto
      // one that can no longer dispatch the entry it just accepted.
      bool drainLoopActive = false;
      bool closed = false;
      bool stalled = false;
      bool shutdownIncomplete = false;
      std::optional<std::string> lastErrorClass;
    };

    template <class TEntry> using State = std::shared_ptr<QueueState<TEntry>>;

    template <class TEntry> std::function<double(const TEntry&)> defaultMeasure() {
      if constexpr (json::has_value_from<TEntry>::value) {
        return [](const TEntry& entry) { return measureJsonPayloadBytes(json::value_from(entry)); };
      } else {
        return nullptr;
      }
    }

    // Reads the shutdown flag through a call. A drain started earlier can set it
    // while a later drain call is being set up.
    template <class TEntry> bool drainIsIncomplete(const QueueState<TEntry>& state) { return state.shutdownIncomplete; }

    template <class TEntry> double timestamp(const QueueState<TEntry>& state) {
      const double value = rt::Math::trunc(state.now());
      return rt::Number::isSafeInteger(value) && value >= 0 ? value : rt::dateNow();
    }

    // Releases the charge of one entry. It is held until the write settles, so the
    // bounds cover in-flight work too.
    template <class TEntry> void releaseCharge(QueueState<TEntry>& state, double bytes) {
      state.retainedEntries -= 1;
      state.retainedBytes -= bytes;
    }

    // Drops queued entries that outlived the age ceiling. Dispatched writes keep
    // their charge until they settle; the queue never abandons an unresolved
    // write by forgetting it.
    template <class TEntry> void dropExpiredQueued(QueueState<TEntry>& state) {
      if (state.queued.empty()) return;
      const double cutoff = timestamp(state) - state.bounds.maxAgeMs;
      for (auto index = static_cast<std::int64_t>(state.queued.size()) - 1; index >= 0; index -= 1) {
        const auto position = static_cast<std::size_t>(index);
        if (state.queued[position].enqueued_at_ms > cutoff) continue;
        releaseCharge(state, state.queued[position].bytes);
        state.queued.erase(state.queued.begin() + index);
        state.counters.dropped_by_age += 1;
      }
    }

    template <class TEntry> bool isExpired(const QueueState<TEntry>& state, const PendingEntry<TEntry>& item) {
      return timestamp(state) - item.enqueued_at_ms >= state.bounds.maxAgeMs;
    }

    // The body of writeOne's `(async () => { ... })()`.
    template <class TEntry> rt::Task<void> writeEntry(State<TEntry> state, TEntry entry) {
      try {
        rt::Promise<bool> written = state->write(std::move(entry));
        const bool recorded = co_await written.wait();
        if (recorded) state->counters.delivered += 1;
        else state->counters.failed += 1;
      } catch (...) {
        state->counters.failed += 1;
        state->lastErrorClass = errorClass(std::current_exception());
      }
    }

    template <class TEntry> void writeOne(const State<TEntry>& state, PendingEntry<TEntry> item) {
      const rt::Promise<void> task(writeEntry(state, item.entry));
      const double bytes = item.bytes;
      state->inFlight.emplace_back(task, std::move(item));
      (void)task.finally_([state, task, bytes] {
        const auto found = std::find_if(state->inFlight.begin(), state->inFlight.end(), [&](const auto& entry) { return entry.first == task; });
        if (found != state->inFlight.end()) state->inFlight.erase(found);
        releaseCharge(*state, bytes);
        // Full capacity is free again, so a stalled drain may dispatch once more.
        if (state->inFlight.empty()) state->stalled = false;
      });
    }

    // Waits for any in-flight write to settle, at most `timeoutMs`. Resolves
    // false when no write made progress in time; the race never cancels a write,
    // so the caller keeps its charge until it settles instead of orphaning it.
    template <class TEntry> rt::Task<bool> waitForProgress(State<TEntry> state, double timeoutMs) {
      if (state->inFlight.empty()) co_return true;
      std::function<void()> cancelTimeout = [] {};
      std::vector<rt::Promise<void>> writes;
      for (const auto& entry : state->inFlight) writes.push_back(entry.first);
      const rt::Promise<bool> progressed = rt::race(std::move(writes)).then([] { return true; });
      const rt::Promise<bool> timedOut = rt::newPromise<bool>([&cancelTimeout, timeoutMs](rt::Resolve<bool> resolve, rt::Reject) {
        const rt::Timeout timer = rt::setTimeout([resolve] { resolve(false); }, timeoutMs);
        cancelTimeout = [timer, resolve] {
          rt::clearTimeout(timer);
          resolve(false);
        };
      });
      std::vector<rt::Promise<bool>> racers = {progressed, timedOut};
      const rt::Promise<bool> raced = rt::race(std::move(racers));
      rt::Reason failure;
      bool result = false;
      try {
        result = co_await raced.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      cancelTimeout();
      if (failure) std::rethrow_exception(failure);
      co_return result;
    }

    template <class TEntry> void dispatchNextQueued(const State<TEntry>& state) {
      if (state->queued.empty()) return;
      PendingEntry<TEntry> item = std::move(state->queued.front());
      state->queued.pop_front();
      if (isExpired(*state, item)) {
        releaseCharge(*state, item.bytes);
        state->counters.dropped_by_age += 1;
        return;
      }
      writeOne(state, std::move(item));
    }

    // True while a queued entry can be dispatched inside the writer bound.
    template <class TEntry> bool hasDispatchCapacity(const QueueState<TEntry>& state) {
      return !state.queued.empty() && static_cast<double>(state.inFlight.size()) < state.bounds.maxConcurrentWrites;
    }

    // Writes queued entries while keeping at most `maxConcurrentWrites` writer
    // calls in flight, and drops queued entries that outlived the age ceiling.
    //
    // The wait bound is absolute for this drain operation: it is measured once
    // from the operation's start, so a slowly progressing sink cannot extend it
    // one item at a time. Running out of it ends the operation with
    // `drain_timeouts` and `dispatch_stalled` set; the unresolved writes keep
    // their charge and no further dispatch happens until full capacity is free.
    // This is the `try` body of drainPending, which clears `drainLoopActive` as soon as
    // it returns, before the drain promise settles.
    template <class TEntry> rt::Task<void> drainLoop(State<TEntry> state) {
      if (drainIsIncomplete(*state)) co_return;
      const double deadlineAtMs = timestamp(*state) + state->bounds.maxDrainWaitMs;
      for (;;) {
        dropExpiredQueued(*state);
        if (drainIsIncomplete(*state)) co_return;
        if (state->stalled && !state->inFlight.empty()) co_return;
        while (hasDispatchCapacity(*state)) dispatchNextQueued(state);
        if (state->queued.empty() && state->inFlight.empty()) co_return;
        const double remainingMs = deadlineAtMs - timestamp(*state);
        if (remainingMs > 0) {
          const rt::Promise<bool> waiting(waitForProgress(state, remainingMs));
          const bool progressed = co_await waiting.wait();
          if (progressed) continue;
        }
        break;
      }
      state->counters.drain_timeouts += 1;
      state->stalled = true;
    }

    template <class TEntry> rt::Task<void> drainPending(State<TEntry> state) {
      state->drainLoopActive = true;
      rt::Reason failure;
      try {
        co_await drainLoop(state);
      } catch (...) {
        failure = std::current_exception();
      }
      // Cleared as the loop returns, so the retirement window below cannot
      // swallow an entry accepted after the loop's final empty check.
      state->drainLoopActive = false;
      if (failure) std::rethrow_exception(failure);
    }

    template <class TEntry> rt::Promise<void> startDrain(const State<TEntry>& state) {
      const std::optional<rt::Promise<void>> running = state->drain;
      // A drain whose loop already returned cannot dispatch newly accepted work,
      // even though its retirement microtask has not cleared `drain` yet.
      // Coalescing onto it would leave that accepted entry without a dispatch, so
      // start a fresh bounded drain; the retiring one still settles its awaiters.
      if (running && state->drainLoopActive) {
        state->counters.coalesced_flushes += 1;
        return *running;
      }
      // The drain promise is published before its loop can run, so a drain that
      // completes synchronously (or coalesces concurrently) still clears the
      // right state and never leaves a settled promise behind as `drain`.
      const rt::Resolvers<void> resolvers = rt::withResolvers<void>();
      const rt::Promise<void> current = resolvers.promise;
      state->drain = current;
      const rt::Promise<void> pending(drainPending(state));
      (void)pending
        .catch_([state](const rt::Reason& error) {
          state->counters.failed += 1;
          state->lastErrorClass = errorClass(error);
        })
        .then([state, current, settle = resolvers.resolve] {
          if (state->drain && *state->drain == current) state->drain.reset();
          settle();
        });
      return current;
    }

    template <class TEntry> OptionalTelemetryEnqueueOutcome enqueue(const State<TEntry>& state, TEntry entry) {
      if (state->closed) {
        state->counters.dropped_after_closed += 1;
        return "dropped_closed";
      }
      dropExpiredQueued(*state);
      const double size = state->measure(entry);
      if (!rt::Number::isFinite(size) || size < 0) {
        state->counters.dropped_by_bytes += 1;
        return "dropped_bytes";
      }
      const double bytes = rt::Math::ceil(size);
      // The charge covers every retained entry, including unresolved in-flight
      // writes, so a stuck sink cannot let extra entries past the ceilings.
      if (static_cast<double>(state->retainedEntries) >= state->bounds.maxEntries) {
        state->counters.dropped_by_entries += 1;
        return "dropped_capacity";
      }
      if (state->retainedBytes + bytes > state->bounds.maxBytes) {
        state->counters.dropped_by_bytes += 1;
        return "dropped_bytes";
      }
      state->queued.push_back(PendingEntry<TEntry>{std::move(entry), bytes, timestamp(*state)});
      state->retainedEntries += 1;
      state->retainedBytes += bytes;
      state->counters.enqueued += 1;
      // Best effort: a writer fault is counted in the snapshot, never surfaced as
      // an unhandled rejection on the request path.
      (void)startDrain(state).catch_(rt::ignoreRejection);
      return "enqueued";
    }

    template <class TEntry> rt::Task<void> close(State<TEntry> state) {
      state->closed = true;
      // One bounded drain operation. If it ends with unresolved work, shutdown is
      // reported incomplete and the queue never dispatches again: shutdown can
      // neither hang nor write after giving up.
      if (!state->shutdownIncomplete) {
        const rt::Promise<void> drained = startDrain(state);
        co_await drained.wait();
        if (state->retainedEntries > 0 || !state->inFlight.empty() || !state->queued.empty()) state->shutdownIncomplete = true;
      }
    }

    template <class TEntry> std::optional<double> oldestRetainedAgeMs(const QueueState<TEntry>& state) {
      if (state.retainedEntries == 0) return std::nullopt;
      std::optional<double> oldestAtMs;
      for (const PendingEntry<TEntry>& item : state.queued) {
        if (!oldestAtMs || item.enqueued_at_ms < *oldestAtMs) oldestAtMs = item.enqueued_at_ms;
      }
      for (const auto& entry : state.inFlight) {
        if (!oldestAtMs || entry.second.enqueued_at_ms < *oldestAtMs) oldestAtMs = entry.second.enqueued_at_ms;
      }
      if (!oldestAtMs) return std::nullopt;
      return rt::Math::max(0, timestamp(state) - *oldestAtMs);
    }

    template <class TEntry> OptionalTelemetryQueueSnapshot snapshot(const QueueState<TEntry>& state) {
      OptionalTelemetryQueueSnapshot out;
      out.bounds = state.bounds;
      out.retained_entries = state.retainedEntries;
      out.retained_bytes = state.retainedBytes;
      out.queued_entries = static_cast<std::int64_t>(state.queued.size());
      out.oldest_retained_age_ms = oldestRetainedAgeMs(state);
      out.writes_in_flight = static_cast<std::int64_t>(state.inFlight.size());
      out.drain_in_progress = state.drain.has_value();
      out.drain_timeouts = state.counters.drain_timeouts;
      out.dispatch_stalled = state.stalled;
      out.shutdown_incomplete = state.shutdownIncomplete;
      out.coalesced_flushes = state.counters.coalesced_flushes;
      out.closed = state.closed;
      out.enqueued = state.counters.enqueued;
      out.delivered = state.counters.delivered;
      out.failed = state.counters.failed;
      out.dropped_by_entries = state.counters.dropped_by_entries;
      out.dropped_by_bytes = state.counters.dropped_by_bytes;
      out.dropped_by_age = state.counters.dropped_by_age;
      out.dropped_after_closed = state.counters.dropped_after_closed;
      out.last_error_class = state.lastErrorClass;
      return out;
    }
  }

  // Throws std::logic_error (a translation error, no JS behaviour) when `measure` is
  // absent and the entry type has no JSON form for the default measure.
  template <class TEntry> OptionalTelemetryQueue<TEntry> createOptionalTelemetryQueue(OptionalTelemetryQueueOptions<TEntry> options) {
    auto state = std::make_shared<detail::QueueState<TEntry>>();
    state->bounds = OptionalTelemetryQueueBounds{
      detail::boundedInteger(options.bounds.maxEntries, OPTIONAL_TELEMETRY_QUEUE_BOUNDS.maxEntries),
      detail::boundedInteger(options.bounds.maxBytes, OPTIONAL_TELEMETRY_QUEUE_BOUNDS.maxBytes),
      detail::boundedInteger(options.bounds.maxAgeMs, OPTIONAL_TELEMETRY_QUEUE_BOUNDS.maxAgeMs),
      detail::boundedInteger(options.bounds.maxConcurrentWrites, OPTIONAL_TELEMETRY_QUEUE_BOUNDS.maxConcurrentWrites),
      detail::boundedInteger(options.bounds.maxDrainWaitMs, OPTIONAL_TELEMETRY_QUEUE_BOUNDS.maxDrainWaitMs),
    };
    state->now = options.now ? std::move(options.now) : rt::globalDateNow();
    state->measure = options.measure ? std::move(options.measure) : detail::defaultMeasure<TEntry>();
    if (!state->measure) throw std::logic_error("createOptionalTelemetryQueue: this entry type has no JSON form; pass options.measure");
    state->write = std::move(options.write);
    OptionalTelemetryQueue<TEntry> queue;
    queue.enqueue = [state](TEntry entry) { return detail::enqueue(state, std::move(entry)); };
    queue.flush = [state] { return detail::startDrain(state); };
    queue.close = [state] { return rt::Promise<void>(detail::close(state)); };
    queue.snapshot = [state] { return detail::snapshot(*state); };
    return queue;
  }
}
