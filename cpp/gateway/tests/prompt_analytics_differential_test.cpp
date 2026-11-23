// Differential tests for src/cache/prompt-analytics-core.ts, prompt-analytics-read.ts and
// prompt-analytics-queue.ts (no direct bun tests): the pure helpers on edge inputs, and seven op
// scenarios (direct recording gate and cohorts, the cohort cap, stored-counter projection, retention
// prune, the queued sink, enqueue, concurrent admissions) replayed through tests/helpers/counting_kv
// with every result compared to Bun's JSON text (prompt_analytics_differential_vectors.inc).
// Links: tests/prompt_analytics_differential_test.cpp tests/helpers/counting_kv.cpp src/cache/prompt_analytics.cpp
//        src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics_queue.cpp
//        src/optional_telemetry_queue.cpp src/release.cpp src/utils.cpp src/kv.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/native_kv.cpp src/storage/value_compatibility.cpp src/storage/key_compatibility.cpp
#include "uos/cache/prompt_analytics.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "tests/prompt_analytics_differential_vectors.inc"
#include "uos/kv.hpp"
#include "uos/optional_telemetry_queue.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace core = uos::cache::prompt_analytics_core;
namespace reads = uos::cache::prompt_analytics_read;
namespace queue = uos::cache::prompt_analytics_queue;
namespace otq = uos::optional_telemetry_queue;
using uos::tests::helpers::counting_kv::CountingKv;
using uos::tests::helpers::counting_kv::encodeKey;
using uos::tests::helpers::counting_kv::KvCommandRecord;

namespace {
  constexpr double DATE_NOW = 1'800'000'123'456;

  // The oracle's `special(value)`: a string is `Number(string)` ("NaN", "-0", ...).
  double special(const json::value& value) { return value.is_string() ? rt::stringToNumber(value.get_string()) : rt::toNumber(value); }

  std::optional<double> specialOrNull(const json::value* value) {
    if (!value || value->is_null()) return std::nullopt;
    return special(*value);
  }

  rt::Nullable<std::string> nullableString(const json::object& spec, std::string_view key) {
    const json::value* value = rt::get(spec, key);
    if (!value) return rt::undefined;
    if (value->is_null()) return nullptr;
    return std::string(value->get_string());
  }

  rt::KvKey keyOf(const json::array& parts) {
    rt::KvKey key;
    for (const json::value& part : parts) {
      if (part.is_string()) key.emplace_back(std::string(part.get_string()));
      else if (part.is_bool()) key.emplace_back(part.get_bool());
      else key.emplace_back(*rt::getNumber(part));
    }
    return key;
  }

  rt::KvValue valueOf(const json::object& spec) {
    if (const json::value* u64 = rt::get(spec, "u64")) return rt::KvValue(rt::KvU64(rt::BigInt::fromString(std::string(u64->get_string()))));
    if (const json::value* number = rt::get(spec, "number")) return rt::KvValue(special(*number));
    if (const json::value* bigint = rt::get(spec, "bigint")) return rt::KvValue(rt::BigInt::fromString(std::string(bigint->get_string())));
    if (const json::value* boolean = rt::get(spec, "bool")) return rt::KvValue(boolean->get_bool());
    if (const json::value* text = rt::get(spec, "string")) return rt::KvValue(std::string(text->get_string()));
    if (rt::get(spec, "null")) return rt::KvValue(nullptr);
    rt::KvObject object;
    for (const auto& member : rt::get(spec, "object")->get_object()) object.set(std::string(member.key()), valueOf(member.value().get_object()));
    return rt::KvValue(std::move(object));
  }

