// Port of tests/prompt-cache-analytics.test.ts (all ten cases).
// Links: tests/prompt_cache_analytics_test.cpp tests/helpers/counting_kv.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp
//        src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics_queue.cpp src/optional_telemetry_queue.cpp src/release.cpp src/utils.cpp
//        src/kv.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp src/storage/value_compatibility.cpp
// src/storage/key_compatibility.cpp
// The cases share module state as the TS file does (the prune-per-bucket marker), so they run in
// file order.
#include "uos/cache/prompt_analytics.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::cache::prompt_analytics;
using uos::cache::prompt_analytics_core::PromptCacheAnalyticsEvent;
using uos::cache::prompt_analytics_core::PromptCacheAnalyticsOptions;
using uos::tests::helpers::counting_kv::CountingKv;
using uos::tests::helpers::counting_kv::encodeKey;

namespace {
  constexpr std::string_view RELEASE = "0123456789abcdef0123456789abcdef01234567";
  constexpr double NOW_MS = 1'800'000'000'000;
  rt::KvKey LEGACY_V1_PREFIX() { return rt::KvKey{"uos_ai", "prompt_cache_analytics", "v1"}; }
  constexpr std::array<std::string_view, 4> V2_PRUNABLE_NAMESPACES{"all", "dimension", "overflow", "meta"};

  // `for await (const entry of source)` re-yielding the entries below the selector's end bucket.
  class SelectorFilterIterator final : public rt::KvListIterator {
  public:
    SelectorFilterIterator(std::unique_ptr<rt::KvListIterator> source_, std::size_t prefixLength_, std::optional<rt::KvKeyPart> endBucket_,
                           std::vector<rt::KvKey>& listedKeys_)
      : source(std::move(source_)), prefixLength(prefixLength_), endBucket(std::move(endBucket_)), listedKeys(listedKeys_) {}

    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      for (;;) {
        std::optional<rt::KvEntry<>> entry = co_await source->next();
        if (!entry) co_return entry;
        const rt::KvKeyPart* bucket = prefixLength < entry->key.size() ? &entry->key[prefixLength] : nullptr;
        if (endBucket && endBucket->isNumber() && bucket && bucket->isNumber() && bucket->number() >= endBucket->number()) continue;
        listedKeys.push_back(entry->key);
        co_return entry;
      }
    }
    std::string cursor() const override { return source->cursor(); }
    rt::Task<void> return_() override { co_await source->return_(); }

  private:
    std::unique_ptr<rt::KvListIterator> source;
    std::size_t prefixLength;
    std::optional<rt::KvKeyPart> endBucket;
    std::vector<rt::KvKey>& listedKeys;
  };

  class ListSelectorCountingKv final : public CountingKv {
  public:
    std::vector<rt::KvListSelector> listSelectors;
    std::vector<rt::KvKey> listedKeys;

    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      listSelectors.push_back(selector);
      std::unique_ptr<rt::KvListIterator> source = CountingKv::list(selector, options);
      const rt::KvKey prefix = selector.prefix ? *selector.prefix : rt::KvKey();
      std::optional<rt::KvKeyPart> endBucket;
      if (selector.end && prefix.size() < selector.end->size()) endBucket = (*selector.end)[prefix.size()];
      return std::make_unique<SelectorFilterIterator>(std::move(source), prefix.size(), std::move(endBucket), listedKeys);
    }
  };

  PromptCacheAnalyticsEvent event() {
    PromptCacheAnalyticsEvent value{
      .provider = "chatgpt_codex",
      .model = "gpt-cache-visible-only-as-a-hash",
      .route = "responses",
      .status = 200,
      .completed = true,
      .usageTelemetryStatus = "reported",
      .inputTokens = 100,
      .cachedInputTokens = 25,
      .cacheWriteInputTokens = 10,
      .promptCacheKeyPresent = true,
      .promptCacheMode = "explicit",
      .fallbackReason = nullptr,
    };
    return value;
  }

  PromptCacheAnalyticsOptions options(const std::shared_ptr<rt::Kv>& kv, double now = NOW_MS) {
    PromptCacheAnalyticsOptions value;
    value.kv = kv;
    value.release = std::string(RELEASE);
    value.now = [now] { return now; };
    return value;
  }

  // `{ ...options(kv), groupBy }`; the read options carry kv and now.
  PromptCacheAnalyticsReadOptions readOptions(const std::shared_ptr<rt::Kv>& kv, std::optional<std::vector<std::string>> groupBy = std::nullopt, double now = NOW_MS) {
    PromptCacheAnalyticsReadOptions value;
    value.kv = kv;
    value.now = [now] { return now; };
    value.groupBy = std::move(groupBy);
    return value;
  }

  json::value toJson(const PromptCacheAnalyticsBucket& bucket) { return json::value_from(bucket); }

  json::array bucketsJson(const PromptCacheAnalyticsView& view) {
    json::array out;
    for (const PromptCacheAnalyticsBucket& bucket : view.buckets) out.push_back(toJson(bucket));
    return out;
  }

  // `{ name: source.a?.b, ... }`: a member that reads as undefined is left out, as deepEqual sees it.
  json::object pick(const json::value& source, const std::vector<std::pair<std::string, std::string>>& fields) {
    json::object out;
    for (const auto& [name, path] : fields) {
      const json::value* value = &source;
      for (const std::string& segment : rt::split(path, ".")) value = value ? rt::get(*value, segment) : nullptr;
      if (value) out[name] = *value;
    }
    return out;
  }

  // `groupedCounters(bucket)`
  json::object groupedCounters(const PromptCacheAnalyticsBucket& bucket) {
    return pick(toJson(bucket), {{"input", "input_tokens"},
                                 {"cached", "cached_input_tokens"},
                                 {"write", "cache_write_input_tokens"},
                                 {"writeSamples", "cache_write_reported_sample_count"},
                                 {"samples", "sample_count"},
                                 {"hits", "request_cache_hit_sample_count"},
                                 {"reported", "usage_reported_sample_count"}});
  }

  // `{ name: bucket.group?.<member>, ...groupedCounters(bucket) }`
  json::object groupedRow(const PromptCacheAnalyticsBucket& bucket, const std::string& name, const std::string& member) {
    json::object row = pick(toJson(bucket), {{name, "group." + member}});
    for (const auto& field : groupedCounters(bucket)) row[field.key()] = field.value();
    return row;
  }

  // `assert.deepEqual(actual, expected)` on JSON-shaped values.
  void expectJson(const json::value& actual, std::string_view expected, const char* file, int line) {
    const json::value parsed = rt::jsonParse(expected);
    if (!rt::deepEqual(actual, parsed)) {
      rt::test::report(file, line, "expected " + rt::jsonStringify(actual) + "\n      to deep-equal " + rt::jsonStringify(parsed), false);
    }
  }

  std::string persistedKeys(const CountingKv& kv) {
    std::string joined;
    bool first = true;
    for (const auto& [encoded, entry] : kv.entries) {
      if (!first) joined += "\n";
      first = false;
      joined += encodeKey(entry.key);
    }
    return joined;
  }

  // `key.includes(value)` on a key array (SameValueZero on number parts).
  bool keyIncludesNumber(const rt::KvKey& key, double value) {
    for (const rt::KvKeyPart& part : key)
      if (part.isNumber() && part.number() == value) return true;
    return false;
  }

  bool sameKey(const std::optional<rt::KvKey>& actual, const rt::KvKey& expected) {
    if (!actual || actual->size() != expected.size()) return false;
    for (std::size_t index = 0; index < expected.size(); ++index)
      if (!((*actual)[index] == expected[index])) return false;
    return true;
  }
}

