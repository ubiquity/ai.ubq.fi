// Differential cases for src/analytics.ts beyond the ported paid-fallback case, recorded from the
// TypeScript under Bun 1.4.0 with Date.now fixed and crypto.randomUUID stubbed (the C++ side fills
// the random source with 0x11, which gives the same UUID): the key helpers, the ledger row
// normalizer over 46 stored shapes, key parts of unexpected types, list limits, and a scripted
// sequence of record/update writes (merges, missing rows, blank keys, id fallbacks, expiry, an
// invalid expireIn and a store whose commits always conflict) with the rows each step leaves.
// Links: tests/analytics_differential_test.cpp src/analytics.cpp src/types.cpp src/kv.cpp src/storage/kv.cpp src/release.cpp
//   src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
#include "uos/analytics.hpp"
#include "uos/kv.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "analytics_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::analytics;

namespace {
  const json::object& vectors() {
    static const json::value parsed = rt::jsonParse(kAnalyticsVectors);
    return parsed.as_object();
  }

  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    std::fprintf(stderr, "    %s\n      actual:   %s\n      expected: %s\n", label.c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(expected).c_str());
    return false;
  }

  // `untag`: {"$num": text} back to the number `Number(text)`.
  rt::KvValue untag(const json::value& value) {
    if (value.is_object()) {
      const json::object& object = value.as_object();
      if (object.size() == 1) {
        if (const std::optional<std::string_view> text = rt::getString(object, "$num")) return rt::KvValue(rt::stringToNumber(*text));
      }
      rt::KvObject out;
      for (const auto& member : object) out.set(std::string(member.key()), untag(member.value()));
      return rt::KvValue(std::move(out));
    }
    if (value.is_array()) {
      rt::KvArray out;
      for (const json::value& element : value.as_array()) out.push_back(untag(element));
      return rt::KvValue(std::move(out));
    }
    return rt::KvValue(value);
  }

  // `tag`: the numbers JSON cannot carry (-0, NaN, +-Infinity) as {"$num": text}.
  json::value tag(const json::value& value) {
    if (rt::isNumber(value)) {
      const double number = rt::toNumber(value);
      if (number == 0 && std::signbit(number)) return json::object{{"$num", "-0"}};
      if (!rt::Number::isFinite(number)) return json::object{{"$num", rt::numberToString(number)}};
      return value;
    }
    if (value.is_array()) {
      json::array out;
      for (const json::value& element : value.as_array()) out.push_back(tag(element));
      return out;
    }
    if (value.is_object()) {
      json::object out;
      for (const auto& member : value.as_object()) out[member.key()] = tag(member.value());
      return out;
    }
    return value;
  }

  json::value keyJson(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) parts.push_back(json::value(part.string()));
      else if (part.isNumber()) parts.push_back(rt::makeNumber(part.number()));
      else parts.push_back(json::value(part.boolean()));
    }
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
    return tag(rows);
  }

  std::shared_ptr<rt::MemoryKv> memoryKv() { return std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); }); }

  // `{ ok: tag(result) }` (no member for undefined) or `{ error: "Name: message" }`.
  rt::Task<json::value> describeList(std::function<rt::Task<std::vector<uos::types::ApiKeyRequestLogRecord>>()> run) {
    json::object outcome;
    try {
      rt::Task<std::vector<uos::types::ApiKeyRequestLogRecord>> running = run();
      const std::vector<uos::types::ApiKeyRequestLogRecord> records = co_await std::move(running);
      outcome["ok"] = tag(json::value_from(records));
    } catch (const rt::JsError& error) {
      outcome["error"] = error.name + ": " + error.message;
    }
    co_return outcome;
  }

  rt::Task<json::value> describeWrite(std::function<rt::Task<void>()> run) {
    json::object outcome;
    try {
      rt::Task<void> running = run();
      co_await std::move(running);
    } catch (const rt::JsError& error) {
      outcome["error"] = error.name + ": " + error.message;
    }
    co_return outcome;
  }

  // A store whose every commit conflicts (the oracle's `commitFailuresForced = Infinity`).
  class ConflictingOperation final : public rt::ForwardingAtomicOperation {
  public:
    ConflictingOperation(std::unique_ptr<rt::AtomicOperation> inner_, std::shared_ptr<int> commits_)
      : rt::ForwardingAtomicOperation(std::move(inner_)), commits(std::move(commits_)) {}
    rt::Task<rt::KvCommitResult> commit() override {
      *commits += 1;
      rt::KvCommitResult conflict;
      conflict.ok = false;
      co_return conflict;
    }

  private:
    std::shared_ptr<int> commits;
  };

  class ConflictingKv final : public rt::ForwardingKv {
  public:
    explicit ConflictingKv(std::shared_ptr<rt::MemoryKv> store_) : rt::ForwardingKv(*store_), store(std::move(store_)) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<ConflictingOperation>(rt::ForwardingKv::atomic(), commits); }
    std::shared_ptr<int> commits = std::make_shared<int>(0);

  private:
    std::shared_ptr<rt::MemoryKv> store;
  };

  ApiKeyRequestLogInput base() {
    return ApiKeyRequestLogInput{.route = "responses", .path = "/v1/responses", .method = "post", .status_code = 0, .stream = false, .created_at_ms = 1'000};
  }
}

