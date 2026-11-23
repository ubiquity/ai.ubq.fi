// Links: tests/metered_quota_test.cpp src/metered_quota.cpp src/codex/quota.cpp src/kv.cpp src/storage/kv.cpp src/storage/key_compatibility.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/utils.cpp
// Port of tests/metered-quota.test.ts. The Codex quota header cases target src/codex/quota.ts;
// the others target src/metered-quota.ts. The TS `MemoryKv` fake is QuotaTestKv below.
#include "uos/codex/quota.hpp"
#include "uos/metered_quota.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace uos;
using codex::quota::buildCodexQuotaHeaders;
using codex::quota::ClientQuotaSnapshot;
using codex::quota::METERED_CODEX_LIMIT_NAME;
using metered_quota::fetchMeteredQuotaObservation;
using metered_quota::FetchMeteredQuotaObservationOptions;
using metered_quota::getCachedMeteredQuotaSnapshot;
using metered_quota::GetCachedMeteredQuotaSnapshotOptions;
using metered_quota::getMeteredQuotaSnapshot;
using metered_quota::GetMeteredQuotaSnapshotOptions;
using metered_quota::invalidateMeteredQuotaSnapshot;
using metered_quota::METERED_API_KEY_ENV;
using metered_quota::METERED_QUOTA_FRESH_MS;
using metered_quota::METERED_QUOTA_INVALIDATION_KEY;
using metered_quota::METERED_QUOTA_RETENTION_MS;
using metered_quota::METERED_QUOTA_STATE_KEY;
using metered_quota::MeteredAccountCredentials;
using metered_quota::MeteredQuotaObservation;
using metered_quota::MeteredQuotaSnapshot;
using metered_quota::MeteredQuotaState;
using metered_quota::MeteredRefillObservation;
using metered_quota::readMeteredAccountCredentials;
using metered_quota::updateMeteredQuotaState;

namespace {
  // `quotaSnapshot(usedPercent)` builds a whole MeteredQuotaSnapshot; buildCodexQuotaHeaders
  // reads only its `used_percent`.
  std::optional<ClientQuotaSnapshot> quotaSnapshot(double usedPercent) { return ClientQuotaSnapshot{.used_percent = usedPercent}; }

