#include "uos/cache/prompt_analytics_read.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace uos::cache::prompt_analytics_read {
  using prompt_analytics_core::aggregatePrefix;
  using prompt_analytics_core::alignedBucketStart;
  using prompt_analytics_core::asFallback;
  using prompt_analytics_core::asMode;
  using prompt_analytics_core::asProvider;
  using prompt_analytics_core::asRoute;
  using prompt_analytics_core::dimensionPrefix;
  using prompt_analytics_core::isKnownCounter;
  using prompt_analytics_core::overflowPrefix;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_BUCKET_MS;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_DIMENSIONS;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_MAX_GROUP_BY;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_WINDOW_BUCKETS;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_WINDOW_MS;
  using prompt_analytics_core::PromptCacheAnalyticsQueryError;
  using prompt_analytics_core::resolveKv;
  using prompt_analytics_core::roundedPercentage;
  using prompt_analytics_core::roundedRatio;
  using prompt_analytics_core::safeCounter;
  using prompt_analytics_core::safeNow;
  using prompt_analytics_core::storedCounter;
  using prompt_analytics_core::Counter;
  using prompt_analytics_core::PromptCacheAnalyticsBucket;
  using prompt_analytics_core::PromptCacheAnalyticsCohort;
  using prompt_analytics_core::PromptCacheAnalyticsDimension;
  using prompt_analytics_core::PromptCacheAnalyticsGroup;
  using prompt_analytics_core::PromptCacheAnalyticsReadOptions;
  using prompt_analytics_core::PromptCacheAnalyticsView;
  using prompt_analytics_core::StoredCounters;

  namespace {
    using GroupBy = std::vector<PromptCacheAnalyticsDimension>;

    // `key[index]`: nullptr past the end (undefined).
    const rt::KvKeyPart* partAt(const rt::KvKey& key, std::size_t index) { return index < key.size() ? &key[index] : nullptr; }

    bool approvedDimension(std::string_view dimension) {
      return std::find(PROMPT_CACHE_ANALYTICS_DIMENSIONS.begin(), PROMPT_CACHE_ANALYTICS_DIMENSIONS.end(), dimension) != PROMPT_CACHE_ANALYTICS_DIMENSIONS.end();
    }

    // `value: unknown`; a null pointer is undefined.
    std::optional<GroupBy> normalizedGroupBy(const json::value* value) {
      if (!value) return GroupBy();
      const json::array* items = value->if_array();
      if (!items || static_cast<double>(items->size()) > PROMPT_CACHE_ANALYTICS_MAX_GROUP_BY) return std::nullopt;
      GroupBy groupBy;
      for (const json::value& dimension : *items) {
        if (!dimension.is_string()) return std::nullopt;
        const std::string text(dimension.get_string());
        if (!approvedDimension(text) || rt::includes(groupBy, text)) return std::nullopt;
        groupBy.push_back(text);
      }
      return groupBy;
    }

    // The typed `groupBy?: readonly PromptCacheAnalyticsDimension[]` of the read options.
    std::optional<GroupBy> normalizedGroupBy(const std::optional<GroupBy>& value) {
      if (!value) return GroupBy();
      if (static_cast<double>(value->size()) > PROMPT_CACHE_ANALYTICS_MAX_GROUP_BY) return std::nullopt;
      GroupBy groupBy;
      for (const std::string& dimension : *value) {
        if (!approvedDimension(dimension) || rt::includes(groupBy, dimension)) return std::nullopt;
        groupBy.push_back(dimension);
      }
      return groupBy;
    }
  }

  bool isValidPromptCacheAnalyticsGroupBy(const json::value& value) { return normalizedGroupBy(&value).has_value(); }

  bool isValidPromptCacheAnalyticsGroupBy(const json::value* value) { return normalizedGroupBy(value).has_value(); }

  bool isValidPromptCacheAnalyticsGroupBy(const std::vector<std::string>& value) { return normalizedGroupBy(std::optional<GroupBy>(value)).has_value(); }

  namespace {
    const rt::Regex& MODEL_HASH() {
      static const rt::Regex pattern("^(?:unknown|[a-f0-9]{64})$");
      return pattern;
    }

    std::optional<PromptCacheAnalyticsCohort> parsedDimensionValues(const rt::KvKey& key) {
      const std::size_t offset = dimensionPrefix.size();
      if (key.size() != offset + 8) return std::nullopt;
      const std::optional<std::string> provider = asProvider(&key[offset + 1]);
      const rt::KvKeyPart& modelHash = key[offset + 2];
      const std::optional<std::string> route = asRoute(&key[offset + 3]);
      const rt::KvKeyPart& keyState = key[offset + 4];
      const std::string mode = asMode(&key[offset + 5]);
      const std::string fallback = asFallback(&key[offset + 6]);
      const rt::KvKeyPart& counter = key[offset + 7];
      if (!provider || !modelHash.isString() || !MODEL_HASH().test(modelHash.string()) || !route ||
          !(keyState.isString() && (keyState.string() == "keyed" || keyState.string() == "unkeyed")) || !isKnownCounter(&counter))
        return std::nullopt;
      return PromptCacheAnalyticsCohort{
        .provider = *provider,
        .modelHash = modelHash.string(),
        .route = *route,
        .promptCacheKeyPresent = keyState.string() == "keyed",
        .mode = mode,
        .fallback = fallback,
      };
    }

    std::optional<Counter> counterFromDimensionKey(const rt::KvKey& key) {
      const rt::KvKeyPart* counter = partAt(key, dimensionPrefix.size() + 7);
      if (!isKnownCounter(counter)) return std::nullopt;
      return counter->string();
    }

    PromptCacheAnalyticsGroup groupForCohort(const PromptCacheAnalyticsCohort& cohort, const GroupBy& groupBy) {
      json::object group;
      for (const PromptCacheAnalyticsDimension& dimension : groupBy) {
        if (dimension == "provider") group["provider"] = cohort.provider;
        if (dimension == "model") group["model_hash"] = cohort.modelHash;
        if (dimension == "route") group["route"] = cohort.route;
        if (dimension == "key_presence") group["prompt_cache_key_present"] = cohort.promptCacheKeyPresent;
        if (dimension == "mode") group["mode"] = cohort.mode;
        if (dimension == "fallback") group["fallback"] = cohort.fallback;
      }
      return group;
    }

    constexpr std::array<std::string_view, 7> GROUP_IDENTITY_MEMBERS{"provider", "model_hash", "route", "prompt_cache_key_present", "mode", "fallback", "cardinality_limited"};

    // `JSON.stringify([bucketStartAtMs, group.provider, ..., group.cardinality_limited])`: an absent member is null.
    std::string groupIdentity(double bucketStartAtMs, const PromptCacheAnalyticsGroup& group) {
      json::array identity;
      identity.push_back(rt::makeNumber(bucketStartAtMs));
      for (const std::string_view member : GROUP_IDENTITY_MEMBERS) {
        const json::value* value = rt::get(group, member);
        identity.push_back(value ? *value : json::value(nullptr));
      }
      return rt::jsonStringify(json::value(std::move(identity)));
    }

    // `Readonly<{ bucketStartAtMs: number; counter: Counter }>`
    struct ParsedCounter {
      double bucketStartAtMs;
      Counter counter;
    };

    std::optional<ParsedCounter> parsedAggregateCounter(const rt::KvKey& key) {
      if (key.size() != aggregatePrefix.size() + 2) return std::nullopt;
      const rt::KvKeyPart& bucketStartAtMs = key[aggregatePrefix.size()];
      const rt::KvKeyPart& counter = key[aggregatePrefix.size() + 1];
      if (!safeCounter(&bucketStartAtMs) || !isKnownCounter(&counter)) return std::nullopt;
      return ParsedCounter{.bucketStartAtMs = bucketStartAtMs.number(), .counter = counter.string()};
    }

    std::optional<ParsedCounter> parsedOverflowCounter(const rt::KvKey& key) {
      if (key.size() != overflowPrefix.size() + 2) return std::nullopt;
      const rt::KvKeyPart& bucketStartAtMs = key[overflowPrefix.size()];
      const rt::KvKeyPart& counter = key[overflowPrefix.size() + 1];
      if (!safeCounter(&bucketStartAtMs) || !isKnownCounter(&counter)) return std::nullopt;
      return ParsedCounter{.bucketStartAtMs = bucketStartAtMs.number(), .counter = counter.string()};
    }

    // Synthetic control-plane fixtures may use the aggregate helper directly.
    // Both token counters are sufficient evidence of reported usage in that case.
    double inferredUsageReportedSampleCount(const StoredCounters& counters, double sampleCount, bool hasInputCounters) {
      return hasInputCounters && safeCounter(counters.input_tokens) && safeCounter(counters.cached_input_tokens) ? sampleCount : 0;
    }

    struct ProjectedTokenCounters {
      std::optional<double> inputTokens;
      std::optional<double> cachedInputTokens;
    };

    std::optional<ProjectedTokenCounters> projectedTokenCounters(const StoredCounters& counters, double usageReportedSampleCount, bool hasInputCounters) {
      if (usageReportedSampleCount > 0) {
        if (!safeCounter(counters.input_tokens) || !safeCounter(counters.cached_input_tokens) || *counters.cached_input_tokens > *counters.input_tokens) {
          return std::nullopt;
        }
        return ProjectedTokenCounters{.inputTokens = counters.input_tokens, .cachedInputTokens = counters.cached_input_tokens};
      }
      if (hasInputCounters) return std::nullopt;
      return ProjectedTokenCounters{.inputTokens = std::nullopt, .cachedInputTokens = std::nullopt};
    }

    struct ProjectedCacheWriteCounters {
      std::optional<double> inputTokens;
      double reportedSampleCount;
    };

    std::optional<ProjectedCacheWriteCounters> projectedCacheWriteCounters(const StoredCounters& counters, double usageReportedSampleCount) {
      const std::optional<double>& cacheWriteInputTokens = counters.cache_write_input_tokens;
      const std::optional<double>& cacheWriteReportedSampleCount = counters.cache_write_reported_sample_count;
      if (!cacheWriteInputTokens && !cacheWriteReportedSampleCount) {
        return ProjectedCacheWriteCounters{.inputTokens = std::nullopt, .reportedSampleCount = 0};
      }
      if (!safeCounter(cacheWriteInputTokens) || !safeCounter(cacheWriteReportedSampleCount) || *cacheWriteReportedSampleCount == 0 ||
          *cacheWriteReportedSampleCount > usageReportedSampleCount)
        return std::nullopt;
      return ProjectedCacheWriteCounters{.inputTokens = cacheWriteInputTokens, .reportedSampleCount = *cacheWriteReportedSampleCount};
    }

    struct ProjectedHitCounters {
      double requestCacheHitSampleCount;
      double cardinalityLimitedSampleCount;
    };

    std::optional<ProjectedHitCounters> projectedHitCounters(const StoredCounters& counters, double sampleCount, double usageReportedSampleCount) {
      const double requestCacheHitSampleCount = counters.request_cache_hit_sample_count.value_or(0);
      const double cardinalityLimitedSampleCount = counters.dimension_cardinality_limited_sample_count.value_or(0);
      if (!safeCounter(requestCacheHitSampleCount) || requestCacheHitSampleCount > usageReportedSampleCount || !safeCounter(cardinalityLimitedSampleCount) ||
          cardinalityLimitedSampleCount > sampleCount)
        return std::nullopt;
      return ProjectedHitCounters{.requestCacheHitSampleCount = requestCacheHitSampleCount, .cardinalityLimitedSampleCount = cardinalityLimitedSampleCount};
    }

    std::optional<PromptCacheAnalyticsBucket> projectedBucket(double bucketStartAtMs, const StoredCounters& counters, const std::optional<PromptCacheAnalyticsGroup>& group) {
      if (!safeCounter(counters.sample_count)) return std::nullopt;
      const double sampleCount = *counters.sample_count;

      const bool hasInputCounters = counters.input_tokens.has_value() || counters.cached_input_tokens.has_value();
      const double usageReportedSampleCount =
        counters.usage_reported_sample_count ? *counters.usage_reported_sample_count : inferredUsageReportedSampleCount(counters, sampleCount, hasInputCounters);
      const double usageInvalidSampleCount = counters.usage_invalid_sample_count.value_or(0);
      if (!safeCounter(usageReportedSampleCount) || !safeCounter(usageInvalidSampleCount) || usageReportedSampleCount > sampleCount ||
          usageInvalidSampleCount > sampleCount || usageReportedSampleCount + usageInvalidSampleCount > sampleCount)
        return std::nullopt;

      const std::optional<ProjectedTokenCounters> tokens = projectedTokenCounters(counters, usageReportedSampleCount, hasInputCounters);
      if (!tokens) return std::nullopt;
      const std::optional<ProjectedCacheWriteCounters> cacheWrite = projectedCacheWriteCounters(counters, usageReportedSampleCount);
      if (!cacheWrite) return std::nullopt;
      const std::optional<ProjectedHitCounters> hitCounters = projectedHitCounters(counters, sampleCount, usageReportedSampleCount);
      if (!hitCounters) return std::nullopt;

      const std::optional<double> tokenHitPercentage =
        !tokens->inputTokens || !tokens->cachedInputTokens ? std::nullopt : roundedPercentage(*tokens->cachedInputTokens, *tokens->inputTokens);
      const std::optional<double> requestHitPercentage = roundedPercentage(hitCounters->requestCacheHitSampleCount, usageReportedSampleCount);
      const double usageMissingSampleCount = sampleCount - usageReportedSampleCount - usageInvalidSampleCount;
      PromptCacheAnalyticsBucket compact{
        .bucket_start_at_ms = bucketStartAtMs,
        .bucket_end_at_ms = bucketStartAtMs + PROMPT_CACHE_ANALYTICS_BUCKET_MS,
        .input_tokens = tokens->inputTokens,
        .cached_input_tokens = tokens->cachedInputTokens,
        .cache_write_input_tokens = cacheWrite->inputTokens,
        .cache_write_reported_sample_count = cacheWrite->reportedSampleCount,
        .cached_percentage = tokenHitPercentage,
        .sample_count = sampleCount,
      };
      if (!group) return compact;
      PromptCacheAnalyticsBucket grouped = std::move(compact);
      grouped.group = *group;
      grouped.token_hit_percentage = rt::orNull(tokenHitPercentage);
      grouped.request_cache_hit_sample_count = hitCounters->requestCacheHitSampleCount;
      grouped.request_hit_percentage = rt::orNull(requestHitPercentage);
      grouped.cache_reads_per_write = !tokens->inputTokens || !tokens->cachedInputTokens || !cacheWrite->inputTokens
                                        ? rt::Nullable<double>(nullptr)
                                        : rt::orNull(roundedRatio(*tokens->cachedInputTokens, *cacheWrite->inputTokens));
      grouped.usage_reported_sample_count = usageReportedSampleCount;
      grouped.usage_invalid_sample_count = usageInvalidSampleCount;
      grouped.usage_missing_sample_count = usageMissingSampleCount;
      grouped.usage_telemetry_coverage_percentage = rt::orNull(roundedPercentage(usageReportedSampleCount, sampleCount));
      grouped.dimension_cardinality_limited_sample_count = hitCounters->cardinalityLimitedSampleCount;
      return grouped;
    }

    struct PromptCacheAnalyticsWindow {
      double currentBucketStartAtMs;
      double windowStartAtMs;
      double windowEndAtMs;
    };

    PromptCacheAnalyticsWindow viewWindow(const std::function<double()>& now) {
      const double currentBucketStartAtMs = alignedBucketStart(safeNow(now));
      const double windowEndAtMs = currentBucketStartAtMs + PROMPT_CACHE_ANALYTICS_BUCKET_MS;
      return PromptCacheAnalyticsWindow{
        .currentBucketStartAtMs = currentBucketStartAtMs,
        .windowStartAtMs = windowEndAtMs - PROMPT_CACHE_ANALYTICS_WINDOW_MS,
        .windowEndAtMs = windowEndAtMs,
      };
    }

    bool inWindow(double bucketStartAtMs, const PromptCacheAnalyticsWindow& window) {
      return bucketStartAtMs >= window.windowStartAtMs && bucketStartAtMs <= window.currentBucketStartAtMs;
    }

    struct PromptCacheAnalyticsStoredBucket {
      double bucketStartAtMs;
      // `PromptCacheAnalyticsGroup | null`
      std::optional<PromptCacheAnalyticsGroup> group;
      StoredCounters counters;
    };

    using PromptCacheAnalyticsStoredBuckets = rt::OrderedMap<std::string, PromptCacheAnalyticsStoredBucket>;

    /** Sums one stored counter into its bucket/group row; a saturated row is left as it was. */
    void accumulateCounter(PromptCacheAnalyticsStoredBuckets& storedBuckets, const std::string& identity, double bucketStartAtMs,
                           const std::optional<PromptCacheAnalyticsGroup>& group, const Counter& counter, double value) {
      PromptCacheAnalyticsStoredBucket* existing = storedBuckets.get(identity);
      std::optional<PromptCacheAnalyticsStoredBucket> created;
      if (!existing) existing = &created.emplace(PromptCacheAnalyticsStoredBucket{.bucketStartAtMs = bucketStartAtMs, .group = group, .counters = StoredCounters()});
      const double previous = existing->counters[counter].value_or(0);
      if (previous > rt::Number::MAX_SAFE_INTEGER - value) return;
      existing->counters[counter] = previous + value;
      if (created) storedBuckets.set(identity, std::move(*created));
    }

    void accumulateAggregateEntry(PromptCacheAnalyticsStoredBuckets& storedBuckets, const rt::KvEntry<>& entry, const PromptCacheAnalyticsWindow& window) {
      const std::optional<ParsedCounter> parsed = parsedAggregateCounter(entry.key);
      if (!parsed || !inWindow(parsed->bucketStartAtMs, window)) return;
      const std::optional<double> value = storedCounter(entry.value);
      if (!value) return;
      accumulateCounter(storedBuckets, rt::numberToString(parsed->bucketStartAtMs), parsed->bucketStartAtMs, std::nullopt, parsed->counter, *value);
    }

    void accumulateDimensionEntry(PromptCacheAnalyticsStoredBuckets& storedBuckets, const rt::KvEntry<>& entry, const PromptCacheAnalyticsWindow& window,
                                  const GroupBy& groupBy) {
      const rt::KvKeyPart* bucketStartAtMs = partAt(entry.key, dimensionPrefix.size());
      if (!safeCounter(bucketStartAtMs) || !inWindow(bucketStartAtMs->number(), window)) return;
      const std::optional<PromptCacheAnalyticsCohort> cohort = parsedDimensionValues(entry.key);
      const std::optional<Counter> counter = counterFromDimensionKey(entry.key);
      const std::optional<double> value = storedCounter(entry.value);
      if (!cohort || !counter || !value) return;
      const PromptCacheAnalyticsGroup group = groupForCohort(*cohort, groupBy);
      accumulateCounter(storedBuckets, groupIdentity(bucketStartAtMs->number(), group), bucketStartAtMs->number(), group, *counter, *value);
    }

    /** Returns true when the entry counts as cardinality-limited traffic for the view. */
    bool accumulateOverflowEntry(PromptCacheAnalyticsStoredBuckets& storedBuckets, const rt::KvEntry<>& entry, const PromptCacheAnalyticsWindow& window) {
      const std::optional<ParsedCounter> parsed = parsedOverflowCounter(entry.key);
      if (!parsed || !inWindow(parsed->bucketStartAtMs, window)) return false;
      const std::optional<double> value = storedCounter(entry.value);
      if (!value) return false;
      PromptCacheAnalyticsGroup group;
      group["cardinality_limited"] = true;
      accumulateCounter(storedBuckets, groupIdentity(parsed->bucketStartAtMs, group), parsed->bucketStartAtMs, group, parsed->counter, *value);
      return true;
    }

    struct PromptCacheAnalyticsStoredRead {
      PromptCacheAnalyticsStoredBuckets storedBuckets;
      /** At least one in-window entry was recorded past the per-bucket cohort cap. */
      bool cardinalityLimited = false;
    };

    std::unique_ptr<rt::KvListIterator> listPrefix(rt::Kv& kv, const rt::KvKey& prefix) {
      rt::KvListSelector selector;
      selector.prefix = prefix;
      return kv.list(std::move(selector));
    }

    /**
     * Reads this window's counters into per-bucket rows. The compact aggregate view
     * lists only the aggregate prefix; a grouped view lists the dimension prefix
     * plus the overflow prefix that carries capped cohorts.
     */
    rt::Task<PromptCacheAnalyticsStoredRead> readStoredBuckets(std::shared_ptr<rt::Kv> kv, GroupBy groupBy, PromptCacheAnalyticsWindow window) {
      PromptCacheAnalyticsStoredRead read;
      if (groupBy.empty()) {
        const std::unique_ptr<rt::KvListIterator> aggregates = listPrefix(*kv, aggregatePrefix);
        for (;;) {
          const std::optional<rt::KvEntry<>> entry = co_await aggregates->next();
          if (!entry) break;
          accumulateAggregateEntry(read.storedBuckets, *entry, window);
        }
        read.cardinalityLimited = false;
        co_return read;
      }
      bool cardinalityLimited = false;
      const std::unique_ptr<rt::KvListIterator> dimensions = listPrefix(*kv, dimensionPrefix);
      for (;;) {
        const std::optional<rt::KvEntry<>> entry = co_await dimensions->next();
        if (!entry) break;
        accumulateDimensionEntry(read.storedBuckets, *entry, window, groupBy);
      }
      const std::unique_ptr<rt::KvListIterator> overflows = listPrefix(*kv, overflowPrefix);
      for (;;) {
        const std::optional<rt::KvEntry<>> entry = co_await overflows->next();
        if (!entry) break;
        if (accumulateOverflowEntry(read.storedBuckets, *entry, window)) cardinalityLimited = true;
      }
      read.cardinalityLimited = cardinalityLimited;
      co_return read;
    }

    // `unavailable()`
    PromptCacheAnalyticsView unavailableView(const PromptCacheAnalyticsWindow& window, const GroupBy& groupBy, double maxBuckets) {
      return PromptCacheAnalyticsView{
        .status = "unavailable",
        .bucket_ms = PROMPT_CACHE_ANALYTICS_BUCKET_MS,
        .window_start_at_ms = window.windowStartAtMs,
        .window_end_at_ms = window.windowEndAtMs,
        .group_by = groupBy,
        .max_buckets = maxBuckets,
        .cardinality_limited = false,
        .truncated = false,
        .buckets = {},
      };
    }

    // `JSON.stringify(bucket.group ?? {})`
    std::string groupText(const PromptCacheAnalyticsBucket& bucket) { return rt::jsonStringify(json::value(bucket.group ? *bucket.group : json::object())); }
  }

  rt::Task<PromptCacheAnalyticsView> readPromptCacheAnalytics() { return readPromptCacheAnalytics(PromptCacheAnalyticsReadOptions()); }

  rt::Task<PromptCacheAnalyticsView> readPromptCacheAnalytics(PromptCacheAnalyticsReadOptions options) {
    const std::optional<GroupBy> groupBy = normalizedGroupBy(options.groupBy);
    if (!groupBy) {
      throw PromptCacheAnalyticsQueryError("group_by must contain distinct approved dimensions only");
    }
    const PromptCacheAnalyticsWindow window = viewWindow(options.now ? options.now : rt::globalDateNow());
    const double maxBuckets = groupBy->empty() ? PROMPT_CACHE_ANALYTICS_WINDOW_BUCKETS : PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS;
    const rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKv(options));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return unavailableView(window, *groupBy, maxBuckets);

    std::optional<PromptCacheAnalyticsView> view;
    try {
      const rt::Promise<PromptCacheAnalyticsStoredRead> reading(readStoredBuckets(kv, *groupBy, window));
      const PromptCacheAnalyticsStoredRead read = co_await reading.wait();

      std::vector<PromptCacheAnalyticsBucket> projected;
      for (const PromptCacheAnalyticsStoredBucket& stored : read.storedBuckets.values()) {
        std::optional<PromptCacheAnalyticsBucket> bucket = projectedBucket(stored.bucketStartAtMs, stored.counters, stored.group);
        if (bucket) projected.push_back(std::move(*bucket));
      }
      rt::sort(projected, [](const PromptCacheAnalyticsBucket& left, const PromptCacheAnalyticsBucket& right) {
        const double difference = left.bucket_start_at_ms - right.bucket_start_at_ms;
        if (rt::truthy(difference)) return difference;
        return static_cast<double>(rt::localeCompare(groupText(left), groupText(right)));
      });
      const bool responseTruncated = static_cast<double>(projected.size()) > maxBuckets;
      const bool truncated = responseTruncated || read.cardinalityLimited;
      std::vector<PromptCacheAnalyticsBucket> buckets =
        responseTruncated ? std::vector<PromptCacheAnalyticsBucket>(projected.end() - static_cast<std::ptrdiff_t>(maxBuckets), projected.end()) : std::move(projected);
      view = PromptCacheAnalyticsView{
        .status = "ready",
        .bucket_ms = PROMPT_CACHE_ANALYTICS_BUCKET_MS,
        .window_start_at_ms = window.windowStartAtMs,
        .window_end_at_ms = window.windowEndAtMs,
        .group_by = *groupBy,
        .max_buckets = maxBuckets,
        .cardinality_limited = read.cardinalityLimited,
        .truncated = truncated,
        .buckets = std::move(buckets),
      };
    } catch (...) {
      view.reset();
    }
    if (!view) co_return unavailableView(window, *groupBy, maxBuckets);
    co_return *view;
  }
}