#define EXPECT_JSON(actual, expected) expectJson((actual), (expected), __FILE__, __LINE__)

ASYNC_TEST("prompt-cache analytics v2 aggregates safe cohorts and keeps model and key material opaque") {
  const auto kv = std::make_shared<CountingKv>();
  const std::string rawModel = "gpt-cache-secret-model";
  const std::string rawKey = "prompt-cache-secret-key";
  PromptCacheAnalyticsEvent first = event();
  first.model = rawModel;
  first.promptCacheKeyPresent = true;
  PromptCacheAnalyticsEvent second = event();
  second.model = "gpt-cache-second-model";
  second.route = "chat.completions";
  second.inputTokens = 300;
  second.cachedInputTokens = 0;
  second.cacheWriteInputTokens = std::nullopt;
  second.promptCacheKeyPresent = false;
  second.promptCacheMode = "implicit";
  second.fallbackReason = "primary_429";
  PromptCacheAnalyticsEvent third = event();
  third.provider = "metered";
  third.model = "gpt-cache-metered-model";
  third.inputTokens = 200;
  third.cachedInputTokens = 100;
  third.cacheWriteInputTokens = 50;
  third.promptCacheKeyPresent = true;
  third.promptCacheMode = "legacy_retention";
  std::vector<rt::Promise<PromptCacheAnalyticsRecordResult>> pending;
  pending.push_back(rt::Promise(recordPromptCacheAnalytics(first, options(kv))));
  pending.push_back(rt::Promise(recordPromptCacheAnalytics(second, options(kv))));
  pending.push_back(rt::Promise(recordPromptCacheAnalytics(third, options(kv))));
  const rt::Promise<std::vector<PromptCacheAnalyticsRecordResult>> all = rt::all(std::move(pending));
  const std::vector<PromptCacheAnalyticsRecordResult> recorded = co_await all.wait();
  std::vector<std::string> statuses;
  for (const PromptCacheAnalyticsRecordResult& result : recorded) statuses.push_back(result.status);
  EXPECT_EQ(statuses, (std::vector<std::string>{"recorded", "recorded", "recorded"}));

  const PromptCacheAnalyticsReadOptions aggregateOptions = readOptions(kv);
  auto readingAggregate = readPromptCacheAnalytics(aggregateOptions);
  const PromptCacheAnalyticsView aggregate = co_await std::move(readingAggregate);
  EXPECT_EQ(aggregate.status, std::string("ready"));
  EXPECT_EQ(aggregate.group_by, std::vector<std::string>{});
  EXPECT_JSON(json::value(bucketsJson(aggregate)), R"([{"bucket_start_at_ms":1800000000000,"bucket_end_at_ms":1800000900000,"input_tokens":600,
    "cached_input_tokens":125,"cache_write_input_tokens":60,"cache_write_reported_sample_count":2,"cached_percentage":20.8333,"sample_count":3}])");

  const PromptCacheAnalyticsReadOptions providerOptions = readOptions(kv, std::vector<std::string>{"provider"});
  auto readingByProvider = readPromptCacheAnalytics(providerOptions);
  const PromptCacheAnalyticsView byProvider = co_await std::move(readingByProvider);
  EXPECT_EQ(byProvider.status, std::string("ready"));
  EXPECT_EQ(byProvider.cardinality_limited, false);
  json::array providerRows;
  for (const PromptCacheAnalyticsBucket& bucket : byProvider.buckets) {
    providerRows.push_back(pick(toJson(bucket), {{"provider", "group.provider"},
                                                 {"input", "input_tokens"},
                                                 {"cached", "cached_input_tokens"},
                                                 {"token", "token_hit_percentage"},
                                                 {"request", "request_hit_percentage"},
                                                 {"coverage", "usage_telemetry_coverage_percentage"},
                                                 {"cacheReadsPerWrite", "cache_reads_per_write"}}));
  }
  EXPECT_JSON(json::value(providerRows), R"([
    {"provider":"chatgpt_codex","input":400,"cached":25,"token":6.25,"request":50,"coverage":100,"cacheReadsPerWrite":2.5},
    {"provider":"metered","input":200,"cached":100,"token":50,"request":100,"coverage":100,"cacheReadsPerWrite":2}])");

  const PromptCacheAnalyticsReadOptions keyAndRouteOptions = readOptions(kv, std::vector<std::string>{"key_presence", "route"});
  auto readingByKeyAndRoute = readPromptCacheAnalytics(keyAndRouteOptions);
  const PromptCacheAnalyticsView byKeyAndRoute = co_await std::move(readingByKeyAndRoute);
  json::array keyAndRouteGroups;
  for (const PromptCacheAnalyticsBucket& bucket : byKeyAndRoute.buckets) keyAndRouteGroups.push_back(bucket.group ? json::value(*bucket.group) : json::value(nullptr));
  EXPECT_JSON(json::value(keyAndRouteGroups),
              R"([{"prompt_cache_key_present":false,"route":"chat.completions"},{"prompt_cache_key_present":true,"route":"responses"}])");

  const PromptCacheAnalyticsReadOptions keyPresenceOptions = readOptions(kv, std::vector<std::string>{"key_presence"});
  auto readingByKeyPresence = readPromptCacheAnalytics(keyPresenceOptions);
  const PromptCacheAnalyticsView byKeyPresence = co_await std::move(readingByKeyPresence);
  json::array keyPresenceRows;
  for (const PromptCacheAnalyticsBucket& bucket : byKeyPresence.buckets) keyPresenceRows.push_back(groupedRow(bucket, "keyed", "prompt_cache_key_present"));
  EXPECT_JSON(json::value(keyPresenceRows), R"([
    {"keyed":false,"input":300,"cached":0,"write":null,"writeSamples":0,"samples":1,"hits":0,"reported":1},
    {"keyed":true,"input":300,"cached":125,"write":60,"writeSamples":2,"samples":2,"hits":2,"reported":2}])");

  const std::string persisted = persistedKeys(*kv);
  EXPECT(!rt::Regex(rawModel).test(persisted));
  EXPECT(!rt::Regex(rawKey).test(persisted));
  bool everyBucketOpaque = true;
  for (const PromptCacheAnalyticsBucket& bucket : byProvider.buckets) everyBucketOpaque = everyBucketOpaque && !rt::includes(rt::jsonStringify(toJson(bucket)), rawModel);
  EXPECT(everyBucketOpaque);
}

