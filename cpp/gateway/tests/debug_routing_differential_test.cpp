// Differential cases for src/debug-routing.ts, whose bun tests (admin-codex-coverage, the
// openai-compat harness, sentinel-incident-index, sentinel-request-capture) drive it only through
// unported handlers. Recorded from the TypeScript under Bun 1.4.0 with Date.now fixed: the stored-row
// normalizer at three clock readings, and one scripted sequence of writes, five-second cache hits,
// direct row edits, validation failures and missing or failing stores, with the rows each step leaves.
// Links: tests/debug_routing_differential_test.cpp src/debug_routing.cpp src/kv.cpp src/storage/kv.cpp src/release.cpp
//   src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
#include "uos/debug_routing.hpp"
#include "uos/kv.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "debug_routing_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::debug_routing;

namespace {
  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(kDebugRoutingVectors);
    return parsed;
  }

  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    std::fprintf(stderr, "    %s\n      actual:   %s\n      expected: %s\n", label.c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(expected).c_str());
    return false;
  }

  json::value keyJson(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(part.isString() ? json::value(part.string()) : rt::makeNumber(part.number()));
    return parts;
  }

  // The oracle's `kv.dump()`: every row in key order as `{ key, value, expiresAtMs }`.
  json::value dump(const rt::MemoryKv& kv) {
    json::array rows;
    for (const rt::MemoryKvDatabase::Row& row : kv.database()->rows()) {
      json::object entry;
      entry["key"] = keyJson(row.key);
      entry["value"] = row.value.toJson();
      entry["expiresAtMs"] = row.expirationMs < 0 ? json::value(nullptr) : rt::makeNumber(row.expirationMs);
      rows.push_back(std::move(entry));
    }
    return rows;
  }

  // `{ ok: result }` or `{ error: message }`.
  rt::Task<json::value> describe(std::function<rt::Task<DebugRoutingConfig>()> run) {
    json::object outcome;
    try {
      rt::Task<DebugRoutingConfig> running = run();
      const DebugRoutingConfig config = co_await std::move(running);
      outcome["ok"] = json::value_from(config);
    } catch (const rt::JsError& error) {
      outcome["error"] = error.message;
    }
    co_return outcome;
  }

  // A store whose reads reject (`{ get: () => Promise.reject(new Error("kv down")) }`).
  class FailingReadsKv final : public rt::ForwardingKv {
  public:
    explicit FailingReadsKv(std::shared_ptr<rt::MemoryKv> store_) : rt::ForwardingKv(*store_), store(std::move(store_)) {}
    using rt::Kv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey, rt::KvGetOptions) override {
      throw rt::JsError("kv down");
      co_return rt::KvEntryMaybe<>();
    }

  private:
    std::shared_ptr<rt::MemoryKv> store;
  };
}

ASYNC_TEST("stored rows normalize as the oracle reads them") {
  int index = 0;
  for (const json::value& item : vectors().as_object().at("normalize").as_array()) {
    const json::array& triple = item.as_array();
    const auto kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
    if (!(triple[0].is_string() && triple[0].get_string() == "<absent>")) {
      const rt::KvValue stored(triple[0]);
      co_await kv->set(DEBUG_ROUTING_KEY, stored);
    }
    uos::kv::setKvForTest(kv);
    resetDebugRoutingCacheForTest();
    rt::Task<DebugRoutingConfig> loading = loadDebugRoutingConfig(*rt::getNumber(triple[1]));
    const DebugRoutingConfig config = co_await std::move(loading);
    EXPECT(same(json::value_from(config), triple[2], "normalize " + std::to_string(index) + " " + rt::jsonStringify(triple[0])));
    ++index;
  }
  EXPECT_EQ(index, 105);
  uos::kv::setKvForTest(nullptr);
  resetDebugRoutingCacheForTest();
}

