// Port of tests/kernel-usage-coverage.test.ts (every case).
// Links: tests/kernel_usage_coverage_test.cpp src/kernel/usage.cpp src/kernel/quota_v2.cpp src/kernel/quota_reservations.cpp src/types.cpp
//        src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp src/http.cpp src/auth/relay.cpp src/config.cpp
//        src/provider/cerebras_rate_limits.cpp src/release.cpp src/utils.cpp src/kv.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/native_kv.cpp tests/helpers/counting_kv.cpp
//
// CountingKv is final in C++, so the test's FaultyKv subclass is a ForwardingKv over its own
// CountingKv. The shared `kv` is installed with setKvForTest at the start of every case and
// removed at its end (the module state outlives each case's loop).
#include "uos/kernel/quota_reservations.hpp"
#include "uos/kernel/quota_v2.hpp"
#include "uos/kernel/usage.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/defaults.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace kvfacade = uos::kv;
  namespace quota_v2 = uos::kernel::quota_v2;
  namespace reservations = uos::kernel::quota_reservations;
  using uos::authority::native_kernel_contracts::KernelQuotaWindowV2;
  using uos::kernel::usage::KernelAuthUsageWithDaily;
  using uos::kernel::usage::KernelOrgUsageWithDaily;
  using uos::kernel::usage::KernelUsageReadOptions;
  using uos::tests::helpers::counting_kv::CountingKv;
  using uos::types::KernelAuthUsageRecord;
  namespace usage = uos::kernel::usage;

  constexpr double DAY_MS = 24 * 60 * 60 * 1'000;

  std::shared_ptr<CountingKv> kv = std::make_shared<CountingKv>();

  class FaultyKv;

  class FaultyAtomicOperation final : public rt::ForwardingAtomicOperation {
  public:
    FaultyAtomicOperation(FaultyKv& owner, std::unique_ptr<rt::AtomicOperation> operation) : rt::ForwardingAtomicOperation(std::move(operation)), owner_(owner) {}
    rt::Task<rt::KvCommitResult> commit() override;

  private:
    FaultyKv& owner_;
  };

  /** The shared KV substitute with individual paths that can be made to fail. */
  class FaultyKv final : public rt::ForwardingKv {
  public:
    FaultyKv() : FaultyKv(std::make_shared<CountingKv>()) {}
    bool failGet = false;
    bool failList = false;
    double failCommits = 0;
    bool throwOnCommit = false;
    void seed(const rt::KvKey& key, const rt::KvValue& value) { counting_->seed(key, value); }
    using rt::Kv::get;
    using rt::Kv::getMany;
    // `get(key)`: throws synchronously, as the TS override does.
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      if (failGet) throw rt::JsError("kv get failed");
      return rt::ForwardingKv::get(std::move(key), options);
    }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      if (failList) throw rt::JsError("kv list failed");
      return rt::ForwardingKv::list(std::move(selector), std::move(options));
    }
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<FaultyAtomicOperation>(*this, rt::ForwardingKv::atomic()); }

  private:
    explicit FaultyKv(std::shared_ptr<CountingKv> counting) : rt::ForwardingKv(*counting), counting_(std::move(counting)) {}
    std::shared_ptr<CountingKv> counting_;
  };

  // `operation.commit = async () => { ... }`
  rt::Task<rt::KvCommitResult> FaultyAtomicOperation::commit() {
    if (owner_.throwOnCommit) throw rt::JsError("kv commit failed");
    if (owner_.failCommits > 0) {
      owner_.failCommits -= 1;
      co_return rt::KvCommitResult{};
    }
    rt::Task<rt::KvCommitResult> committing = rt::ForwardingAtomicOperation::commit();
    rt::KvCommitResult result = co_await std::move(committing);
    co_return result;
  }

  /** Awaits a promise that must reject and returns the failure's message. */
  rt::Task<std::optional<std::string>> rejection(rt::Promise<void> promise) {
    std::optional<std::string> message;
    try {
      co_await promise.wait();
    } catch (const rt::JsError& error) {
      message = error.message;
    }
    co_return message;
  }

  std::string dayKeyUtc(double ms) {
    const rt::Date date(ms);
    return rt::numberToString(date.getUTCFullYear()) + "-" + rt::padStart(rt::numberToString(date.getUTCMonth() + 1), 2, "0") + "-" +
           rt::padStart(rt::numberToString(date.getUTCDate()), 2, "0");
  }

  // `{ ...base, ...overrides }`
  json::object spread(json::object base, const json::object& overrides) {
    for (const auto& member : overrides) rt::objectDefine(base, member.key(), member.value());
    return base;
  }

  rt::KvValue stored(const json::object& value) { return rt::KvValue(json::value(value)); }

  void seedUsage(const std::string& owner, const std::string& repo, const rt::KvValue& value) { kv->seed(usage::kernelUsageKey(owner, repo), value); }
  void seedOrgUsage(const std::string& owner, const rt::KvValue& value) { kv->seed(usage::kernelOrgUsageKey(owner), value); }

  /** A complete, well-formed stored usage record. */
  json::object usageRecord(const std::string& owner, const std::string& repo, const json::object& overrides = {}) {
    json::object record;
    record["owner"] = owner;
    record["repo"] = repo;
    record["total_requests"] = 10;
    record["stream_requests"] = 4;
    record["non_stream_requests"] = 6;
    record["completed_requests"] = 9;
    record["error_requests"] = 1;
    record["input_tokens"] = 100;
    record["output_tokens"] = 200;
    record["total_tokens"] = 300;
    record["first_seen_at_ms"] = 1'000;
    record["last_seen_at_ms"] = 2'000;
    record["last_model"] = "gpt-5.6-sol";
    record["last_reasoning"] = "low";
    record["last_route"] = "/v1/responses";
    return spread(std::move(record), overrides);
  }

  /** A complete, well-formed stored org usage record. */
  json::object orgUsageRecord(const std::string& owner, const json::object& overrides = {}) {
    json::object record = usageRecord(owner, "unused", overrides);
    rt::objectDelete(record, "repo");
    return record;
  }

  constexpr double WINDOW_MS = 60'000;

  json::object reservationRow(const json::object& overrides = {}) {
    json::object row;
    row["v"] = 2;
    row["scope"] = "repo";
    row["owner"] = "acme";
    row["repo"] = "demo";
    row["request_id"] = "req-1";
    row["route"] = "/v1/responses";
    row["window_created_at_ms"] = 5'000;
    row["window_reset_at_ms"] = rt::makeNumber(rt::dateNow() + WINDOW_MS);
    row["state"] = "reserved";
    row["terminal_intent"] = nullptr;
    row["reserved_at_ms"] = 4'000;
    row["lease_expires_at_ms"] = rt::makeNumber(rt::dateNow() + 300'000);
    row["committed_at_ms"] = nullptr;
    row["released_at_ms"] = nullptr;
    row["release_reason"] = nullptr;
    return spread(std::move(row), overrides);
  }

  json::object windowRow(const json::object& overrides = {}) {
    json::object row;
    row["v"] = 2;
    row["scope"] = "repo";
    row["owner"] = "acme";
    row["repo"] = "demo";
    row["usage_requests"] = 0;
    row["reserved_requests"] = 1;
    row["usage_reset_at_ms"] = rt::makeNumber(rt::dateNow() + WINDOW_MS);
    row["applied_window_ms"] = rt::makeNumber(WINDOW_MS);
    row["created_at_ms"] = 5'000;
    row["updated_at_ms"] = 6'000;
    return spread(std::move(row), overrides);
  }

  /** A window stored before the reserved_requests aggregate existed. */
  json::object legacyWindowRow() {
    json::object row = windowRow();
    rt::objectDelete(row, "reserved_requests");
    return row;
  }

  /** The same row, typed the way the reservation API declares it. */
  KernelQuotaWindowV2 typedWindow(const json::object& overrides = {}) { return json::value_to<KernelQuotaWindowV2>(json::value(windowRow(overrides))); }

  json::object members(std::initializer_list<std::pair<std::string_view, json::value>> entries) {
    json::object out;
    for (const auto& [key, value] : entries) rt::objectDefine(out, key, value);
    return out;
  }

  // `entry.value?.[key]` of a stored record
  const rt::KvValue& member(const rt::KvEntryMaybe<>& entry, std::string_view key) { return entry.value[key]; }

  std::vector<double> numbers(std::initializer_list<double> values) { return std::vector<double>(values); }
}