ASYNC_TEST("prompt-cache analytics groups and aggregates model, route, mode, and fallback cohorts") {
  const auto kv = std::make_shared<CountingKv>();
  const std::vector<std::string> models{"gpt-dimension-shared", "gpt-dimension-route", "gpt-dimension-mode", "gpt-dimension-fallback"};
  struct Input {
    std::string model;
    std::string route;
    bool promptCacheKeyPresent;
    std::string promptCacheMode;
    std::optional<std::string> fallbackReason;
    double inputTokens;
    double cachedInputTokens;
    double cacheWriteInputTokens;
  };
  const std::vector<Input> inputs{
    {models[0], "responses", true, "explicit", std::nullopt, 100, 10, 5},
    {models[0], "chat.completions", false, "implicit", "primary_429", 200, 20, 10},
    {models[1], "responses", false, "implicit", "primary_429", 300, 30, 15},
    {models[2], "chat.completions", true, "explicit", "primary_429", 400, 40, 20},
    {models[3], "responses", false, "explicit", std::nullopt, 500, 50, 25},
  };
  for (const Input& input : inputs) {
    PromptCacheAnalyticsEvent recordedEvent = event();
    recordedEvent.model = input.model;
    recordedEvent.route = input.route;
    recordedEvent.promptCacheKeyPresent = input.promptCacheKeyPresent;
    recordedEvent.promptCacheMode = input.promptCacheMode;
    recordedEvent.fallbackReason = rt::orNull(input.fallbackReason);
    recordedEvent.inputTokens = input.inputTokens;
    recordedEvent.cachedInputTokens = input.cachedInputTokens;
    recordedEvent.cacheWriteInputTokens = input.cacheWriteInputTokens;
    auto recording = recordPromptCacheAnalytics(recordedEvent, options(kv));
    const PromptCacheAnalyticsRecordResult result = co_await std::move(recording);
    EXPECT_EQ(result.status, std::string("recorded"));
  }

  const PromptCacheAnalyticsReadOptions modelOptions = readOptions(kv, std::vector<std::string>{"model"});
  auto readingByModel = readPromptCacheAnalytics(modelOptions);
  const PromptCacheAnalyticsView byModel = co_await std::move(readingByModel);
  std::vector<json::object> modelRows;
  for (const PromptCacheAnalyticsBucket& bucket : byModel.buckets) modelRows.push_back(groupedRow(bucket, "modelHash", "model_hash"));
  rt::sort(modelRows, [](const json::object& left, const json::object& right) {
    const double samples = rt::getNumber(left, "samples").value_or(rt::Number::NaN) - rt::getNumber(right, "samples").value_or(rt::Number::NaN);
    if (rt::truthy(samples)) return samples;
    return rt::getNumber(left, "input").value_or(-1) - rt::getNumber(right, "input").value_or(-1);
  });
  EXPECT_EQ(modelRows.size(), std::size_t{4});
  bool everyHash = true;
  rt::OrderedSet<std::string> hashes;
  for (const json::object& row : modelRows) {
    const std::optional<std::string_view> hash = rt::getString(row, "modelHash");
    everyHash = everyHash && hash && rt::Regex("^[a-f0-9]{64}$").test(*hash);
    if (hash) hashes.add(std::string(*hash));
  }
  EXPECT(everyHash);
  EXPECT_EQ(hashes.size(), std::size_t{4});
  json::array counters;
  for (json::object row : modelRows) {
    rt::objectDelete(row, "modelHash");
    counters.push_back(json::value(std::move(row)));
  }
  EXPECT_JSON(json::value(counters), R"([
    {"input":300,"cached":30,"write":15,"writeSamples":1,"samples":1,"hits":1,"reported":1},
    {"input":400,"cached":40,"write":20,"writeSamples":1,"samples":1,"hits":1,"reported":1},
    {"input":500,"cached":50,"write":25,"writeSamples":1,"samples":1,"hits":1,"reported":1},
    {"input":300,"cached":30,"write":15,"writeSamples":2,"samples":2,"hits":2,"reported":2}])");
  json::array modelBuckets = bucketsJson(byModel);
  const std::string modelBucketsText = rt::jsonStringify(json::value(modelBuckets));
  bool noModelText = true;
  for (const std::string& model : models) noModelText = noModelText && !rt::includes(modelBucketsText, model);
  EXPECT(noModelText);

  // `.sort((left, right) => String(left.<name>).localeCompare(String(right.<name>)))`
  const auto byName = [](const std::string& name) {
    return [name](const json::object& left, const json::object& right) {
      return static_cast<double>(rt::localeCompare(rt::toString(rt::get(left, name)), rt::toString(rt::get(right, name))));
    };
  };

  const PromptCacheAnalyticsReadOptions routeOptions = readOptions(kv, std::vector<std::string>{"route"});
  auto readingByRoute = readPromptCacheAnalytics(routeOptions);
  const PromptCacheAnalyticsView byRoute = co_await std::move(readingByRoute);
  std::vector<json::object> routeRows;
  for (const PromptCacheAnalyticsBucket& bucket : byRoute.buckets) routeRows.push_back(groupedRow(bucket, "route", "route"));
  rt::sort(routeRows, byName("route"));
  EXPECT_JSON(json::value(json::array(routeRows.begin(), routeRows.end())), R"([
    {"route":"chat.completions","input":600,"cached":60,"write":30,"writeSamples":2,"samples":2,"hits":2,"reported":2},
    {"route":"responses","input":900,"cached":90,"write":45,"writeSamples":3,"samples":3,"hits":3,"reported":3}])");

  const PromptCacheAnalyticsReadOptions modeOptions = readOptions(kv, std::vector<std::string>{"mode"});
  auto readingByMode = readPromptCacheAnalytics(modeOptions);
  const PromptCacheAnalyticsView byMode = co_await std::move(readingByMode);
  std::vector<json::object> modeRows;
  for (const PromptCacheAnalyticsBucket& bucket : byMode.buckets) modeRows.push_back(groupedRow(bucket, "mode", "mode"));
  rt::sort(modeRows, byName("mode"));
  EXPECT_JSON(json::value(json::array(modeRows.begin(), modeRows.end())), R"([
    {"mode":"explicit","input":1000,"cached":100,"write":50,"writeSamples":3,"samples":3,"hits":3,"reported":3},
    {"mode":"implicit","input":500,"cached":50,"write":25,"writeSamples":2,"samples":2,"hits":2,"reported":2}])");

  const PromptCacheAnalyticsReadOptions fallbackOptions = readOptions(kv, std::vector<std::string>{"fallback"});
  auto readingByFallback = readPromptCacheAnalytics(fallbackOptions);
  const PromptCacheAnalyticsView byFallback = co_await std::move(readingByFallback);
  std::vector<json::object> fallbackRows;
  for (const PromptCacheAnalyticsBucket& bucket : byFallback.buckets) fallbackRows.push_back(groupedRow(bucket, "fallback", "fallback"));
  rt::sort(fallbackRows, byName("fallback"));
  EXPECT_JSON(json::value(json::array(fallbackRows.begin(), fallbackRows.end())), R"([
    {"fallback":"none","input":600,"cached":60,"write":30,"writeSamples":2,"samples":2,"hits":2,"reported":2},
    {"fallback":"primary_429","input":900,"cached":90,"write":45,"writeSamples":3,"samples":3,"hits":3,"reported":3}])");
}