  // `JSON.stringify(key)`
  std::string keyString(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) parts.push_back(json::string(part.string()));
      else if (part.isNumber()) parts.push_back(rt::makeNumber(part.number()));
      else if (part.isBool()) parts.push_back(part.boolean());
      else parts.push_back(json::string(part.toString()));
    }
    return rt::jsonStringify(json::value(parts));
  }

  std::string versionstampOf(double version) { return rt::padStart(rt::numberToString(version), 20, "0"); }

  // The TS test's `MemoryKv`: rows keyed by JSON.stringify(key), each with the version it was
  // written at; every call resolves at once; commit checks every versionstamp, then applies.
  class QuotaTestKv final : public rt::Kv {
  public:
    using rt::Kv::get;
    using rt::Kv::getMany;

    struct Row {
      rt::KvValue value;
      double version;
    };

    void seed(const rt::KvKey& key, rt::KvValue value) { rows_[keyString(key)] = Row{std::move(value), nextVersion_++}; }

    // `this.#values.get(keyString(key))?.value ?? null`
    rt::KvValue value(const rt::KvKey& key) const {
      const auto found = rows_.find(keyString(key));
      if (found == rows_.end() || found->second.value.isNullish()) return rt::KvValue(nullptr);
      return found->second.value;
    }

    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions) override {
      rt::KvEntryMaybe<> entry;
      entry.key = key;
      const auto found = rows_.find(keyString(key));
      if (found != rows_.end()) {
        entry.value = found->second.value.isNullish() ? rt::KvValue(nullptr) : found->second.value;
        entry.versionstamp = versionstampOf(found->second.version);
      } else {
        entry.value = rt::KvValue(nullptr);
      }
      co_return entry;
    }

    std::unique_ptr<rt::AtomicOperation> atomic() override {
      return std::make_unique<rt::GenericAtomicOperation>(
        [this](std::vector<rt::AtomicCheck> checks, std::vector<rt::KvMutation> mutations) { return commit(std::move(checks), std::move(mutations)); });
    }

    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions) override { throw rt::TypeError("kv.getMany is not a function"); }
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions) override { throw rt::TypeError("kv.set is not a function"); }
    rt::Task<void> delete_(rt::KvKey) override { throw rt::TypeError("kv.delete is not a function"); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions) override { throw rt::TypeError("kv.list is not a function"); }
    void close() override {}

  private:
    rt::Task<rt::KvCommitResult> commit(std::vector<rt::AtomicCheck> checks, std::vector<rt::KvMutation> mutations) {
      for (const rt::AtomicCheck& check : checks) {
        const auto found = rows_.find(keyString(check.key));
        const std::optional<std::string> versionstamp = found == rows_.end() ? std::nullopt : std::optional<std::string>(versionstampOf(found->second.version));
        if (versionstamp != check.versionstamp) {
          rt::KvCommitResult failed;
          failed.ok = false;
          co_return failed;
        }
      }
      for (const rt::KvMutation& mutation : mutations) {
        if (mutation.type == rt::KvMutationType::delete_) rows_.erase(keyString(mutation.key));
        else seed(mutation.key, mutation.value);
      }
      rt::KvCommitResult committed;
      committed.ok = true;
      committed.versionstamp = versionstampOf(nextVersion_);
      co_return committed;
    }

    std::map<std::string, Row> rows_;
    double nextVersion_ = 1;
  };

  const MeteredAccountCredentials credentials{.apiKey = "metered-api-key"};

  MeteredQuotaObservation observation() {
    return MeteredQuotaObservation{
      .balance_quota = 50'000'000,
      .used_quota = 100'000,
      .quota_per_credit = 500'000,
      .observed_at_ms = 1'000'000,
      .latest_refill = MeteredRefillObservation{.id = "refill-1", .amount_credits = 100, .completed_at_ms = 900'000},
    };
  }

  MeteredQuotaState state() {
    return MeteredQuotaState{
      .current_balance_quota = 50'000'000,
      .post_refill_baseline_quota = 50'000'000,
      .last_observed_used_quota = 100'000,
      .quota_per_credit = 500'000,
      .observed_at_ms = 1'000'000,
      .cycle_started_at_ms = 1'000'000,
      .confidence = "provisional",
      .last_known_debits_quota = 0,
      .last_inferred_credit_quota = 0,
      .last_credit_at_ms = std::nullopt,
      .latest_refill_id = "refill-1",
      .latest_refill_amount_credits = 100,
      .latest_refill_completed_at_ms = 900'000,
    };
  }

  rt::KvValue stored(const MeteredQuotaState& value) { return rt::KvValue(json::value_from(value)); }

  rt::Response jsonResponse(const json::value& body) {
    rt::ResponseInit init;
    init.status = 200;
    init.headers = rt::Headers{{"Content-Type", "application/json; charset=utf-8"}};
    return rt::Response(rt::BodyInit(rt::jsonStringify(body)), std::move(init));
  }

  /** Request URL as text; `String(input)` would render a `Request` as "[object Object]". */
  std::string requestUrl(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->toString();
    return std::get<rt::Request>(input).url();
  }

  struct FetchCall {
    std::string url;
    rt::Headers headers;
  };

  rt::Fetch meteredFetcher(std::shared_ptr<std::vector<FetchCall>> calls) {
    return rt::Fetch([calls](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
      const std::string url = requestUrl(input);
      calls->push_back(FetchCall{.url = url, .headers = rt::Headers(init.headers.value_or(rt::Headers()))});
      if (url == "https://api.openlux.ai/api/usage/token/") {
        return jsonResponse(rt::jsonParse(R"({
          "success": true,
          "data": {
            "expires_at": 0,
            "model_limits": {},
            "model_limits_enabled": false,
            "name": "business-key",
            "object": "token_usage",
            "total_available": -53413,
            "total_granted": -545,
            "total_used": 52868,
            "unlimited_quota": true
          }
        })"));
      }
      throw rt::JsError("Unexpected URL: " + url);
    });
  }

  GetMeteredQuotaSnapshotOptions snapshotOptions(const std::shared_ptr<QuotaTestKv>& kv, double now) {
    GetMeteredQuotaSnapshotOptions options;
    options.kv = std::shared_ptr<rt::Kv>(kv);
    options.now = [now] { return now; };
    return options;
  }
}

