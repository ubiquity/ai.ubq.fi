// Differential cases for src/cache/telemetry-gate.ts beyond the ported bun cases, recorded from the
// TypeScript under Bun 1.4.0: release normalization, the completed-2xx gate, provider/route/model
// validation (UTF-16 length, control characters, trimming), the exact model hashes and counter
// keys, the counters a sequence of records leaves, and baselines over seeded counters.
#include "uos/cache/telemetry_gate.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "prompt_cache_telemetry_gate_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::cache::telemetry_gate;

namespace {
  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(kTelemetryGateVectors);
    return parsed;
  }
  const json::value& at(const json::value& value, std::string_view key) {
    static const json::value null;
    const json::value* member = rt::get(value, key);
    return member != nullptr ? *member : null;
  }
  std::optional<std::string> textOrNull(const json::value& value) {
    if (!value.is_string()) return std::nullopt;
    return std::string(value.as_string());
  }
  std::string encodeKey(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(json::value(part.string()));
    return rt::jsonStringify(json::value(std::move(parts)));
  }

  // The oracle script's KV: get, and atomic sum/commit over a Map of KvU64 values.
  class SumKv final : public rt::Kv {
  public:
    rt::OrderedMap<std::string, rt::KvValue> values;
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions) override {
      const rt::KvValue* stored = values.get(encodeKey(key));
      rt::KvEntryMaybe<> entry{key, stored != nullptr ? *stored : rt::KvValue(nullptr), stored != nullptr ? std::optional<std::string>("1") : std::nullopt};
      co_return entry;
    }
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions) override { throw rt::TypeError("kv.getMany is not a function"); }
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions) override { throw rt::TypeError("kv.set is not a function"); }
    rt::Task<void> delete_(rt::KvKey) override { throw rt::TypeError("kv.delete is not a function"); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions) override { throw rt::TypeError("kv.list is not a function"); }
    std::unique_ptr<rt::AtomicOperation> atomic() override {
      return std::make_unique<rt::GenericAtomicOperation>([this](std::vector<rt::AtomicCheck>, std::vector<rt::KvMutation> mutations) -> rt::Task<rt::KvCommitResult> {
        for (const rt::KvMutation& mutation : mutations) {
          const rt::KvValue* existing = values.get(encodeKey(mutation.key));
          const rt::BigInt current = existing != nullptr && existing->isU64() ? existing->asU64().toBigInt() : rt::BigInt(0);
          values.set(encodeKey(mutation.key), rt::KvValue(rt::KvU64(current + mutation.value.asU64().toBigInt())));
        }
        rt::KvCommitResult committed{true, "1"};
        co_return committed;
      });
    }
    void close() override {}
  };

  PromptCacheTelemetryEvent eventOf(const json::value& value) {
    return PromptCacheTelemetryEvent{.provider = textOrNull(at(value, "provider")), .model = textOrNull(at(value, "model")), .route = std::string(at(value, "route").as_string()),
                                     .status = *rt::getNumber(value, "status"), .completed = at(value, "completed").as_bool(),
                                     .usageTelemetryStatus = std::string(at(value, "usageTelemetryStatus").as_string()),
                                     .cacheWriteTokensPresent = at(value, "cacheWriteTokensPresent").as_bool()};
  }
  json::value keyJson(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(json::value(part.string()));
    return parts;
  }
  json::value keysJson(const PromptCacheTelemetryCounterKeys& keys) {
    json::array routes;
    for (const PromptCacheTelemetryCounterKeys::Route& route : keys.routes) {
      json::object entry;
      entry["route"] = route.route;
      entry["completed"] = keyJson(route.completed);
      entry["reported"] = keyJson(route.reported);
      entry["cache_write_reported"] = keyJson(route.cache_write_reported);
      entry["invalid"] = keyJson(route.invalid);
      routes.push_back(std::move(entry));
    }
    json::object out;
    out["release"] = keys.release;
    out["provider"] = keys.provider;
    out["model_hash"] = keys.model_hash;
    out["routes"] = std::move(routes);
    return out;
  }
  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    std::fprintf(stderr, "    %s\n      actual:   %s\n      expected: %s\n", label.c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(expected).c_str());
    return false;
  }
  PromptCacheTelemetryGateOptions optionsFor(std::shared_ptr<rt::Kv> kv, std::string release) {
    PromptCacheTelemetryGateOptions options;
    options.kv = std::move(kv);
    options.release = std::move(release);
    return options;
  }
}