ASYNC_TEST("prompt-cache analytics distinguishes reported zeroes from missing and invalid telemetry") {
  const auto kv = std::make_shared<CountingKv>();
  PromptCacheAnalyticsEvent partial = event();
  partial.usageTelemetryStatus = "partial";
  partial.inputTokens = std::nullopt;
  partial.cachedInputTokens = std::nullopt;
  auto recordingPartial = recordPromptCacheAnalytics(partial, options(kv));
  co_await std::move(recordingPartial);
  PromptCacheAnalyticsEvent invalid = event();
  invalid.inputTokens = 10;
  invalid.cachedInputTokens = 11;
  auto recordingInvalid = recordPromptCacheAnalytics(invalid, options(kv));
  co_await std::move(recordingInvalid);
  PromptCacheAnalyticsEvent zeroes = event();
  zeroes.inputTokens = 20;
  zeroes.cachedInputTokens = 0;
  zeroes.cacheWriteInputTokens = 0;
  auto recordingZeroes = recordPromptCacheAnalytics(zeroes, options(kv));
  co_await std::move(recordingZeroes);

  const PromptCacheAnalyticsReadOptions keyPresenceOptions = readOptions(kv, std::vector<std::string>{"key_presence"});
  auto reading = readPromptCacheAnalytics(keyPresenceOptions);
  const PromptCacheAnalyticsView view = co_await std::move(reading);
  ASSERT(!view.buckets.empty());  // key_presence grouping must produce a bucket
  const PromptCacheAnalyticsBucket& bucket = view.buckets[0];
  EXPECT_JSON(json::value(pick(toJson(bucket), {{"input", "input_tokens"},
                                                {"cached", "cached_input_tokens"},
                                                {"samples", "sample_count"},
                                                {"reported", "usage_reported_sample_count"},
                                                {"invalid", "usage_invalid_sample_count"},
                                                {"missing", "usage_missing_sample_count"},
                                                {"token", "token_hit_percentage"},
                                                {"request", "request_hit_percentage"},
                                                {"coverage", "usage_telemetry_coverage_percentage"},
                                                {"write", "cache_write_input_tokens"},
                                                {"writeSamples", "cache_write_reported_sample_count"}})),
              R"({"input":20,"cached":0,"samples":3,"reported":1,"invalid":1,"missing":1,"token":0,"request":0,"coverage":33.3333,"write":0,
                  "writeSamples":1})");
}

