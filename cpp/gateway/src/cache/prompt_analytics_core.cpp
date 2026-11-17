#include "uos/cache/prompt_analytics_core.hpp"

#include "uos/cache/telemetry_gate.hpp"
#include "uos/kv.hpp"
#include "uos/release.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>

namespace uos::cache::prompt_analytics_core {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using telemetry_gate::PROMPT_CACHE_TELEMETRY_PROVIDERS;
    using telemetry_gate::PROMPT_CACHE_TELEMETRY_ROUTES;

    const rt::KvKey LEGACY_PROMPT_CACHE_ANALYTICS_V1_KV_PREFIX{"uos_ai", "prompt_cache_analytics", "v1"};

    const rt::Regex& RELEASE_SHA() {
      static const rt::Regex pattern("^[a-f0-9]{7,64}$", "i");
      return pattern;
    }
    constexpr double MAX_MODEL_CHARS = 256;

    // `key[index]`: nullptr past the end (undefined).

    // A key part as `typeof part === "string" ? part : <not a string>`.
    std::optional<std::string_view> keyPartString(const rt::KvKeyPart* part) {
      if (!part || !part->isString()) return std::nullopt;
      return std::string_view(part->string());
    }

    // A `string | null | undefined` member as `typeof value === "string" ? value : <not a string>`.
    std::optional<std::string_view> nullableString(const rt::Nullable<std::string>& value) {
      if (!value.hasValue()) return std::nullopt;
      return std::string_view(*value);
    }

    template <std::size_t N> bool listIncludes(const std::array<std::string_view, N>& list, std::string_view value) {
      return std::find(list.begin(), list.end(), value) != list.end();
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const StoredCounters& value) {
    Writer writer;
    for (const std::string_view counter : COUNTERS) writer.orUndefined(counter, value[counter]);
    out = writer.done();
  }

  StoredCounters tag_invoke(const json::value_to_tag<StoredCounters>&, const json::value& value) {
    const Reader in(value, "StoredCounters");
    StoredCounters counters;
    for (const std::string_view counter : COUNTERS) counters[counter] = in.orUndefined<double>(counter);
    return counters;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheAnalyticsRecordResult& value) {
    out = Writer().required("status", value.status).required("reason", value.reason).orNull("bucket_start_at_ms", value.bucket_start_at_ms).done();
  }