ASYNC_TEST("recordPromptCacheTelemetry results, counters and keys match the oracle") {
  const std::shared_ptr<SumKv> kv = std::make_shared<SumKv>();
  int cases = 0;
  for (const json::value& record : at(vectors(), "records").as_array()) {
    const PromptCacheTelemetryRecordResult result =
      co_await recordPromptCacheTelemetry(eventOf(at(record, "event")), optionsFor(kv, std::string(at(record, "release").as_string())));
    EXPECT(same(json::value(toJson(result)), at(record, "result"), std::string(at(record, "name").as_string())));
    ++cases;
  }
  EXPECT_EQ(cases, 26);
  json::array stored;
  for (const auto& [key, value] : kv->values) stored.push_back(json::array{json::value(key), json::value(value.asU64().toBigInt().toString())});
  EXPECT(same(stored, at(vectors(), "stored"), "stored counters"));
  const std::string release(at(at(vectors(), "keys"), "release").as_string());
  PromptCacheTelemetryGateOptions releaseOnly;
  releaseOnly.release = "0123456789ABCDEF0123456789abcdef01234567";
  const PromptCacheTelemetryBaselineTarget target{.provider = "chatgpt_codex", .model = "gpt-cache-target"};
  const std::optional<PromptCacheTelemetryCounterKeys> keys = co_await resolvePromptCacheTelemetryCounterKeys(target, releaseOnly);
  ASSERT(keys.has_value());
  EXPECT(same(keysJson(*keys), at(vectors(), "keys"), "keys"));
  const PromptCacheTelemetryBaselineResult baseline = co_await readPromptCacheTelemetryBaseline(target, optionsFor(kv, "0123456789ABCDEF0123456789abcdef01234567"));
  EXPECT(same(json::value(toJson(baseline)), at(vectors(), "baseline"), "baseline"));
}

ASYNC_TEST("readPromptCacheTelemetryBaseline coverage arithmetic matches the oracle") {
  const PromptCacheTelemetryBaselineTarget target{.provider = "chatgpt_codex", .model = "gpt-cache-target"};
  PromptCacheTelemetryGateOptions releaseOnly;
  releaseOnly.release = "0123456789ABCDEF0123456789abcdef01234567";
  const std::optional<PromptCacheTelemetryCounterKeys> keys = co_await resolvePromptCacheTelemetryCounterKeys(target, releaseOnly);
  ASSERT(keys.has_value());
  int cases = 0;
  for (const json::value& coverage : at(vectors(), "coverageCases").as_array()) {
    const json::array& counters = at(coverage, "counters").as_array();
    const auto u64 = [](const json::value& text) { return rt::KvValue(rt::KvU64(rt::BigInt::fromString(text.as_string()))); };
    const std::shared_ptr<SumKv> seeded = std::make_shared<SumKv>();
    seeded->values.set(encodeKey(keys->routes[0].completed), u64(counters[0]));
    seeded->values.set(encodeKey(keys->routes[0].reported), u64(counters[1]));
    seeded->values.set(encodeKey(keys->routes[0].cache_write_reported), u64(counters[2]));
    seeded->values.set(encodeKey(keys->routes[0].invalid), u64(counters[3]));
    if (counters[4].as_string() != "0") seeded->values.set(encodeKey(keys->routes[1].completed), u64(counters[4]));
    const PromptCacheTelemetryBaselineResult result = co_await readPromptCacheTelemetryBaseline(target, optionsFor(seeded, "0123456789ABCDEF0123456789abcdef01234567"));
    EXPECT(same(json::value(toJson(result)), at(coverage, "result"), "coverage " + std::to_string(cases)));
    ++cases;
  }
  EXPECT_EQ(cases, 6);
}

ASYNC_TEST("a null KV is unavailable for records and baselines") {
  PromptCacheTelemetryGateOptions noKv;
  noKv.kv = std::shared_ptr<rt::Kv>();
  noKv.release = "0123456789ABCDEF0123456789abcdef01234567";
  const PromptCacheTelemetryEvent event{.provider = "chatgpt_codex", .model = "gpt-cache-target", .route = "responses", .status = 200, .completed = true,
                                        .usageTelemetryStatus = "reported", .cacheWriteTokensPresent = true};
  const PromptCacheTelemetryRecordResult recorded = co_await recordPromptCacheTelemetry(event, noKv);
  EXPECT(same(json::value(toJson(recorded)), at(vectors(), "nullKv"), "null kv record"));
  const PromptCacheTelemetryBaselineTarget target{.provider = "chatgpt_codex", .model = "gpt-cache-target"};
  const PromptCacheTelemetryBaselineResult baseline = co_await readPromptCacheTelemetryBaseline(target, noKv);
  EXPECT(same(json::value(toJson(baseline)), at(vectors(), "nullKvBaseline"), "null kv baseline"));
}