ASYNC_TEST("prompt-cache analytics preserves concurrent counters and admits its first grouped cohort") {
  const auto kv = std::make_shared<CountingKv>();
  std::vector<rt::Promise<PromptCacheAnalyticsRecordResult>> pending;
  for (int index = 0; index < 100; index += 1) pending.push_back(rt::Promise(recordPromptCacheAnalytics(event(), options(kv))));
  const rt::Promise<std::vector<PromptCacheAnalyticsRecordResult>> all = rt::all(std::move(pending));
  const std::vector<PromptCacheAnalyticsRecordResult> results = co_await all.wait();
  bool everyRecorded = true;
  for (const PromptCacheAnalyticsRecordResult& result : results) everyRecorded = everyRecorded && result.status == "recorded";
  EXPECT(everyRecorded);

  const PromptCacheAnalyticsReadOptions providerOptions = readOptions(kv, std::vector<std::string>{"provider"});
  auto reading = readPromptCacheAnalytics(providerOptions);
  const PromptCacheAnalyticsView grouped = co_await std::move(reading);
  EXPECT_EQ(grouped.buckets.size(), std::size_t{1});
  ASSERT(!grouped.buckets.empty());
  EXPECT_JSON(json::value(pick(toJson(grouped.buckets[0]), {{"provider", "group.provider"},
                                                           {"input", "input_tokens"},
                                                           {"cached", "cached_input_tokens"},
                                                           {"samples", "sample_count"},
                                                           {"hits", "request_cache_hit_sample_count"}})),
              R"({"provider":"chatgpt_codex","input":10000,"cached":2500,"samples":100,"hits":100})");
}

ASYNC_TEST("prompt-cache analytics preserves concurrent samples across new-cohort admission conflicts") {
  const auto kv = std::make_shared<CountingKv>();
  const std::array<std::string, 3> modes{"implicit", "explicit", "legacy_retention"};
  std::vector<PromptCacheAnalyticsEvent> cohorts;
  for (int index = 0; index < 12; index += 1) {
    PromptCacheAnalyticsEvent cohort = event();
    cohort.model = nullptr;
    cohort.route = index % 2 == 0 ? "responses" : "chat.completions";
    cohort.promptCacheKeyPresent = (index / 2) % 2 == 0;
    cohort.promptCacheMode = modes[static_cast<std::size_t>(index / 4)];
    cohorts.push_back(cohort);
  }

  std::vector<rt::Promise<PromptCacheAnalyticsRecordResult>> pending;
  for (const PromptCacheAnalyticsEvent& cohort : cohorts) pending.push_back(rt::Promise(recordPromptCacheAnalytics(cohort, options(kv))));
  const rt::Promise<std::vector<PromptCacheAnalyticsRecordResult>> all = rt::all(std::move(pending));
  const std::vector<PromptCacheAnalyticsRecordResult> results = co_await all.wait();
  bool someConflict = false;
  for (const auto& command : kv->commands) someConflict = someConflict || (command.command == "atomic.commit" && command.atomicResult == "conflict");
  EXPECT(someConflict);
  bool everyRecorded = true;
  for (const PromptCacheAnalyticsRecordResult& result : results) everyRecorded = everyRecorded && result.status == "recorded" && result.reason == "recorded";
  EXPECT(everyRecorded);

  const PromptCacheAnalyticsReadOptions aggregateOptions = readOptions(kv);
  auto readingAggregate = readPromptCacheAnalytics(aggregateOptions);
  const PromptCacheAnalyticsView aggregate = co_await std::move(readingAggregate);
  ASSERT(!aggregate.buckets.empty());
  EXPECT_EQ(aggregate.buckets[0].sample_count, static_cast<double>(cohorts.size()));
  const PromptCacheAnalyticsReadOptions routeModeOptions = readOptions(kv, std::vector<std::string>{"route", "mode"});
  auto readingGrouped = readPromptCacheAnalytics(routeModeOptions);
  const PromptCacheAnalyticsView grouped = co_await std::move(readingGrouped);
  EXPECT_EQ(grouped.cardinality_limited, false);
  EXPECT_EQ(grouped.truncated, false);
  double sampleCount = 0;
  for (const PromptCacheAnalyticsBucket& bucket : grouped.buckets) sampleCount += bucket.sample_count;
  EXPECT_EQ(sampleCount, static_cast<double>(cohorts.size()));
}

