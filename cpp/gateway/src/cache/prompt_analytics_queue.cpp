#include "uos/cache/prompt_analytics_queue.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"

#include <string_view>
#include <utility>
#include <variant>

namespace uos::cache::prompt_analytics_queue {
  using optional_telemetry_queue::createOptionalTelemetryQueue;
  using optional_telemetry_queue::measureJsonPayloadBytes;
  using optional_telemetry_queue::OptionalTelemetryQueue;
  using optional_telemetry_queue::OptionalTelemetryQueueOptions;
  using optional_telemetry_queue::OptionalTelemetryQueueSnapshot;
  using prompt_analytics_core::alignedBucketStart;
  using prompt_analytics_core::asFallback;
  using prompt_analytics_core::asMode;
  using prompt_analytics_core::asProvider;
  using prompt_analytics_core::asRoute;
  using prompt_analytics_core::commitCohortOutcome;
  using prompt_analytics_core::COUNTERS;
  using prompt_analytics_core::promptCacheAnalyticsTarget;
  using prompt_analytics_core::recordResult;
  using prompt_analytics_core::recordUsage;
  using prompt_analytics_core::resolveCohort;
  using prompt_analytics_core::resolveKv;
  using prompt_analytics_core::safeCounter;
  using prompt_analytics_core::safeNow;
  using prompt_analytics_core::CounterDeltas;
  using prompt_analytics_core::PromptCacheAnalyticsCohort;
  using prompt_analytics_core::PromptCacheAnalyticsEvent;
  using prompt_analytics_core::PromptCacheAnalyticsOptions;
  using prompt_analytics_core::PromptCacheAnalyticsRecordResult;
  using prompt_analytics_core::PromptCacheAnalyticsTarget;
  using prompt_analytics_core::PromptCacheAnalyticsUsage;
  using prompt_analytics_core::StoredCounters;

  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheAnalyticsQueueEntry& value) {
    out = Writer()
            .required("bucket_start_at_ms", value.bucket_start_at_ms)
            .required("provider", value.provider)
            .required("model_hash", value.model_hash)
            .required("route", value.route)
            .required("prompt_cache_key_present", value.prompt_cache_key_present)
            .required("mode", value.mode)
            .required("fallback", value.fallback)
            .required("usage_kind", value.usage_kind)
            .required("deltas", value.deltas)
            .done();
  }

  PromptCacheAnalyticsQueueEntry tag_invoke(const json::value_to_tag<PromptCacheAnalyticsQueueEntry>&, const json::value& value) {
    const Reader in(value, "PromptCacheAnalyticsQueueEntry");
    return PromptCacheAnalyticsQueueEntry{
      .bucket_start_at_ms = in.required<double>("bucket_start_at_ms"),
      .provider = in.required<std::string>("provider"),
      .model_hash = in.required<std::string>("model_hash"),
      .route = in.required<std::string>("route"),
      .prompt_cache_key_present = in.required<bool>("prompt_cache_key_present"),
      .mode = in.required<std::string>("mode"),
      .fallback = in.required<std::string>("fallback"),
      .usage_kind = in.required<std::string>("usage_kind"),
      .deltas = in.required<StoredCounters>("deltas"),
      .kv = std::nullopt,
    };
  }

  namespace {
    const rt::Regex& MODEL_HASH_PATTERN() {
      static const rt::Regex pattern("^(?:unknown|[a-f0-9]{64})$");
      return pattern;
    }

    StoredCounters queueEntryDeltas(const CounterDeltas& deltas) {
      StoredCounters bounded;
      for (const std::string_view counter : COUNTERS) {
        const std::optional<rt::BigInt>& amount = deltas[counter];
        if (!amount || *amount < rt::BigInt(0) || *amount > rt::BigInt::fromNumber(rt::Number::MAX_SAFE_INTEGER)) continue;
        bounded[counter] = amount->toNumber();
      }
      return bounded;
    }

    // `typeof entry.model_hash !== "string"` cannot hold for the typed entry.
    std::optional<PromptCacheAnalyticsCohort> cohortFromQueueEntry(const PromptCacheAnalyticsQueueEntry& entry) {
      const std::optional<std::string> provider = asProvider(entry.provider);
      const std::optional<std::string> route = asRoute(entry.route);
      if (!provider || !route || !MODEL_HASH_PATTERN().test(entry.model_hash)) return std::nullopt;
      return PromptCacheAnalyticsCohort{
        .provider = *provider,
        .modelHash = entry.model_hash,
        .route = *route,
        .promptCacheKeyPresent = entry.prompt_cache_key_present,
        .mode = asMode(entry.mode),
        .fallback = asFallback(entry.fallback),
      };
    }

    /** Rebuilds the bounded counter deltas; a malformed retained entry is dropped, never partially written. */
    std::optional<PromptCacheAnalyticsUsage> usageFromQueueEntry(const PromptCacheAnalyticsQueueEntry& entry) {
      CounterDeltas deltas;
      for (const std::string_view counter : COUNTERS) {
        const std::optional<double>& amount = entry.deltas[counter];
        if (!amount) continue;
        if (!safeCounter(*amount)) return std::nullopt;
        deltas[counter] = rt::BigInt::fromNumber(*amount);
      }
      if (!deltas.sample_count) return std::nullopt;
      return PromptCacheAnalyticsUsage{.kind = entry.usage_kind, .deltas = std::move(deltas)};
    }
  }

  rt::Task<bool> writePromptCacheAnalyticsQueueEntry(PromptCacheAnalyticsQueueEntry entry) {
    const std::optional<PromptCacheAnalyticsCohort> cohort = cohortFromQueueEntry(entry);
    const std::optional<PromptCacheAnalyticsUsage> usage = usageFromQueueEntry(entry);
    if (!cohort || !usage || !safeCounter(entry.bucket_start_at_ms)) co_return false;
    std::shared_ptr<rt::Kv> kv;
    if (!entry.kv) {
      const rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKv(PromptCacheAnalyticsOptions()));
      kv = co_await resolving.wait();
    } else {
      kv = *entry.kv;
    }
    if (!kv) co_return false;
    bool recorded = false;
    try {
      const rt::Promise<PromptCacheAnalyticsRecordResult> committing(commitCohortOutcome(kv, entry.bucket_start_at_ms, *cohort, *usage, entry.bucket_start_at_ms));
      const PromptCacheAnalyticsRecordResult outcome = co_await committing.wait();
      recorded = outcome.status == "recorded";
    } catch (...) {
      recorded = false;
    }
    co_return recorded;
  }

  namespace {
    /**
     * Measures only the retained metadata, in the same UTF-8 byte accounting the
     * queue's default uses. A process-local KV delivery handle adds no payload
     * bytes, and an unserializable payload fails closed as `NaN`.
     */
    double measureQueueEntry(const PromptCacheAnalyticsQueueEntry& entry) { return measureJsonPayloadBytes(json::value_from(entry)); }

    std::optional<OptionalTelemetryQueue<PromptCacheAnalyticsQueueEntry>> optionalPromptCacheAnalyticsQueue;
    /**
     * Terminal once close is requested. It is checked before the lazy queue is
     * created, so a close that arrives before the first eligible sample cannot be
     * undone by a later enqueue creating a fresh open queue.
     */
    bool optionalPromptCacheAnalyticsClosed = false;

    // `optionalPromptCacheAnalyticsQueue ??= createOptionalTelemetryQueue(...)`; copies share the queue.
    OptionalTelemetryQueue<PromptCacheAnalyticsQueueEntry> promptCacheAnalyticsTelemetryQueue() {
      if (!optionalPromptCacheAnalyticsQueue) {
        OptionalTelemetryQueueOptions<PromptCacheAnalyticsQueueEntry> options;
        options.write = [](PromptCacheAnalyticsQueueEntry entry) { return rt::Promise<bool>(writePromptCacheAnalyticsQueueEntry(std::move(entry))); };
        options.measure = measureQueueEntry;
        optionalPromptCacheAnalyticsQueue = createOptionalTelemetryQueue(std::move(options));
      }
      return *optionalPromptCacheAnalyticsQueue;
    }
  }

  rt::Task<PromptCacheAnalyticsRecordResult> enqueuePromptCacheAnalytics(PromptCacheAnalyticsEvent event) {
    return enqueuePromptCacheAnalytics(std::move(event), PromptCacheAnalyticsEnqueueOptions());
  }

  rt::Task<PromptCacheAnalyticsRecordResult> enqueuePromptCacheAnalytics(PromptCacheAnalyticsEvent event, PromptCacheAnalyticsEnqueueOptions options) {
    const std::variant<PromptCacheAnalyticsTarget, PromptCacheAnalyticsRecordResult> target = promptCacheAnalyticsTarget(event, options);
    if (const PromptCacheAnalyticsRecordResult* refused = std::get_if<PromptCacheAnalyticsRecordResult>(&target)) co_return *refused;
    const PromptCacheAnalyticsTarget& accepted = std::get<PromptCacheAnalyticsTarget>(target);

    const double nowMs = safeNow(options.now ? options.now : rt::globalDateNow());
    const double bucketStartAtMs = alignedBucketStart(nowMs);
    std::optional<PromptCacheAnalyticsCohort> cohort;
    try {
      const rt::Promise<PromptCacheAnalyticsCohort> resolving(resolveCohort(accepted.provider, accepted.route, event));
      cohort = co_await resolving.wait();
    } catch (...) {
      cohort.reset();
    }
    if (!cohort) co_return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);

    const PromptCacheAnalyticsUsage usage = recordUsage(event);
    // Terminal module close, checked where the queue would be created: once close
    // ran without a queue, an eligible sample must not create one, even if it was
    // already resolving its cohort when the close arrived. A queue that already
    // exists was closed by that same call and reports the refusal itself; an
    // explicitly injected queue stays caller-owned.
    if (!options.queue && optionalPromptCacheAnalyticsClosed && !optionalPromptCacheAnalyticsQueue) {
      co_return recordResult("dropped", "dropped_closed", bucketStartAtMs);
    }
    const OptionalTelemetryQueue<PromptCacheAnalyticsQueueEntry> queue = options.queue ? *options.queue : promptCacheAnalyticsTelemetryQueue();
    PromptCacheAnalyticsQueueEntry entry{
      .bucket_start_at_ms = bucketStartAtMs,
      .provider = cohort->provider,
      .model_hash = cohort->modelHash,
      .route = cohort->route,
      .prompt_cache_key_present = cohort->promptCacheKeyPresent,
      .mode = cohort->mode,
      .fallback = cohort->fallback,
      .usage_kind = usage.kind,
      .deltas = queueEntryDeltas(usage.deltas),
      .kv = options.kv,
    };
    const std::string outcome = queue.enqueue(std::move(entry));
    if (outcome == "enqueued") co_return recordResult("queued", "queued", bucketStartAtMs);
    if (outcome == "dropped_capacity") co_return recordResult("dropped", "dropped_capacity", bucketStartAtMs);
    if (outcome == "dropped_bytes") co_return recordResult("dropped", "dropped_bytes", bucketStartAtMs);
    co_return recordResult("dropped", "dropped_closed", bucketStartAtMs);
  }

  rt::Promise<void> flushOptionalPromptCacheAnalytics() {
    if (optionalPromptCacheAnalyticsQueue) return optionalPromptCacheAnalyticsQueue->flush();
    return rt::Promise<void>::resolve();
  }

  rt::Task<void> closeOptionalPromptCacheAnalytics() {
    optionalPromptCacheAnalyticsClosed = true;
    if (optionalPromptCacheAnalyticsQueue) {
      const rt::Promise<void> closing = optionalPromptCacheAnalyticsQueue->close();
      co_await closing.wait();
    } else {
      // `await undefined`
      co_await rt::microtask();
    }
  }

  std::optional<OptionalTelemetryQueueSnapshot> optionalPromptCacheAnalyticsSnapshot() {
    if (!optionalPromptCacheAnalyticsQueue) return std::nullopt;
    return optionalPromptCacheAnalyticsQueue->snapshot();
  }

  void resetOptionalPromptCacheAnalyticsForTest() {
    optionalPromptCacheAnalyticsClosed = false;
    optionalPromptCacheAnalyticsQueue.reset();
  }
}
