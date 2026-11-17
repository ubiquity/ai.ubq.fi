#pragma once
// Port of src/cache/prompt-analytics-queue.ts.
// Prompt cache analytics optional telemetry queue, split out of src/prompt_cache_analytics.ts.
#include "uos/cache/prompt_analytics_core.hpp"
#include "uos/optional_telemetry_queue.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>

namespace uos::cache::prompt_analytics_queue {
  struct PromptCacheAnalyticsQueueEntry {
    double bucket_start_at_ms;
    prompt_analytics_core::PromptCacheAnalyticsProvider provider;
    std::string model_hash;
    prompt_analytics_core::PromptCacheAnalyticsRoute route;
    bool prompt_cache_key_present;
    prompt_analytics_core::PromptCacheAnalyticsMode mode;
    prompt_analytics_core::PromptCacheAnalyticsFallback fallback;
    // `PromptCacheAnalyticsUsage["kind"]`: "reported" | "missing" | "invalid"
    std::string usage_kind;
    /** Absent counters stay absent, so a missing cache count is never defaulted. */
    prompt_analytics_core::StoredCounters deltas;
    /**
     * Process-level delivery reference only: an explicitly injected database
     * handle, or undefined in production so the sink resolves the environment KV
     * with `getKv()` at delivery time. Request, response and prompt data never
     * enter this field.
     */
    // std::nullopt is undefined, a null pointer is null.
    std::optional<std::shared_ptr<rt::Kv>> kv{};
  };
  // The JSON payload: every member except the process-local `kv` (what JSON.stringify writes for
  // `{ ...entry, kv: undefined }`). value_to reads an entry whose `kv` is undefined.
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheAnalyticsQueueEntry& value);
  PromptCacheAnalyticsQueueEntry tag_invoke(const json::value_to_tag<PromptCacheAnalyticsQueueEntry>&, const json::value& value);

  /**
   * The durable sink for one queued entry. It applies exactly the counters a
   * direct write would have applied; it exists so the queued path cannot drift
   * from the direct path.
   */
  rt::Task<bool> writePromptCacheAnalyticsQueueEntry(PromptCacheAnalyticsQueueEntry entry);

  // `PromptCacheAnalyticsOptions & Readonly<{ queue? }>`
  struct PromptCacheAnalyticsEnqueueOptions : prompt_analytics_core::PromptCacheAnalyticsOptions {
    /**
     * Test and integration seam: the bounded queue instance that owns optional
     * writes. It stays caller-owned, so the module's terminal close does not
     * gate it.
     */
    std::optional<optional_telemetry_queue::OptionalTelemetryQueue<PromptCacheAnalyticsQueueEntry>> queue{};
  };

  /**
   * Queues one completed inference outcome onto the bounded optional-telemetry
   * queue and returns before any durable write starts.
   *
   * This is the drop-in replacement for `recordPromptCacheAnalytics` on the
   * request path: it shares the same gate and the same counters, but a slow or
   * unavailable analytics sink can no longer extend client-visible latency. The
   * return value reports the queue outcome, and the queue snapshot reports
   * retained, delivered, failed, dropped and drain state.
   */
  // The overload without `options` is `{}`.
  rt::Task<prompt_analytics_core::PromptCacheAnalyticsRecordResult> enqueuePromptCacheAnalytics(prompt_analytics_core::PromptCacheAnalyticsEvent event);
  rt::Task<prompt_analytics_core::PromptCacheAnalyticsRecordResult> enqueuePromptCacheAnalytics(prompt_analytics_core::PromptCacheAnalyticsEvent event,
                                                                                               PromptCacheAnalyticsEnqueueOptions options);

  /**
   * Drains every retained optional sample. Shutdown calls this before the process
   * exits. It always settles: a write that stops making progress bounds the wait,
   * and the snapshot then reports `drain_timeouts`/`dispatch_stalled` with the
   * retained and in-flight state instead of hanging the shutdown.
   */
  rt::Promise<void> flushOptionalPromptCacheAnalytics();

  /**
   * Stops accepting optional samples, then drains the retained ones under the same
   * bounded wait. Terminal before the lazy queue exists too: a close that arrives
   * first is remembered, so a later eligible sample is refused instead of creating
   * an open queue.
   */
  rt::Task<void> closeOptionalPromptCacheAnalytics();

  /** Observable queue state: retained, delivered, failed, dropped, timeout and drain evidence. */
  // std::nullopt is null (no sample has created the module queue).
  std::optional<optional_telemetry_queue::OptionalTelemetryQueueSnapshot> optionalPromptCacheAnalyticsSnapshot();

  /** Test seam: clears the lazy queue and its terminal close state so a suite can re-exercise first use. */
  void resetOptionalPromptCacheAnalyticsForTest();
}