  // The oracle's `describe(value)`.
  json::value describe(const rt::KvValue& value) {
    json::object out;
    if (value.isU64()) out["u64"] = value.asU64().toBigInt().toString();
    else if (value.isNull()) out["null"] = true;
    else if (value.isUndefined()) out["undefined"] = true;
    else if (value.isNumber()) out["number"] = rt::makeNumber(value.asNumber());
    else if (value.isBigInt()) out["bigint"] = value.asBigInt().toString();
    else if (value.isBool()) out["bool"] = value.asBool();
    else if (value.isString()) out["string"] = value.asString();
    else if (value.isArray()) {
      json::array items;
      for (const rt::KvValue& item : value.asArray()) items.push_back(describe(item));
      out["array"] = std::move(items);
    } else if (value.isObject()) {
      json::object members;
      for (const auto& [key, member] : value.asObject()) members[key] = describe(member);
      out["object"] = std::move(members);
    } else {
      out["other"] = "object";
    }
    return out;
  }

  core::PromptCacheAnalyticsEvent eventOf(const json::object& spec) {
    core::PromptCacheAnalyticsEvent event{
      .provider = std::nullopt,
      .model = nullableString(spec, "model"),
      .route = std::string(rt::get(spec, "route")->get_string()),
      .status = special(*rt::get(spec, "status")),
      .completed = rt::get(spec, "completed")->get_bool(),
      .usageTelemetryStatus = std::string(rt::get(spec, "usageTelemetryStatus")->get_string()),
      .inputTokens = specialOrNull(rt::get(spec, "inputTokens")),
      .cachedInputTokens = specialOrNull(rt::get(spec, "cachedInputTokens")),
      .cacheWriteInputTokens = specialOrNull(rt::get(spec, "cacheWriteInputTokens")),
      .promptCacheKeyPresent = rt::getBool(spec, "promptCacheKeyPresent"),
      .promptCacheMode = nullableString(spec, "promptCacheMode"),
      .fallbackReason = nullableString(spec, "fallbackReason"),
    };
    if (const std::optional<std::string_view> provider = rt::getString(spec, "provider")) event.provider = std::string(*provider);
    return event;
  }

  struct State {
    std::shared_ptr<CountingKv> kv = std::make_shared<CountingKv>();
  };

  // `kvOption(spec)`: "null" is null, "undefined" leaves the member out, anything else is the scenario KV.
  std::optional<std::shared_ptr<rt::Kv>> kvOption(const State& state, const json::object& spec) {
    const std::optional<std::string_view> kv = rt::getString(spec, "kv");
    if (kv == "undefined") return std::nullopt;
    if (kv == "null") return std::shared_ptr<rt::Kv>();
    return std::shared_ptr<rt::Kv>(state.kv);
  }

  std::function<double()> nowOf(const json::object& spec) {
    const json::value* now = rt::get(spec, "now");
    if (!now) return {};
    const double value = special(*now);
    return [value] { return value; };
  }

  core::PromptCacheAnalyticsOptions optionsOf(const State& state, const json::object& spec) {
    core::PromptCacheAnalyticsOptions options;
    options.kv = kvOption(state, spec);
    if (const std::optional<std::string_view> release = rt::getString(spec, "release")) options.release = std::string(*release);
    options.now = nowOf(spec);
    return options;
  }

  core::PromptCacheAnalyticsReadOptions readOptionsOf(const State& state, const json::object& spec) {
    core::PromptCacheAnalyticsReadOptions options;
    options.kv = kvOption(state, spec);
    options.now = nowOf(spec);
    if (const json::array* groupBy = rt::getArray(spec, "groupBy")) {
      std::vector<std::string> dimensions;
      for (const json::value& dimension : *groupBy) dimensions.emplace_back(dimension.get_string());
      options.groupBy = std::move(dimensions);
    }
    return options;
  }

  queue::PromptCacheAnalyticsQueueEntry entryOf(const json::object& spec) {
    queue::PromptCacheAnalyticsQueueEntry entry{
      .bucket_start_at_ms = special(*rt::get(spec, "bucket_start_at_ms")),
      .provider = std::string(rt::get(spec, "provider")->get_string()),
      .model_hash = std::string(rt::get(spec, "model_hash")->get_string()),
      .route = std::string(rt::get(spec, "route")->get_string()),
      .prompt_cache_key_present = rt::get(spec, "prompt_cache_key_present")->get_bool(),
      .mode = std::string(rt::get(spec, "mode")->get_string()),
      .fallback = std::string(rt::get(spec, "fallback")->get_string()),
      .usage_kind = std::string(rt::get(spec, "usage_kind")->get_string()),
      .deltas = {},
      .kv = std::nullopt,
    };
    for (const auto& member : rt::get(spec, "deltas")->get_object()) entry.deltas[std::string(member.key())] = special(member.value());
    return entry;
  }