ASYNC_TEST("stored values that are not JSON records read as the normal scenario") {
  const auto kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  uos::kv::setKvForTest(kv);
  rt::KvMap map;
  map.set(rt::KvValue("scenario"), rt::KvValue("codex_429"));
  map.set(rt::KvValue("updated_at_ms"), rt::KvValue(5));
  rt::KvObject dated;
  dated.set("scenario", rt::KvValue("codex_429"));
  dated.set("updated_at_ms", rt::KvValue(rt::KvDate{.time = 5}));
  rt::KvObject bigUpdated;
  bigUpdated.set("scenario", rt::KvValue("codex_429"));
  bigUpdated.set("updated_at_ms", rt::KvValue(rt::BigInt(5)));
  const std::vector<rt::KvValue> values{rt::KvValue(map), rt::KvValue(rt::KvDate{.time = 5}), rt::KvValue(rt::KvU64(5)), rt::KvValue(rt::KvBytes{1, 2}),
                                        rt::KvValue(dated), rt::KvValue(bigUpdated), rt::KvValue(rt::BigInt(5))};
  for (const rt::KvValue& value : values) {
    co_await kv->set(DEBUG_ROUTING_KEY, value);
    resetDebugRoutingCacheForTest();
    rt::Task<DebugRoutingConfig> loading = loadDebugRoutingConfig(1'500);
    const DebugRoutingConfig config = co_await std::move(loading);
    EXPECT(config == (DebugRoutingConfig{.scenario = "normal", .expires_at_ms = std::nullopt, .updated_at_ms = 1'500}));
  }
  uos::kv::setKvForTest(nullptr);
  resetDebugRoutingCacheForTest();
}

ASYNC_TEST("the scripted write, cache and failure sequence matches the oracle") {
  double clock = 1'800'000'000'000;
  const auto realNow = rt::globalDateNow();
  rt::globalDateNow() = [&clock] { return clock; };
  const json::array& expected = vectors().as_object().at("steps").as_array();
  const auto kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  uos::kv::setKvForTest(kv);
  resetDebugRoutingCacheForTest();
  std::size_t index = 0;
  const auto check = [&](const std::string& label, const json::value& outcome, bool compareOutcome) {
    ASSERT(index < expected.size());
    const json::array& step = expected[index].as_array();
    EXPECT_EQ(std::string(step[0].as_string()), label);
    if (compareOutcome) EXPECT(same(outcome, step[1], label + " outcome"));
    EXPECT(same(dump(*kv), step[2], label + " rows"));
    ++index;
  };
  const auto load = [](double nowMs) { return std::function<rt::Task<DebugRoutingConfig>()>([nowMs] { return loadDebugRoutingConfig(nowMs); }); };
  const auto set = [](std::string scenario, double durationMs, double nowMs) {
    return std::function<rt::Task<DebugRoutingConfig>()>([scenario, durationMs, nowMs] { return setDebugRoutingConfig(scenario, durationMs, nowMs); });
  };
  json::value outcome;
  // The step's Task is bound before it is awaited: no std::function temporary in a co_await.
#define DESCRIBE(...)                                              \
  do {                                                             \
    rt::Task<json::value> describing = describe(__VA_ARGS__);      \
    outcome = co_await std::move(describing);                      \
  } while (0)

  DESCRIBE(load(1'000));
  check("load empty", outcome, true);
  DESCRIBE(set("codex_429", 60'000, 1'000));
  check("set codex_429 60000 @1000", outcome, true);
  const rt::KvValue normalRow = rt::KvValue::object({{"scenario", rt::KvValue("normal")}, {"updated_at_ms", rt::KvValue(1)}});
  const rt::KvCommitResult overwrite = co_await kv->set(DEBUG_ROUTING_KEY, normalRow);
  EXPECT(overwrite.ok);
  check("overwrite row normal", json::value(), false);
  DESCRIBE(load(1'000));
  check("load @1000 (cached)", outcome, true);
  DESCRIBE(load(5'999));
  check("load @5999 (cached)", outcome, true);
  DESCRIBE(load(6'000));
  check("load @6000 (reread)", outcome, true);
  DESCRIBE(load(6'001));
  check("load @6001 (cached normal row)", outcome, true);
  DESCRIBE(set("codex_503", 2'000, 20'000));
  check("set codex_503 2000 @20000", outcome, true);
  DESCRIBE(load(21'999));
  check("load @21999 (cached)", outcome, true);
  DESCRIBE(load(22'000));
  check("load @22000 (cached but expired)", outcome, true);
  DESCRIBE(load(25'000));
  check("load @25000 (reread expired row)", outcome, true);
  DESCRIBE(load(24'999));
  check("load @24999 (stale cache after expired reread)", outcome, true);
  DESCRIBE(set("normal", 5'000, 30'000));
  check("set normal 5000 @30000", outcome, true);
  DESCRIBE(set("codex_401", 0, 31'000));
  check("set codex_401 0 @31000", outcome, true);
  DESCRIBE(set("metered_first", 3'600'000, 32'000));
  check("set metered_first 3600000 @32000", outcome, true);
  DESCRIBE(set("bogus", 1'000, 33'000));
  check("set bogus", outcome, true);
  DESCRIBE(set("bogus", -1, 33'000));
  check("set bogus bad duration", outcome, true);
  DESCRIBE(set("codex_429", -1, 33'000));
  check("set duration -1", outcome, true);
  DESCRIBE(set("codex_429", 3'600'001, 33'000));
  check("set duration 3600001", outcome, true);
  DESCRIBE(set("codex_429", 1.5, 33'000));
  check("set duration 1.5", outcome, true);
  DESCRIBE(set("codex_429", rt::Number::NaN, 33'000));
  check("set duration NaN", outcome, true);
  DESCRIBE(set("codex_429", rt::Number::POSITIVE_INFINITY, 33'000));
  check("set duration Infinity", outcome, true);
  DESCRIBE(load(32'000));
  check("load @32000 (cached metered_first)", outcome, true);
  clock = 40'000;
  DESCRIBE([] { return loadDebugRoutingConfig(); });
  check("load default now (cached? no: expired cache)", outcome, true);
  DESCRIBE([] { return setDebugRoutingConfig("codex_403", 1'000); });
  check("set default now codex_403 1000", outcome, true);
  uos::kv::setKvForTest(nullptr);
  resetDebugRoutingCacheForTest();
  DESCRIBE(load(50'000));
  check("load without kv", outcome, true);
  DESCRIBE(set("codex_429", 1'000, 50'000));
  check("set without kv", outcome, true);
  uos::kv::setKvForTest(std::make_shared<FailingReadsKv>(std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); })));
  DESCRIBE(load(60'000));
  check("load with throwing kv", outcome, true);
  EXPECT_EQ(index, expected.size());

  uos::kv::setKvForTest(nullptr);
  resetDebugRoutingCacheForTest();
  rt::globalDateNow() = realNow;
#undef DESCRIBE
}