TEST("Metered account credentials require a non-whitespace API key") {
  const rt::env::ScopedOverride restore({{std::string(METERED_API_KEY_ENV), std::nullopt}});
  rt::env::set(METERED_API_KEY_ENV, credentials.apiKey);
  EXPECT(readMeteredAccountCredentials() == std::optional<MeteredAccountCredentials>(credentials));

  rt::env::set(METERED_API_KEY_ENV, "");
  EXPECT(!readMeteredAccountCredentials());

  rt::env::set(METERED_API_KEY_ENV, "contains whitespace");
  EXPECT(!readMeteredAccountCredentials());
}

ASYNC_TEST("Metered account observation reads wallet balance and latest successful top-up") {
  const auto calls = std::make_shared<std::vector<FetchCall>>();
  FetchMeteredQuotaObservationOptions options;
  options.fetcher = meteredFetcher(calls);
  options.now = [] { return 2'000'000.0; };
  auto task = fetchMeteredQuotaObservation(credentials, options);
  const MeteredQuotaObservation result = co_await std::move(task);

  const MeteredQuotaObservation expected{
    .balance_quota = std::nullopt,
    .used_quota = std::nullopt,
    .quota_per_credit = std::nullopt,
    .observed_at_ms = 2'000'000,
    .latest_refill = std::nullopt,
    .unlimited_quota = true,
    .total_available = -53'413,
    .total_granted = -545,
    .total_used = 52'868,
  };
  EXPECT(result == expected);
  ASSERT_EQ(calls->size(), std::size_t{1});
  EXPECT_EQ(calls->at(0).url, std::string("https://api.openlux.ai/api/usage/token/"));
  EXPECT_EQ(calls->at(0).headers.get("Authorization"), std::optional<std::string>("Bearer metered-api-key"));
  EXPECT_EQ(calls->at(0).headers.has("New-API-User"), false);
  EXPECT_EQ(calls->at(0).headers.has("Cookie"), false);
}