  json::value commandKeys(const KvCommandRecord& command) {
    json::array keys;
    for (const rt::KvKey& key : command.keys) keys.push_back(json::value(encodeKey(key)));
    return keys;
  }

  json::value orNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }

  // The oracle's `dumpKv()`.
  json::value dumpKv(CountingKv& kv) {
    json::array entries;
    for (const auto& [encoded, entry] : kv.entries) {
      json::array row;
      row.push_back(json::value(encodeKey(entry.key)));
      row.push_back(describe(entry.value));
      row.push_back(json::value(entry.versionstamp));
      entries.push_back(std::move(row));
    }
    json::array commands;
    for (const KvCommandRecord& command : kv.commands) {
      json::object item;
      item["command"] = command.command;
      item["classification"] = command.classification;
      item["keys"] = commandKeys(command);
      item["atomicChecks"] = rt::makeNumber(command.atomicChecks);
      item["atomicMutations"] = rt::makeNumber(command.atomicMutations);
      item["atomicResult"] = orNull(command.atomicResult);
      commands.push_back(std::move(item));
    }
    kv.clearMeasurements();
    json::object out;
    out["entries"] = std::move(entries);
    out["commands"] = std::move(commands);
    return out;
  }

  // `setTimeout(resolve, 0)`: every pending microtask chain (background prunes) runs first.
  rt::Task<void> settle() { co_await rt::sleep(0); }

  // The oracle's prefill entry.
  queue::PromptCacheAnalyticsQueueEntry prefillEntry(double index) {
    queue::PromptCacheAnalyticsQueueEntry entry{
      .bucket_start_at_ms = index,
      .provider = "metered",
      .model_hash = "unknown",
      .route = "responses",
      .prompt_cache_key_present = false,
      .mode = "unspecified",
      .fallback = "none",
      .usage_kind = "missing",
      .deltas = {},
      .kv = std::nullopt,
    };
    entry.deltas.sample_count = 1;
    return entry;
  }

  rt::Task<json::value> runEnqueue(std::shared_ptr<State> state, json::object op) {
    const json::object& spec = rt::get(op, "queue")->get_object();
    auto captured = std::make_shared<json::array>();
    std::optional<rt::Promise<bool>> held;
    if (rt::getBool(spec, "hold") == true) held = rt::withResolvers<bool>().promise;
    otq::OptionalTelemetryQueueOptions<queue::PromptCacheAnalyticsQueueEntry> queueOptions;
    queueOptions.write = [captured, held](queue::PromptCacheAnalyticsQueueEntry entry) {
      const std::string serialized = rt::jsonStringify(json::value_from(entry));
      json::object item;
      item["json"] = serialized;
      item["bytes"] = rt::makeNumber(static_cast<double>(rt::utf8ByteLength(serialized)));
      item["kv"] = !entry.kv ? "undefined" : *entry.kv ? "counting" : "null";
      captured->push_back(std::move(item));
      return held ? *held : rt::Promise<bool>::resolve(true);
    };
    if (const std::optional<double> maxEntries = rt::getNumber(spec, "maxEntries")) queueOptions.bounds.maxEntries = *maxEntries;
    if (const std::optional<double> maxBytes = rt::getNumber(spec, "maxBytes")) queueOptions.bounds.maxBytes = *maxBytes;
    const otq::OptionalTelemetryQueue<queue::PromptCacheAnalyticsQueueEntry> injected = otq::createOptionalTelemetryQueue(std::move(queueOptions));
    if (rt::getBool(spec, "closed") == true) {
      const rt::Promise<void> closing = injected.close();
      co_await closing.wait();
    }
    const double prefill = rt::getNumber(spec, "prefill").value_or(0);
    for (double index = 0; index < prefill; index += 1) (void)injected.enqueue(prefillEntry(index));
    queue::PromptCacheAnalyticsEnqueueOptions options;
    static_cast<core::PromptCacheAnalyticsOptions&>(options) = optionsOf(*state, rt::get(op, "options")->get_object());
    options.queue = injected;
    const core::PromptCacheAnalyticsEvent event = eventOf(rt::get(op, "event")->get_object());
    auto enqueuing = queue::enqueuePromptCacheAnalytics(event, options);
    const core::PromptCacheAnalyticsRecordResult result = co_await std::move(enqueuing);
    co_await settle();
    const otq::OptionalTelemetryQueueSnapshot snapshot = injected.snapshot();
    json::object snapshotJson;
    snapshotJson["enqueued"] = rt::makeNumber(snapshot.enqueued);
    snapshotJson["dropped_by_entries"] = rt::makeNumber(snapshot.dropped_by_entries);
    snapshotJson["dropped_by_bytes"] = rt::makeNumber(snapshot.dropped_by_bytes);
    snapshotJson["dropped_after_closed"] = rt::makeNumber(snapshot.dropped_after_closed);
    snapshotJson["retained_bytes"] = rt::makeNumber(snapshot.retained_bytes);
    json::object out;
    out["result"] = json::value_from(result);
    out["captured"] = *captured;
    out["snapshot"] = std::move(snapshotJson);
    co_return out;
  }

  // One op; the result as the oracle's JSON text.
  rt::Task<std::string> run(std::shared_ptr<State> state, json::object op) {
    const std::string kind(rt::get(op, "op")->get_string());
    json::value result = nullptr;
    if (kind == "reset") {
      state->kv = std::make_shared<CountingKv>();
    } else if (kind == "seed") {
      state->kv->seed(keyOf(rt::get(op, "key")->get_array()), valueOf(rt::get(op, "value")->get_object()));
    } else if (kind == "record") {
      const core::PromptCacheAnalyticsEvent event = eventOf(rt::get(op, "event")->get_object());
      const core::PromptCacheAnalyticsOptions options = optionsOf(*state, rt::get(op, "options")->get_object());
      auto recording = core::recordPromptCacheAnalytics(event, options);
      const core::PromptCacheAnalyticsRecordResult recorded = co_await std::move(recording);
      co_await settle();
      result = json::value_from(recorded);
    } else if (kind == "recordAll") {
      const core::PromptCacheAnalyticsOptions options = optionsOf(*state, rt::get(op, "options")->get_object());
      std::vector<rt::Promise<core::PromptCacheAnalyticsRecordResult>> pending;
      for (const json::value& event : rt::get(op, "events")->get_array()) {
        pending.push_back(rt::Promise(core::recordPromptCacheAnalytics(eventOf(event.get_object()), options)));
      }
      const rt::Promise<std::vector<core::PromptCacheAnalyticsRecordResult>> all = rt::all(std::move(pending));
      const std::vector<core::PromptCacheAnalyticsRecordResult> results = co_await all.wait();
      co_await settle();
      json::array resultsJson;
      for (const core::PromptCacheAnalyticsRecordResult& item : results) resultsJson.push_back(json::value_from(item));
      json::array commands;
      for (const KvCommandRecord& command : state->kv->commands) {
        if (command.command == "list") continue;
        json::array row;
        row.push_back(json::value(command.command));
        row.push_back(commandKeys(command));
        row.push_back(orNull(command.atomicResult));
        commands.push_back(std::move(row));
      }
      state->kv->clearMeasurements();
      json::object out;
      out["results"] = std::move(resultsJson);
      out["commands"] = std::move(commands);
      result = std::move(out);
    } else if (kind == "read") {
      const core::PromptCacheAnalyticsReadOptions options = readOptionsOf(*state, rt::get(op, "options")->get_object());
      std::optional<core::PromptCacheAnalyticsView> view;
      json::object error;
      try {
        auto reading = reads::readPromptCacheAnalytics(options);
        view = co_await std::move(reading);
      } catch (const rt::JsError& failure) {
        error["name"] = failure.name;
        error["message"] = failure.message;
      }
      co_await settle();
      if (view) co_return rt::jsonStringify(json::value_from(*view));
      json::object out;
      out["error"] = std::move(error);
      result = std::move(out);
    } else if (kind == "prune") {
      const core::PromptCacheAnalyticsOptions options = optionsOf(*state, rt::get(op, "options")->get_object());
      auto pruning = core::prunePromptCacheAnalytics(options);
      const core::PromptCacheAnalyticsPruneResult pruned = co_await std::move(pruning);
      co_await settle();
      result = json::value_from(pruned);
    } else if (kind == "write") {
      queue::PromptCacheAnalyticsQueueEntry entry = entryOf(rt::get(op, "entry")->get_object());
      entry.kv = kvOption(*state, op);
      auto writing = queue::writePromptCacheAnalyticsQueueEntry(entry);
      const bool written = co_await std::move(writing);
      co_await settle();
      result = written;
    } else if (kind == "enqueue") {
      auto enqueuing = runEnqueue(state, op);
      result = co_await std::move(enqueuing);
    } else if (kind == "dump") {
      result = dumpKv(*state->kv);
    } else {
      throw std::logic_error("unknown op " + kind);
    }
    co_return rt::jsonStringify(result);
  }

  std::string digestOf(const std::string& text) { return "sha256:" + rt::crypto::createHash("sha256").update(text).digest("hex"); }

  // Object.is between a number and the oracle's JSON (non-finite numbers and -0 are strings there).
  bool sameNumber(std::optional<double> actual, const json::value& expected) {
    if (expected.is_null()) return !actual;
    return actual && rt::sameValue(*actual, special(expected));
  }

  const json::array& pureList(const json::value& pure, std::string_view key) { return rt::get(pure, key)->get_array(); }

  std::vector<double> pureNumbers() {
    return {0, -0.0, 1, 1.5, -1, 9007199254740991.0, 9007199254740992.0, rt::Number::NaN, rt::Number::POSITIVE_INFINITY, rt::Number::NEGATIVE_INFINITY, 1e21,
            899'999, 900'000, 900'001, -900'001, 1'800'000'000'000.5};
  }
}