ASYNC_TEST("prompt-cache analytics caps cohort cardinality while retaining aggregate evidence") {
  const auto kv = std::make_shared<CountingKv>();
  for (double index = 0; index < PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET; index += 1) {
    PromptCacheAnalyticsEvent cohort = event();
    cohort.model = "gpt-cardinality-" + rt::numberToString(index);
    auto recording = recordPromptCacheAnalytics(cohort, options(kv));
    const PromptCacheAnalyticsRecordResult result = co_await std::move(recording);
    EXPECT_EQ(result.reason, std::string("recorded"));
  }
  PromptCacheAnalyticsEvent overflow = event();
  overflow.model = "gpt-cardinality-overflow";
  PromptCacheAnalyticsEvent overflowSecond = event();
  overflowSecond.model = "gpt-cardinality-overflow-second";
  overflowSecond.inputTokens = 200;
  overflowSecond.cachedInputTokens = 50;
  overflowSecond.cacheWriteInputTokens = 20;
  const std::vector<PromptCacheAnalyticsEvent> overflowInputs{overflow, overflowSecond};
  for (const PromptCacheAnalyticsEvent& input : overflowInputs) {
    auto recording = recordPromptCacheAnalytics(input, options(kv));
    const PromptCacheAnalyticsRecordResult result = co_await std::move(recording);
    EXPECT_EQ(result, (PromptCacheAnalyticsRecordResult{.status = "recorded", .reason = "recorded_cardinality_capped", .bucket_start_at_ms = NOW_MS}));
  }

  const PromptCacheAnalyticsReadOptions aggregateOptions = readOptions(kv);
  auto readingAggregate = readPromptCacheAnalytics(aggregateOptions);
  const PromptCacheAnalyticsView aggregate = co_await std::move(readingAggregate);
  ASSERT(!aggregate.buckets.empty());
  EXPECT_JSON(json::value(pick(toJson(aggregate.buckets[0]), {{"input", "input_tokens"},
                                                             {"cached", "cached_input_tokens"},
                                                             {"write", "cache_write_input_tokens"},
                                                             {"writeSamples", "cache_write_reported_sample_count"},
                                                             {"samples", "sample_count"}})),
              R"({"input":3500,"cached":875,"write":350,"writeSamples":34,"samples":34})");
  const PromptCacheAnalyticsReadOptions modelOptions = readOptions(kv, std::vector<std::string>{"model"});
  auto readingByModel = readPromptCacheAnalytics(modelOptions);
  const PromptCacheAnalyticsView byModel = co_await std::move(readingByModel);
  EXPECT_EQ(static_cast<double>(byModel.buckets.size()), PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET + 1);
  EXPECT_EQ(byModel.cardinality_limited, true);
  EXPECT_EQ(byModel.truncated, true);
  std::size_t detailed = 0;
  for (const PromptCacheAnalyticsBucket& bucket : byModel.buckets) {
    const std::optional<bool> limited = bucket.group ? rt::getBool(*bucket.group, "cardinality_limited") : std::nullopt;
    if (limited != true) detailed += 1;
  }
  EXPECT_EQ(detailed, std::size_t{32});
  EXPECT(!rt::Regex("gpt-cardinality-overflow").test(rt::jsonStringify(json::value_from(byModel))));

  for (const std::string_view dimension : PROMPT_CACHE_ANALYTICS_DIMENSIONS) {
    const PromptCacheAnalyticsReadOptions groupedOptions = readOptions(kv, std::vector<std::string>{std::string(dimension)});
    auto readingGrouped = readPromptCacheAnalytics(groupedOptions);
    const PromptCacheAnalyticsView grouped = co_await std::move(readingGrouped);
    EXPECT_EQ(grouped.cardinality_limited, true);
    EXPECT_EQ(grouped.truncated, true);
    const PromptCacheAnalyticsBucket* overflowBucket = nullptr;
    for (const PromptCacheAnalyticsBucket& bucket : grouped.buckets) {
      if (!overflowBucket && bucket.group && rt::getBool(*bucket.group, "cardinality_limited") == true) overflowBucket = &bucket;
    }
    ASSERT(overflowBucket);
    EXPECT_JSON(json::value(pick(toJson(*overflowBucket), {{"group", "group"},
                                                          {"input", "input_tokens"},
                                                          {"cached", "cached_input_tokens"},
                                                          {"write", "cache_write_input_tokens"},
                                                          {"writeSamples", "cache_write_reported_sample_count"},
                                                          {"samples", "sample_count"},
                                                          {"hits", "request_cache_hit_sample_count"},
                                                          {"reported", "usage_reported_sample_count"},
                                                          {"limited", "dimension_cardinality_limited_sample_count"}})),
                R"({"group":{"cardinality_limited":true},"input":300,"cached":75,"write":30,"writeSamples":2,"samples":2,"hits":2,"reported":2,
                    "limited":2})");
  }
}