TEST("kernel usage keys address one repo and one org row") {
  EXPECT(usage::kernelUsageKey("acme", "demo") == (rt::KvKey{"ubq_ai", "kernel_auth", "usage", "acme", "demo"}));
  EXPECT(usage::kernelUsageDailyKey("acme", "demo") == (rt::KvKey{"ubq_ai", "kernel_auth", "usage_daily", "acme", "demo"}));
  EXPECT(usage::kernelOrgUsageKey("acme") == (rt::KvKey{"ubq_ai", "kernel_auth", "org_usage", "acme"}));
  EXPECT(usage::kernelOrgUsageDailyKey("acme") == (rt::KvKey{"ubq_ai", "kernel_auth", "org_usage_daily", "acme"}));
  EXPECT(usage::KERNEL_AUTH_USAGE_PREFIX == (rt::KvKey{"ubq_ai", "kernel_auth", "usage"}));
  EXPECT(usage::KERNEL_AUTH_USAGE_DAILY_PREFIX == (rt::KvKey{"ubq_ai", "kernel_auth", "usage_daily"}));
  EXPECT(usage::KERNEL_AUTH_ORG_USAGE_PREFIX == (rt::KvKey{"ubq_ai", "kernel_auth", "org_usage"}));
  EXPECT(usage::KERNEL_AUTH_ORG_USAGE_DAILY_PREFIX == (rt::KvKey{"ubq_ai", "kernel_auth", "org_usage_daily"}));
}