ASYNC_TEST("Metered token usage preserves provider totals, including negative available values") {
  FetchMeteredQuotaObservationOptions options;
  options.fetcher = meteredFetcher(std::make_shared<std::vector<FetchCall>>());
  options.now = [] { return 2'000'000.0; };
  auto task = fetchMeteredQuotaObservation(credentials, options);
  const MeteredQuotaObservation result = co_await std::move(task);
  EXPECT_EQ(result.unlimited_quota, std::optional<bool>(true));
  EXPECT_EQ(result.total_available, std::optional<double>(-53'413));
  EXPECT_EQ(result.total_granted, std::optional<double>(-545));
  EXPECT_EQ(result.total_used, std::optional<double>(52'868));
}

TEST("Metered quota seeds a provisional baseline from the first observation") {
  const MeteredQuotaState next = updateMeteredQuotaState(std::nullopt, observation());
  EXPECT_EQ(next.current_balance_quota, 50'000'000.0);
  EXPECT_EQ(next.post_refill_baseline_quota, 50'000'000.0);
  EXPECT_EQ(next.confidence, std::string("provisional"));
  EXPECT_EQ(next.last_inferred_credit_quota, 0.0);
}

TEST("Metered quota seeds a partially spent first cycle from the latest refill amount") {
  MeteredQuotaObservation input = observation();
  input.balance_quota = 40'000'000;
  const MeteredQuotaState next = updateMeteredQuotaState(std::nullopt, input);
  EXPECT_EQ(next.current_balance_quota, 40'000'000.0);
  EXPECT_EQ(next.post_refill_baseline_quota, 50'000'000.0);
  EXPECT_EQ(next.confidence, std::string("provisional"));
}

TEST("Metered quota keeps the refill baseline across known and external debits") {
  const MeteredQuotaState previous = state();
  MeteredQuotaObservation input = observation();
  input.balance_quota = 48'500'000;
  input.used_quota = 1'600'000;
  input.observed_at_ms = 2'000'000;
  const MeteredQuotaState next = updateMeteredQuotaState(previous, input);
  EXPECT_EQ(next.post_refill_baseline_quota, 50'000'000.0);
  EXPECT_EQ(next.last_known_debits_quota, 1'500'000.0);
  EXPECT_EQ(next.last_inferred_credit_quota, 0.0);
  EXPECT_EQ(next.confidence, std::string("provisional"));
}

TEST("Metered quota detects a refill despite intervening debits and starts a new baseline") {
  MeteredQuotaState previous = state();
  previous.current_balance_quota = 20'000'000;
  previous.post_refill_baseline_quota = 50'000'000;
  previous.last_observed_used_quota = 30'000'000;
  MeteredQuotaObservation input = observation();
  input.balance_quota = 44'000'000;
  input.used_quota = 31'000'000;
  input.observed_at_ms = 2'000'000;
  input.latest_refill = MeteredRefillObservation{.id = "refill-2", .amount_credits = 50, .completed_at_ms = 1'900'000};
  const MeteredQuotaState next = updateMeteredQuotaState(previous, input);
  EXPECT_EQ(next.last_known_debits_quota, 1'000'000.0);
  EXPECT_EQ(next.last_inferred_credit_quota, 25'000'000.0);
  EXPECT_EQ(next.post_refill_baseline_quota, 45'000'000.0);
  EXPECT_EQ(next.cycle_started_at_ms, 1'900'000.0);
  EXPECT_EQ(next.confidence, std::string("refill_observed"));
  EXPECT_EQ(next.latest_refill_amount_credits, std::optional<double>(50));
}

TEST("Metered quota restores post-refill debits when the wallet carries a prior balance") {
  MeteredQuotaState previous = state();
  previous.current_balance_quota = 10'000'000;
  previous.post_refill_baseline_quota = 50'000'000;
  previous.last_observed_used_quota = 30'000'000;
  MeteredQuotaObservation input = observation();
  input.balance_quota = 30'000'000;
  input.used_quota = 35'000'000;
  input.observed_at_ms = 2'000'000;
  input.latest_refill = MeteredRefillObservation{.id = "refill-2", .amount_credits = 50, .completed_at_ms = 1'900'000};
  const MeteredQuotaState next = updateMeteredQuotaState(previous, input);
  EXPECT_EQ(next.last_known_debits_quota, 5'000'000.0);
  EXPECT_EQ(next.last_inferred_credit_quota, 25'000'000.0);
  EXPECT_EQ(next.current_balance_quota, 30'000'000.0);
  EXPECT_EQ(next.post_refill_baseline_quota, 35'000'000.0);
}

TEST("Metered quota keeps reconstructed refill capacity within safe integer bounds") {
  MeteredQuotaState previous = state();
  previous.current_balance_quota = rt::Number::MAX_SAFE_INTEGER;
  previous.last_observed_used_quota = 100'000;
  MeteredQuotaObservation input = observation();
  input.balance_quota = rt::Number::MAX_SAFE_INTEGER;
  input.used_quota = 100'001;
  input.observed_at_ms = 2'000'000;
  input.latest_refill = MeteredRefillObservation{.id = "refill-2", .amount_credits = 1, .completed_at_ms = 1'900'000};
  const MeteredQuotaState next = updateMeteredQuotaState(previous, input);
  EXPECT_EQ(next.post_refill_baseline_quota, rt::Number::MAX_SAFE_INTEGER);
  EXPECT_EQ(rt::Number::isSafeInteger(next.post_refill_baseline_quota), true);
}

TEST("Metered quota never starts a refill cycle below the observed top-up amount") {
  MeteredQuotaState previous = state();
  previous.current_balance_quota = 20'000'000;
  previous.post_refill_baseline_quota = 50'000'000;
  previous.last_observed_used_quota = 30'000'000;
  MeteredQuotaObservation input = observation();
  input.balance_quota = 20'000'000;
  input.used_quota = 30'000'000;
  input.observed_at_ms = 2'000'000;
  input.latest_refill = MeteredRefillObservation{.id = "refill-2", .amount_credits = 50, .completed_at_ms = 1'900'000};
  const MeteredQuotaState next = updateMeteredQuotaState(previous, input);
  EXPECT_EQ(next.last_inferred_credit_quota, 0.0);
  EXPECT_EQ(next.post_refill_baseline_quota, 25'000'000.0);
  EXPECT_EQ(next.confidence, std::string("refill_observed"));
}

TEST("Metered quota treats an unlisted positive adjustment as an inferred credit") {
  MeteredQuotaState previous = state();
  previous.current_balance_quota = 10'000'000;
  MeteredQuotaObservation input = observation();
  input.balance_quota = 11'000'000;
  input.used_quota = previous.last_observed_used_quota;
  input.observed_at_ms = 2'000'000;
  const MeteredQuotaState next = updateMeteredQuotaState(previous, input);
  EXPECT_EQ(next.last_inferred_credit_quota, 1'000'000.0);
  EXPECT_EQ(next.post_refill_baseline_quota, 11'000'000.0);
  EXPECT_EQ(next.confidence, std::string("inferred_adjustment"));
}

TEST("Metered quota ignores a reset used counter unless the wallet itself rises") {
  MeteredQuotaState previous = state();
  previous.current_balance_quota = 10'000'000;
  previous.last_observed_used_quota = 5'000'000;
  MeteredQuotaObservation debitInput = observation();
  debitInput.balance_quota = 9'000'000;
  debitInput.used_quota = 100;
  debitInput.observed_at_ms = 2'000'000;
  const MeteredQuotaState debit = updateMeteredQuotaState(previous, debitInput);
  EXPECT_EQ(debit.last_inferred_credit_quota, 0.0);
  EXPECT_EQ(debit.post_refill_baseline_quota, 50'000'000.0);

  MeteredQuotaObservation creditInput = observation();
  creditInput.balance_quota = 12'000'000;
  creditInput.used_quota = 100;
  creditInput.observed_at_ms = 2'000'000;
  const MeteredQuotaState credit = updateMeteredQuotaState(previous, creditInput);
  EXPECT_EQ(credit.last_inferred_credit_quota, 2'000'000.0);
}

ASYNC_TEST("Metered quota cache serves fresh state without an upstream request") {
  const auto kv = std::make_shared<QuotaTestKv>();
  const double now = 10'000'000;
  MeteredQuotaState seeded = state();
  seeded.observed_at_ms = now - METERED_QUOTA_FRESH_MS + 1;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(seeded));
  int fetches = 0;
  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv, now);
  options.fetcher = rt::Fetch([&fetches](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    fetches += 1;
    throw rt::JsError("should not fetch");
  });
  auto task = getMeteredQuotaSnapshot(credentials, options);
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(task);
  EXPECT_EQ(fetches, 0);
  ASSERT(snapshot);
  EXPECT_EQ(snapshot->cache_state, std::string("fresh"));
  EXPECT_EQ(snapshot->remaining_percent, std::optional<double>(100));
}

ASYNC_TEST("Metered quota forced refresh bypasses a fresh cached state") {
  const auto kv = std::make_shared<QuotaTestKv>();
  const double now = 11'000'000;
  MeteredQuotaState seeded = state();
  seeded.observed_at_ms = now - 1'000;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(seeded));
  const auto calls = std::make_shared<std::vector<FetchCall>>();
  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv, now);
  options.fetcher = meteredFetcher(calls);
  options.forceRefresh = true;
  options.createLeaseOwner = [] { return std::string("forced-refresh-owner"); };
  auto task = getMeteredQuotaSnapshot(credentials, options);
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(task);
  ASSERT(snapshot);
  EXPECT_EQ(snapshot->cache_state, std::string("refreshed"));
  EXPECT_EQ(calls->size(), std::size_t{1});
}

ASYNC_TEST("Metered quota cache marks an invalidated observation stale") {
  const auto kv = std::make_shared<QuotaTestKv>();
  const double now = 15'000'000;
  MeteredQuotaState seeded = state();
  seeded.observed_at_ms = now;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(seeded));
  json::object invalidation;
  invalidation["invalidated_at_ms"] = rt::makeNumber(now);
  kv->seed(METERED_QUOTA_INVALIDATION_KEY, rt::KvValue(invalidation));

  GetCachedMeteredQuotaSnapshotOptions options;
  options.kv = std::shared_ptr<rt::Kv>(kv);
  options.now = [now] { return now; };
  auto task = getCachedMeteredQuotaSnapshot(options);
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(task);

  ASSERT(snapshot);
  EXPECT_EQ(snapshot->cache_state, std::string("stale"));
  EXPECT_EQ(snapshot->remaining_percent, std::optional<double>(100));
}