TEST("key helpers and constants match the oracle") {
  const json::object& keys = vectors().at("keys").as_object();
  EXPECT(same(keyJson(apiKeyUsageKey("k-1")), keys.at("usage"), "usage"));
  EXPECT(same(keyJson(apiKeyUsageDailyKey("k-1")), keys.at("daily"), "daily"));
  EXPECT(same(keyJson(legacyApiKeyRequestLogPrefix("k-1")), keys.at("legacy"), "legacy"));
  EXPECT(same(keyJson(apiKeyRequestLogPrefix("k-1")), keys.at("prefix"), "prefix"));
  EXPECT(same(keyJson(apiKeyRequestLogKey("k-1", 1000.5, "r-1")), keys.at("key"), "key"));
  EXPECT_EQ(API_KEY_REQUEST_LOG_RETENTION_MS, *rt::getNumber(keys.at("retention")));
  const json::array prefixes{keyJson(API_KEY_USAGE_PREFIX), keyJson(API_KEY_USAGE_DAILY_PREFIX), keyJson(LEGACY_API_KEY_REQUEST_LOG_PREFIX), keyJson(API_KEY_REQUEST_LOG_PREFIX)};
  EXPECT(same(prefixes, keys.at("prefixes"), "prefixes"));
}

ASYNC_TEST("stored ledger rows normalize as the oracle reads them") {
  const double now = *rt::getNumber(vectors().at("NOW"));
  const auto realNow = rt::globalDateNow();
  rt::globalDateNow() = [now] { return now; };
  int index = 0;
  for (const json::value& item : vectors().at("normalized").as_array()) {
    const json::array& pair = item.as_array();
    const auto kv = memoryKv();
    const std::string keyId = "key-" + std::to_string(index);
    const rt::KvKey key = apiKeyRequestLogKey(keyId, 4242, "row-" + std::to_string(index));
    const rt::KvValue stored = untag(pair[0]);
    co_await kv->set(key, stored);
    ListApiKeyRequestLogsOptions options;
    options.kv = kv;
    rt::Task<std::vector<uos::types::ApiKeyRequestLogRecord>> listing = listApiKeyRequestLogs(keyId, options);
    const std::vector<uos::types::ApiKeyRequestLogRecord> records = co_await std::move(listing);
    EXPECT(same(tag(json::value_from(records)), pair[1], "row " + std::to_string(index)));
    ++index;
  }
  EXPECT_EQ(index, 46);

  // Key parts of unexpected types: a numeric request id, a fractional timestamp with a blank id, a boolean timestamp.
  const auto oddKv = memoryKv();
  const rt::KvKey numericId{"uos_ai", "paid_fallback", "ledger", "odd", "not-a-number", 7};
  const rt::KvKey blankId{"uos_ai", "paid_fallback", "ledger", "odd", 3.9, "   "};
  const rt::KvKey booleanTimestamp{"uos_ai", "paid_fallback", "ledger", "odd", true, "bool-ts"};
  const rt::KvValue oddRow = rt::KvValue::object({{"route", rt::KvValue("odd")}});
  const rt::KvValue blankRow = rt::KvValue::object({{"route", rt::KvValue("blank-id")}});
  const rt::KvValue boolRow = rt::KvValue::object({{"route", rt::KvValue("bool")}});
  co_await oddKv->set(numericId, oddRow);
  co_await oddKv->set(blankId, blankRow);
  co_await oddKv->set(booleanTimestamp, boolRow);
  ListApiKeyRequestLogsOptions oddOptions;
  oddOptions.kv = oddKv;
  rt::Task<std::vector<uos::types::ApiKeyRequestLogRecord>> listing = listApiKeyRequestLogs("odd", oddOptions);
  const std::vector<uos::types::ApiKeyRequestLogRecord> odd = co_await std::move(listing);
  EXPECT(same(tag(json::value_from(odd)), vectors().at("oddKeys"), "odd keys"));
  rt::globalDateNow() = realNow;
}