ASYNC_TEST("kernel usage reads report the stored counters and labels") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  seedUsage("acme", "demo", stored(usageRecord("acme", "demo")));

  auto reading = usage::getKernelUsage("acme", "demo");
  const std::optional<KernelAuthUsageWithDaily> found = co_await std::move(reading);
  ASSERT(found.has_value());
  const KernelAuthUsageRecord expected{
    .owner = "acme",
    .repo = "demo",
    .total_requests = 10,
    .stream_requests = 4,
    .non_stream_requests = 6,
    .completed_requests = 9,
    .error_requests = 1,
    .input_tokens = 100,
    .output_tokens = 200,
    .total_tokens = 300,
    .first_seen_at_ms = 1'000,
    .last_seen_at_ms = 2'000,
    .last_model = "gpt-5.6-sol",
    .last_reasoning = "low",
    .last_route = "/v1/responses",
  };
  EXPECT(static_cast<const KernelAuthUsageRecord&>(*found) == expected);
  EXPECT(!found->daily_requests.has_value());

  seedOrgUsage("acme", stored(orgUsageRecord("acme", members({{"total_requests", 3}}))));
  auto orgReading = usage::getKernelOrgUsage("acme");
  const std::optional<KernelOrgUsageWithDaily> orgUsage = co_await std::move(orgReading);
  ASSERT(orgUsage.has_value());
  EXPECT_EQ(orgUsage->total_requests, 3.0);
  EXPECT_EQ(orgUsage->owner, std::string("acme"));
  EXPECT_EQ(orgUsage->last_model, std::optional<std::string>("gpt-5.6-sol"));
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel usage reads coerce malformed stored values instead of trusting them") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  // A stored scalar is not a record: the projection reports an untouched row.
  seedUsage("acme", "scalar", rt::KvValue(7));
  auto scalarReading = usage::getKernelUsage("acme", "scalar");
  const std::optional<KernelAuthUsageWithDaily> scalar = co_await std::move(scalarReading);
  ASSERT(scalar.has_value());
  const KernelAuthUsageRecord untouched{
    .owner = "acme",
    .repo = "scalar",
    .total_requests = 0,
    .stream_requests = 0,
    .non_stream_requests = 0,
    .completed_requests = 0,
    .error_requests = 0,
    .input_tokens = 0,
    .output_tokens = 0,
    .total_tokens = 0,
    .first_seen_at_ms = scalar->first_seen_at_ms,
    .last_seen_at_ms = scalar->last_seen_at_ms,
    .last_model = std::nullopt,
    .last_reasoning = std::nullopt,
    .last_route = std::nullopt,
  };
  EXPECT(static_cast<const KernelAuthUsageRecord&>(*scalar) == untouched);
  // `typeof scalar.first_seen_at_ms === "number"`: the projection time.
  EXPECT(rt::Number::isFinite(scalar->first_seen_at_ms));

  // Field-level coercion: non-numeric counters fall back, fractional counters
  // truncate, identity fields fall back to the requested key, labels trim.
  const json::object coercions = members({
    {"owner", 12},
    {"repo", members({{"name", "demo"}})},
    {"total_requests", "many"},
    {"stream_requests", 3.9},
    {"non_stream_requests", std::numeric_limits<double>::infinity()},
    {"completed_requests", 2},
    {"error_requests", nullptr},
    {"input_tokens", -5},
    {"output_tokens", 1.2},
    {"total_tokens", std::numeric_limits<double>::quiet_NaN()},
    {"first_seen_at_ms", "yesterday"},
    {"last_seen_at_ms", 5.75},
    {"last_model", "  gpt-5.6-sol  "},
    {"last_reasoning", "   "},
    {"last_route", 9},
  });
  seedUsage("acme", "coerced", stored(usageRecord("", "", coercions)));
  auto coercedReading = usage::getKernelUsage("acme", "coerced");
  const std::optional<KernelAuthUsageWithDaily> coerced = co_await std::move(coercedReading);
  ASSERT(coerced.has_value());
  EXPECT_EQ(coerced->owner, std::string("acme"));
  EXPECT_EQ(coerced->repo, std::string("coerced"));
  EXPECT_EQ(coerced->total_requests, 0.0);
  EXPECT_EQ(coerced->stream_requests, 3.0);
  EXPECT_EQ(coerced->non_stream_requests, 0.0);
  EXPECT_EQ(coerced->error_requests, 0.0);
  EXPECT_EQ(coerced->input_tokens, -5.0);
  EXPECT_EQ(coerced->output_tokens, 1.0);
  EXPECT_EQ(coerced->total_tokens, 0.0);
  EXPECT_EQ(coerced->last_seen_at_ms, 5.0);
  EXPECT_EQ(coerced->last_model, std::optional<std::string>("gpt-5.6-sol"));
  EXPECT_EQ(coerced->last_reasoning, std::optional<std::string>());
  EXPECT_EQ(coerced->last_route, std::optional<std::string>());

  // A very long label is retained only up to the stored maximum.
  const std::string longLabel(200, 'm');
  seedUsage("acme", "labels", stored(usageRecord("acme", "labels", members({{"last_model", json::string(longLabel)}, {"last_route", json::string(rt::repeat(" r ", 1))}}))));
  auto labelsReading = usage::getKernelUsage("acme", "labels");
  const std::optional<KernelAuthUsageWithDaily> labels = co_await std::move(labelsReading);
  ASSERT(labels.has_value());
  EXPECT(labels->last_model.has_value() && rt::length(*labels->last_model) == 120);
  EXPECT_EQ(labels->last_model, std::optional<std::string>(std::string(120, 'm')));
  EXPECT_EQ(labels->last_route, std::optional<std::string>("r"));

  seedOrgUsage("acme", rt::KvValue(3));
  auto orgScalarReading = usage::getKernelOrgUsage("acme");
  const std::optional<KernelOrgUsageWithDaily> orgScalar = co_await std::move(orgScalarReading);
  ASSERT(orgScalar.has_value());
  EXPECT_EQ(orgScalar->total_requests, 0.0);
  EXPECT_EQ(orgScalar->owner, std::string("acme"));
  EXPECT_EQ(orgScalar->last_model, std::optional<std::string>());

  seedOrgUsage("beta", stored(orgUsageRecord("", members({{"owner", "  "}, {"total_requests", 4.9}, {"last_model", 5}}))));
  auto orgCoercedReading = usage::getKernelOrgUsage("beta");
  const std::optional<KernelOrgUsageWithDaily> orgCoerced = co_await std::move(orgCoercedReading);
  ASSERT(orgCoerced.has_value());
  EXPECT_EQ(orgCoerced->owner, std::string("beta"));
  EXPECT_EQ(orgCoerced->total_requests, 4.0);
  EXPECT_EQ(orgCoerced->last_model, std::optional<std::string>());
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel usage reads build the daily series from stored day rows") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  const double nowMs = rt::dateNow();
  const std::string today = dayKeyUtc(nowMs);
  const std::string yesterday = dayKeyUtc(nowMs - DAY_MS);
  seedUsage("acme", "demo", stored(usageRecord("acme", "demo")));
  json::array days;
  days.push_back(members({{"day", json::string(today)}, {"request_count", 5}}));
  days.push_back(members({{"day", json::string(yesterday)}, {"request_count", 2.9}}));
  days.push_back(members({{"day", "1970-01-01"}, {"request_count", -4}}));
  days.push_back(members({{"day", "not-a-day"}, {"request_count", 9}}));
  days.push_back(members({{"day", "2024-1-1"}, {"request_count", 9}}));
  days.push_back(members({{"day", ""}, {"request_count", 9}}));
  days.push_back(members({{"day", "2024-02-30"}, {"request_count", 7}}));
  days.push_back(7);
  days.push_back(members({{"request_count", 3}}));
  kv->seed(usage::kernelUsageDailyKey("acme", "demo"), stored(members({{"owner", "acme"}, {"repo", "demo"}, {"days", days}, {"updated_at_ms", 5}})));

  KernelUsageReadOptions withDaily;
  withDaily.includeDaily = true;
  auto seriesReading = usage::getKernelUsage("acme", "demo", withDaily);
  const std::optional<KernelAuthUsageWithDaily> series = co_await std::move(seriesReading);
  ASSERT(series.has_value());
  ASSERT(series->daily_requests.has_value());
  const std::vector<double>& daily = *series->daily_requests;
  EXPECT_EQ(daily.size(), std::size_t{30});
  EXPECT_EQ(daily[29], 5.0);
  EXPECT_EQ(daily[28], 2.0);
  double sum = 0;
  for (const double count : daily) sum += count;
  EXPECT_EQ(sum, 7.0);

  KernelUsageReadOptions shortWindow = withDaily;
  shortWindow.dailyDays = 2.9;
  auto shortReading = usage::getKernelUsage("acme", "demo", shortWindow);
  const std::optional<KernelAuthUsageWithDaily> shortSeries = co_await std::move(shortReading);
  EXPECT(shortSeries.has_value() && shortSeries->daily_requests == std::optional<std::vector<double>>(numbers({2, 5})));
  KernelUsageReadOptions single = withDaily;
  single.dailyDays = 0;
  auto singleReading = usage::getKernelUsage("acme", "demo", single);
  const std::optional<KernelAuthUsageWithDaily> singleSeries = co_await std::move(singleReading);
  EXPECT(singleSeries.has_value() && singleSeries->daily_requests == std::optional<std::vector<double>>(numbers({5})));

  seedOrgUsage("acme", stored(orgUsageRecord("acme")));
  json::array orgDays;
  orgDays.push_back(members({{"day", json::string(today)}, {"request_count", 8}}));
  kv->seed(usage::kernelOrgUsageDailyKey("acme"), stored(members({{"days", orgDays}})));
  KernelUsageReadOptions threeDays = withDaily;
  threeDays.dailyDays = 3;
  auto orgReading = usage::getKernelOrgUsage("acme", threeDays);
  const std::optional<KernelOrgUsageWithDaily> orgSeries = co_await std::move(orgReading);
  EXPECT(orgSeries.has_value() && orgSeries->daily_requests == std::optional<std::vector<double>>(numbers({0, 0, 8})));

  // A daily row that is not a record, and a non-array day list, both read as empty.
  seedUsage("acme", "no-days", stored(usageRecord("acme", "no-days")));
  kv->seed(usage::kernelUsageDailyKey("acme", "no-days"), rt::KvValue(42));
  KernelUsageReadOptions twoDays = withDaily;
  twoDays.dailyDays = 2;
  auto noDaysReading = usage::getKernelUsage("acme", "no-days", twoDays);
  const std::optional<KernelAuthUsageWithDaily> noDays = co_await std::move(noDaysReading);
  EXPECT(noDays.has_value() && noDays->daily_requests == std::optional<std::vector<double>>(numbers({0, 0})));

  seedOrgUsage("beta", stored(orgUsageRecord("beta")));
  kv->seed(usage::kernelOrgUsageDailyKey("beta"), stored(members({{"days", "nope"}})));
  auto orgNoDaysReading = usage::getKernelOrgUsage("beta", twoDays);
  const std::optional<KernelOrgUsageWithDaily> orgNoDays = co_await std::move(orgNoDaysReading);
  EXPECT(orgNoDays.has_value() && orgNoDays->daily_requests == std::optional<std::vector<double>>(numbers({0, 0})));

  // A daily row whose owner/repo are unusable falls back to the requested key.
  seedUsage("acme", "identity", stored(usageRecord("acme", "identity")));
  json::array identityDays;
  identityDays.push_back(members({{"day", json::string(today)}, {"request_count", 1}}));
  kv->seed(usage::kernelUsageDailyKey("acme", "identity"), stored(members({{"owner", 5}, {"repo", nullptr}, {"days", identityDays}})));
  KernelUsageReadOptions oneDay = withDaily;
  oneDay.dailyDays = 1;
  auto identityReading = usage::getKernelUsage("acme", "identity", oneDay);
  const std::optional<KernelAuthUsageWithDaily> identity = co_await std::move(identityReading);
  ASSERT(identity.has_value());
  EXPECT(identity->daily_requests == std::optional<std::vector<double>>(numbers({1})));
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel usage reads fail closed without KV, without a row, and on KV errors") {
  kv->clearData();
  kvfacade::setKvForTest(nullptr);
  {
    auto repoReading = usage::getKernelUsage("acme", "demo");
    const std::optional<KernelAuthUsageWithDaily> repo = co_await std::move(repoReading);
    EXPECT(!repo.has_value());
    auto orgReading = usage::getKernelOrgUsage("acme");
    const std::optional<KernelOrgUsageWithDaily> org = co_await std::move(orgReading);
    EXPECT(!org.has_value());
  }
  kvfacade::setKvForTest(kv);

  KernelUsageReadOptions withDaily;
  withDaily.includeDaily = true;
  auto missingReading = usage::getKernelUsage("acme", "missing");
  const std::optional<KernelAuthUsageWithDaily> missing = co_await std::move(missingReading);
  EXPECT(!missing.has_value());
  auto missingDailyReading = usage::getKernelUsage("acme", "missing", withDaily);
  const std::optional<KernelAuthUsageWithDaily> missingDaily = co_await std::move(missingDailyReading);
  EXPECT(!missingDaily.has_value());
  auto missingOrgReading = usage::getKernelOrgUsage("acme");
  const std::optional<KernelOrgUsageWithDaily> missingOrg = co_await std::move(missingOrgReading);
  EXPECT(!missingOrg.has_value());
  auto missingOrgDailyReading = usage::getKernelOrgUsage("acme", withDaily);
  const std::optional<KernelOrgUsageWithDaily> missingOrgDaily = co_await std::move(missingOrgDailyReading);
  EXPECT(!missingOrgDaily.has_value());

  const auto faulty = std::make_shared<FaultyKv>();
  kvfacade::setKvForTest(faulty);
  {
    faulty->failGet = true;
    auto repoReading = usage::getKernelUsage("acme", "demo");
    const std::optional<KernelAuthUsageWithDaily> repo = co_await std::move(repoReading);
    EXPECT(!repo.has_value());
    auto orgReading = usage::getKernelOrgUsage("acme");
    const std::optional<KernelOrgUsageWithDaily> org = co_await std::move(orgReading);
    EXPECT(!org.has_value());
  }
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel usage listings sort their rows, attach daily series and stay inside their scope") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  seedUsage("beta", "alpha", stored(usageRecord("beta", "alpha", members({{"total_requests", 1}}))));
  seedUsage("acme", "zeta", stored(usageRecord("acme", "zeta", members({{"total_requests", 2}}))));
  seedUsage("acme", "alpha", stored(members({{"owner", 5}, {"repo", 6}, {"total_requests", 3}})));
  // A row stored without owner/repo key parts keeps an empty identity instead of throwing.
  kv->seed(rt::KvKey(usage::KERNEL_AUTH_USAGE_PREFIX), stored(usageRecord("", "", members({{"owner", 7}, {"repo", 8}, {"total_requests", 4}}))));
  // Daily rows live in their own namespace and must never be projected as usage rows.
  kv->seed(rt::KvKey{usage::KERNEL_AUTH_USAGE_DAILY_PREFIX, "ghost", "row"}, stored(members({{"days", json::array()}})));
  json::array alphaDays;
  alphaDays.push_back(members({{"day", json::string(dayKeyUtc(rt::dateNow()))}, {"request_count", 6}}));
  kv->seed(usage::kernelUsageDailyKey("acme", "alpha"), stored(members({{"days", alphaDays}})));
  kv->seed(usage::kernelUsageDailyKey("acme", "zeta"), rt::KvValue(5));
  json::array emptyIdentityDays;
  emptyIdentityDays.push_back(members({{"day", json::string(dayKeyUtc(rt::dateNow()))}, {"request_count", 2}}));
  kv->seed(usage::kernelUsageDailyKey("", ""), stored(members({{"days", emptyIdentityDays}})));

  auto plainListing = usage::listKernelUsageRecords();
  const std::optional<std::vector<KernelAuthUsageWithDaily>> plain = co_await std::move(plainListing);
  ASSERT(plain.has_value());
  std::vector<std::string> identities;
  for (const KernelAuthUsageWithDaily& row : *plain) identities.push_back(row.owner + "/" + row.repo);
  EXPECT(identities == (std::vector<std::string>{"/", "acme/alpha", "acme/zeta", "beta/alpha"}));
  EXPECT(!(*plain)[0].daily_requests.has_value());
  EXPECT_EQ((*plain)[1].total_requests, 3.0);

  KernelUsageReadOptions twoDays;
  twoDays.includeDaily = true;
  twoDays.dailyDays = 2;
  auto dailyListing = usage::listKernelUsageRecords(twoDays);
  const std::optional<std::vector<KernelAuthUsageWithDaily>> withDaily = co_await std::move(dailyListing);
  ASSERT(withDaily.has_value() && withDaily->size() == 4);
  EXPECT((*withDaily)[0].daily_requests == std::optional<std::vector<double>>(numbers({0, 2})));
  EXPECT((*withDaily)[1].daily_requests == std::optional<std::vector<double>>(numbers({0, 6})));
  EXPECT((*withDaily)[2].daily_requests == std::optional<std::vector<double>>(numbers({0, 0})));
  EXPECT((*withDaily)[3].daily_requests == std::optional<std::vector<double>>(numbers({0, 0})));
  EXPECT_EQ((*withDaily)[3].total_requests, 1.0);

  seedOrgUsage("beta", rt::KvValue(9));
  seedOrgUsage("acme", stored(orgUsageRecord("acme", members({{"total_requests", 5}}))));
  json::array orgDays;
  orgDays.push_back(members({{"day", json::string(dayKeyUtc(rt::dateNow()))}, {"request_count", 3}}));
  kv->seed(usage::kernelOrgUsageDailyKey("acme"), stored(members({{"days", orgDays}})));

  auto orgPlainListing = usage::listKernelOrgUsageRecords();
  const std::optional<std::vector<KernelOrgUsageWithDaily>> orgPlain = co_await std::move(orgPlainListing);
  ASSERT(orgPlain.has_value() && orgPlain->size() == 2);
  EXPECT_EQ((*orgPlain)[0].owner, std::string("acme"));
  EXPECT_EQ((*orgPlain)[1].owner, std::string("beta"));
  EXPECT(!(*orgPlain)[0].daily_requests.has_value());
  EXPECT_EQ((*orgPlain)[0].total_requests, 5.0);
  EXPECT_EQ((*orgPlain)[1].total_requests, 0.0);

  KernelUsageReadOptions oneDay;
  oneDay.includeDaily = true;
  oneDay.dailyDays = 1;
  auto orgDailyListing = usage::listKernelOrgUsageRecords(oneDay);
  const std::optional<std::vector<KernelOrgUsageWithDaily>> orgWithDaily = co_await std::move(orgDailyListing);
  ASSERT(orgWithDaily.has_value() && orgWithDaily->size() == 2);
  EXPECT((*orgWithDaily)[0].daily_requests == std::optional<std::vector<double>>(numbers({3})));
  EXPECT((*orgWithDaily)[1].daily_requests == std::optional<std::vector<double>>(numbers({0})));
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel usage listings return an empty array for an empty ledger") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  KernelUsageReadOptions withDaily;
  withDaily.includeDaily = true;
  KernelUsageReadOptions fourDays = withDaily;
  fourDays.dailyDays = 4;
  auto plainListing = usage::listKernelUsageRecords();
  const std::optional<std::vector<KernelAuthUsageWithDaily>> plain = co_await std::move(plainListing);
  EXPECT(plain.has_value() && plain->empty());
  auto dailyListing = usage::listKernelUsageRecords(withDaily);
  const std::optional<std::vector<KernelAuthUsageWithDaily>> daily = co_await std::move(dailyListing);
  EXPECT(daily.has_value() && daily->empty());
  auto orgListing = usage::listKernelOrgUsageRecords();
  const std::optional<std::vector<KernelOrgUsageWithDaily>> org = co_await std::move(orgListing);
  EXPECT(org.has_value() && org->empty());
  auto orgDailyListing = usage::listKernelOrgUsageRecords(fourDays);
  const std::optional<std::vector<KernelOrgUsageWithDaily>> orgDaily = co_await std::move(orgDailyListing);
  EXPECT(orgDaily.has_value() && orgDaily->empty());
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel usage listings fail closed without KV and on KV errors") {
  KernelUsageReadOptions withDaily;
  withDaily.includeDaily = true;
  kvfacade::setKvForTest(nullptr);
  {
    auto plainListing = usage::listKernelUsageRecords();
    const std::optional<std::vector<KernelAuthUsageWithDaily>> plain = co_await std::move(plainListing);
    EXPECT(!plain.has_value());
    auto dailyListing = usage::listKernelUsageRecords(withDaily);
    const std::optional<std::vector<KernelAuthUsageWithDaily>> daily = co_await std::move(dailyListing);
    EXPECT(!daily.has_value());
    auto orgListing = usage::listKernelOrgUsageRecords();
    const std::optional<std::vector<KernelOrgUsageWithDaily>> org = co_await std::move(orgListing);
    EXPECT(!org.has_value());
    auto orgDailyListing = usage::listKernelOrgUsageRecords(withDaily);
    const std::optional<std::vector<KernelOrgUsageWithDaily>> orgDaily = co_await std::move(orgDailyListing);
    EXPECT(!orgDaily.has_value());
  }
  kvfacade::setKvForTest(kv);

  const auto faulty = std::make_shared<FaultyKv>();
  faulty->failList = true;
  kvfacade::setKvForTest(faulty);
  {
    auto plainListing = usage::listKernelUsageRecords();
    const std::optional<std::vector<KernelAuthUsageWithDaily>> plain = co_await std::move(plainListing);
    EXPECT(!plain.has_value());
    auto dailyListing = usage::listKernelUsageRecords(withDaily);
    const std::optional<std::vector<KernelAuthUsageWithDaily>> daily = co_await std::move(dailyListing);
    EXPECT(!daily.has_value());
    auto orgListing = usage::listKernelOrgUsageRecords();
    const std::optional<std::vector<KernelOrgUsageWithDaily>> org = co_await std::move(orgListing);
    EXPECT(!org.has_value());
    auto orgDailyListing = usage::listKernelOrgUsageRecords(withDaily);
    const std::optional<std::vector<KernelOrgUsageWithDaily>> orgDaily = co_await std::move(orgDailyListing);
    EXPECT(!orgDaily.has_value());
  }
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel quota policy state resolves the repo scope, the org scope and malformed policies") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  reservations::KernelQuotaPolicyStateOptions withKv;
  withKv.kv = std::shared_ptr<rt::Kv>(kv);
  auto repoOnlyResolving = reservations::resolveKernelQuotaPolicyState("acme", "demo", withKv);
  const quota_v2::KernelQuotaPolicyStateDecision repoOnly = co_await std::move(repoOnlyResolving);
  ASSERT(std::holds_alternative<quota_v2::KernelQuotaPolicyStateDecisionOk>(repoOnly));
  EXPECT_EQ(std::get<quota_v2::KernelQuotaPolicyStateDecisionOk>(repoOnly).limit_scope, std::string("org"));
  EXPECT_EQ(std::get<quota_v2::KernelQuotaPolicyStateDecisionOk>(repoOnly).has_policy, false);

  kv->seed(quota_v2::kernelOrgPolicyKey("acme"), stored(members({{"v", 2},
                                                                   {"scope", "org"},
                                                                   {"owner", "acme"},
                                                                   {"usage_limit_requests", 5},
                                                                   {"window_ms", rt::makeNumber(WINDOW_MS)},
                                                                   {"expires_at_ms", -1},
                                                                   {"created_at_ms", 1},
                                                                   {"updated_at_ms", 2}})));
  auto orgResolving = reservations::resolveKernelQuotaPolicyState("acme", "demo", withKv);
  const quota_v2::KernelQuotaPolicyStateDecision orgPolicy = co_await std::move(orgResolving);
  ASSERT(std::holds_alternative<quota_v2::KernelQuotaPolicyStateDecisionOk>(orgPolicy));
  EXPECT_EQ(std::get<quota_v2::KernelQuotaPolicyStateDecisionOk>(orgPolicy).limit_scope, std::string("org"));
  EXPECT_EQ(std::get<quota_v2::KernelQuotaPolicyStateDecisionOk>(orgPolicy).has_policy, true);

  kv->seed(quota_v2::kernelRepoPolicyKey("acme", "demo"), stored(members({{"v", 2},
                                                                            {"scope", "repo"},
                                                                            {"owner", "acme"},
                                                                            {"repo", "demo"},
                                                                            {"usage_limit_requests", 5},
                                                                            {"window_ms", rt::makeNumber(WINDOW_MS)},
                                                                            {"expires_at_ms", -1},
                                                                            {"created_at_ms", 1},
                                                                            {"updated_at_ms", 2}})));
  auto repoResolving = reservations::resolveKernelQuotaPolicyState("acme", "demo", withKv);
  const quota_v2::KernelQuotaPolicyStateDecision repoPolicy = co_await std::move(repoResolving);
  ASSERT(std::holds_alternative<quota_v2::KernelQuotaPolicyStateDecisionOk>(repoPolicy));
  EXPECT_EQ(std::get<quota_v2::KernelQuotaPolicyStateDecisionOk>(repoPolicy).limit_scope, std::string("repo"));
  EXPECT_EQ(std::get<quota_v2::KernelQuotaPolicyStateDecisionOk>(repoPolicy).has_policy, true);

  auto stateReading = reservations::readKernelQuotaPolicyState(kv, "acme", "demo");
  const reservations::KernelQuotaPolicyState state = co_await std::move(stateReading);
  EXPECT_EQ(state.limit_scope, std::string("repo"));
  EXPECT_EQ(state.repo_entry.value.isNull(), false);
  EXPECT_EQ(state.org_entry.value.isNull(), false);

  kv->clearData();
  reservations::KernelQuotaPolicyStateOptions nullKv;
  nullKv.kv = std::shared_ptr<rt::Kv>();
  auto noKvResolving = reservations::resolveKernelQuotaPolicyState("acme", "demo", nullKv);
  const quota_v2::KernelQuotaPolicyStateDecision noKv = co_await std::move(noKvResolving);
  ASSERT(std::holds_alternative<quota_v2::KernelQuotaPolicyStateDecisionNotOk>(noKv));
  EXPECT_EQ(std::get<quota_v2::KernelQuotaPolicyStateDecisionNotOk>(noKv).response.status(), 503);

  kv->seed(quota_v2::kernelOrgPolicyKey("acme"), stored(members({{"v", 2}, {"scope", "org"}, {"owner", "acme"}})));
  auto malformedOrgResolving = reservations::resolveKernelQuotaPolicyState("acme", "demo", withKv);
  const quota_v2::KernelQuotaPolicyStateDecision malformedOrg = co_await std::move(malformedOrgResolving);
  EXPECT(std::holds_alternative<quota_v2::KernelQuotaPolicyStateDecisionNotOk>(malformedOrg));
  auto malformedOrgReading = reservations::readKernelQuotaPolicyState(kv, "acme", "demo");
  EXPECT_REJECTS_MESSAGE(std::move(malformedOrgReading), rt::JsError, "Kernel org quota policy is malformed");

  kv->clearData();
  kv->seed(quota_v2::kernelRepoPolicyKey("acme", "demo"), stored(members({{"v", 2}, {"scope", "repo"}, {"owner", "acme"}, {"repo", "other"}})));
  auto malformedRepoResolving = reservations::resolveKernelQuotaPolicyState("acme", "demo", withKv);
  const quota_v2::KernelQuotaPolicyStateDecision malformedRepo = co_await std::move(malformedRepoResolving);
  EXPECT(std::holds_alternative<quota_v2::KernelQuotaPolicyStateDecisionNotOk>(malformedRepo));
  auto malformedRepoReading = reservations::readKernelQuotaPolicyState(kv, "acme", "demo");
  EXPECT_REJECTS_MESSAGE(std::move(malformedRepoReading), rt::JsError, "Kernel repo quota policy is malformed");
  kvfacade::setKvForTest(nullptr);
}