ASYNC_TEST("prompt-cache analytics caps grouped response cardinality and marks the result") {
  const auto kv = std::make_shared<CountingKv>();
  const double bucketCount = rt::Math::ceil(PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS / PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET) + 1;
  for (double bucket = 0; bucket < bucketCount; bucket += 1) {
    const double bucketNow = NOW_MS - bucket * PROMPT_CACHE_ANALYTICS_BUCKET_MS;
    for (double cohort = 0; cohort < PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET; cohort += 1) {
      PromptCacheAnalyticsEvent input = event();
      input.model = "gpt-response-cap-" + rt::numberToString(bucket) + "-" + rt::numberToString(cohort);
      auto recording = recordPromptCacheAnalytics(input, options(kv, bucketNow));
      const PromptCacheAnalyticsRecordResult result = co_await std::move(recording);
      EXPECT_EQ(result.status, std::string("recorded"));
    }
  }
  const PromptCacheAnalyticsReadOptions modelOptions = readOptions(kv, std::vector<std::string>{"model"});
  auto reading = readPromptCacheAnalytics(modelOptions);
  const PromptCacheAnalyticsView view = co_await std::move(reading);
  EXPECT_EQ(view.truncated, true);
  EXPECT_EQ(view.max_buckets, PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS);
  EXPECT_EQ(static_cast<double>(view.buckets.size()), PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS);
}

ASYNC_TEST("prompt-cache analytics returns the complete ungrouped seven-day window") {
  const auto kv = std::make_shared<CountingKv>();
  for (double offset = 0; offset <= PROMPT_CACHE_ANALYTICS_WINDOW_BUCKETS; offset += 1) {
    kv->seed(promptCacheAnalyticsCounterKey(NOW_MS - offset * PROMPT_CACHE_ANALYTICS_BUCKET_MS, "sample_count"), rt::KvValue(rt::KvU64(1)));
  }

  const PromptCacheAnalyticsReadOptions aggregateOptions = readOptions(kv);
  auto reading = readPromptCacheAnalytics(aggregateOptions);
  const PromptCacheAnalyticsView view = co_await std::move(reading);
  EXPECT_EQ(view.group_by, std::vector<std::string>{});
  EXPECT_EQ(view.max_buckets, PROMPT_CACHE_ANALYTICS_WINDOW_BUCKETS);
  EXPECT_EQ(view.truncated, false);
  std::vector<double> starts;
  for (const PromptCacheAnalyticsBucket& bucket : view.buckets) starts.push_back(bucket.bucket_start_at_ms);
  std::vector<double> expected;
  for (double index = 0; index < PROMPT_CACHE_ANALYTICS_WINDOW_BUCKETS; index += 1) {
    expected.push_back(NOW_MS - (PROMPT_CACHE_ANALYTICS_WINDOW_BUCKETS - 1 - index) * PROMPT_CACHE_ANALYTICS_BUCKET_MS);
  }
  EXPECT_EQ(starts, expected);
}

