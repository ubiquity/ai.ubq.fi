// Differential cases for stored values JSON cannot hold (Date, Map, bigint, KvU64, undefined
// members), as src/kernel/quota-v2.ts, quota-reserve.ts and usage.ts read them: they pin
// quota_v2::kernelQuotaValue (the JSON view the native-kernel normalizers read) and the rt::KvValue
// readers of usage.cpp against the bun oracle in kernel_quota_values_differential_vectors.inc.
// Links: tests/kernel_quota_values_differential_test.cpp src/kernel/usage.cpp src/kernel/quota_v2.cpp src/kernel/quota_reserve.cpp
//        src/kernel/quota_reservations.cpp src/types.cpp src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp
//        src/http.cpp src/auth/relay.cpp src/config.cpp src/provider/cerebras_rate_limits.cpp src/release.cpp src/utils.cpp src/kv.cpp
//        src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
//        tests/helpers/counting_kv.cpp
#include "uos/kernel/quota_reserve.hpp"
#include "uos/kernel/quota_v2.hpp"
#include "uos/kernel/usage.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/defaults.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace kvfacade = uos::kv;
  namespace quota_v2 = uos::kernel::quota_v2;
  namespace usage = uos::kernel::usage;
  using uos::tests::helpers::counting_kv::CountingKv;

  constexpr std::string_view ORACLE =