ASYNC_TEST("differential: pure helpers match Bun on edge inputs") {
  rt::globalDateNow() = [] { return DATE_NOW; };
  const json::value pure = rt::jsonParse(prompt_analytics_vectors::PURE);
  const std::vector<double> numbers = pureNumbers();
  for (std::size_t index = 0; index < numbers.size(); ++index) {
    EXPECT(sameNumber(core::alignedBucketStart(numbers[index]), pureList(pure, "alignedBucketStart")[index]));
    EXPECT_EQ(core::safeCounter(numbers[index]), pureList(pure, "safeCounter")[index].get_bool());
    const double value = numbers[index];
    EXPECT(sameNumber(core::safeNow([value] { return value; }), pureList(pure, "safeNow")[index]));
  }
  const std::vector<std::pair<double, double>> ratios{{1, 3}, {2, 3}, {1, 8}, {1, 0}, {0, 0}, {5, -0.0}, {-1, 3}, {1, 7}, {12345, 10000}, {1, 9007199254740992.0},
                                                      {9007199254740992.0, 3}, {0.5, 1e6}, {0.00005, 1}};
  for (std::size_t index = 0; index < ratios.size(); ++index) {
    const json::array& expected = pureList(pure, "roundedPercentage")[index].get_array();
    EXPECT(sameNumber(core::roundedPercentage(ratios[index].first, ratios[index].second), expected[0]));
    EXPECT(sameNumber(core::roundedRatio(ratios[index].first, ratios[index].second), expected[1]));
  }

  rt::KvObject five;
  five.set("value", rt::KvValue(rt::BigInt(5)));
  rt::KvObject negative;
  negative.set("value", rt::KvValue(rt::BigInt(-1)));
  rt::KvObject number;
  number.set("value", rt::KvValue(5));
  rt::KvObject undefinedValue;
  undefinedValue.set("value", rt::KvValue());
  rt::KvObject other;
  other.set("other", rt::KvValue(rt::BigInt(1)));
  const std::vector<rt::KvValue> stored{
    rt::KvValue(rt::KvU64(0)),
    rt::KvValue(rt::KvU64(9007199254740991ULL)),
    rt::KvValue(rt::KvU64(9007199254740992ULL)),
    rt::KvValue(five),
    rt::KvValue(negative),
    rt::KvValue(number),
    rt::KvValue(undefinedValue),
    rt::KvValue(rt::KvObject()),
    rt::KvValue(nullptr),
    rt::KvValue(5),
    rt::KvValue("x"),
    rt::KvValue(true),
    rt::KvValue(rt::KvArray{rt::KvValue(1)}),
    rt::KvValue(other),
  };
  for (std::size_t index = 0; index < stored.size(); ++index) EXPECT(sameNumber(core::storedCounter(stored[index]), pureList(pure, "storedCounter")[index]));

  // `null` and `5` are not strings: std::nullopt.
  const std::vector<std::optional<std::string_view>> providers{"chatgpt_codex", "metered", "surplus", "Metered", " metered", "", std::nullopt, std::nullopt};
  for (std::size_t index = 0; index < providers.size(); ++index) {
    const json::value& expected = pureList(pure, "asProvider")[index];
    EXPECT_EQ(core::asProvider(providers[index]), expected.is_null() ? std::nullopt : std::optional<std::string>(std::string(expected.get_string())));
  }
  const std::vector<std::optional<std::string_view>> routes{"responses", "chat.completions", "chat", "", std::nullopt};
  for (std::size_t index = 0; index < routes.size(); ++index) {
    const json::value& expected = pureList(pure, "asRoute")[index];
    EXPECT_EQ(core::asRoute(routes[index]), expected.is_null() ? std::nullopt : std::optional<std::string>(std::string(expected.get_string())));
  }
  const std::vector<std::optional<std::string_view>> modes{"implicit", "explicit", "legacy_retention", "unspecified", "IMPLICIT", "", std::nullopt, std::nullopt};
  for (std::size_t index = 0; index < modes.size(); ++index) EXPECT_EQ(core::asMode(modes[index]), std::string(pureList(pure, "asMode")[index].get_string()));
  const std::vector<std::optional<std::string_view>> fallbacks{"none", "primary_429", "primary_quota_blocked", "other", "weird", " none", "", "  ",
                                                               "\xC2\xA0", "\xE2\x80\xA8", std::nullopt, std::nullopt};
  for (std::size_t index = 0; index < fallbacks.size(); ++index) {
    EXPECT_EQ(core::asFallback(fallbacks[index]), std::string(pureList(pure, "asFallback")[index].get_string()));
  }
  std::vector<std::optional<std::string_view>> counters(core::COUNTERS.begin(), core::COUNTERS.end());
  counters.insert(counters.end(), {"marker", "", "SAMPLE_COUNT", std::nullopt});
  for (std::size_t index = 0; index < counters.size(); ++index) EXPECT_EQ(core::isKnownCounter(counters[index]), pureList(pure, "isKnownCounter")[index].get_bool());

  // `undefined` is a null pointer; the rest are the JSON values the TS passes.
  const std::vector<std::optional<json::value>> groupBys{
    std::nullopt,
    json::value(nullptr),
    rt::jsonParse("[]"),
    rt::jsonParse(R"(["provider"])"),
    rt::jsonParse(R"(["provider","model"])"),
    rt::jsonParse(R"(["provider","provider"])"),
    rt::jsonParse("[1]"),
    rt::jsonParse(R"("provider")"),
    rt::jsonParse("{}"),
    rt::jsonParse(R"(["model","route","mode"])"),
    rt::jsonParse("[null]"),
    rt::jsonParse(R"(["fallback","key_presence"])"),
    rt::jsonParse(R"(["route","Route"])"),
  };
  for (std::size_t index = 0; index < groupBys.size(); ++index) {
    const bool valid = groupBys[index] ? reads::isValidPromptCacheAnalyticsGroupBy(*groupBys[index]) : reads::isValidPromptCacheAnalyticsGroupBy(nullptr);
    EXPECT_EQ(valid, pureList(pure, "isValidPromptCacheAnalyticsGroupBy")[index].get_bool());
  }
  EXPECT_EQ(reads::isValidPromptCacheAnalyticsGroupBy(std::vector<std::string>{"fallback", "key_presence"}), true);
  EXPECT_EQ(reads::isValidPromptCacheAnalyticsGroupBy(std::vector<std::string>{"route", "route"}), false);

  EXPECT_EQ(encodeKey(core::promptCacheAnalyticsBucketKey(1'800'000'000'000)), std::string(pureList(pure, "counterKeys")[0].get_string()));
  EXPECT_EQ(encodeKey(core::promptCacheAnalyticsCounterKey(1'800'000'000'000.5, "sample_count")), std::string(pureList(pure, "counterKeys")[1].get_string()));
  const std::vector<rt::KvKey> prefixes{core::PROMPT_CACHE_ANALYTICS_KV_PREFIX, core::aggregatePrefix, core::dimensionPrefix, core::overflowPrefix};
  for (std::size_t index = 0; index < prefixes.size(); ++index) EXPECT_EQ(encodeKey(prefixes[index]), std::string(pureList(pure, "prefixes")[index].get_string()));
  const std::vector<double> constants{core::PROMPT_CACHE_ANALYTICS_BUCKET_MS,          core::PROMPT_CACHE_ANALYTICS_WINDOW_MS,
                                      core::PROMPT_CACHE_ANALYTICS_WINDOW_BUCKETS,     core::PROMPT_CACHE_ANALYTICS_RETENTION_MS,
                                      core::PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET, core::PROMPT_CACHE_ANALYTICS_MAX_GROUP_BY,
                                      core::PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS};
  for (std::size_t index = 0; index < constants.size(); ++index) EXPECT(sameNumber(constants[index], pureList(pure, "constants")[index]));
  co_return;
}