  PromptCacheAnalyticsRecordResult tag_invoke(const json::value_to_tag<PromptCacheAnalyticsRecordResult>&, const json::value& value) {
    const Reader in(value, "PromptCacheAnalyticsRecordResult");
    return PromptCacheAnalyticsRecordResult{
      .status = in.required<std::string>("status"),
      .reason = in.required<std::string>("reason"),
      .bucket_start_at_ms = in.orNull<double>("bucket_start_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheAnalyticsBucket& value) {
    out = Writer()
            .required("bucket_start_at_ms", value.bucket_start_at_ms)
            .required("bucket_end_at_ms", value.bucket_end_at_ms)
            .orNull("input_tokens", value.input_tokens)
            .orNull("cached_input_tokens", value.cached_input_tokens)
            .orNull("cache_write_input_tokens", value.cache_write_input_tokens)
            .required("cache_write_reported_sample_count", value.cache_write_reported_sample_count)
            .orNull("cached_percentage", value.cached_percentage)
            .required("sample_count", value.sample_count)
            .orUndefined("group", value.group)
            .nullable("token_hit_percentage", value.token_hit_percentage)
            .orUndefined("request_cache_hit_sample_count", value.request_cache_hit_sample_count)
            .nullable("request_hit_percentage", value.request_hit_percentage)
            .nullable("cache_reads_per_write", value.cache_reads_per_write)
            .orUndefined("usage_reported_sample_count", value.usage_reported_sample_count)
            .orUndefined("usage_invalid_sample_count", value.usage_invalid_sample_count)
            .orUndefined("usage_missing_sample_count", value.usage_missing_sample_count)
            .nullable("usage_telemetry_coverage_percentage", value.usage_telemetry_coverage_percentage)
            .orUndefined("dimension_cardinality_limited_sample_count", value.dimension_cardinality_limited_sample_count)
            .done();
  }

  PromptCacheAnalyticsBucket tag_invoke(const json::value_to_tag<PromptCacheAnalyticsBucket>&, const json::value& value) {
    const Reader in(value, "PromptCacheAnalyticsBucket");
    return PromptCacheAnalyticsBucket{
      .bucket_start_at_ms = in.required<double>("bucket_start_at_ms"),
      .bucket_end_at_ms = in.required<double>("bucket_end_at_ms"),
      .input_tokens = in.orNull<double>("input_tokens"),
      .cached_input_tokens = in.orNull<double>("cached_input_tokens"),
      .cache_write_input_tokens = in.orNull<double>("cache_write_input_tokens"),
      .cache_write_reported_sample_count = in.required<double>("cache_write_reported_sample_count"),
      .cached_percentage = in.orNull<double>("cached_percentage"),
      .sample_count = in.required<double>("sample_count"),
      .group = in.orUndefined<json::object>("group"),
      .token_hit_percentage = in.nullable<double>("token_hit_percentage"),
      .request_cache_hit_sample_count = in.orUndefined<double>("request_cache_hit_sample_count"),
      .request_hit_percentage = in.nullable<double>("request_hit_percentage"),
      .cache_reads_per_write = in.nullable<double>("cache_reads_per_write"),
      .usage_reported_sample_count = in.orUndefined<double>("usage_reported_sample_count"),
      .usage_invalid_sample_count = in.orUndefined<double>("usage_invalid_sample_count"),
      .usage_missing_sample_count = in.orUndefined<double>("usage_missing_sample_count"),
      .usage_telemetry_coverage_percentage = in.nullable<double>("usage_telemetry_coverage_percentage"),
      .dimension_cardinality_limited_sample_count = in.orUndefined<double>("dimension_cardinality_limited_sample_count"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheAnalyticsView& value) {
    out = Writer()
            .required("status", value.status)
            .required("bucket_ms", value.bucket_ms)
            .required("window_start_at_ms", value.window_start_at_ms)
            .required("window_end_at_ms", value.window_end_at_ms)
            .required("group_by", value.group_by)
            .required("max_buckets", value.max_buckets)
            .required("cardinality_limited", value.cardinality_limited)
            .required("truncated", value.truncated)
            .required("buckets", value.buckets)
            .done();
  }

  PromptCacheAnalyticsView tag_invoke(const json::value_to_tag<PromptCacheAnalyticsView>&, const json::value& value) {
    const Reader in(value, "PromptCacheAnalyticsView");
    return PromptCacheAnalyticsView{
      .status = in.required<std::string>("status"),
      .bucket_ms = in.required<double>("bucket_ms"),
      .window_start_at_ms = in.required<double>("window_start_at_ms"),
      .window_end_at_ms = in.required<double>("window_end_at_ms"),
      .group_by = in.required<std::vector<PromptCacheAnalyticsDimension>>("group_by"),
      .max_buckets = in.required<double>("max_buckets"),
      .cardinality_limited = in.required<bool>("cardinality_limited"),
      .truncated = in.required<bool>("truncated"),
      .buckets = in.required<std::vector<PromptCacheAnalyticsBucket>>("buckets"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheAnalyticsPruneResult& value) {
    out = Writer().required("status", value.status).required("deleted", value.deleted).done();
  }

  PromptCacheAnalyticsPruneResult tag_invoke(const json::value_to_tag<PromptCacheAnalyticsPruneResult>&, const json::value& value) {
    const Reader in(value, "PromptCacheAnalyticsPruneResult");
    return PromptCacheAnalyticsPruneResult{.status = in.required<std::string>("status"), .deleted = in.required<double>("deleted")};
  }

  double safeNow(const std::function<double()>& now) {
    const double value = rt::Math::trunc(now());
    return rt::Number::isSafeInteger(value) && value >= 0 ? value : rt::dateNow();
  }

  double alignedBucketStart(double timestamp) {
    return rt::Math::floor(timestamp / PROMPT_CACHE_ANALYTICS_BUCKET_MS) * PROMPT_CACHE_ANALYTICS_BUCKET_MS;
  }

  namespace {
    // `value: unknown`: every caller passes `options.release ?? RELEASE_GIT_SHA`, a string.
    bool knownRelease(const std::string& value) {
      const std::string release = rt::trim(value);
      return rt::toLowerCase(release) != "unknown" && RELEASE_SHA().test(release);
    }
  }

  bool safeCounter(double value) { return rt::Number::isSafeInteger(value) && value >= 0; }

  bool safeCounter(const std::optional<double>& value) { return value && safeCounter(*value); }

  bool safeCounter(const rt::KvKeyPart* value) { return value && value->isNumber() && safeCounter(value->number()); }

  std::optional<double> storedCounter(const rt::KvValue& value) {
    // `typeof value !== "object" || value === null || !("value" in value)`: a KvU64 carries its
    // `value`, a plain object may own one; no other stored object has that property.
    std::optional<rt::BigInt> counter;
    if (value.isU64()) {
      counter = value.asU64().toBigInt();
    } else if (value.isObject()) {
      const rt::KvValue* member = value.asObject().find("value");
      if (!member) return std::nullopt;
      if (!member->isBigInt()) return std::nullopt;
      counter = member->asBigInt();
    } else {
      return std::nullopt;
    }
    if (*counter < rt::BigInt(0) || *counter > rt::BigInt::fromNumber(rt::Number::MAX_SAFE_INTEGER)) return std::nullopt;
    return counter->toNumber();
  }

  namespace {
    // `value: unknown`, read from a `KvEntryMaybe<number>` (null when absent).
    std::optional<double> storedCardinality(const rt::KvValue& value) {
      if (!value.isNumber()) return std::nullopt;
      const double number = value.asNumber();
      if (!rt::Number::isSafeInteger(number) || number < 0 || number > PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET) return std::nullopt;
      return number;
    }
  }

  std::optional<double> roundedPercentage(double numerator, double denominator) {
    if (denominator == 0) return std::nullopt;
    return rt::Math::round((numerator / denominator) * 1'000'000) / 10'000;
  }

  std::optional<double> roundedRatio(double numerator, double denominator) {
    if (denominator == 0) return std::nullopt;
    return rt::Math::round((numerator / denominator) * 10'000) / 10'000;
  }

  namespace {
    bool hasAsciiControlCharacter(const std::string& value) {
      for (const std::uint32_t code : rt::codePoints(value)) {
        if (code < 0x20 || code == 0x7f) return true;
      }
      return false;
    }

    // `value: unknown` (the event's `model?: string | null`).
    std::optional<std::string> normalizedModel(const rt::Nullable<std::string>& value) {
      if (!value.hasValue()) return std::nullopt;
      const std::string model = rt::trim(*value);
      if (model.empty() || static_cast<double>(rt::length(model)) > MAX_MODEL_CHARS || hasAsciiControlCharacter(model)) return std::nullopt;
      return model;
    }
  }

  std::optional<PromptCacheAnalyticsProvider> asProvider(std::optional<std::string_view> value) {
    if (!value || !listIncludes(PROMPT_CACHE_TELEMETRY_PROVIDERS, *value)) return std::nullopt;
    return std::string(*value);
  }

  std::optional<PromptCacheAnalyticsProvider> asProvider(const rt::KvKeyPart* value) { return asProvider(keyPartString(value)); }

  std::optional<PromptCacheAnalyticsRoute> asRoute(std::optional<std::string_view> value) {
    if (!value || !listIncludes(PROMPT_CACHE_TELEMETRY_ROUTES, *value)) return std::nullopt;
    return std::string(*value);
  }

  std::optional<PromptCacheAnalyticsRoute> asRoute(const rt::KvKeyPart* value) { return asRoute(keyPartString(value)); }

  PromptCacheAnalyticsMode asMode(std::optional<std::string_view> value) {
    if (value && listIncludes(PROMPT_CACHE_ANALYTICS_MODES, *value)) return std::string(*value);
    return "unspecified";
  }

  PromptCacheAnalyticsMode asMode(const rt::KvKeyPart* value) { return asMode(keyPartString(value)); }

  PromptCacheAnalyticsFallback asFallback(std::optional<std::string_view> value) {
    if (!value || rt::trim(*value).empty()) return "none";
    return listIncludes(PROMPT_CACHE_ANALYTICS_FALLBACKS, *value) ? std::string(*value) : "other";
  }

  PromptCacheAnalyticsFallback asFallback(const rt::KvKeyPart* value) { return asFallback(keyPartString(value)); }

  PromptCacheAnalyticsRecordResult recordResult(std::string status, std::string reason, std::optional<double> bucketStartAtMs) {
    return PromptCacheAnalyticsRecordResult{.status = std::move(status), .reason = std::move(reason), .bucket_start_at_ms = bucketStartAtMs};
  }

  namespace {
    /** The reason string that reports which kind of usage evidence was recorded. */
    std::string recordedReason(const PromptCacheAnalyticsUsage& usage) {
      if (usage.kind == "reported") return "recorded";
      if (usage.kind == "invalid") return "recorded_invalid_usage";
      return "recorded_without_usage";
    }

    PromptCacheAnalyticsRecordResult recordedResult(double bucketStartAtMs, const PromptCacheAnalyticsUsage& usage) {
      return recordResult("recorded", recordedReason(usage), bucketStartAtMs);
    }

    rt::Task<std::shared_ptr<rt::Kv>> resolveKvOption(std::optional<std::shared_ptr<rt::Kv>> option) {
      std::shared_ptr<rt::Kv> resolved;
      try {
        if (!option) {
          rt::Promise<std::shared_ptr<rt::Kv>> pending = ::uos::kv::getKv();
          resolved = co_await pending.wait();
        } else {
          resolved = *option;
        }
      } catch (...) {
        resolved = nullptr;
      }
      co_return resolved;
    }
  }

  rt::Task<std::shared_ptr<rt::Kv>> resolveKv(PromptCacheAnalyticsOptions options) { return resolveKvOption(std::move(options.kv)); }

  rt::Task<std::shared_ptr<rt::Kv>> resolveKv(PromptCacheAnalyticsReadOptions options) { return resolveKvOption(std::move(options.kv)); }

  namespace {
    const rt::KvKey metaPrefix{PROMPT_CACHE_ANALYTICS_KV_PREFIX, "meta"};
  }

  rt::KvKey promptCacheAnalyticsBucketKey(double bucketStartAtMs) { return rt::KvKey{aggregatePrefix, bucketStartAtMs}; }

  rt::KvKey promptCacheAnalyticsCounterKey(double bucketStartAtMs, const Counter& counter) {
    return rt::KvKey{promptCacheAnalyticsBucketKey(bucketStartAtMs), counter};
  }

  namespace {
    // `readonly [provider, modelHash, route, "keyed" | "unkeyed", mode, fallback]`, spread into keys.
    rt::KvKey dimensionValues(const PromptCacheAnalyticsCohort& cohort) {
      return rt::KvKey{cohort.provider, cohort.modelHash, cohort.route, cohort.promptCacheKeyPresent ? "keyed" : "unkeyed", cohort.mode, cohort.fallback};
    }

    rt::KvKey dimensionCounterKey(double bucketStartAtMs, const PromptCacheAnalyticsCohort& cohort, const Counter& counter) {
      return rt::KvKey{dimensionPrefix, bucketStartAtMs, dimensionValues(cohort), counter};
    }

    rt::KvKey dimensionMarkerKey(double bucketStartAtMs, const PromptCacheAnalyticsCohort& cohort) {
      return rt::KvKey{dimensionPrefix, bucketStartAtMs, dimensionValues(cohort), "marker"};
    }

    rt::KvKey overflowCounterKey(double bucketStartAtMs, const Counter& counter) { return rt::KvKey{overflowPrefix, bucketStartAtMs, counter}; }

    rt::KvKey cardinalityKey(double bucketStartAtMs) { return rt::KvKey{metaPrefix, bucketStartAtMs, "cardinality"}; }
  }

  bool isKnownCounter(std::optional<std::string_view> value) { return value && listIncludes(COUNTERS, *value); }

  bool isKnownCounter(const rt::KvKeyPart* value) { return isKnownCounter(keyPartString(value)); }

  namespace {
    using CounterKeys = std::function<rt::KvKey(const Counter& counter)>;

    rt::AtomicOperation& incrementCounters(rt::AtomicOperation& operation, const CounterKeys& keys, const CounterDeltas& deltas) {
      rt::AtomicOperation* next = &operation;
      for (const std::string_view name : COUNTERS) {
        const Counter counter(name);
        const std::optional<rt::BigInt>& amount = deltas[counter];
        // A valid upstream zero is presence evidence. In particular, a zero cache
        // read or cache write must not become indistinguishable from a missing
        // field during a later grouped read.
        if (!amount) continue;
        next = &next->sum(keys(counter), *amount);
      }
      return *next;
    }
  }

  PromptCacheAnalyticsUsage recordUsage(const PromptCacheAnalyticsEvent& event) {
    CounterDeltas base;
    base.sample_count = rt::BigInt(1);
    if (event.usageTelemetryStatus == "reported") {
      if (safeCounter(event.inputTokens) && safeCounter(event.cachedInputTokens) && *event.cachedInputTokens <= *event.inputTokens &&
          (!event.cacheWriteInputTokens || safeCounter(event.cacheWriteInputTokens))) {
        CounterDeltas deltas = base;
        deltas.input_tokens = rt::BigInt::fromNumber(*event.inputTokens);
        deltas.cached_input_tokens = rt::BigInt::fromNumber(*event.cachedInputTokens);
        deltas.usage_reported_sample_count = rt::BigInt(1);
        if (*event.cachedInputTokens > 0) deltas.request_cache_hit_sample_count = rt::BigInt(1);
        if (event.cacheWriteInputTokens) {
          deltas.cache_write_input_tokens = rt::BigInt::fromNumber(*event.cacheWriteInputTokens);
          deltas.cache_write_reported_sample_count = rt::BigInt(1);
        }
        return PromptCacheAnalyticsUsage{.kind = "reported", .deltas = std::move(deltas)};
      }
      CounterDeltas invalid = base;
      invalid.usage_invalid_sample_count = rt::BigInt(1);
      return PromptCacheAnalyticsUsage{.kind = "invalid", .deltas = std::move(invalid)};
    }
    if (event.usageTelemetryStatus == "invalid") {
      CounterDeltas invalid = base;
      invalid.usage_invalid_sample_count = rt::BigInt(1);
      return PromptCacheAnalyticsUsage{.kind = "invalid", .deltas = std::move(invalid)};
    }
    return PromptCacheAnalyticsUsage{.kind = "missing", .deltas = std::move(base)};
  }

  rt::Task<PromptCacheAnalyticsCohort> resolveCohort(PromptCacheAnalyticsProvider provider, PromptCacheAnalyticsRoute route, PromptCacheAnalyticsEvent event) {
    const std::optional<std::string> model = normalizedModel(event.model);
    std::string modelHash = "unknown";
    if (model) {
      std::string text = "uos-prompt-cache-analytics-model-v2";
      text.push_back('\0');
      text += *model;
      const rt::Promise<std::string> hashing(utils::sha256Hex(std::move(text)));
      modelHash = co_await hashing.wait();
    }
    PromptCacheAnalyticsCohort cohort{
      .provider = std::move(provider),
      .modelHash = std::move(modelHash),
      .route = std::move(route),
      .promptCacheKeyPresent = event.promptCacheKeyPresent.value_or(false),
      .mode = asMode(nullableString(event.promptCacheMode)),
      .fallback = asFallback(nullableString(event.fallbackReason)),
    };
    co_return cohort;
  }

  namespace {
    rt::Task<bool> commitAggregateAndOverflow(std::shared_ptr<rt::Kv> kv, double bucketStartAtMs, CounterDeltas deltas) {
      bool ok = false;
      try {
        const CounterKeys overflowKeys = [bucketStartAtMs](const Counter& counter) { return overflowCounterKey(bucketStartAtMs, counter); };
        const CounterKeys aggregateKeys = [bucketStartAtMs](const Counter& counter) { return promptCacheAnalyticsCounterKey(bucketStartAtMs, counter); };
        const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
        rt::AtomicOperation* operation = &incrementCounters(*atomic, overflowKeys, deltas);
        operation = &incrementCounters(*operation, aggregateKeys, deltas);
        const rt::KvCommitResult committed = co_await operation->commit();
        ok = committed.ok;
      } catch (...) {
        ok = false;
      }
      co_return ok;
    }

    /** Adds this cohort's dimension counters and the shared aggregate counters. */
    rt::AtomicOperation& addCohortCounters(rt::AtomicOperation& operation, double bucketStartAtMs, const PromptCacheAnalyticsCohort& cohort, const CounterDeltas& deltas) {
      const CounterKeys dimensionKeys = [bucketStartAtMs, &cohort](const Counter& counter) { return dimensionCounterKey(bucketStartAtMs, cohort, counter); };
      const CounterKeys aggregateKeys = [bucketStartAtMs](const Counter& counter) { return promptCacheAnalyticsCounterKey(bucketStartAtMs, counter); };
      rt::AtomicOperation& next = incrementCounters(operation, dimensionKeys, deltas);
      return incrementCounters(next, aggregateKeys, deltas);
    }

    rt::Task<bool> commitCohortCounters(std::shared_ptr<rt::Kv> kv, double bucketStartAtMs, PromptCacheAnalyticsCohort cohort, CounterDeltas deltas) {
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      rt::AtomicOperation& operation = addCohortCounters(*atomic, bucketStartAtMs, cohort, deltas);
      const rt::KvCommitResult committed = co_await operation.commit();
      co_return committed.ok;
    }

    /** Records a cohort that arrived after the per-bucket cohort cap was reached. */
    rt::Task<PromptCacheAnalyticsRecordResult> commitCappedCohort(std::shared_ptr<rt::Kv> kv, double bucketStartAtMs, CounterDeltas deltas) {
      CounterDeltas cappedDeltas = std::move(deltas);
      cappedDeltas.dimension_cardinality_limited_sample_count = rt::BigInt(1);
      const rt::Promise<bool> committing(commitAggregateAndOverflow(kv, bucketStartAtMs, cappedDeltas));
      const bool committed = co_await committing.wait();
      if (!committed) co_return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
      co_return recordResult("recorded", "recorded_cardinality_capped", bucketStartAtMs);
    }

    struct CohortAdmission {
      std::shared_ptr<rt::Kv> kv;
      double bucketStartAtMs;
      PromptCacheAnalyticsCohort cohort;
      PromptCacheAnalyticsUsage usage;
      rt::KvKey markerKey;
      rt::KvKey bucketCardinalityKey;
    };

    /**
     * One bounded admission attempt for a cohort inside the current bucket. Returns
     * the recorded outcome, or `null` when the caller must retry with fresh state.
     */
    rt::Task<std::optional<PromptCacheAnalyticsRecordResult>> admitCohort(CohortAdmission admission) {
      const std::shared_ptr<rt::Kv>& kv = admission.kv;
      const double bucketStartAtMs = admission.bucketStartAtMs;
      const PromptCacheAnalyticsCohort& cohort = admission.cohort;
      const PromptCacheAnalyticsUsage& usage = admission.usage;
      std::vector<rt::KvKey> keys{admission.markerKey, admission.bucketCardinalityKey};
      rt::Task<std::vector<rt::KvEntryMaybe<>>> reading = kv->getMany(std::move(keys));
      const std::vector<rt::KvEntryMaybe<>> entries = co_await std::move(reading);
      const rt::KvEntryMaybe<>& marker = entries[0];
      const rt::KvEntryMaybe<>& cardinality = entries[1];
      if (marker.value.isBool() && marker.value.asBool()) {
        const rt::Promise<bool> committing(commitCohortCounters(kv, bucketStartAtMs, cohort, usage.deltas));
        const bool committed = co_await committing.wait();
        if (!committed) co_return std::nullopt;
        co_return recordedResult(bucketStartAtMs, usage);
      }

      const std::optional<double> cardinalityValue = cardinality.value.isNull() ? std::optional<double>(0) : storedCardinality(cardinality.value);
      if (!cardinalityValue) co_return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
      if (*cardinalityValue >= PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET) {
        // `return commitCappedCohort(...)` from an async function adopts that promise.
        const rt::Promise<PromptCacheAnalyticsRecordResult> capping(commitCappedCohort(kv, bucketStartAtMs, usage.deltas));
        co_await rt::microtask();
        PromptCacheAnalyticsRecordResult capped = co_await capping.wait();
        co_return capped;
      }

      const double retentionMs = PROMPT_CACHE_ANALYTICS_RETENTION_MS;
      rt::KvSetOptions expiring;
      expiring.expireIn = retentionMs;
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      rt::AtomicOperation* operation = &atomic->check(marker)
                                          .check(cardinality)
                                          .set(admission.markerKey, rt::KvValue(true), expiring)
                                          .set(admission.bucketCardinalityKey, rt::KvValue(*cardinalityValue + 1), expiring);
      operation = &addCohortCounters(*operation, bucketStartAtMs, cohort, usage.deltas);
      const rt::KvCommitResult committed = co_await operation->commit();
      if (!committed.ok) co_return std::nullopt;
      co_return recordedResult(bucketStartAtMs, usage);
    }
  }

  std::variant<PromptCacheAnalyticsTarget, PromptCacheAnalyticsRecordResult> promptCacheAnalyticsTarget(const PromptCacheAnalyticsEvent& event,
                                                                                                        const PromptCacheAnalyticsOptions& options) {
    const std::string release = options.release ? *options.release : std::string(::uos::release::RELEASE_GIT_SHA);
    if (!knownRelease(release)) return recordResult("ignored", "unknown_release");
    if (!event.completed || !rt::Number::isInteger(event.status) || event.status < 200 || event.status >= 300) return recordResult("ignored", "not_completed_2xx");
    const std::optional<PromptCacheAnalyticsProvider> provider = asProvider(event.provider);
    if (!provider) return recordResult("ignored", "unsupported_provider");
    const std::optional<PromptCacheAnalyticsRoute> route = asRoute(event.route);
    if (!route) return recordResult("ignored", "unsupported_route");
    return PromptCacheAnalyticsTarget{.provider = *provider, .route = *route};
  }

  namespace {
    void pruneRetentionOnEvent(const std::shared_ptr<rt::Kv>& kv, double bucketStartAtMs, double nowMs);
  }

  rt::Task<PromptCacheAnalyticsRecordResult> commitCohortOutcome(std::shared_ptr<rt::Kv> kv, double bucketStartAtMs, PromptCacheAnalyticsCohort cohort,
                                                                PromptCacheAnalyticsUsage usage, double nowMs) {
    const CohortAdmission admission{
      .kv = kv,
      .bucketStartAtMs = bucketStartAtMs,
      .cohort = cohort,
      .usage = usage,
      .markerKey = dimensionMarkerKey(bucketStartAtMs, cohort),
      .bucketCardinalityKey = cardinalityKey(bucketStartAtMs),
    };
    try {
      for (double attempt = 0; attempt <= PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET; attempt += 1) {
        const rt::Promise<std::optional<PromptCacheAnalyticsRecordResult>> admitting(admitCohort(admission));
        const std::optional<PromptCacheAnalyticsRecordResult> outcome = co_await admitting.wait();
        if (outcome) {
          pruneRetentionOnEvent(kv, bucketStartAtMs, nowMs);
          co_return *outcome;
        }
      }
    } catch (...) {
      // The admission failed; the outcome below reports the store as unavailable.
    }
    co_return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
  }

  rt::Task<PromptCacheAnalyticsRecordResult> recordPromptCacheAnalytics(PromptCacheAnalyticsEvent event) {
    return recordPromptCacheAnalytics(std::move(event), PromptCacheAnalyticsOptions());
  }

  rt::Task<PromptCacheAnalyticsRecordResult> recordPromptCacheAnalytics(PromptCacheAnalyticsEvent event, PromptCacheAnalyticsOptions options) {
    const std::variant<PromptCacheAnalyticsTarget, PromptCacheAnalyticsRecordResult> target = promptCacheAnalyticsTarget(event, options);
    if (const PromptCacheAnalyticsRecordResult* refused = std::get_if<PromptCacheAnalyticsRecordResult>(&target)) co_return *refused;
    const PromptCacheAnalyticsTarget& accepted = std::get<PromptCacheAnalyticsTarget>(target);

    const double nowMs = safeNow(options.now ? options.now : rt::globalDateNow());
    const double bucketStartAtMs = alignedBucketStart(nowMs);
    const rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKv(options));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);

    std::optional<PromptCacheAnalyticsCohort> cohort;
    try {
      const rt::Promise<PromptCacheAnalyticsCohort> resolvingCohort(resolveCohort(accepted.provider, accepted.route, event));
      cohort = co_await resolvingCohort.wait();
    } catch (...) {
      cohort.reset();
    }
    if (!cohort) co_return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
    const rt::Promise<PromptCacheAnalyticsRecordResult> committing(commitCohortOutcome(kv, bucketStartAtMs, *cohort, recordUsage(event), nowMs));
    PromptCacheAnalyticsRecordResult result = co_await committing.wait();
    co_return result;
  }

  namespace {
    std::optional<double> storageBucketStart(const rt::KvKey& key) {
      const rt::KvKeyPart* storageNamespace = rt::element(key, static_cast<double>(PROMPT_CACHE_ANALYTICS_KV_PREFIX.size()));
      const rt::KvKeyPart* bucketStartAtMs = rt::element(key, static_cast<double>(PROMPT_CACHE_ANALYTICS_KV_PREFIX.size() + 1));
      const std::optional<std::string_view> name = keyPartString(storageNamespace);
      if ((name != "all" && name != "dimension" && name != "overflow" && name != "meta") || !safeCounter(bucketStartAtMs)) {
        return std::nullopt;
      }
      return bucketStartAtMs->number();
    }

    std::optional<double> legacyStorageBucketStart(const rt::KvKey& key) {
      const rt::KvKeyPart* bucketStartAtMs = rt::element(key, static_cast<double>(LEGACY_PROMPT_CACHE_ANALYTICS_V1_KV_PREFIX.size()));
      if (!safeCounter(bucketStartAtMs)) return std::nullopt;
      return bucketStartAtMs->number();
    }

    /**
     * One prune per analytics bucket, driven by a write instead of the retired
     * hourly cron. Retention still removes everything past its cutoff: the first
     * write in a new bucket prunes, later writes in the same bucket are a no-op, so
     * pruning costs one scan per bucket no matter how much traffic arrives.
     */
    double lastPrunedBucketStartAtMs = 0;

    void pruneRetentionOnEvent(const std::shared_ptr<rt::Kv>& kv, double bucketStartAtMs, double nowMs) {
      if (bucketStartAtMs <= lastPrunedBucketStartAtMs) return;
      lastPrunedBucketStartAtMs = bucketStartAtMs;
      PromptCacheAnalyticsOptions options;
      options.kv = kv;
      options.now = [nowMs] { return nowMs; };
      rt::spawn(prunePromptCacheAnalytics(std::move(options)), rt::ignoreRejection);
    }

    // `deleteBatch` without its closure: the caller takes the batch (`const current = batch;
    // batch = []`) and adds the count it returns to `deleted` once every delete settled.
    rt::Task<double> deleteBatch(std::shared_ptr<rt::Kv> kv, std::vector<rt::KvKey> current) {
      std::vector<rt::Promise<void>> deletions;
      for (const rt::KvKey& key : current) deletions.push_back(rt::Promise<void>(kv->delete_(key)));
      const rt::Promise<void> settled = rt::all(std::move(deletions));
      co_await settled.wait();
      co_return static_cast<double>(current.size());
    }

    using BucketStart = std::optional<double> (*)(const rt::KvKey& key);

    struct PrefixScan {
      const rt::KvKey* prefix;
      BucketStart bucketStart;
    };
  }

  rt::Task<PromptCacheAnalyticsPruneResult> prunePromptCacheAnalytics() { return prunePromptCacheAnalytics(PromptCacheAnalyticsOptions()); }

  rt::Task<PromptCacheAnalyticsPruneResult> prunePromptCacheAnalytics(PromptCacheAnalyticsOptions options) {
    const double nowMs = safeNow(options.now ? options.now : rt::globalDateNow());
    const double cutoffBucketStartAtMs = alignedBucketStart(rt::Math::max(0, nowMs - PROMPT_CACHE_ANALYTICS_RETENTION_MS));
    const rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKv(options));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) {
      PromptCacheAnalyticsPruneResult unavailable{.status = "unavailable", .deleted = 0};
      co_return unavailable;
    }

    double deleted = 0;
    std::vector<rt::KvKey> batch;
    try {
      const double endAtExclusive = cutoffBucketStartAtMs + 1;
      const std::array<PrefixScan, 5> scans{{
        {&aggregatePrefix, &storageBucketStart},
        {&dimensionPrefix, &storageBucketStart},
        {&overflowPrefix, &storageBucketStart},
        {&metaPrefix, &storageBucketStart},
        {&LEGACY_PROMPT_CACHE_ANALYTICS_V1_KV_PREFIX, &legacyStorageBucketStart},
      }};
      for (const PrefixScan& scan : scans) {
        rt::KvListSelector selector;
        selector.prefix = *scan.prefix;
        selector.end = rt::KvKey{*scan.prefix, endAtExclusive};
        const std::unique_ptr<rt::KvListIterator> entries = kv->list(std::move(selector));
        for (;;) {
          const std::optional<rt::KvEntry<>> entry = co_await entries->next();
          if (!entry) break;
          const std::optional<double> bucketStartAtMs = scan.bucketStart(entry->key);
          if (!bucketStartAtMs || *bucketStartAtMs > cutoffBucketStartAtMs) continue;
          batch.push_back(entry->key);
          if (batch.size() == 64) {
            rt::Reason failure;
            try {
              const rt::Promise<double> deleting(deleteBatch(kv, std::exchange(batch, {})));
              const double removed = co_await deleting.wait();
              deleted += removed;
            } catch (...) {
              failure = std::current_exception();
            }
            if (failure) {
              // `for await` closes the iterator when its body throws, then rethrows.
              try {
                co_await entries->return_();
              } catch (...) {
              }
              std::rethrow_exception(failure);
            }
          }
        }
      }
      if (!batch.empty()) {
        const rt::Promise<double> deleting(deleteBatch(kv, std::exchange(batch, {})));
        const double removed = co_await deleting.wait();
        deleted += removed;
      }
      PromptCacheAnalyticsPruneResult pruned{.status = "pruned", .deleted = deleted};
      co_return pruned;
    } catch (...) {
      // Reported below with the deletions that completed.
    }
    PromptCacheAnalyticsPruneResult unavailable{.status = "unavailable", .deleted = deleted};
    co_return unavailable;
  }
}