ASYNC_TEST("Metered quota invalidation forces a fresh account observation") {
  const auto kv = std::make_shared<QuotaTestKv>();
  const double now = 16'000'000;
  MeteredQuotaState seeded = state();
  seeded.observed_at_ms = now;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(seeded));
  GetCachedMeteredQuotaSnapshotOptions invalidateOptions;
  invalidateOptions.kv = std::shared_ptr<rt::Kv>(kv);
  invalidateOptions.now = [now] { return now; };
  auto invalidating = invalidateMeteredQuotaSnapshot(invalidateOptions);
  co_await std::move(invalidating);
  const auto calls = std::make_shared<std::vector<FetchCall>>();

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv, now);
  options.fetcher = meteredFetcher(calls);
  options.createLeaseOwner = [] { return std::string("invalidation-refresh-owner"); };
  auto task = getMeteredQuotaSnapshot(credentials, options);
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(task);

  ASSERT(snapshot);
  EXPECT_EQ(snapshot->cache_state, std::string("refreshed"));
  EXPECT_EQ(calls->size(), std::size_t{1});
  EXPECT(kv->value(METERED_QUOTA_INVALIDATION_KEY).isNull());
}

ASYNC_TEST("Metered quota refresh stores a new observation and computes its percentage") {
  const auto kv = std::make_shared<QuotaTestKv>();
  const double now = 20'000'000;
  MeteredQuotaState seeded = state();
  seeded.current_balance_quota = 50'000'000;
  seeded.post_refill_baseline_quota = 50'000'000;
  seeded.last_observed_used_quota = 100'000;
  seeded.observed_at_ms = now - METERED_QUOTA_FRESH_MS;
  seeded.latest_refill_id = "10";
  kv->seed(METERED_QUOTA_STATE_KEY, stored(seeded));
  const auto calls = std::make_shared<std::vector<FetchCall>>();
  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv, now);
  options.fetcher = meteredFetcher(calls);
  options.createLeaseOwner = [] { return std::string("refresh-owner"); };
  auto task = getMeteredQuotaSnapshot(credentials, options);
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(task);
  ASSERT(snapshot);
  EXPECT_EQ(snapshot->cache_state, std::string("refreshed"));
  EXPECT_EQ(snapshot->unlimited_quota, true);
  EXPECT(snapshot->state.total_available == rt::Nullable<double>(-53'413));
  EXPECT_EQ(snapshot->total_used, std::optional<double>(52'868));
  EXPECT_EQ(snapshot->balance_credits, std::optional<double>());
  EXPECT_EQ(snapshot->remaining_percent, std::optional<double>());
  EXPECT_EQ(calls->size(), std::size_t{1});
}

ASYNC_TEST("Metered quota serves stale state on refresh failure and drops expired state") {
  const double now = 30'000'000;
  const auto staleKv = std::make_shared<QuotaTestKv>();
  MeteredQuotaState staleState = state();
  staleState.observed_at_ms = now - METERED_QUOTA_FRESH_MS;
  staleKv->seed(METERED_QUOTA_STATE_KEY, stored(staleState));
  const rt::Fetch outage([](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
    throw rt::JsError("temporary outage");
    co_return rt::Response();
  });
  GetMeteredQuotaSnapshotOptions staleOptions = snapshotOptions(staleKv, now);
  staleOptions.fetcher = outage;
  auto staleTask = getMeteredQuotaSnapshot(credentials, staleOptions);
  const std::optional<MeteredQuotaSnapshot> stale = co_await std::move(staleTask);
  ASSERT(stale);
  EXPECT_EQ(stale->cache_state, std::string("stale"));

  const auto expiredKv = std::make_shared<QuotaTestKv>();
  MeteredQuotaState expiredState = state();
  expiredState.observed_at_ms = now - METERED_QUOTA_RETENTION_MS;
  expiredKv->seed(METERED_QUOTA_STATE_KEY, stored(expiredState));
  GetMeteredQuotaSnapshotOptions expiredOptions = snapshotOptions(expiredKv, now);
  expiredOptions.fetcher = outage;
  auto expiredTask = getMeteredQuotaSnapshot(credentials, expiredOptions);
  const std::optional<MeteredQuotaSnapshot> expired = co_await std::move(expiredTask);
  EXPECT(!expired);
}

ASYNC_TEST("Metered quota refresh lease permits only one upstream refresh") {
  const auto kv = std::make_shared<QuotaTestKv>();
  const double now = 40'000'000;
  MeteredQuotaState seeded = state();
  seeded.observed_at_ms = now - METERED_QUOTA_FRESH_MS;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(seeded));
  const auto calls = std::make_shared<std::vector<FetchCall>>();
  const rt::Resolvers<void> gate = rt::withResolvers<void>();
  const rt::Fetch baseFetcher = meteredFetcher(calls);
  const rt::Promise<void> gatePromise = gate.promise;
  const rt::Fetch fetcher([gatePromise, baseFetcher](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
    co_await gatePromise.wait();
    auto forwarded = baseFetcher(std::move(input), std::move(init));
    rt::Response response = co_await std::move(forwarded);
    co_return response;
  });
  const auto owner = std::make_shared<int>(0);
  std::vector<rt::Promise<std::optional<MeteredQuotaSnapshot>>> requests;
  for (int index = 0; index < 12; index += 1) {
    GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv, now);
    options.fetcher = fetcher;
    options.createLeaseOwner = [owner] { return "owner-" + std::to_string(++*owner); };
    requests.push_back(rt::Promise<std::optional<MeteredQuotaSnapshot>>(getMeteredQuotaSnapshot(credentials, options)));
  }
  co_await rt::microtask();
  co_await rt::microtask();
  gate.resolve();
  auto settled = rt::all(std::move(requests));
  const std::vector<std::optional<MeteredQuotaSnapshot>> snapshots = co_await settled.wait();
  EXPECT_EQ(calls->size(), std::size_t{1});
  int refreshed = 0;
  int staleCount = 0;
  for (const auto& snapshot : snapshots) {
    if (snapshot && snapshot->cache_state == "refreshed") refreshed += 1;
    if (snapshot && snapshot->cache_state == "stale") staleCount += 1;
  }
  EXPECT_EQ(refreshed, 1);
  EXPECT_EQ(staleCount, 11);
}

ASYNC_TEST("Metered quota invalidation rejects an in-flight pre-debit refresh") {
  const auto kv = std::make_shared<QuotaTestKv>();
  const double now = 50'000'000;
  MeteredQuotaState previous = state();
  previous.observed_at_ms = now - METERED_QUOTA_FRESH_MS;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(previous));
  const rt::Resolvers<void> fetchStarted = rt::withResolvers<void>();
  const rt::Resolvers<void> gate = rt::withResolvers<void>();
  const rt::Fetch baseFetcher = meteredFetcher(std::make_shared<std::vector<FetchCall>>());
  const rt::Resolve<void> markFetchStarted = fetchStarted.resolve;
  const rt::Promise<void> gatePromise = gate.promise;
  const rt::Fetch fetcher([markFetchStarted, gatePromise, baseFetcher](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
    markFetchStarted();
    co_await gatePromise.wait();
    auto forwarded = baseFetcher(std::move(input), std::move(init));
    rt::Response response = co_await std::move(forwarded);
    co_return response;
  });
  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv, now);
  options.fetcher = fetcher;
  options.createLeaseOwner = [] { return std::string("pre-debit-refresh-owner"); };
  const rt::Promise<std::optional<MeteredQuotaSnapshot>> pending(getMeteredQuotaSnapshot(credentials, options));
  co_await fetchStarted.promise.wait();
  GetCachedMeteredQuotaSnapshotOptions invalidateOptions;
  invalidateOptions.kv = std::shared_ptr<rt::Kv>(kv);
  invalidateOptions.now = [now] { return now + 1; };
  auto invalidating = invalidateMeteredQuotaSnapshot(invalidateOptions);
  co_await std::move(invalidating);
  gate.resolve();

  const std::optional<MeteredQuotaSnapshot> snapshot = co_await pending.wait();
  ASSERT(snapshot);
  EXPECT_EQ(snapshot->cache_state, std::string("stale"));
  EXPECT(kv->value(METERED_QUOTA_STATE_KEY) == stored(previous));
  json::object invalidation;
  invalidation["invalidated_at_ms"] = rt::makeNumber(now + 1);
  EXPECT(kv->value(METERED_QUOTA_INVALIDATION_KEY) == rt::KvValue(invalidation));
}