#include "kernel_quota_values_differential_vectors.inc"
    ;

  constexpr double NOW = 1'800'000'000'000;
  constexpr double W = NOW - 30'000;

  rt::KvObject orgPolicy() {
    rt::KvObject row;
    row.set("v", rt::KvValue(2));
    row.set("scope", rt::KvValue("org"));
    row.set("owner", rt::KvValue("acme"));
    row.set("usage_limit_requests", rt::KvValue(4));
    row.set("window_ms", rt::KvValue(60'000));
    row.set("expires_at_ms", rt::KvValue(-1));
    row.set("created_at_ms", rt::KvValue(1'000));
    row.set("updated_at_ms", rt::KvValue(2'000));
    return row;
  }

  rt::KvObject orgWindow() {
    rt::KvObject row;
    row.set("v", rt::KvValue(2));
    row.set("scope", rt::KvValue("org"));
    row.set("owner", rt::KvValue("acme"));
    row.set("usage_requests", rt::KvValue(1));
    row.set("reserved_requests", rt::KvValue(0));
    row.set("usage_reset_at_ms", rt::KvValue(NOW + 30'000));
    row.set("applied_window_ms", rt::KvValue(60'000));
    row.set("created_at_ms", rt::KvValue(W));
    row.set("updated_at_ms", rt::KvValue(W));
    return row;
  }

  json::value snapshotJson(const std::optional<quota_v2::KernelOrgLimitSnapshot>& snapshot) {
    if (!snapshot) return nullptr;
    json::object out;
    out["record"] = json::value_from(snapshot->record);
    out["source"] = snapshot->source;
    return json::value(std::move(out));
  }

  template <class Records> json::value rowsJson(const std::optional<Records>& rows) {
    if (!rows) return nullptr;
    json::array out;
    for (const auto& row : *rows) out.push_back(json::value_from(row));
    return json::value(std::move(out));
  }

  template <class Record> json::value recordJson(const std::optional<Record>& record) { return record ? json::value_from(*record) : json::value(nullptr); }

  // The oracle's entry of that name.
  json::value expectedOf(std::string_view name) {
    json::value oracle = rt::jsonParse(ORACLE);
    for (json::value& scenario : oracle.as_array()) {
      if (rt::getString(scenario, "name") == std::optional<std::string_view>(name)) return scenario;
    }
    return nullptr;
  }

  // A fresh store installed as the facade KV, Date.now at NOW, warnings silenced.
  struct Fixture {
    std::shared_ptr<CountingKv> kv = std::make_shared<CountingKv>();
    rt::console::Capture quiet{{rt::console::Method::warn}};
    Fixture() {
      rt::globalDateNow() = [] { return NOW; };
      kvfacade::setKvForTest(kv);
    }
    ~Fixture() { kvfacade::setKvForTest(nullptr); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
  };

  void expectOracle(std::string_view name, json::value result, std::optional<json::value> window = std::nullopt) {
    json::object actual;
    actual["name"] = name;
    actual["result"] = std::move(result);
    if (window) actual["window"] = std::move(*window);
    EXPECT_EQ(rt::jsonStringify(json::value(std::move(actual))), rt::jsonStringify(expectedOf(name)));
  }
}

ASYNC_TEST("a snapshot ignores members JSON cannot hold") {
  const Fixture fixture;
  rt::KvObject policy = orgPolicy();
  policy.set("note", rt::KvValue(rt::KvDate{0}));
  policy.set("big", rt::KvValue(rt::BigInt(5)));
  fixture.kv->seed(quota_v2::kernelOrgPolicyKey("acme"), rt::KvValue(std::move(policy)));
  rt::KvObject window = orgWindow();
  window.set("seen", rt::KvValue(rt::KvDate{5}));
  rt::KvMap tags;
  tags.set(rt::KvValue("a"), rt::KvValue(1));
  window.set("tags", rt::KvValue(std::move(tags)));
  fixture.kv->seed(quota_v2::kernelOrgWindowKey("acme"), rt::KvValue(std::move(window)));
  auto snapshotting = quota_v2::getKernelOrgUsageLimitSnapshot("acme");
  const std::optional<quota_v2::KernelOrgLimitSnapshot> snapshot = co_await std::move(snapshotting);
  auto listing = quota_v2::listKernelOrgUsageLimits();
  const auto rows = co_await std::move(listing);
  json::object result;
  result["snapshot"] = snapshotJson(snapshot);
  result["rows"] = rowsJson(rows);
  expectOracle("a snapshot ignores members JSON cannot hold", json::value(std::move(result)));
}

ASYNC_TEST("a policy that is not a record is malformed") {
  const Fixture fixture;
  rt::KvMap map;
  map.set(rt::KvValue("v"), rt::KvValue(2));
  fixture.kv->seed(quota_v2::kernelOrgPolicyKey("acme"), rt::KvValue(std::move(map)));
  fixture.kv->seed(quota_v2::kernelOrgPolicyKey("beta"), rt::KvValue(rt::BigInt(7)));
  auto snapshotting = quota_v2::getKernelOrgUsageLimitSnapshot("acme");
  const std::optional<quota_v2::KernelOrgLimitSnapshot> snapshot = co_await std::move(snapshotting);
  auto listing = quota_v2::listKernelOrgUsageLimits();
  const auto rows = co_await std::move(listing);
  json::object result;
  result["snapshot"] = snapshotJson(snapshot);
  result["rows"] = rowsJson(rows);
  expectOracle("a policy that is not a record is malformed", json::value(std::move(result)));
}

ASYNC_TEST("a bigint default limit falls back") {
  const Fixture fixture;
  fixture.kv->seed(uos::defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY, rt::KvValue(rt::BigInt(7)));
  fixture.kv->seed(uos::defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY, rt::KvValue(rt::KvDate{5}));
  auto snapshotting = quota_v2::getKernelOrgUsageLimitSnapshot("nobody");
  const std::optional<quota_v2::KernelOrgLimitSnapshot> snapshot = co_await std::move(snapshotting);
  expectOracle("a bigint default limit falls back", snapshotJson(snapshot));
}

ASYNC_TEST("usage readers coerce members JSON cannot hold") {
  const Fixture fixture;
  rt::KvObject record;
  record.set("owner", rt::KvValue("acme"));
  record.set("repo", rt::KvValue(rt::KvDate{1}));
  record.set("total_requests", rt::KvValue(rt::BigInt(5)));
  record.set("stream_requests", rt::KvValue(rt::KvDate{3}));
  record.set("last_model", rt::KvValue(rt::KvDate{0}));
  record.set("last_route", rt::KvValue("r"));
  fixture.kv->seed(usage::kernelUsageKey("acme", "demo"), rt::KvValue(std::move(record)));
  rt::KvObject bigintDay;
  bigintDay.set("day", rt::KvValue("2027-01-15"));
  bigintDay.set("request_count", rt::KvValue(rt::BigInt(4)));
  rt::KvObject numberDay;
  numberDay.set("day", rt::KvValue("2027-01-15"));
  numberDay.set("request_count", rt::KvValue(2));
  rt::KvArray days;
  days.push_back(rt::KvValue(rt::KvMap()));
  days.push_back(rt::KvValue(std::move(bigintDay)));
  days.push_back(rt::KvValue(std::move(numberDay)));
  rt::KvObject daily;
  daily.set("days", rt::KvValue(std::move(days)));
  fixture.kv->seed(usage::kernelUsageDailyKey("acme", "demo"), rt::KvValue(std::move(daily)));
  usage::KernelUsageReadOptions oneDay;
  oneDay.includeDaily = true;
  oneDay.dailyDays = 1;
  auto reading = usage::getKernelUsage("acme", "demo", oneDay);
  const std::optional<usage::KernelAuthUsageWithDaily> found = co_await std::move(reading);
  expectOracle("usage readers coerce members JSON cannot hold", recordJson(found));
}

ASYNC_TEST("a falsy bigint usage value reads as absent and a KvU64 as a record") {
  const Fixture fixture;
  fixture.kv->seed(usage::kernelUsageKey("acme", "zero"), rt::KvValue(rt::BigInt(0)));
  fixture.kv->seed(usage::kernelUsageKey("acme", "big"), rt::KvValue(rt::BigInt(9)));
  fixture.kv->seed(usage::kernelOrgUsageKey("acme"), rt::KvValue(rt::KvU64(0)));
  auto zeroReading = usage::getKernelUsage("acme", "zero");
  const std::optional<usage::KernelAuthUsageWithDaily> zero = co_await std::move(zeroReading);
  auto bigReading = usage::getKernelUsage("acme", "big");
  const std::optional<usage::KernelAuthUsageWithDaily> big = co_await std::move(bigReading);
  auto orgReading = usage::getKernelOrgUsage("acme");
  const std::optional<usage::KernelOrgUsageWithDaily> org = co_await std::move(orgReading);
  auto listing = usage::listKernelUsageRecords();
  const auto list = co_await std::move(listing);
  json::object result;
  result["zero"] = recordJson(zero);
  result["big"] = recordJson(big);
  result["org"] = recordJson(org);
  result["list"] = rowsJson(list);
  expectOracle("a falsy bigint usage value reads as absent and a KvU64 as a record", json::value(std::move(result)));
}

ASYNC_TEST("an undefined reserved_requests member is still the aggregate") {
  const Fixture fixture;
  rt::KvObject policy = orgPolicy();
  policy.set("usage_limit_requests", rt::KvValue(10));
  fixture.kv->seed(quota_v2::kernelOrgPolicyKey("acme"), rt::KvValue(std::move(policy)));
  rt::KvObject window = orgWindow();
  window.set("reserved_requests", rt::KvValue());
  fixture.kv->seed(quota_v2::kernelOrgWindowKey("acme"), rt::KvValue(std::move(window)));
  json::object reservation;
  reservation["v"] = 2;
  reservation["scope"] = "org";
  reservation["owner"] = "acme";
  reservation["request_id"] = "req-old";
  reservation["route"] = "/v1/responses";
  reservation["window_created_at_ms"] = rt::makeNumber(W);
  reservation["window_reset_at_ms"] = rt::makeNumber(NOW + 30'000);
  reservation["state"] = "reserved";
  reservation["terminal_intent"] = nullptr;
  reservation["reserved_at_ms"] = rt::makeNumber(NOW - 10);
  reservation["lease_expires_at_ms"] = rt::makeNumber(NOW + 290'000);
  reservation["committed_at_ms"] = nullptr;
  reservation["released_at_ms"] = nullptr;
  reservation["release_reason"] = nullptr;
  fixture.kv->seed(quota_v2::kernelOrgReservationKey("acme", W, "req-old"), rt::KvValue(json::value(std::move(reservation))));
  uos::kernel::quota_reserve::ReserveKernelUsageLimitOptions options;
  options.kv = std::shared_ptr<rt::Kv>(fixture.kv);
  auto reserving = quota_v2::reserveKernelOrgUsageLimit("acme", "req-1", "/v1/responses", options);
  const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
  json::object result;
  if (const auto* admitted = std::get_if<quota_v2::KernelQuotaReservationDecisionOk>(&decision)) {
    rt::Promise<void> committing = admitted->reservation.commit();
    co_await committing.wait();
    result["ok"] = true;
  } else {
    result["ok"] = false;
    result["status"] = std::get<quota_v2::KernelQuotaReservationDecisionNotOk>(decision).response.status();
  }
  const rt::KvKey windowKey = quota_v2::kernelOrgWindowKey("acme");
  const rt::KvEntryMaybe<> stored = co_await fixture.kv->get(windowKey);
  expectOracle("an undefined reserved_requests member is still the aggregate", json::value(std::move(result)), rt::canonicalNumbers(stored.value.toJson()));
}