ASYNC_TEST("list limits, store resolution and blank key ids match the oracle") {
  const double now = *rt::getNumber(vectors().at("NOW"));
  const auto realNow = rt::globalDateNow();
  rt::globalDateNow() = [now] { return now; };
  const auto limitsKv = memoryKv();
  for (int index = 1; index <= 5; index += 1) {
    const rt::KvKey key = apiKeyRequestLogKey("limits", index * 10, "r" + std::to_string(index));
    const rt::KvValue row = rt::KvValue::object({{"route", rt::KvValue("route-" + std::to_string(index))}});
    co_await limitsKv->set(key, row);
  }
  int cases = 0;
  for (const json::value& item : vectors().at("limitCases").as_array()) {
    const json::array& pair = item.as_array();
    ListApiKeyRequestLogsOptions options;
    options.kv = limitsKv;
    const bool absent = pair[0].is_string() && pair[0].get_string() == "<absent>";
    if (!absent) options.limit = untag(pair[0]).asNumber();
    rt::Task<json::value> describing = describeList([options] { return listApiKeyRequestLogs("limits", options); });
    const json::value outcome = co_await std::move(describing);
    EXPECT(same(outcome, pair[1], "limit " + rt::jsonStringify(pair[0])));
    ++cases;
  }
  EXPECT_EQ(cases, 10);

  uos::kv::setKvForTest(limitsKv);
  rt::Task<json::value> describing = describeList([] {
    ListApiKeyRequestLogsOptions options;
    options.limit = 2;
    return listApiKeyRequestLogs("limits", options);
  });
  json::value outcome = co_await std::move(describing);
  EXPECT(same(outcome, vectors().at("viaGetKv"), "via getKv"));
  describing = describeList([limitsKv] {
    ListApiKeyRequestLogsOptions options;
    options.kv = limitsKv;
    return listApiKeyRequestLogs("   ", options);
  });
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, vectors().at("blankKey"), "blank key"));
  describing = describeList([] {
    ListApiKeyRequestLogsOptions options;
    options.kv = std::shared_ptr<rt::Kv>();
    return listApiKeyRequestLogs("limits", options);
  });
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, vectors().at("nullKv"), "null kv"));
  uos::kv::setKvForTest(nullptr);
  describing = describeList([] { return listApiKeyRequestLogs("limits"); });
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, vectors().at("noKv"), "no kv"));
  rt::globalDateNow() = realNow;
}