TEST("Codex quota headers replace every parseable upstream family with canonical Metered") {
  const rt::Headers headers = buildCodexQuotaHeaders(
    rt::Headers{
      {"x-codex-primary-used-percent", "96.5"},
      {"x-codex-primary-window-minutes", "10080"},
      {"x-codex-primary-reset-at", "1785000000"},
      {"x-codex-primary-reset-after-seconds", "86400"},
      {"x-codex-secondary-used-percent", "20"},
      {"x-codex-secondary-window-minutes", "300"},
      {"x-codex-secondary-reset-after-seconds", "120"},
      {"x-codex-credits-has-credits", "true"},
      {"x-codex-credits-unlimited", "false"},
      {"x-codex-credits-balance", "12"},
      {"x-codex-rate-limit-reached-type", "workspace_owner_usage_limit_reached"},
      {"x-codex-spark-limit-name", "GPT-5.3-Codex-Spark Weekly limit"},
      {"x-codex-spark-primary-used-percent", "0"},
      {"x-codex-spark-primary-window-minutes", "10080"},
      {"x-codex-spark-primary-reset-at", "1785100000"},
      {"x-openai-subscription-limit-name", "OpenAI subscription"},
      {"x-openai-subscription-primary-used-percent", "96.5"},
      {"x-metered-limit-name", "stale Metered family"},
      {"x-metered-primary-used-percent", "40"},
      {"x-codex-model", "gpt-5.6-sol"},
      {"x-codex-plan-type", "pro"},
      {"x-codex-safety-identifier", "safety-id"},
      {"x-uos-router-revision", "routing-revision"},
    },
    quotaSnapshot(95));

  EXPECT_EQ(headers.get("x-codex-limit-name"), std::optional<std::string>(METERED_CODEX_LIMIT_NAME));
  EXPECT_EQ(headers.get("x-codex-primary-used-percent"), std::optional<std::string>("95"));
  EXPECT_EQ(headers.has("x-codex-primary-window-minutes"), false);
  EXPECT_EQ(headers.has("x-codex-primary-reset-at"), false);
  EXPECT_EQ(headers.has("x-codex-primary-reset-after-seconds"), false);
  EXPECT_EQ(headers.has("x-codex-secondary-used-percent"), false);
  EXPECT_EQ(headers.has("x-codex-secondary-reset-after-seconds"), false);
  EXPECT_EQ(headers.has("x-codex-spark-limit-name"), false);
  EXPECT_EQ(headers.has("x-codex-spark-primary-used-percent"), false);
  EXPECT_EQ(headers.has("x-codex-spark-primary-window-minutes"), false);
  EXPECT_EQ(headers.has("x-codex-spark-primary-reset-at"), false);
  EXPECT_EQ(headers.has("x-openai-subscription-limit-name"), false);
  EXPECT_EQ(headers.has("x-openai-subscription-primary-used-percent"), false);
  EXPECT_EQ(headers.has("x-metered-limit-name"), false);
  EXPECT_EQ(headers.has("x-metered-primary-used-percent"), false);
  EXPECT_EQ(headers.has("x-codex-credits-has-credits"), false);
  EXPECT_EQ(headers.has("x-codex-rate-limit-reached-type"), false);
  EXPECT_EQ(headers.get("x-codex-model"), std::optional<std::string>("gpt-5.6-sol"));
  EXPECT_EQ(headers.get("x-codex-plan-type"), std::optional<std::string>("pro"));
  EXPECT_EQ(headers.get("x-codex-safety-identifier"), std::optional<std::string>("safety-id"));
  EXPECT_EQ(headers.get("x-uos-router-revision"), std::optional<std::string>("routing-revision"));
}