ASYNC_TEST("prompt-cache analytics isolates v1 reads and prunes stale v1 and v2 entries with bounded scans") {
  const auto kv = std::make_shared<ListSelectorCountingKv>();
  const double expiredBucket = NOW_MS - PROMPT_CACHE_ANALYTICS_RETENTION_MS;
  const double freshBucket = NOW_MS;
  kv->seed(rt::KvKey{LEGACY_V1_PREFIX(), freshBucket, "input_tokens"}, rt::KvValue(rt::KvU64(777)));
  kv->seed(rt::KvKey{LEGACY_V1_PREFIX(), freshBucket, "cached_input_tokens"}, rt::KvValue(rt::KvU64(777)));
  kv->seed(rt::KvKey{LEGACY_V1_PREFIX(), freshBucket, "sample_count"}, rt::KvValue(rt::KvU64(1)));
  const PromptCacheAnalyticsReadOptions aggregateOptions = readOptions(kv);
  auto readingLegacy = readPromptCacheAnalytics(aggregateOptions);
  const PromptCacheAnalyticsView legacy = co_await std::move(readingLegacy);
  EXPECT(legacy.buckets.empty());
  kv->listSelectors.clear();
  kv->listedKeys.clear();
  for (const std::string_view storageNamespace : V2_PRUNABLE_NAMESPACES) {
    kv->seed(rt::KvKey{PROMPT_CACHE_ANALYTICS_KV_PREFIX, storageNamespace, freshBucket, "fresh-fixture"}, rt::KvValue(rt::KvU64(1)));
  }

  PromptCacheAnalyticsEvent expired = event();
  expired.model = "gpt-expired";
  auto recording = recordPromptCacheAnalytics(expired, options(kv, expiredBucket));
  co_await std::move(recording);
  kv->seed(rt::KvKey{PROMPT_CACHE_ANALYTICS_KV_PREFIX, "overflow", expiredBucket, "sample_count"}, rt::KvValue(rt::KvU64(1)));
  kv->seed(rt::KvKey{LEGACY_V1_PREFIX(), expiredBucket, "input_tokens"}, rt::KvValue(rt::KvU64(1)));
  PromptCacheAnalyticsOptions pruneOptions;
  pruneOptions.kv = kv;
  pruneOptions.now = [] { return NOW_MS; };
  auto pruning = prunePromptCacheAnalytics(pruneOptions);
  const auto pruned = co_await std::move(pruning);
  EXPECT_EQ(pruned.status, std::string("pruned"));
  EXPECT(pruned.deleted > 1);

  std::vector<std::pair<rt::KvKey, rt::KvKey>> expectedSelectors;
  for (const std::string_view storageNamespace : V2_PRUNABLE_NAMESPACES) {
    const rt::KvKey prefix{PROMPT_CACHE_ANALYTICS_KV_PREFIX, storageNamespace};
    expectedSelectors.emplace_back(prefix, rt::KvKey{prefix, expiredBucket + 1});
  }
  expectedSelectors.emplace_back(LEGACY_V1_PREFIX(), rt::KvKey{LEGACY_V1_PREFIX(), expiredBucket + 1});
  ASSERT_EQ(kv->listSelectors.size(), expectedSelectors.size());
  for (std::size_t index = 0; index < expectedSelectors.size(); ++index) {
    const rt::KvListSelector& selector = kv->listSelectors[index];
    EXPECT(sameKey(selector.prefix, expectedSelectors[index].first));
    EXPECT(sameKey(selector.end, expectedSelectors[index].second));
    EXPECT(!selector.start);
  }
  bool listedFresh = false;
  bool listedExpired = false;
  for (const rt::KvKey& key : kv->listedKeys) {
    listedFresh = listedFresh || keyIncludesNumber(key, freshBucket);
    listedExpired = listedExpired || keyIncludesNumber(key, expiredBucket);
  }
  EXPECT_EQ(listedFresh, false);
  EXPECT_EQ(listedExpired, true);

  std::vector<std::string> remainingKeys;
  for (const auto& [encoded, entry] : kv->entries) remainingKeys.push_back(encodeKey(entry.key));
  const std::string expiredText = rt::numberToString(expiredBucket);
  const std::string freshText = rt::numberToString(freshBucket);
  bool someExpired = false;
  bool someV1 = false;
  bool everyV1Fresh = true;
  bool noV2Expired = true;
  std::size_t freshV2 = 0;
  for (const std::string& key : remainingKeys) {
    someExpired = someExpired || rt::includes(key, expiredText);
    someV1 = someV1 || rt::includes(key, "\"v1\"");
    everyV1Fresh = everyV1Fresh && (!rt::includes(key, "\"v1\"") || rt::includes(key, freshText));
    noV2Expired = noV2Expired && (!rt::includes(key, "\"v2\"") || !rt::includes(key, expiredText));
    if (rt::includes(key, "\"v2\"") && rt::includes(key, freshText)) freshV2 += 1;
  }
  EXPECT_EQ(someExpired, false);
  EXPECT_EQ(someV1, true);
  EXPECT(everyV1Fresh);
  EXPECT(noV2Expired);
  EXPECT_EQ(freshV2, V2_PRUNABLE_NAMESPACES.size());
  ASSERT(!PROMPT_CACHE_ANALYTICS_KV_PREFIX.empty());
  EXPECT_EQ(PROMPT_CACHE_ANALYTICS_KV_PREFIX.back().string(), std::string("v2"));
  auto readingAfter = readPromptCacheAnalytics(aggregateOptions);
  const PromptCacheAnalyticsView after = co_await std::move(readingAfter);
  EXPECT(after.buckets.empty());
  EXPECT_EQ(kv->entries.has(encodeKey(promptCacheAnalyticsCounterKey(expiredBucket, "sample_count"))), false);
}

ASYNC_TEST("prompt-cache analytics rejects unknown, duplicate, and over-broad grouping") {
  const auto kv = std::make_shared<CountingKv>();
  auto duplicate = readPromptCacheAnalytics(readOptions(kv, std::vector<std::string>{"provider", "provider"}));
  EXPECT_REJECTS(std::move(duplicate), PromptCacheAnalyticsQueryError);
  auto overBroad = readPromptCacheAnalytics(readOptions(kv, std::vector<std::string>{"provider", "route", "mode"}));
  EXPECT_REJECTS(std::move(overBroad), PromptCacheAnalyticsQueryError);
  auto unknown = readPromptCacheAnalytics(readOptions(kv, std::vector<std::string>{"unknown"}));
  EXPECT_REJECTS(std::move(unknown), PromptCacheAnalyticsQueryError);
  PromptCacheAnalyticsReadOptions nullKv;
  nullKv.kv = std::shared_ptr<rt::Kv>();
  nullKv.groupBy = std::vector<std::string>{"provider"};
  auto reading = readPromptCacheAnalytics(nullKv);
  const PromptCacheAnalyticsView unavailable = co_await std::move(reading);
  EXPECT(unavailable.buckets.empty());
  EXPECT_EQ(unavailable.status, std::string("unavailable"));
}