ASYNC_TEST("differential: recording, reading, pruning and queueing scenarios replay Bun's results") {
  rt::globalDateNow() = [] { return DATE_NOW; };
  // The oracle's `setKvForTest(null)`: an absent KV option resolves null and never opens a store.
  uos::kv::setKvForTest(nullptr);
  const auto restore = rt::scopeExit([] { uos::kv::setKvForTest(nullptr); });
  std::size_t compared = 0;
  for (const std::string_view text : prompt_analytics_vectors::SCENARIOS) {
    const json::value scenario = rt::jsonParse(text);
    const std::string name(rt::get(scenario, "name")->get_string());
    const json::array& ops = rt::get(scenario, "ops")->get_array();
    const json::array& expected = rt::get(scenario, "expected")->get_array();
    ASSERT_EQ(ops.size(), expected.size());
    auto state = std::make_shared<State>();
    for (std::size_t index = 0; index < ops.size(); ++index) {
      auto running = run(state, ops[index].get_object());
      const std::string actual = co_await std::move(running);
      const std::string wanted(expected[index].get_string());
      const bool digest = rt::startsWith(wanted, "sha256:");
      const bool same = digest ? digestOf(actual) == wanted : actual == wanted;
      compared += 1;
      if (!same) {
        rt::test::report(__FILE__, __LINE__,
                         "scenario \"" + name + "\" op " + std::to_string(index) + " " + rt::jsonStringify(ops[index]).substr(0, 300) + "\n      actual:   " +
                           actual.substr(0, 1500) + "\n      expected: " + wanted.substr(0, 1500),
                         false);
      }
    }
  }
  EXPECT_EQ(compared, std::size_t{440});
}

