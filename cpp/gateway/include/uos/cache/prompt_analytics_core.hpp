#pragma once
// Port of src/cache/prompt-analytics-core.ts.
// Prompt cache analytics core: counters, keys and the recording path, split out of src/prompt_cache_analytics.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::cache::prompt_analytics_core {
  /**
   * V2 is a hard namespace cutover. V1 remains outside this prefix and expires
   * according to its existing retention policy; readers never combine it with V2.
   */
  inline const rt::KvKey PROMPT_CACHE_ANALYTICS_KV_PREFIX{"uos_ai", "prompt_cache_analytics", "v2"};
  inline constexpr double PROMPT_CACHE_ANALYTICS_BUCKET_MS = 15 * 60'000;
  inline constexpr double PROMPT_CACHE_ANALYTICS_WINDOW_MS = 7 * 24 * 60 * 60'000.0;
  inline constexpr double PROMPT_CACHE_ANALYTICS_WINDOW_BUCKETS = PROMPT_CACHE_ANALYTICS_WINDOW_MS / PROMPT_CACHE_ANALYTICS_BUCKET_MS;
  inline constexpr double PROMPT_CACHE_ANALYTICS_RETENTION_MS = 8 * 24 * 60 * 60'000.0;
  inline constexpr double PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET = 32;
  inline constexpr double PROMPT_CACHE_ANALYTICS_MAX_GROUP_BY = 2;
  inline constexpr double PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS = 512;

  inline constexpr std::array<std::string_view, 6> PROMPT_CACHE_ANALYTICS_DIMENSIONS{"provider", "model", "route", "key_presence", "mode", "fallback"};
  inline constexpr std::array<std::string_view, 4> PROMPT_CACHE_ANALYTICS_MODES{"implicit", "explicit", "legacy_retention", "unspecified"};
  inline constexpr std::array<std::string_view, 4> PROMPT_CACHE_ANALYTICS_FALLBACKS{"none", "primary_429", "primary_quota_blocked", "other"};

  inline constexpr std::array<std::string_view, 9> COUNTERS{
    "input_tokens",
    "cached_input_tokens",
    "cache_write_input_tokens",
    "cache_write_reported_sample_count",
    "sample_count",
    "request_cache_hit_sample_count",
    "usage_reported_sample_count",
    "usage_invalid_sample_count",
    "dimension_cardinality_limited_sample_count",
  };

  // `(typeof COUNTERS)[number]`
  using Counter = std::string;
  // `(typeof PROMPT_CACHE_TELEMETRY_PROVIDERS)[number]`
  using PromptCacheAnalyticsProvider = std::string;
  // `(typeof PROMPT_CACHE_TELEMETRY_ROUTES)[number]`
  using PromptCacheAnalyticsRoute = std::string;
  // `(typeof PROMPT_CACHE_ANALYTICS_DIMENSIONS)[number]`
  using PromptCacheAnalyticsDimension = std::string;
  // `(typeof PROMPT_CACHE_ANALYTICS_MODES)[number]`
  using PromptCacheAnalyticsMode = std::string;
  // `(typeof PROMPT_CACHE_ANALYTICS_FALLBACKS)[number]`
  using PromptCacheAnalyticsFallback = std::string;

  struct PromptCacheAnalyticsCohort {
    PromptCacheAnalyticsProvider provider;
    std::string modelHash;
    PromptCacheAnalyticsRoute route;
    bool promptCacheKeyPresent;
    PromptCacheAnalyticsMode mode;
    PromptCacheAnalyticsFallback fallback;
    friend bool operator==(const PromptCacheAnalyticsCohort&, const PromptCacheAnalyticsCohort&) = default;
  };

  // `Partial<Record<Counter, T>>`: one optional member per counter, in COUNTERS order.
  template <class T> struct CounterRecord {
    std::optional<T> input_tokens{};
    std::optional<T> cached_input_tokens{};
    std::optional<T> cache_write_input_tokens{};
    std::optional<T> cache_write_reported_sample_count{};
    std::optional<T> sample_count{};
    std::optional<T> request_cache_hit_sample_count{};
    std::optional<T> usage_reported_sample_count{};
    std::optional<T> usage_invalid_sample_count{};
    std::optional<T> dimension_cardinality_limited_sample_count{};

    // `record[counter]`. A name outside COUNTERS is a translation error (std::logic_error):
    // the TS type admits only counters.
    std::optional<T>& operator[](std::string_view counter) { return member(*this, counter); }
    const std::optional<T>& operator[](std::string_view counter) const { return member(*this, counter); }
    friend bool operator==(const CounterRecord&, const CounterRecord&) = default;

  private:
    template <class Self> static auto& member(Self& self, std::string_view counter) {
      if (counter == "input_tokens") return self.input_tokens;
      if (counter == "cached_input_tokens") return self.cached_input_tokens;
      if (counter == "cache_write_input_tokens") return self.cache_write_input_tokens;
      if (counter == "cache_write_reported_sample_count") return self.cache_write_reported_sample_count;
      if (counter == "sample_count") return self.sample_count;
      if (counter == "request_cache_hit_sample_count") return self.request_cache_hit_sample_count;
      if (counter == "usage_reported_sample_count") return self.usage_reported_sample_count;
      if (counter == "usage_invalid_sample_count") return self.usage_invalid_sample_count;
      if (counter == "dimension_cardinality_limited_sample_count") return self.dimension_cardinality_limited_sample_count;
      throw std::logic_error("CounterRecord: not a prompt cache analytics counter: " + std::string(counter));
    }
  };

  // `Partial<Record<Counter, number>>`
  using StoredCounters = CounterRecord<double>;
  // `Partial<Record<Counter, bigint>>`
  using CounterDeltas = CounterRecord<rt::BigInt>;

  // The JSON object of the present counters, in COUNTERS order (the queue entry's `deltas`).
  void tag_invoke(const json::value_from_tag&, json::value& out, const StoredCounters& value);
  StoredCounters tag_invoke(const json::value_to_tag<StoredCounters>&, const json::value& value);

  /** Usage evidence carried by one recorded outcome. */
  struct PromptCacheAnalyticsUsage {
    std::string kind;  // "reported" | "missing" | "invalid"
    CounterDeltas deltas;
    friend bool operator==(const PromptCacheAnalyticsUsage&, const PromptCacheAnalyticsUsage&) = default;
  };

  struct PromptCacheAnalyticsEvent {
    std::optional<std::string> provider;
    /** Raw model text is accepted only long enough to derive an opaque cohort hash. */
    rt::Nullable<std::string> model{};
    std::string route;
    double status;
    bool completed;
    std::string usageTelemetryStatus;
    std::optional<double> inputTokens;
    std::optional<double> cachedInputTokens;
    std::optional<double> cacheWriteInputTokens;
    std::optional<bool> promptCacheKeyPresent{};
    rt::Nullable<std::string> promptCacheMode{};
    rt::Nullable<std::string> fallbackReason{};
  };

  struct PromptCacheAnalyticsOptions {
    // `kv?: Kv | null`: std::nullopt is undefined (the process KV), a null pointer is null.
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::optional<std::string> release{};
    // An empty function is undefined (`Date.now`).
    std::function<double()> now{};
  };

  // `Pick<PromptCacheAnalyticsOptions, "kv" | "now"> & Readonly<{ groupBy? }>`
  struct PromptCacheAnalyticsReadOptions {
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::function<double()> now{};
    /** Only these bounded, public-safe dimensions may be selected. */
    std::optional<std::vector<PromptCacheAnalyticsDimension>> groupBy{};
  };

  struct PromptCacheAnalyticsRecordResult {
    std::string status;  // "recorded" | "ignored" | "unavailable" | "queued" | "dropped"
    // "recorded" | "recorded_without_usage" | "recorded_invalid_usage" | "recorded_cardinality_capped" |
    // "unknown_release" | "not_completed_2xx" | "unsupported_provider" | "unsupported_route" | "kv_unavailable" |
    // "queued" | "dropped_capacity" | "dropped_bytes" | "dropped_closed"
    std::string reason;
    std::optional<double> bucket_start_at_ms;
    friend bool operator==(const PromptCacheAnalyticsRecordResult&, const PromptCacheAnalyticsRecordResult&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheAnalyticsRecordResult& value);
  PromptCacheAnalyticsRecordResult tag_invoke(const json::value_to_tag<PromptCacheAnalyticsRecordResult>&, const json::value& value);

  // `Readonly<{ provider?; model_hash?; route?; prompt_cache_key_present?; mode?; fallback?; cardinality_limited?: true }>`.
  // The object is built member by member in group_by order (`{ cardinality_limited: true }` for capped
  // cohorts) and that order is observable (JSON output, bucket sort), so it stays a JSON object:
  // `model_hash` is a domain-separated digest or the bounded literal `unknown`, never raw model text.
  using PromptCacheAnalyticsGroup = json::object;

  struct PromptCacheAnalyticsBucket {
    double bucket_start_at_ms;
    double bucket_end_at_ms;
    std::optional<double> input_tokens;
    std::optional<double> cached_input_tokens;
    std::optional<double> cache_write_input_tokens;
    double cache_write_reported_sample_count;
    /** Kept for the existing capacity view. This is token_hit_percentage when available. */
    std::optional<double> cached_percentage;
    double sample_count;
    /** Present for the v2 grouped admin API and absent from the compact capacity projection. */
    std::optional<PromptCacheAnalyticsGroup> group{};
    rt::Nullable<double> token_hit_percentage{};
    std::optional<double> request_cache_hit_sample_count{};
    rt::Nullable<double> request_hit_percentage{};
    rt::Nullable<double> cache_reads_per_write{};
    std::optional<double> usage_reported_sample_count{};
    std::optional<double> usage_invalid_sample_count{};
    std::optional<double> usage_missing_sample_count{};
    rt::Nullable<double> usage_telemetry_coverage_percentage{};
    std::optional<double> dimension_cardinality_limited_sample_count{};
    friend bool operator==(const PromptCacheAnalyticsBucket&, const PromptCacheAnalyticsBucket&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheAnalyticsBucket& value);
  PromptCacheAnalyticsBucket tag_invoke(const json::value_to_tag<PromptCacheAnalyticsBucket>&, const json::value& value);

  struct PromptCacheAnalyticsView {
    std::string status;  // "ready" | "unavailable"
    double bucket_ms;
    double window_start_at_ms;
    double window_end_at_ms;
    std::vector<PromptCacheAnalyticsDimension> group_by;
    double max_buckets;
    /** At least one group is a fixed synthetic cohort after the detail cap. */
    bool cardinality_limited;
    /** Response or cohort-detail truncation means the grouped view is incomplete. */
    bool truncated;
    std::vector<PromptCacheAnalyticsBucket> buckets;
    friend bool operator==(const PromptCacheAnalyticsView&, const PromptCacheAnalyticsView&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheAnalyticsView& value);
  PromptCacheAnalyticsView tag_invoke(const json::value_to_tag<PromptCacheAnalyticsView>&, const json::value& value);

  struct PromptCacheAnalyticsPruneResult {
    std::string status;  // "pruned" | "unavailable"
    double deleted;
    friend bool operator==(const PromptCacheAnalyticsPruneResult&, const PromptCacheAnalyticsPruneResult&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheAnalyticsPruneResult& value);
  PromptCacheAnalyticsPruneResult tag_invoke(const json::value_to_tag<PromptCacheAnalyticsPruneResult>&, const json::value& value);

  class PromptCacheAnalyticsQueryError : public rt::JsError {
  public:
    explicit PromptCacheAnalyticsQueryError(std::string message_) : rt::JsError(std::move(message_), "PromptCacheAnalyticsQueryError") {}
  };

  double safeNow(const std::function<double()>& now);

  double alignedBucketStart(double timestamp);

  // `(value: unknown): value is number`: a number that is a non-negative safe integer. A key part
  // (nullptr is an index past the end, undefined) or a `number | null | undefined` value.
  bool safeCounter(double value);
  bool safeCounter(const std::optional<double>& value);
  bool safeCounter(const rt::KvKeyPart* value);

  // `(value: unknown): number | null` over a stored KV value: a KvU64, or an object whose own
  // `value` is a bigint, within 0..Number.MAX_SAFE_INTEGER.
  std::optional<double> storedCounter(const rt::KvValue& value);

  std::optional<double> roundedPercentage(double numerator, double denominator);

  std::optional<double> roundedRatio(double numerator, double denominator);

  // The `value: unknown` guards. std::nullopt (or a key part that is absent or not a string) is a
  // value whose `typeof` is not "string".
  std::optional<PromptCacheAnalyticsProvider> asProvider(std::optional<std::string_view> value);
  std::optional<PromptCacheAnalyticsProvider> asProvider(const rt::KvKeyPart* value);

  std::optional<PromptCacheAnalyticsRoute> asRoute(std::optional<std::string_view> value);
  std::optional<PromptCacheAnalyticsRoute> asRoute(const rt::KvKeyPart* value);

  PromptCacheAnalyticsMode asMode(std::optional<std::string_view> value);
  PromptCacheAnalyticsMode asMode(const rt::KvKeyPart* value);

  PromptCacheAnalyticsFallback asFallback(std::optional<std::string_view> value);
  PromptCacheAnalyticsFallback asFallback(const rt::KvKeyPart* value);

  // `recordResult(status, reason, bucketStartAtMs = null)`
  PromptCacheAnalyticsRecordResult recordResult(std::string status, std::string reason, std::optional<double> bucketStartAtMs = std::nullopt);

  // `options.kv === undefined ? await getKv() : options.kv`, null when that throws. Every options
  // type that carries `kv` is accepted, as TS structural typing allows.
  rt::Task<std::shared_ptr<rt::Kv>> resolveKv(PromptCacheAnalyticsOptions options);
  rt::Task<std::shared_ptr<rt::Kv>> resolveKv(PromptCacheAnalyticsReadOptions options);

  inline const rt::KvKey aggregatePrefix{PROMPT_CACHE_ANALYTICS_KV_PREFIX, "all"};
  inline const rt::KvKey dimensionPrefix{PROMPT_CACHE_ANALYTICS_KV_PREFIX, "dimension"};
  inline const rt::KvKey overflowPrefix{PROMPT_CACHE_ANALYTICS_KV_PREFIX, "overflow"};

  rt::KvKey promptCacheAnalyticsBucketKey(double bucketStartAtMs);

  /**
   * This is the aggregate counter helper kept for capacity callers and focused
   * fixtures. Dimension counters remain private to this module.
   */
  rt::KvKey promptCacheAnalyticsCounterKey(double bucketStartAtMs, const Counter& counter);

  // `(value: unknown): value is Counter`
  bool isKnownCounter(std::optional<std::string_view> value);
  bool isKnownCounter(const rt::KvKeyPart* value);

  PromptCacheAnalyticsUsage recordUsage(const PromptCacheAnalyticsEvent& event);

  /**
   * Builds the bounded cohort for an event whose provider and route were already
   * validated by the caller, so this cannot fail to produce a cohort.
   */
  rt::Task<PromptCacheAnalyticsCohort> resolveCohort(PromptCacheAnalyticsProvider provider, PromptCacheAnalyticsRoute route, PromptCacheAnalyticsEvent event);

  // `Readonly<{ provider; route }>`: the gate passed.
  struct PromptCacheAnalyticsTarget {
    PromptCacheAnalyticsProvider provider;
    PromptCacheAnalyticsRoute route;
    friend bool operator==(const PromptCacheAnalyticsTarget&, const PromptCacheAnalyticsTarget&) = default;
  };

  /** The optional-write gate shared by the direct and the queued entry points. */
  // `"provider" in target` is `std::holds_alternative<PromptCacheAnalyticsTarget>(target)`.
  std::variant<PromptCacheAnalyticsTarget, PromptCacheAnalyticsRecordResult> promptCacheAnalyticsTarget(const PromptCacheAnalyticsEvent& event,
                                                                                                        const PromptCacheAnalyticsOptions& options);

  /**
   * One bounded admission sequence for an already-validated cohort.
   *
   * Each failed admission CAS means this cohort was admitted concurrently or
   * another cohort advanced the shared cardinality row. One extra read after the
   * maximum number of conflicts must therefore observe this marker or cap.
   */
  rt::Task<PromptCacheAnalyticsRecordResult> commitCohortOutcome(std::shared_ptr<rt::Kv> kv, double bucketStartAtMs, PromptCacheAnalyticsCohort cohort,
                                                                PromptCacheAnalyticsUsage usage, double nowMs);

  /**
   * Adds one completed inference outcome to aggregate and bounded cohort
   * counters. The model is hashed before it reaches a durable KV key. A full
   * bucket never admits more than the fixed number of cohort combinations.
   */
  // The overload without `options` is `{}`.
  rt::Task<PromptCacheAnalyticsRecordResult> recordPromptCacheAnalytics(PromptCacheAnalyticsEvent event);
  rt::Task<PromptCacheAnalyticsRecordResult> recordPromptCacheAnalytics(PromptCacheAnalyticsEvent event, PromptCacheAnalyticsOptions options);

  /** Removes V2 and legacy V1 entries at the eight-day boundary without scanning fresh buckets. */
  // `options: Pick<PromptCacheAnalyticsOptions, "kv" | "now"> = {}`: `release` is not read.
  rt::Task<PromptCacheAnalyticsPruneResult> prunePromptCacheAnalytics();
  rt::Task<PromptCacheAnalyticsPruneResult> prunePromptCacheAnalytics(PromptCacheAnalyticsOptions options);
}