TEST("kernel reservation rows normalize only complete, consistent terminal states") {
  const auto normalize = [](const json::object& value, const std::string& scope = "repo", const std::optional<std::string>& repo = std::string("demo")) {
    return reservations::normalizeKernelQuotaReservationRowV2(json::value(value), scope, "acme", repo);
  };
  const auto valid = normalize(reservationRow());
  ASSERT(valid.has_value());
  EXPECT_EQ(valid->request_id, std::string("req-1"));
  EXPECT_EQ(valid->terminal_intent, std::optional<std::string>());

  const auto committed = normalize(reservationRow(members({{"state", "committed"}, {"terminal_intent", "committed"}, {"committed_at_ms", 9'000}})));
  EXPECT(committed.has_value() && committed->state == "committed");
  const auto released =
    normalize(reservationRow(members({{"state", "released"}, {"terminal_intent", "released"}, {"released_at_ms", 9'000}, {"release_reason", "done"}})));
  EXPECT(released.has_value() && released->release_reason == std::optional<std::string>("done"));
  // `reservationRow({ terminal_intent: undefined })`: JSON holds no undefined member, so the
  // member is absent, which the normalizer reads the same way.
  json::object legacyIntentRow = reservationRow();
  rt::objectDelete(legacyIntentRow, "terminal_intent");
  const auto legacyIntent = normalize(legacyIntentRow);
  EXPECT(legacyIntent.has_value() && !legacyIntent->terminal_intent.has_value());

  std::vector<json::value> rejected;
  rejected.push_back(json::value(7));
  rejected.push_back(json::value(spread(reservationRow(), members({{"scope", "org"}}))));
  rejected.push_back(json::value(spread(reservationRow(), members({{"owner", "other"}}))));
  rejected.push_back(json::value(spread(reservationRow(), members({{"repo", "other"}}))));
  rejected.push_back(json::value(spread(reservationRow(), members({{"v", 3}}))));
  rejected.push_back(json::value(reservationRow(members({{"request_id", ""}}))));
  rejected.push_back(json::value(reservationRow(members({{"request_id", 7}}))));
  rejected.push_back(json::value(reservationRow(members({{"route", ""}}))));
  rejected.push_back(json::value(reservationRow(members({{"route", nullptr}}))));
  rejected.push_back(json::value(reservationRow(members({{"window_created_at_ms", -1}}))));
  rejected.push_back(json::value(reservationRow(members({{"window_reset_at_ms", 0}}))));
  rejected.push_back(json::value(reservationRow(members({{"state", "unknown"}}))));
  rejected.push_back(json::value(reservationRow(members({{"terminal_intent", "unknown"}}))));
  rejected.push_back(json::value(reservationRow(members({{"reserved_at_ms", -1}}))));
  rejected.push_back(json::value(reservationRow(members({{"lease_expires_at_ms", 0}}))));
  rejected.push_back(json::value(reservationRow(members({{"committed_at_ms", 5}}))));
  rejected.push_back(json::value(reservationRow(members({{"released_at_ms", "5"}}))));
  rejected.push_back(json::value(reservationRow(members({{"release_reason", 5}}))));
  rejected.push_back(json::value(reservationRow(members({{"committed_at_ms", 5}, {"release_reason", "x"}}))));
  rejected.push_back(json::value(reservationRow(members({{"state", "committed"}, {"committed_at_ms", nullptr}}))));
  rejected.push_back(json::value(reservationRow(members({{"state", "committed"}, {"committed_at_ms", 5}, {"released_at_ms", 5}}))));
  rejected.push_back(json::value(reservationRow(members({{"state", "committed"}, {"committed_at_ms", 5}, {"release_reason", "x"}}))));
  rejected.push_back(json::value(reservationRow(members({{"state", "committed"}, {"committed_at_ms", 5}, {"terminal_intent", "released"}}))));
  rejected.push_back(json::value(reservationRow(members({{"state", "released"}, {"released_at_ms", nullptr}}))));
  rejected.push_back(json::value(reservationRow(members({{"state", "released"}, {"released_at_ms", 5}, {"committed_at_ms", 5}, {"release_reason", "x"}}))));
  rejected.push_back(json::value(reservationRow(members({{"state", "released"}, {"released_at_ms", 5}, {"release_reason", ""}}))));
  rejected.push_back(json::value(reservationRow(members({{"state", "released"}, {"released_at_ms", 5}, {"release_reason", "x"}, {"terminal_intent", "committed"}}))));
  for (const json::value& value : rejected) {
    EXPECT(!reservations::normalizeKernelQuotaReservationRowV2(value, "repo", "acme", std::string("demo")).has_value());
  }
  EXPECT(!normalize(reservationRow(), "org", std::nullopt).has_value());
}

ASYNC_TEST("kernel window reservation reconciliation counts legacy rows and rejects malformed ones") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  const rt::KvKey entryKey = quota_v2::kernelRepoWindowKey("acme", "demo");
  const rt::KvEntryMaybe<> entry = co_await kv->get(entryKey);
  EXPECT(entry.value.isNull());
  auto emptyReconciling = reservations::reconcileKernelQuotaWindowReservations(kv, entry, "repo", "acme", "demo");
  const std::optional<KernelQuotaWindowV2> empty = co_await std::move(emptyReconciling);
  EXPECT(!empty.has_value());

  // A window written before the aggregate field existed is rebuilt from its rows.
  kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"), stored(legacyWindowRow()));
  kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, "req-1"), stored(reservationRow()));
  kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, "req-2"),
           stored(reservationRow(members({{"request_id", "req-2"}, {"state", "released"}, {"released_at_ms", 1}, {"release_reason", "x"}}))));
  const rt::KvKey legacyEntryKey = quota_v2::kernelRepoWindowKey("acme", "demo");
  const rt::KvEntryMaybe<> legacyEntry = co_await kv->get(legacyEntryKey);
  auto legacyReconciling = reservations::reconcileKernelQuotaWindowReservations(kv, legacyEntry, "repo", "acme", "demo");
  const std::optional<KernelQuotaWindowV2> reconciled = co_await std::move(legacyReconciling);
  ASSERT(reconciled.has_value());
  EXPECT_EQ(reconciled->reserved_requests, 1.0);
  EXPECT_EQ(reconciled->usage_requests, 0.0);

  // A window that already carries the aggregate is returned untouched.
  kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"), stored(windowRow(members({{"reserved_requests", 7}}))));
  const rt::KvKey storedEntryKey = quota_v2::kernelRepoWindowKey("acme", "demo");
  const rt::KvEntryMaybe<> storedEntry = co_await kv->get(storedEntryKey);
  auto storedReconciling = reservations::reconcileKernelQuotaWindowReservations(kv, storedEntry, "repo", "acme", "demo");
  const std::optional<KernelQuotaWindowV2> untouched = co_await std::move(storedReconciling);
  EXPECT(untouched.has_value() && untouched->reserved_requests == 7);

  // A reservation row that disagrees with its key is malformed, not ignored.
  kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"), stored(legacyWindowRow()));
  kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, "req-1"), stored(reservationRow(members({{"request_id", "different"}}))));
  const rt::KvKey brokenEntryKey = quota_v2::kernelRepoWindowKey("acme", "demo");
  const rt::KvEntryMaybe<> brokenEntry = co_await kv->get(brokenEntryKey);
  auto brokenReconciling = reservations::reconcileKernelQuotaWindowReservations(kv, brokenEntry, "repo", "acme", "demo");
  EXPECT_REJECTS_MESSAGE(std::move(brokenReconciling), rt::JsError, "Kernel quota reservation is malformed");
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel expired reservations are reclaimed once and fail closed when every commit is lost") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  const KernelQuotaWindowV2 liveWindow = typedWindow();
  auto liveReclaiming = reservations::reclaimExpiredKernelReservationUnlocked(kv, "repo", "acme", "demo", liveWindow, rt::dateNow());
  const bool liveReclaimed = co_await std::move(liveReclaiming);
  EXPECT_EQ(liveReclaimed, false);

  // An expired lease that cannot be committed keeps the aggregate reserved.
  const auto faulty = std::make_shared<FaultyKv>();
  kvfacade::setKvForTest(faulty);
  faulty->seed(quota_v2::kernelRepoWindowKey("acme", "demo"), stored(windowRow(members({{"reserved_requests", 1}}))));
  faulty->seed(quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, "req-1"),
               stored(reservationRow(members({{"lease_expires_at_ms", rt::makeNumber(rt::dateNow() - 1)}}))));
  const KernelQuotaWindowV2 expiredWindow = typedWindow();
  faulty->failCommits = 3;
  auto lostReclaiming = reservations::reclaimExpiredKernelReservationUnlocked(faulty, "repo", "acme", "demo", expiredWindow, rt::dateNow());
  EXPECT_REJECTS_MESSAGE(std::move(lostReclaiming), rt::JsError, "Kernel quota reservation changed concurrently");
  faulty->failCommits = 0;
  auto reclaiming = reservations::reclaimExpiredKernelReservationUnlocked(faulty, "repo", "acme", "demo", expiredWindow, rt::dateNow());
  const bool reclaimed = co_await std::move(reclaiming);
  EXPECT_EQ(reclaimed, true);
  const rt::KvKey windowEntryKey = quota_v2::kernelRepoWindowKey("acme", "demo");
  const rt::KvEntryMaybe<> windowEntry = co_await faulty->get(windowEntryKey);
  EXPECT_EQ(member(windowEntry, "reserved_requests").asNumber(), 0.0);
  const rt::KvKey rowEntryKey = quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, "req-1");
  const rt::KvEntryMaybe<> rowEntry = co_await faulty->get(rowEntryKey);
  ASSERT(rowEntry.value.truthy());
  EXPECT_EQ(member(rowEntry, "state").asString(), std::string("released"));
  EXPECT_EQ(member(rowEntry, "release_reason").asString(), std::string("lease_expired"));
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel expired reservation reclaim rejects a row whose window lost its reservation") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"), stored(windowRow(members({{"reserved_requests", 0}}))));
  kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, "req-expired"),
           stored(reservationRow(members({{"request_id", "req-expired"}, {"lease_expires_at_ms", rt::makeNumber(rt::dateNow() - 1)}}))));
  auto reclaiming = reservations::reclaimExpiredKernelReservationUnlocked(kv, "repo", "acme", "demo", typedWindow(), rt::dateNow());
  EXPECT_REJECTS_MESSAGE(std::move(reclaiming), rt::JsError, "Kernel quota reservation is malformed");
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel reservation context commits and releases its own row exactly once") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  const std::string route = "/v1/responses";
  kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"), stored(windowRow()));
  kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, "req-commit"), stored(reservationRow(members({{"request_id", "req-commit"}}))));

  const quota_v2::KernelQuotaReservation committed =
    reservations::kernelReservationContext(kv, "repo", "acme", "demo", 5'000, "req-commit", route, rt::dateNow() + 60'000);
  EXPECT_EQ(committed.signal.aborted(), false);
  rt::Promise<void> committing = committed.commit();
  co_await committing.wait();
  EXPECT_EQ(committed.signal.aborted(), false);
  rt::Promise<void> recommitting = committed.commit();
  co_await recommitting.wait();

  const rt::KvKey windowEntryKey = quota_v2::kernelRepoWindowKey("acme", "demo");

  const rt::KvEntryMaybe<> windowEntry = co_await kv->get(windowEntryKey);
  ASSERT(windowEntry.value.truthy());
  EXPECT_EQ(member(windowEntry, "usage_requests").asNumber(), 1.0);
  EXPECT_EQ(member(windowEntry, "reserved_requests").asNumber(), 0.0);
  const rt::KvKey rowEntryKey = quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, "req-commit");
  const rt::KvEntryMaybe<> rowEntry = co_await kv->get(rowEntryKey);
  ASSERT(rowEntry.value.truthy());
  EXPECT_EQ(member(rowEntry, "state").asString(), std::string("committed"));
  EXPECT_EQ(member(rowEntry, "terminal_intent").asString(), std::string("committed"));
  EXPECT(member(rowEntry, "release_reason").isNull());

  rt::Promise<void> lateReleasing = committed.release(std::string("too late"));
  auto lateReleaseRejection = rejection(lateReleasing);
  const std::optional<std::string> lateRelease = co_await std::move(lateReleaseRejection);
  EXPECT(lateRelease.has_value() && rt::includes(*lateRelease, "terminal state is already committed"));

  kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"), stored(windowRow(members({{"usage_requests", 1}, {"reserved_requests", 1}}))));
  kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, "req-release"), stored(reservationRow(members({{"request_id", "req-release"}}))));
  const quota_v2::KernelQuotaReservation released =
    reservations::kernelReservationContext(kv, "repo", "acme", "demo", 5'000, "req-release", route, rt::dateNow() + 60'000);
  rt::Promise<void> releasing = released.release(std::string(""));
  co_await releasing.wait();
  EXPECT_EQ(released.signal.aborted(), false);

  const rt::KvKey releasedWindowKey = quota_v2::kernelRepoWindowKey("acme", "demo");

  const rt::KvEntryMaybe<> releasedWindow = co_await kv->get(releasedWindowKey);
  ASSERT(releasedWindow.value.truthy());
  EXPECT_EQ(member(releasedWindow, "usage_requests").asNumber(), 1.0);
  EXPECT_EQ(member(releasedWindow, "reserved_requests").asNumber(), 0.0);
  const rt::KvKey releasedRowKey = quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, "req-release");
  const rt::KvEntryMaybe<> releasedRow = co_await kv->get(releasedRowKey);
  ASSERT(releasedRow.value.truthy());
  EXPECT_EQ(member(releasedRow, "state").asString(), std::string("released"));
  EXPECT_EQ(member(releasedRow, "terminal_intent").asString(), std::string("released"));
  EXPECT_EQ(member(releasedRow, "release_reason").asString(), std::string("request_incomplete"));

  rt::Promise<void> lateCommitting = released.commit();
  auto lateCommitRejection = rejection(lateCommitting);
  const std::optional<std::string> lateCommit = co_await std::move(lateCommitRejection);
  EXPECT(lateCommit.has_value() && rt::includes(*lateCommit, "terminal state is already released"));
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel default-window cutover acquires a guard only while no default-backed work is live") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  const rt::KvEntryMaybe<> expectedLimitEntry = co_await kv->get(uos::defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY);
  const rt::KvEntryMaybe<> expectedWindowEntry = co_await kv->get(uos::defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY);

  auto acquiring = reservations::acquireKernelDefaultWindowCutover(kv, expectedLimitEntry, expectedWindowEntry);
  const quota_v2::KernelDefaultWindowCutoverDecision acquired = co_await std::move(acquiring);
  EXPECT(std::holds_alternative<quota_v2::KernelDefaultWindowCutoverDecisionOk>(acquired));
  const rt::KvEntryMaybe<> marker = co_await kv->get(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY);
  ASSERT(marker.value.truthy());
  EXPECT_EQ(member(marker, "v").asNumber(), 2.0);
  EXPECT(member(marker, "id").isString());
  EXPECT_EQ(member(marker, "expires_at_ms").asNumber(), member(marker, "created_at_ms").asNumber() + 5 * 60'000);

  // A live reservation holds the guard back, and the marker is released again.
  kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"),
           stored(windowRow(members({{"reserved_requests", 1}, {"usage_reset_at_ms", rt::makeNumber(rt::dateNow() + WINDOW_MS)}}))));
  kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, "req-live"), stored(reservationRow(members({{"request_id", "req-live"}}))));
  co_await kv->delete_(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY);
  auto blockedAcquiring = reservations::acquireKernelDefaultWindowCutover(kv, expectedLimitEntry, expectedWindowEntry);
  const quota_v2::KernelDefaultWindowCutoverDecision blocked = co_await std::move(blockedAcquiring);
  ASSERT(std::holds_alternative<quota_v2::KernelDefaultWindowCutoverDecisionNotOk>(blocked));
  EXPECT_EQ(std::get<quota_v2::KernelDefaultWindowCutoverDecisionNotOk>(blocked).reason, std::string("active_reservations"));
  const rt::KvEntryMaybe<> blockedMarker = co_await kv->get(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY);
  EXPECT(blockedMarker.value.isNull());

  // An explicit policy on the scope supersedes the defaults, so its live
  // reservations do not block the cutover.
  kv->seed(quota_v2::kernelRepoPolicyKey("acme", "demo"), stored(windowRow(members({{"v", 2},
                                                                                     {"scope", "repo"},
                                                                                     {"usage_requests", 0},
                                                                                     {"reserved_requests", 0},
                                                                                     {"usage_limit_requests", 5},
                                                                                     {"window_ms", rt::makeNumber(WINDOW_MS)},
                                                                                     {"expires_at_ms", -1},
                                                                                     {"created_at_ms", 1},
                                                                                     {"updated_at_ms", 2}}))));
  auto policyAcquiring = reservations::acquireKernelDefaultWindowCutover(kv, expectedLimitEntry, expectedWindowEntry);
  const quota_v2::KernelDefaultWindowCutoverDecision withPolicy = co_await std::move(policyAcquiring);
  ASSERT(std::holds_alternative<quota_v2::KernelDefaultWindowCutoverDecisionOk>(withPolicy));

  // A held lease that is still valid is a concurrent change.
  auto askingAgain = reservations::acquireKernelDefaultWindowCutover(kv, expectedLimitEntry, expectedWindowEntry);
  const quota_v2::KernelDefaultWindowCutoverDecision stillAsking = co_await std::move(askingAgain);
  ASSERT(std::holds_alternative<quota_v2::KernelDefaultWindowCutoverDecisionNotOk>(stillAsking));
  EXPECT_EQ(std::get<quota_v2::KernelDefaultWindowCutoverDecisionNotOk>(stillAsking).reason, std::string("concurrent_change"));

  // A commit that fails must not silently drop the guard, and the retry must.
  const auto faulty = std::make_shared<FaultyKv>();
  faulty->throwOnCommit = true;
  const quota_v2::KernelDefaultWindowCutoverGuard guard = std::get<quota_v2::KernelDefaultWindowCutoverDecisionOk>(withPolicy).guard;
  auto failingRelease = reservations::releaseKernelDefaultWindowCutover(faulty, guard);
  co_await std::move(failingRelease);
  const rt::KvEntryMaybe<> keptMarker = co_await kv->get(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY);
  EXPECT(!keptMarker.value.isNull());
  auto release = reservations::releaseKernelDefaultWindowCutover(kv, guard);
  co_await std::move(release);
  const rt::KvEntryMaybe<> releasedMarker = co_await kv->get(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY);
  EXPECT(releasedMarker.value.isNull());
  kvfacade::setKvForTest(nullptr);
}