TEST("Codex quota headers publish a healthy Metered balance canonically") {
  const rt::Headers headers = buildCodexQuotaHeaders(rt::Headers{}, quotaSnapshot(50));
  EXPECT_EQ(headers.get("x-codex-limit-name"), std::optional<std::string>(METERED_CODEX_LIMIT_NAME));
  EXPECT_EQ(headers.get("x-codex-primary-used-percent"), std::optional<std::string>("50"));
  EXPECT_EQ(headers.has("x-codex-primary-window-minutes"), false);
  EXPECT_EQ(headers.has("x-codex-primary-reset-at"), false);
}

TEST("Codex quota headers emit no percentage and strip every family without Metered state") {
  const rt::Headers headers = buildCodexQuotaHeaders(
    rt::Headers{
      {"x-codex-primary-used-percent", "42"},
      {"x-codex-primary-window-minutes", "300"},
      {"x-codex-spark-primary-used-percent", "25"},
      {"x-openai-subscription-primary-used-percent", "42"},
      {"x-metered-primary-used-percent", "50"},
      {"x-codex-model", "gpt-5.6-sol"},
    },
    std::nullopt);
  EXPECT_EQ(headers.has("x-codex-primary-used-percent"), false);
  EXPECT_EQ(headers.has("x-codex-primary-window-minutes"), false);
  EXPECT_EQ(headers.has("x-codex-spark-primary-used-percent"), false);
  EXPECT_EQ(headers.has("x-openai-subscription-primary-used-percent"), false);
  EXPECT_EQ(headers.has("x-metered-primary-used-percent"), false);
  EXPECT_EQ(headers.get("x-codex-model"), std::optional<std::string>("gpt-5.6-sol"));
}
