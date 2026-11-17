// Differential cases for src/paid-fallback/ledger-stats.ts beyond the ported bun cases, recorded
// from the TypeScript under Bun 1.4.0: UTC day buckets, JSON byte estimates, the row validator, a
// sequence of settlement and projection merges (onto a corrupt day and onto a row carrying an
// undeclared member, which the merge keeps), the rows left, and growth views.
#include "uos/paid_fallback/ledger_stats.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "paid_fallback_ledger_stats_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::paid_fallback::ledger_stats;

namespace {
  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(kLedgerStatsVectors);
    return parsed;
  }
  const json::value& at(const json::value& value, std::string_view key) {
    static const json::value null;
    const json::value* member = rt::get(value, key);
    return member != nullptr ? *member : null;
  }
  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    std::fprintf(stderr, "    %s\n      actual:   %s\n      expected: %s\n", label.c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(expected).c_str());
    return false;
  }
  std::optional<double> optionalNumber(const json::value& value) { return value.is_null() ? std::nullopt : rt::getNumber(value); }
}

TEST("day buckets, byte estimates and the row validator match the oracle") {
  for (const json::value& pair : at(vectors(), "dayStarts").as_array()) {
    const double ms = *rt::getNumber(pair.as_array()[0]);
    EXPECT_EQ(paidFallbackLedgerStatsDayStart(ms), *rt::getNumber(pair.as_array()[1]));
  }
  for (const json::value& pair : at(vectors(), "bytes").as_array())
    EXPECT_EQ(estimatePaidFallbackRecordBytes(pair.as_array()[0]), *rt::getNumber(pair.as_array()[1]));
  int index = 0;
  for (const json::value& pair : at(vectors(), "validators").as_array()) {
    const bool actual = isPaidFallbackLedgerDailyStats(rt::KvValue(pair.as_array()[0]));
    if (actual != pair.as_array()[1].as_bool()) std::fprintf(stderr, "    validator %d\n", index);
    EXPECT_EQ(actual, pair.as_array()[1].as_bool());
    ++index;
  }
  EXPECT_EQ(index, 10);
}

ASYNC_TEST("merges, stored rows and growth views match the oracle") {
  const double now = *rt::getNumber(vectors(), "NOW");
  const double day = PAID_FALLBACK_LEDGER_STATS_DAY_MS;
  const std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  const double day0 = paidFallbackLedgerStatsDayStart(now);
  // `{ ...empty(day0 - 2 days), settled_rows: 5, settled_row_bytes: 4000, extra_member: "survives", updated_at_ms: 1 }`
  json::object extra = json::value_from(emptyPaidFallbackLedgerDailyStats(day0 - 2 * day)).as_object();
  extra["settled_rows"] = 5;
  extra["settled_row_bytes"] = 4000;
  extra["extra_member"] = "survives";
  extra["updated_at_ms"] = 1;
  const rt::KvValue extraValue{json::value(extra)};
  co_await kv->set(paidFallbackLedgerStatsDayKey(day0 - 2 * day), extraValue);
  const rt::KvValue corrupt = rt::KvValue::object({{"corrupt", rt::KvValue(true)}});
  co_await kv->set(paidFallbackLedgerStatsDayKey(day0 - day), corrupt);

  const auto settle = [&kv](double settledRowBytes, std::optional<double> rollupBytes, double nowMs) {
    return recordPaidFallbackLedgerSettlementStats(kv, PaidFallbackLedgerSettlementStatsInput{.settledRowBytes = settledRowBytes, .rollupBytes = rollupBytes, .nowMs = nowMs});
  };
  const auto project = [&kv](double windowDays, double readUnits, double rollupRows, double nowMs) {
    return recordPaidFallbackLedgerProjectionStats(kv, PaidFallbackLedgerProjectionStatsInput{.windowDays = windowDays, .readUnits = readUnits, .rollupRows = rollupRows, .nowMs = nowMs});
  };
  std::vector<bool> writes;
  const bool written1 = co_await settle(812.7, 2400.2, now);
  writes.push_back(written1);
  const bool written2 = co_await settle(790, std::nullopt, now + 1);
  writes.push_back(written2);
  const bool written3 = co_await settle(-5, -1, now + 2);
  writes.push_back(written3);
  const bool written4 = co_await settle(100, 10, day0 - day + 5);
  writes.push_back(written4);
  const bool written5 = co_await settle(100, std::nullopt, day0 - 2 * day + 5);
  writes.push_back(written5);
  const bool written6 = co_await project(7, 12.9, 3, now);
  writes.push_back(written6);
  const bool written7 = co_await project(30, 120, 100, now);
  writes.push_back(written7);
  const bool written8 = co_await project(90, 400, 380, now);
  writes.push_back(written8);
  const bool written9 = co_await project(14, 1, 1, now);
  writes.push_back(written9);
  const bool written10 = co_await settle(1, std::nullopt, -1);
  writes.push_back(written10);
  const bool written11 = co_await project(7, 1, 1, now + 0.9);
  writes.push_back(written11);
  const PaidFallbackLedgerSettlementStatsInput nullInput{.settledRowBytes = 1, .rollupBytes = std::nullopt, .nowMs = now};
  const bool written12 = co_await recordPaidFallbackLedgerSettlementStats(nullptr, nullInput);
  writes.push_back(written12);
  const json::array& expectedWrites = at(vectors(), "writes").as_array();
  ASSERT_EQ(writes.size(), expectedWrites.size());
  for (std::size_t index = 0; index < writes.size(); ++index) EXPECT_EQ(writes[index], expectedWrites[index].as_array()[1].as_bool());

  rt::KvListSelector selector;
  selector.prefix = PAID_FALLBACK_LEDGER_STATS_PREFIX;
  const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector);
  json::array stored;
  while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
    json::array key;
    for (const rt::KvKeyPart& part : entry->key) key.push_back(part.isString() ? json::value(part.string()) : rt::makeNumber(part.number()));
    stored.push_back(json::array{key, entry->value.toJson()});
  }
  EXPECT(same(stored, at(vectors(), "stored"), "stored rows"));

  int views = 0;
  for (const json::value& view : at(vectors(), "views").as_array()) {
    const json::value& options = view.as_array()[0];
    PaidFallbackLedgerGrowthOptions growthOptions{.nowMs = *rt::getNumber(options, "nowMs"), .retentionMs = *rt::getNumber(options, "retentionMs")};
    growthOptions.leaderboardDays = optionalNumber(at(options, "leaderboardDays"));
    const std::shared_ptr<rt::Kv> target = at(options, "nullKv").is_bool() ? nullptr : kv;
    const PaidFallbackLedgerGrowthView growth = co_await readPaidFallbackLedgerGrowth(target, growthOptions);
    EXPECT(same(json::value(toJson(growth)), view.as_array()[1], "view " + std::to_string(views)));
    ++views;
  }
  EXPECT_EQ(views, 7);
  kv->close();
}