ASYNC_TEST("kernel default-window cutover refuses a stale marker plus malformed windows") {
  kvfacade::setKvForTest(kv);
  kv->clearData();
  const rt::KvEntryMaybe<> expectedLimitEntry = co_await kv->get(uos::defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY);
  const rt::KvEntryMaybe<> expectedWindowEntry = co_await kv->get(uos::defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY);
  const auto refusal = [](const quota_v2::KernelDefaultWindowCutoverDecision& decision) -> std::optional<std::string> {
    if (const auto* refused = std::get_if<quota_v2::KernelDefaultWindowCutoverDecisionNotOk>(&decision)) return refused->reason;
    return std::nullopt;
  };

  kv->seed(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, stored(members({{"v", 1}, {"id", "stale"}})));
  auto staleAcquiring = reservations::acquireKernelDefaultWindowCutover(kv, expectedLimitEntry, expectedWindowEntry);
  const quota_v2::KernelDefaultWindowCutoverDecision staleMarker = co_await std::move(staleAcquiring);
  EXPECT_EQ(refusal(staleMarker), std::optional<std::string>("unavailable"));

  kv->clearData();
  kv->seed(rt::KvKey(quota_v2::KERNEL_REPO_WINDOW_V2_PREFIX), stored(windowRow()));
  auto ownerlessAcquiring = reservations::acquireKernelDefaultWindowCutover(kv, expectedLimitEntry, expectedWindowEntry);
  const quota_v2::KernelDefaultWindowCutoverDecision ownerlessKey = co_await std::move(ownerlessAcquiring);
  EXPECT_EQ(refusal(ownerlessKey), std::optional<std::string>("unavailable"));
  const rt::KvEntryMaybe<> ownerlessMarker = co_await kv->get(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY);
  EXPECT(ownerlessMarker.value.isNull());

  kv->clearData();
  kv->seed(rt::KvKey{quota_v2::KERNEL_REPO_WINDOW_V2_PREFIX, "acme"}, stored(windowRow()));
  auto repolessAcquiring = reservations::acquireKernelDefaultWindowCutover(kv, expectedLimitEntry, expectedWindowEntry);
  const quota_v2::KernelDefaultWindowCutoverDecision repolessKey = co_await std::move(repolessAcquiring);
  EXPECT_EQ(refusal(repolessKey), std::optional<std::string>("unavailable"));

  // A window with no live reservations never blocks the cutover.
  kv->clearData();
  kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"),
           stored(windowRow(members({{"reserved_requests", 0}, {"usage_reset_at_ms", rt::makeNumber(rt::dateNow() - 1)}}))));
  auto idleAcquiring = reservations::acquireKernelDefaultWindowCutover(kv, expectedLimitEntry, expectedWindowEntry);
  const quota_v2::KernelDefaultWindowCutoverDecision idle = co_await std::move(idleAcquiring);
  ASSERT(std::holds_alternative<quota_v2::KernelDefaultWindowCutoverDecisionOk>(idle));
  auto idleRelease = reservations::releaseKernelDefaultWindowCutover(kv, std::get<quota_v2::KernelDefaultWindowCutoverDecisionOk>(idle).guard);
  co_await std::move(idleRelease);

  // Expired rows that keep reclaiming do not converge: the guard is refused.
  kv->clearData();
  kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"),
           stored(windowRow(members({{"reserved_requests", 3}, {"usage_reset_at_ms", rt::makeNumber(rt::dateNow() + WINDOW_MS)}}))));
  const std::vector<std::string> requestIds{"expired-1", "expired-2", "expired-3"};
  for (const std::string& requestId : requestIds) {
    kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", 5'000, requestId),
             stored(reservationRow(members({{"request_id", json::string(requestId)}, {"lease_expires_at_ms", rt::makeNumber(rt::dateNow() - 1)}}))));
  }
  auto racingAcquiring = reservations::acquireKernelDefaultWindowCutover(kv, expectedLimitEntry, expectedWindowEntry);
  const quota_v2::KernelDefaultWindowCutoverDecision racing = co_await std::move(racingAcquiring);
  EXPECT_EQ(refusal(racing), std::optional<std::string>("unavailable"));
  const rt::KvEntryMaybe<> racingMarker = co_await kv->get(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY);
  EXPECT(racingMarker.value.isNull());

  kv->clearData();
  kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"), stored(members({{"v", 2}, {"scope", "repo"}})));
  auto corruptWindowAcquiring = reservations::acquireKernelDefaultWindowCutover(kv, expectedLimitEntry, expectedWindowEntry);
  const quota_v2::KernelDefaultWindowCutoverDecision corruptWindow = co_await std::move(corruptWindowAcquiring);
  EXPECT_EQ(refusal(corruptWindow), std::optional<std::string>("unavailable"));
  const rt::KvEntryMaybe<> corruptWindowMarker = co_await kv->get(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY);
  EXPECT(corruptWindowMarker.value.isNull());

  kv->clearData();
  kv->seed(quota_v2::kernelOrgPolicyKey("acme"), stored(members({{"v", 2}, {"scope", "org"}, {"owner", "acme"}})));
  // `windowRow({ scope: "org", repo: undefined, reserved_requests: 1 })`: `repo` stays an own
  // member holding undefined.
  rt::KvValue orgWindow = stored(windowRow(members({{"scope", "org"}, {"reserved_requests", 1}})));
  orgWindow.asObject().set("repo", rt::KvValue());
  kv->seed(rt::KvKey{"uos_ai", "kernel_quota", "v2", "org_window", "acme"}, orgWindow);
  auto corruptPolicyAcquiring = reservations::acquireKernelDefaultWindowCutover(kv, expectedLimitEntry, expectedWindowEntry);
  const quota_v2::KernelDefaultWindowCutoverDecision corruptPolicy = co_await std::move(corruptPolicyAcquiring);
  EXPECT_EQ(refusal(corruptPolicy), std::optional<std::string>("unavailable"));
  const rt::KvEntryMaybe<> corruptPolicyMarker = co_await kv->get(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY);
  EXPECT(corruptPolicyMarker.value.isNull());
  kvfacade::setKvForTest(nullptr);
}