ASYNC_TEST("record and update writes leave the rows the oracle leaves") {
  const double now = *rt::getNumber(vectors().at("NOW"));
  const auto realNow = rt::globalDateNow();
  rt::globalDateNow() = [now] { return now; };
  const rt::crypto::ScopedRandomSource random([](std::span<std::uint8_t> bytes) { std::fill(bytes.begin(), bytes.end(), std::uint8_t{0x11}); });
  const json::array& expected = vectors().at("steps").as_array();
  const auto kv = memoryKv();
  const std::shared_ptr<rt::Kv> store = kv;
  uos::kv::setKvForTest(kv);
  std::size_t index = 0;
  const auto check = [&](const std::string& label, const json::value& outcome, bool compareOutcome) {
    ASSERT(index < expected.size());
    const json::array& step = expected[index].as_array();
    EXPECT_EQ(std::string(step[0].as_string()), label);
    if (compareOutcome) EXPECT(same(outcome, step[1], label + " outcome"));
    EXPECT(same(dump(*kv), step[2], label + " rows"));
    ++index;
  };
  json::value outcome;
  // The step's Task is bound before it is awaited: no std::function temporary in a co_await.
#define WRITE(...)                                                \
  do {                                                            \
    rt::Task<json::value> describing = describeWrite(__VA_ARGS__); \
    outcome = co_await std::move(describing);                     \
  } while (0)

  ApiKeyRequestLogInput first = base();
  first.id = "req-w";
  first.model = "m1";
  first.reasoning = "low";
  first.provider = "metered";
  first.billing_status = "pending";
  first.input_tokens = 3;
  WRITE([first, store] { return recordApiKeyRequestLog("key-w", first, store); });
  check("record with id", outcome, true);

  ApiKeyRequestLogInput second = base();
  second.id = "req-w";
  second.model = nullptr;
  second.status_code = 201;
  second.fallback_reason = "codex_429";
  second.spend_microcredits = 12.5;
  WRITE([second, store] { return recordApiKeyRequestLog("key-w", second, store); });
  check("record again merges", outcome, true);

  ApiKeyRequestLogPatch patch;
  patch.status_code = 404;
  patch.model = "m2";
  patch.provider = "surplus";
  patch.billing_status = "reconciled";
  patch.completed_at_ms = 1'500;
  patch.latency_ms = nullptr;
  patch.method = "get";
  WRITE([patch, store] { return updateApiKeyRequestLog("key-w", 1'000, "req-w", patch, store); });
  check("update existing", outcome, true);

  ApiKeyRequestLogPatch streamOnly;
  streamOnly.stream = true;
  WRITE([streamOnly, store] { return updateApiKeyRequestLog("key-w", 1'000, "req-w", streamOnly, store); });
  check("update stream only", outcome, true);

  ApiKeyRequestLogPatch missing;
  missing.status_code = 500;
  WRITE([missing, store] { return updateApiKeyRequestLog("key-w", 1'000, "absent", missing, store); });
  check("update missing row", outcome, true);

  ApiKeyRequestLogInput blank = base();
  blank.id = "req-blank";
  WRITE([blank, store] { return recordApiKeyRequestLog("   ", blank, store); });
  check("record blank key id", outcome, true);

  ApiKeyRequestLogInput nanCreated = base();
  nanCreated.id = "req-nan";
  nanCreated.created_at_ms = rt::Number::NaN;
  WRITE([nanCreated, store] { return recordApiKeyRequestLog("key-w", nanCreated, store); });
  check("record NaN created_at", outcome, true);

  ApiKeyRequestLogInput fractional = base();
  fractional.id = "req-frac";
  fractional.created_at_ms = 1000.9;
  WRITE([fractional, store] { return recordApiKeyRequestLog("key-w", fractional, store); });
  check("record fractional created_at", outcome, true);

  ApiKeyRequestLogInput withoutId = base();
  withoutId.created_at_ms = 2'000;
  WRITE([withoutId, store] { return recordApiKeyRequestLog("key-w", withoutId, store); });
  check("record without id", outcome, true);

  ApiKeyRequestLogInput blankId = base();
  blankId.id = "   ";
  blankId.created_at_ms = 3'000;
  WRITE([blankId, store] { return recordApiKeyRequestLog("key-w", blankId, store); });
  check("record blank id", outcome, true);

  ApiKeyRequestLogInput longId = base();
  longId.id = rt::repeat("x", 130);
  longId.created_at_ms = 4'000;
  WRITE([longId, store] { return recordApiKeyRequestLog("key-w", longId, store); });
  check("record long id", outcome, true);

  ApiKeyRequestLogInput current = base();
  current.id = "req-now";
  current.created_at_ms = now;
  WRITE([current, store] { return recordApiKeyRequestLog("key-w", current, store); });
  check("record current created_at", outcome, true);

  ApiKeyRequestLogInput viaGetKv = base();
  viaGetKv.id = "req-getkv";
  viaGetKv.created_at_ms = 5'000;
  WRITE([viaGetKv] { return recordApiKeyRequestLog("key-w", viaGetKv); });
  check("record via getKv", outcome, true);

  const rt::KvKey halfKey = apiKeyRequestLogKey("key-w", now + 0.5, "req-half");
  const rt::KvValue seeded = rt::KvValue::object({{"route", rt::KvValue("seeded")}});
  const rt::KvCommitResult seed = co_await kv->set(halfKey, seeded);
  EXPECT(seed.ok);
  check("seed fractional row", json::value(), false);

  ApiKeyRequestLogPatch status;
  status.status_code = 1;
  WRITE([status, store, now] { return updateApiKeyRequestLog("key-w", now + 0.5, "req-half", status, store); });
  check("update fractional expireIn", outcome, true);

  const auto conflicting = std::make_shared<ConflictingKv>(kv);
  const std::shared_ptr<rt::Kv> conflictingStore = conflicting;
  ApiKeyRequestLogInput failing = base();
  failing.id = "req-fail";
  failing.created_at_ms = 6'000;
  WRITE([failing, conflictingStore] { return recordApiKeyRequestLog("key-w", failing, conflictingStore); });
  check("record with failing commits", outcome, true);
  EXPECT_EQ(static_cast<double>(*conflicting->commits), *rt::getNumber(vectors().at("failingCommits")));

  uos::kv::setKvForTest(nullptr);
  ApiKeyRequestLogInput noKv = base();
  noKv.id = "req-nokv";
  WRITE([noKv] { return recordApiKeyRequestLog("key-w", noKv); });
  check("record without kv", outcome, true);

  ApiKeyRequestLogInput nullKv = base();
  nullKv.id = "req-nullkv";
  WRITE([nullKv] { return recordApiKeyRequestLog("key-w", nullKv, std::shared_ptr<rt::Kv>()); });
  check("record with null kv", outcome, true);
  EXPECT_EQ(index, expected.size());
#undef WRITE
  rt::globalDateNow() = realNow;
}