namespace {
  // The same direct-path ops on any store; every result as JSON text.
  rt::Task<std::vector<std::string>> storeRun(std::shared_ptr<rt::Kv> kv) {
    constexpr double NOW = 1'800'000'000'000;
    const auto options = [kv](double now) {
      core::PromptCacheAnalyticsOptions value;
      value.kv = kv;
      value.release = std::string("0123456789abcdef0123456789abcdef01234567");
      value.now = [now] { return now; };
      return value;
    };
    std::vector<std::string> results;
    std::vector<core::PromptCacheAnalyticsEvent> events;
    for (int index = 0; index < 36; index += 1) {
      core::PromptCacheAnalyticsEvent event{
        .provider = index % 3 == 0 ? "metered" : "chatgpt_codex",
        .model = index < 34 ? rt::Nullable<std::string>("store-model-" + std::to_string(index % 34)) : rt::Nullable<std::string>(nullptr),
        .route = index % 2 == 0 ? "responses" : "chat.completions",
        .status = 200,
        .completed = true,
        .usageTelemetryStatus = index % 5 == 0 ? "invalid" : "reported",
        .inputTokens = 100 + index,
        .cachedInputTokens = index,
        .cacheWriteInputTokens = index % 4 == 0 ? std::optional<double>() : std::optional<double>(index),
        .promptCacheKeyPresent = index % 2 == 0,
        .promptCacheMode = "explicit",
        .fallbackReason = nullptr,
      };
      events.push_back(event);
    }
    events.push_back(events[3]);
    for (const core::PromptCacheAnalyticsEvent& event : events) {
      auto recording = core::recordPromptCacheAnalytics(event, options(NOW));
      const core::PromptCacheAnalyticsRecordResult recorded = co_await std::move(recording);
      results.push_back(rt::jsonStringify(json::value_from(recorded)));
      co_await settle();
    }
    for (const std::vector<std::string>& groupBy : std::vector<std::vector<std::string>>{{}, {"provider"}, {"model", "route"}, {"key_presence", "fallback"}}) {
      core::PromptCacheAnalyticsReadOptions readOptions;
      readOptions.kv = kv;
      readOptions.now = [] { return NOW; };
      if (!groupBy.empty()) readOptions.groupBy = groupBy;
      auto reading = reads::readPromptCacheAnalytics(readOptions);
      const core::PromptCacheAnalyticsView view = co_await std::move(reading);
      results.push_back(rt::jsonStringify(json::value_from(view)));
    }
    auto pruning = core::prunePromptCacheAnalytics(options(NOW + 9 * 24 * 3'600'000.0));
    const core::PromptCacheAnalyticsPruneResult pruned = co_await std::move(pruning);
    results.push_back(rt::jsonStringify(json::value_from(pruned)));
    co_return results;
  }
}

ASYNC_TEST("prompt analytics records, reads and prunes on rt::MemoryKv exactly as on the counting KV") {
  rt::globalDateNow() = [] { return DATE_NOW; };
  uos::kv::setKvForTest(nullptr);
  const auto restore = rt::scopeExit([] { uos::kv::setKvForTest(nullptr); });
  const auto memory = std::make_shared<rt::MemoryKv>([] { return DATE_NOW; });
  auto onMemory = storeRun(memory);
  const std::vector<std::string> memoryResults = co_await std::move(onMemory);
  const auto counting = std::make_shared<CountingKv>();
  auto onCounting = storeRun(counting);
  const std::vector<std::string> countingResults = co_await std::move(onCounting);
  ASSERT_EQ(memoryResults.size(), countingResults.size());
  for (std::size_t index = 0; index < memoryResults.size(); ++index) EXPECT_EQ(memoryResults[index], countingResults[index]);
  EXPECT(rt::includes(memoryResults[33], "recorded_cardinality_capped"));
  EXPECT_EQ(memory->database()->rows().size(), std::size_t{0});  // the eight-day prune removed every row
}
