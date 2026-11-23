// Port of tests/metered-quota-coverage.test.ts (all 64 cases).
// Links: tests/metered_quota_coverage_test.cpp tests/helpers/counting_kv.cpp src/api_keys.cpp src/authority/native_paid.cpp
//        src/authority/native_paid_contracts.cpp src/bounded_response_body.cpp src/inference_deadline.cpp src/kv.cpp src/metered_quota.cpp
//        src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp
//        src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/rollups.cpp src/provider/anthropic_replay.cpp
//        src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/metered.cpp
//        src/provider/surplus_claude_request.cpp src/responses_stream.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
//        src/types.cpp src/utils.cpp
// `MeteredKv` is the TS file's in-memory Deno.Kv substitute on rt::Kv; `encodeKey` is counting-kv's
// `JSON.stringify(key)`.
#include "uos/metered_quota.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/authority/native_paid.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/paid_fallback/rollups.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/types.hpp"

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::metered_quota;
using uos::tests::helpers::counting_kv::encodeKey;
namespace ledger_backfill = uos::paid_fallback::ledger_backfill;
namespace ledger_state = uos::paid_fallback::ledger_state;
namespace rollups = uos::paid_fallback::rollups;
using uos::types::PaidFallbackRequestV3;

namespace {
  // "value" | "null" | "throw"
  using ReadBehavior = std::string;

  struct StoredEntry {
    rt::KvKey key;
    rt::KvValue value;
    std::string versionstamp;
  };

  class MeteredKv;

  class MeteredListIterator final : public rt::KvListIterator {
  public:
    explicit MeteredListIterator(std::vector<rt::KvEntry<>> entries_) : entries(std::move(entries_)) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      co_await rt::microtask();
      if (index >= entries.size()) co_return std::nullopt;
      co_return entries[index++];
    }
    std::string cursor() const override { return ""; }
    rt::Task<void> return_() override {
      index = entries.size();
      co_return;
    }

  private:
    std::vector<rt::KvEntry<>> entries;
    std::size_t index = 0;
  };

  class MeteredAtomicOperation final : public rt::AtomicOperation {
  public:
    explicit MeteredAtomicOperation(MeteredKv& kv_) : kv(kv_) {}
    using rt::AtomicOperation::check;
    using rt::AtomicOperation::max;
    using rt::AtomicOperation::min;
    using rt::AtomicOperation::sum;
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
      checks.insert(checks.end(), items.begin(), items.end());
      return *this;
    }
    rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("operation.mutate is not a function"); }
    rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.sum is not a function"); }
    rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.min is not a function"); }
    rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.max is not a function"); }
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      mutations.push_back(Mutation{.kind = "set", .key = std::move(key), .value = std::move(value), .expireIn = options.expireIn});
      return *this;
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override {
      mutations.push_back(Mutation{.kind = "delete", .key = std::move(key), .value = {}, .expireIn = std::nullopt});
      return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override;

  private:
    struct Mutation {
      std::string kind;
      rt::KvKey key;
      rt::KvValue value;
      std::optional<double> expireIn;
    };
    MeteredKv& kv;
    std::vector<rt::AtomicCheck> checks;
    std::vector<Mutation> mutations;
  };

  /**
   * Small in-memory Deno.Kv substitute. `Deno.openKv` needs `--unstable-kv`, which
   * the repository test command does not pass, so every metered-quota KV path is
   * driven through this stub. `planReads` scripts the first N reads of one key
   * (`value`, `null` or `throw`) and `conflictOnSet` forces a CAS conflict.
   */
  class MeteredKv final : public rt::Kv {
  public:
    rt::OrderedMap<std::string, StoredEntry> stored;
    rt::OrderedMap<std::string, std::optional<double>> ttlOf;
    std::vector<rt::KvKey> readLog;

    void seed(const rt::KvKey& key, rt::KvValue value, std::optional<double> expireIn = std::nullopt) {
      stored.set(encodeKey(key), StoredEntry{key, std::move(value), stamp()});
      ttlOf.set(encodeKey(key), expireIn);
    }

    // `stored.get(encodeKey(key))?.value ?? null`
    rt::KvValue value(const rt::KvKey& key) const {
      const StoredEntry* entry = stored.get(encodeKey(key));
      return entry && !entry->value.isNullish() ? entry->value : rt::KvValue(nullptr);
    }

    bool has(const rt::KvKey& key) const { return stored.has(encodeKey(key)); }

    void planReads(const rt::KvKey& key, std::vector<ReadBehavior> behaviors) { readPlan[encodeKey(key)] = std::deque<ReadBehavior>(behaviors.begin(), behaviors.end()); }

    void conflictOnSet(const rt::KvKey& key) { conflicts.insert(encodeKey(key)); }

    bool conflicted(const rt::KvKey& key) const { return conflicts.count(encodeKey(key)) > 0; }

    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions = {}) override {
      readLog.push_back(key);
      std::optional<ReadBehavior> behavior;
      if (auto plan = readPlan.find(encodeKey(key)); plan != readPlan.end() && !plan->second.empty()) {
        behavior = plan->second.front();
        plan->second.pop_front();
      }
      co_await rt::microtask();
      if (behavior == "throw") throw rt::JsError("kv read failed for " + encodeKey(key));
      if (behavior == "null") co_return rt::KvEntryMaybe<>{key, rt::KvValue(nullptr), std::nullopt};
      const StoredEntry* entry = stored.get(encodeKey(key));
      co_return rt::KvEntryMaybe<>{key, entry ? entry->value : rt::KvValue(nullptr), entry ? std::optional<std::string>(entry->versionstamp) : std::nullopt};
    }

    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions = {}) override {
      throw rt::TypeError("kv.getMany is not a function");
      co_return std::vector<rt::KvEntryMaybe<>>{};
    }

    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      const std::optional<rt::KvKey> start = selector.start;
      const std::optional<rt::KvKey> end = selector.end;
      const std::optional<rt::KvKey> prefix = selector.prefix;
      const auto matches = [&](const rt::KvKey& key) {
        if (prefix) {
          for (std::size_t index = 0; index < prefix->size(); ++index) {
            if (index >= key.size() || !((*prefix)[index] == key[index])) return false;
          }
          return true;
        }
        if (!start || !end) return false;
        if (start->size() != end->size() || key.size() != start->size() || key.empty()) return false;
        for (std::size_t index = 0; index + 1 < start->size(); ++index) {
          if (!((*start)[index] == key[index])) return false;
        }
        const double value = numberOf(key.back());
        return value >= numberOf(start->back()) && value < numberOf(end->back());
      };
      std::vector<const StoredEntry*> selected;
      for (const auto& [encoded, entry] : stored) {
        if (matches(entry.key)) selected.push_back(&entry);
      }
      rt::sort(selected, [](const StoredEntry* left, const StoredEntry* right) -> double {
        const rt::KvKeyPart& leftPart = left->key.back();
        const rt::KvKeyPart& rightPart = right->key.back();
        if (leftPart.isNumber() && rightPart.isNumber()) return leftPart.number() - rightPart.number();
        return static_cast<double>(rt::localeCompare(leftPart.toString(), rightPart.toString()));
      });
      const double limit = options.limit.value_or(1'000);
      std::vector<rt::KvEntry<>> entries;
      for (const StoredEntry* entry : selected) {
        if (static_cast<double>(entries.size()) >= limit) break;
        entries.push_back(rt::KvEntry<>{entry->key, entry->value, entry->versionstamp});
      }
      return std::make_unique<MeteredListIterator>(std::move(entries));
    }

    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      rt::KvCommitResult result;
      if (conflicted(key)) {
        result.ok = false;
      } else {
        const std::string versionstamp = stamp();
        stored.set(encodeKey(key), StoredEntry{key, std::move(value), versionstamp});
        ttlOf.set(encodeKey(key), options.expireIn);
        result.ok = true;
        result.versionstamp = versionstamp;
      }
      co_await rt::microtask();
      co_return result;
    }

    rt::Task<void> delete_(rt::KvKey key) override {
      stored.delete_(encodeKey(key));
      co_await rt::microtask();
    }

    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<MeteredAtomicOperation>(*this); }

    void close() override {}

    std::string stamp() {
      nextVersion += 1;
      return rt::padStart(rt::numberToString(nextVersion), 20, "0");
    }

  private:
    // `Number(part)` of a key part parsed back from JSON.
    static double numberOf(const rt::KvKeyPart& part) { return part.isNumber() ? part.number() : rt::stringToNumber(part.toString()); }

    std::map<std::string, std::deque<ReadBehavior>> readPlan;
    std::set<std::string> conflicts;
    double nextVersion = 1;
  };

  rt::Task<rt::KvCommitResult> MeteredAtomicOperation::commit() {
    rt::KvCommitResult result;
    result.ok = true;
    for (const rt::AtomicCheck& check : checks) {
      const StoredEntry* current = kv.stored.get(encodeKey(check.key));
      const std::optional<std::string> versionstamp = current ? std::optional<std::string>(current->versionstamp) : std::nullopt;
      if (versionstamp != check.versionstamp) result.ok = false;
    }
    for (const Mutation& mutation : mutations) {
      if (mutation.kind == "set" && kv.conflicted(mutation.key)) result.ok = false;
    }
    if (result.ok) {
      const std::string versionstamp = kv.stamp();
      for (const Mutation& mutation : mutations) {
        if (mutation.kind == "delete") {
          kv.stored.delete_(encodeKey(mutation.key));
        } else {
          kv.stored.set(encodeKey(mutation.key), StoredEntry{mutation.key, mutation.value, versionstamp});
          kv.ttlOf.set(encodeKey(mutation.key), mutation.expireIn);
        }
      }
      result.versionstamp = versionstamp;
    }
    co_await rt::microtask();
    co_return result;
  }

  const MeteredAccountCredentials& credentials() {
    static const MeteredAccountCredentials value{.apiKey = "metered-api-key"};
    return value;
  }

  // `FetchCall = Readonly<{ url; init; headers }>`
  struct FetchCall {
    std::string url;
    rt::RequestInit init;
    rt::Headers headers;
  };
  using FetchCalls = std::shared_ptr<std::vector<FetchCall>>;

  std::string requestUrl(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->href();
    return std::get<rt::Request>(input).url();
  }

  rt::Response textResponse(const std::string& body, double status, const std::string& contentType) {
    rt::ResponseInit init;
    init.status = status;
    init.headers = rt::Headers{{"Content-Type", contentType}};
    return rt::Response(rt::BodyInit(body), init);
  }

  rt::Fetch rawFetcher(std::string body, FetchCalls calls = std::make_shared<std::vector<FetchCall>>(), double status = 200, std::string contentType = "application/json") {
    return rt::Fetch([body, calls, status, contentType](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
      const rt::Headers headers(init.headers.value_or(rt::Headers()));
      calls->push_back(FetchCall{.url = requestUrl(input), .init = init, .headers = headers});
      return textResponse(body, status, contentType);
    });
  }

  rt::Fetch jsonFetcher(const json::value& payload, FetchCalls calls = std::make_shared<std::vector<FetchCall>>(), double status = 200,
                        std::string contentType = "application/json") {
    return rawFetcher(rt::jsonStringify(payload), std::move(calls), status, std::move(contentType));
  }

  rt::Task<rt::Response> failingResponse(std::string message) {
    throw rt::JsError(message);
    co_return rt::Response();
  }

  // `() => Promise.reject(new Error(message))`
  rt::Fetch rejectingFetcher(std::string message) {
    return rt::Fetch([message](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> { return failingResponse(message); });
  }

  // `tokenUsageEnvelope(overrides)`: an override of `undefined` (std::nullopt) removes the member,
  // as JSON.stringify does.
  json::value tokenUsageEnvelope(std::vector<std::pair<std::string, std::optional<json::value>>> overrides = {}) {
    json::object data = rt::jsonParse(R"({"expires_at":0,"model_limits":{},"model_limits_enabled":false,"name":"business-key","object":"token_usage",
      "total_available":9000,"total_granted":10000,"total_used":1000,"unlimited_quota":false})")
                          .as_object();
    for (auto& [key, value] : overrides) {
      if (value) rt::objectSet(data, key, *value);
      else rt::objectDelete(data, key);
    }
    json::object envelope;
    envelope["success"] = true;
    envelope["data"] = std::move(data);
    return envelope;
  }

  MeteredQuotaObservation observation() {
    return MeteredQuotaObservation{
      .balance_quota = 50'000'000,
      .used_quota = 100'000,
      .quota_per_credit = 500'000,
      .observed_at_ms = 1'000'000,
      .latest_refill = MeteredRefillObservation{.id = "refill-1", .amount_credits = 100, .completed_at_ms = 900'000},
    };
  }

  MeteredQuotaState walletState() {
    return MeteredQuotaState{
      .current_balance_quota = 50'000'000,
      .post_refill_baseline_quota = 50'000'000,
      .last_observed_used_quota = 100'000,
      .quota_per_credit = 500'000,
      .observed_at_ms = 1'000'000,
      .cycle_started_at_ms = 900'000,
      .confidence = "provisional",
      .last_known_debits_quota = 0,
      .last_inferred_credit_quota = 0,
      .last_credit_at_ms = std::nullopt,
      .latest_refill_id = "refill-1",
      .latest_refill_amount_credits = 100,
      .latest_refill_completed_at_ms = 900'000,
    };
  }

  MeteredQuotaBalanceSample balanceSample() {
    return MeteredQuotaBalanceSample{
      .v = 1,
      .bucket_start_at_ms = METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS,
      .observed_at_ms = METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS + 1,
      .balance_quota = 10,
      .baseline_quota = 20,
      .quota_per_credit = 1,
      .remaining_percent = 50,
    };
  }

  rt::KvValue kvOf(const json::value& value) { return rt::KvValue(value); }
  template <class T> rt::KvValue stored(const T& record) { return rt::KvValue(json::value_from(record)); }

  // `{ owner, lease_until_ms }`
  rt::KvValue lease(const std::string& owner, double leaseUntilMs) {
    json::object value;
    value["owner"] = json::string(owner);
    value["lease_until_ms"] = rt::makeNumber(leaseUntilMs);
    return rt::KvValue(json::value(std::move(value)));
  }

  bool sameJson(const json::value& actual, std::string_view expected) { return rt::deepEqual(rt::canonicalNumbers(actual), rt::jsonParse(expected)); }

  // `assert.rejects(() => task, /pattern/)`: the rejection's String(error) matches.
  template <class T> rt::Task<bool> rejectsMatching(rt::Task<T> task, std::string pattern) {
    rt::Reason failure;
    try {
      co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    if (!failure) co_return false;
    co_return rt::Regex(pattern).test(rt::reasonToString(failure));
  }

  GetMeteredQuotaSnapshotOptions snapshotOptions(const std::shared_ptr<rt::Kv>& kv) {
    GetMeteredQuotaSnapshotOptions options;
    options.kv = kv;
    return options;
  }

  std::function<double()> fixedNow(double now) {
    return [now] { return now; };
  }

  std::function<std::string()> owner(std::string name) {
    return [name] { return name; };
  }

  rt::Task<std::optional<MeteredQuotaSnapshot>> snapshotFor(GetMeteredQuotaSnapshotOptions options) {
    rt::Task<std::optional<MeteredQuotaSnapshot>> reading = getMeteredQuotaSnapshot(credentials(), std::move(options));
    std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);
    co_return snapshot;
  }

  rt::Task<std::optional<MeteredQuotaSnapshot>> cachedFor(std::shared_ptr<rt::Kv> kv, std::optional<double> now) {
    GetCachedMeteredQuotaSnapshotOptions options;
    options.kv = std::move(kv);
    if (now) options.now = fixedNow(*now);
    rt::Task<std::optional<MeteredQuotaSnapshot>> reading = getCachedMeteredQuotaSnapshot(std::move(options));
    std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);
    co_return snapshot;
  }

  bool hasBalanceHistory(const MeteredKv& kv) {
    for (const auto& [encoded, entry] : kv.stored) {
      if (encoded.find("balance_history") != std::string::npos) return true;
    }
    return false;
  }

  std::size_t readsOf(const MeteredKv& kv, const rt::KvKey& key) {
    std::size_t count = 0;
    for (const rt::KvKey& read : kv.readLog) {
      if (encodeKey(read) == encodeKey(key)) ++count;
    }
    return count;
  }

}

// ---------------------------------------------------------------------------
// Account fingerprint and window normalization
// ---------------------------------------------------------------------------

ASYNC_TEST("the account fingerprint is a stable non-secret digest of the configured key") {
  rt::Task<std::optional<std::string>> first = meterQuotaAccountFingerprint(credentials());
  const std::optional<std::string> fingerprint = co_await std::move(first);
  EXPECT(rt::Regex("^[0-9a-f]{16}$").test(fingerprint.value_or("")));
  rt::Task<std::optional<std::string>> second = meterQuotaAccountFingerprint(credentials());
  const std::optional<std::string> again = co_await std::move(second);
  EXPECT_EQ(again, fingerprint);
  rt::Task<std::optional<std::string>> third = meterQuotaAccountFingerprint(MeteredAccountCredentials{.apiKey = "another-metered-key"});
  const std::optional<std::string> other = co_await std::move(third);
  EXPECT(other != fingerprint);
  EXPECT_EQ(fingerprint.value_or("").find(credentials().apiKey) != std::string::npos, false);
}

ASYNC_TEST("the account fingerprint refuses missing, blank and whitespace keys") {
  rt::Task<std::optional<std::string>> missing = meterQuotaAccountFingerprint(std::nullopt);
  const auto missingValue = co_await std::move(missing);
  EXPECT(!missingValue.has_value());
  rt::Task<std::optional<std::string>> blank = meterQuotaAccountFingerprint(MeteredAccountCredentials{.apiKey = ""});
  const auto blankValue = co_await std::move(blank);
  EXPECT(!blankValue.has_value());
  rt::Task<std::optional<std::string>> spaces = meterQuotaAccountFingerprint(MeteredAccountCredentials{.apiKey = "  "});
  const auto spacesValue = co_await std::move(spaces);
  EXPECT(!spacesValue.has_value());
  rt::Task<std::optional<std::string>> inner = meterQuotaAccountFingerprint(MeteredAccountCredentials{.apiKey = "before after"});
  const auto innerValue = co_await std::move(inner);
  EXPECT(!innerValue.has_value());
}

TEST("the balance window accepts only published day counts") {
  EXPECT_EQ(normalizeMeteredQuotaBalanceWindowDays("7"), 7.0);
  EXPECT_EQ(normalizeMeteredQuotaBalanceWindowDays("30"), 30.0);
  EXPECT_EQ(normalizeMeteredQuotaBalanceWindowDays("365"), 365.0);
  EXPECT_EQ(normalizeMeteredQuotaBalanceWindowDays("5"), 7.0);
  EXPECT_EQ(normalizeMeteredQuotaBalanceWindowDays(std::nullopt), 7.0);
  EXPECT_EQ(normalizeMeteredQuotaBalanceWindowDays("7.5"), 7.0);
}

// ---------------------------------------------------------------------------
// Observation fetch
// ---------------------------------------------------------------------------

ASYNC_TEST("a Metered observation rejects invalid credentials before any request") {
  auto calls = std::make_shared<std::vector<FetchCall>>();
  FetchMeteredQuotaObservationOptions options;
  options.fetcher = jsonFetcher(tokenUsageEnvelope(), calls);
  rt::Task<bool> rejecting = rejectsMatching(fetchMeteredQuotaObservation(MeteredAccountCredentials{.apiKey = "not valid"}, std::move(options)),
                                             "Metered account credentials are invalid");
  const bool rejectingOk = co_await std::move(rejecting);
  EXPECT(rejectingOk);
  EXPECT_EQ(calls->size(), std::size_t{0});
}

ASYNC_TEST("a Metered observation request is a manual-redirect JSON GET with a combined deadline") {
  auto calls = std::make_shared<std::vector<FetchCall>>();
  FetchMeteredQuotaObservationOptions options;
  options.fetcher = jsonFetcher(tokenUsageEnvelope(), calls);
  options.now = fixedNow(2'000'000);
  rt::Task<MeteredQuotaObservation> fetching = fetchMeteredQuotaObservation(credentials(), std::move(options));
  co_await std::move(fetching);

  EXPECT_EQ(calls->size(), std::size_t{1});
  const FetchCall& call = calls->at(0);
  EXPECT_EQ(call.url, "https://api.openlux.ai/api/usage/token/");
  EXPECT_EQ(call.init.method, std::optional<std::string>("GET"));
  EXPECT_EQ(call.init.redirect, std::optional<std::string>("manual"));
  EXPECT_EQ(call.headers.get("Accept"), std::optional<std::string>("application/json"));
  EXPECT_EQ(call.headers.get("Authorization"), std::optional<std::string>("Bearer metered-api-key"));
  EXPECT(call.init.signal.has_value());
}

ASYNC_TEST("a Metered observation reports every upstream transport fault") {
  FetchMeteredQuotaObservationOptions limited;
  limited.fetcher = rawFetcher("rate limited", std::make_shared<std::vector<FetchCall>>(), 429, "text/plain");
  rt::Task<bool> first = rejectsMatching(fetchMeteredQuotaObservation(credentials(), std::move(limited)), "Metered account API returned HTTP 429");
  const bool firstOk = co_await std::move(first);
  EXPECT(firstOk);
  FetchMeteredQuotaObservationOptions plain;
  plain.fetcher = rawFetcher("not json", std::make_shared<std::vector<FetchCall>>(), 200, "text/plain");
  rt::Task<bool> second = rejectsMatching(fetchMeteredQuotaObservation(credentials(), std::move(plain)), "Metered account API returned non-JSON data");
  const bool secondOk = co_await std::move(second);
  EXPECT(secondOk);
  FetchMeteredQuotaObservationOptions broken;
  broken.fetcher = rawFetcher("{not json", std::make_shared<std::vector<FetchCall>>(), 200, "application/json");
  rt::Task<bool> third = rejectsMatching(fetchMeteredQuotaObservation(credentials(), std::move(broken)), "Metered account API returned invalid JSON");
  const bool thirdOk = co_await std::move(third);
  EXPECT(thirdOk);
}

ASYNC_TEST("a Metered observation rejects envelopes that do not carry token usage") {
  FetchMeteredQuotaObservationOptions failed;
  failed.fetcher = jsonFetcher(rt::jsonParse(R"({"success":false,"message":"bad key"})"));
  rt::Task<bool> first = rejectsMatching(fetchMeteredQuotaObservation(credentials(), std::move(failed)), "Metered account API returned an invalid envelope");
  const bool firstOk = co_await std::move(first);
  EXPECT(firstOk);
  FetchMeteredQuotaObservationOptions text;
  text.fetcher = jsonFetcher(rt::jsonParse(R"({"success":true,"data":"nope"})"));
  rt::Task<bool> second = rejectsMatching(fetchMeteredQuotaObservation(credentials(), std::move(text)), "Metered account API returned an invalid envelope");
  const bool secondOk = co_await std::move(second);
  EXPECT(secondOk);
  FetchMeteredQuotaObservationOptions stringUsed;
  stringUsed.fetcher = jsonFetcher(tokenUsageEnvelope({{"total_used", json::value("1000")}}));
  rt::Task<bool> third = rejectsMatching(fetchMeteredQuotaObservation(credentials(), std::move(stringUsed)), "Metered account API returned invalid token usage data");
  const bool thirdOk = co_await std::move(third);
  EXPECT(thirdOk);
  FetchMeteredQuotaObservationOptions missingFlag;
  missingFlag.fetcher = jsonFetcher(tokenUsageEnvelope({{"unlimited_quota", std::nullopt}}));
  rt::Task<bool> fourth = rejectsMatching(fetchMeteredQuotaObservation(credentials(), std::move(missingFlag)), "Metered account API returned invalid token usage data");
  const bool fourthOk = co_await std::move(fourth);
  EXPECT(fourthOk);
}

ASYNC_TEST("a Metered observation rejects an invalid clock") {
  FetchMeteredQuotaObservationOptions negative;
  negative.fetcher = jsonFetcher(tokenUsageEnvelope());
  negative.now = fixedNow(-1);
  rt::Task<bool> first = rejectsMatching(fetchMeteredQuotaObservation(credentials(), std::move(negative)), "Metered quota observation clock is invalid");
  const bool firstOk = co_await std::move(first);
  EXPECT(firstOk);
  FetchMeteredQuotaObservationOptions notANumber;
  notANumber.fetcher = jsonFetcher(tokenUsageEnvelope());
  notANumber.now = fixedNow(rt::Number::NaN);
  rt::Task<bool> second = rejectsMatching(fetchMeteredQuotaObservation(credentials(), std::move(notANumber)), "Metered quota observation clock is invalid");
  const bool secondOk = co_await std::move(second);
  EXPECT(secondOk);
}

ASYNC_TEST("a Metered token usage observation keeps provider totals and neutral wallet fields") {
  FetchMeteredQuotaObservationOptions options;
  options.fetcher = jsonFetcher(tokenUsageEnvelope({{"unlimited_quota", json::value(true)}, {"total_available", rt::makeNumber(-53'413.0)}}),
                                std::make_shared<std::vector<FetchCall>>(), 200);
  options.now = fixedNow(2'000'000.9);
  rt::Task<MeteredQuotaObservation> fetching = fetchMeteredQuotaObservation(credentials(), std::move(options));
  const MeteredQuotaObservation result = co_await std::move(fetching);

  EXPECT(sameJson(json::value_from(result), R"({"balance_quota":null,"used_quota":null,"quota_per_credit":null,"observed_at_ms":2000000,"latest_refill":null,
    "unlimited_quota":true,"total_available":-53413,"total_granted":10000,"total_used":1000})"));
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------

TEST("token usage becomes a neutral provisional state with no spendable credit") {
  MeteredQuotaObservation input = observation();
  input.unlimited_quota = true;
  input.total_available = 9'000;
  input.total_granted = 10'000;
  input.total_used = 1'000;
  const MeteredQuotaState next = updateMeteredQuotaState(std::nullopt, input);

  EXPECT_EQ(next.current_balance_quota, 0.0);
  EXPECT_EQ(next.post_refill_baseline_quota, 0.0);
  EXPECT_EQ(next.last_observed_used_quota, 0.0);
  EXPECT_EQ(next.quota_per_credit, 1.0);
  EXPECT_EQ(next.confidence, "provisional");
  EXPECT_EQ(next.observed_at_ms, 1'000'000.0);
  EXPECT_EQ(next.cycle_started_at_ms, 1'000'000.0);
  EXPECT_EQ(next.unlimited_quota, std::optional<bool>(true));
  EXPECT(next.total_available == rt::Nullable<double>(9'000.0));
  EXPECT(next.total_granted == rt::Nullable<double>(10'000.0));
  EXPECT(next.total_used == rt::Nullable<double>(1'000.0));
  EXPECT(!next.latest_refill_id.has_value());
}

TEST("token usage supersedes a retained wallet state") {
  MeteredQuotaObservation input = observation();
  input.unlimited_quota = false;
  input.total_available = 1;
  input.total_granted = 2;
  input.total_used = 1;
  const MeteredQuotaState next = updateMeteredQuotaState(walletState(), input);
  EXPECT_EQ(next.current_balance_quota, 0.0);
  EXPECT(!next.latest_refill_id.has_value());
}

TEST("an incomplete token usage observation is rejected") {
  MeteredQuotaObservation onlyUsed = observation();
  onlyUsed.total_used = 5;
  EXPECT_THROWS_MATCHING(updateMeteredQuotaState(std::nullopt, onlyUsed), "Metered token usage observation is incomplete");
  MeteredQuotaObservation onlyFlag = observation();
  onlyFlag.unlimited_quota = true;
  EXPECT_THROWS_MATCHING(updateMeteredQuotaState(std::nullopt, onlyFlag), "Metered token usage observation is incomplete");
}

TEST("a wallet observation without credit fields is rejected") {
  MeteredQuotaObservation noCredit = observation();
  noCredit.balance_quota = std::nullopt;
  noCredit.used_quota = std::nullopt;
  noCredit.quota_per_credit = std::nullopt;
  EXPECT_THROWS_MATCHING(updateMeteredQuotaState(std::nullopt, noCredit), "Metered wallet observation is incomplete");
  MeteredQuotaObservation noRate = observation();
  noRate.quota_per_credit = std::nullopt;
  EXPECT_THROWS_MATCHING(updateMeteredQuotaState(std::nullopt, noRate), "Metered wallet observation is incomplete");
}

TEST("a first wallet observation without a refill keeps neutral refill metadata") {
  MeteredQuotaObservation input = observation();
  input.latest_refill = std::nullopt;
  const MeteredQuotaState next = updateMeteredQuotaState(std::nullopt, input);

  EXPECT_EQ(next.current_balance_quota, 50'000'000.0);
  EXPECT_EQ(next.post_refill_baseline_quota, 50'000'000.0);
  EXPECT_EQ(next.cycle_started_at_ms, 1'000'000.0);
  EXPECT(!next.latest_refill_id.has_value());
  EXPECT(!next.latest_refill_amount_credits.has_value());
  EXPECT(!next.latest_refill_completed_at_ms.has_value());
  EXPECT_EQ(next.confidence, "provisional");
}

TEST("an advanced wallet observation keeps prior refill metadata when no refill is reported") {
  const MeteredQuotaState previous = walletState();
  MeteredQuotaObservation input = observation();
  input.balance_quota = 49'000'000;
  input.used_quota = 1'100'000;
  input.latest_refill = std::nullopt;
  const MeteredQuotaState next = updateMeteredQuotaState(previous, input);

  EXPECT_EQ(next.latest_refill_id, previous.latest_refill_id);
  EXPECT_EQ(next.latest_refill_amount_credits, previous.latest_refill_amount_credits);
  EXPECT_EQ(next.latest_refill_completed_at_ms, previous.latest_refill_completed_at_ms);
  EXPECT_EQ(next.last_known_debits_quota, 1'000'000.0);
  EXPECT_EQ(next.confidence, "provisional");
}

// ---------------------------------------------------------------------------
// Snapshot reads
// ---------------------------------------------------------------------------

ASYNC_TEST("a fresh cached wallet state answers without touching the network") {
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 10'000'000;
  MeteredQuotaState state = walletState();
  state.observed_at_ms = now;
  state.post_refill_baseline_quota = 50'000'000;
  state.current_balance_quota = 25'000'000;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(state), METERED_QUOTA_RETENTION_MS);

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.now = fixedNow(now);
  options.fetcher = rejectingFetcher("must not fetch");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "fresh");
  EXPECT_EQ(snapshot->balance_credits, std::optional<double>(50));
  EXPECT_EQ(snapshot->baseline_credits, std::optional<double>(100));
  EXPECT_EQ(snapshot->remaining_percent, std::optional<double>(50));
  EXPECT_EQ(snapshot->used_percent, std::optional<double>(50));
  EXPECT_EQ(snapshot->last_inferred_credit_credits, std::optional<double>(0));
  EXPECT_EQ(snapshot->unlimited_quota, false);
  EXPECT(!snapshot->total_available.has_value());
  EXPECT(!snapshot->total_granted.has_value());
  EXPECT(!snapshot->total_used.has_value());
}

ASYNC_TEST("a zero baseline reports no remaining percentage instead of dividing by zero") {
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 12'000'000;
  MeteredQuotaState state = walletState();
  state.observed_at_ms = now;
  state.current_balance_quota = 0;
  state.post_refill_baseline_quota = 0;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(state));

  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = cachedFor(kv, now);
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "fresh");
  EXPECT(!snapshot->remaining_percent.has_value());
  EXPECT(!snapshot->used_percent.has_value());
  EXPECT_EQ(snapshot->balance_credits, std::optional<double>(0));
  EXPECT_EQ(snapshot->baseline_credits, std::optional<double>(0));
}

ASYNC_TEST("a token usage snapshot carries no credit or percentage fields") {
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 13'000'000;
  MeteredQuotaState state = walletState();
  state.observed_at_ms = now;
  state.unlimited_quota = true;
  state.total_available = -53'413.0;
  state.total_granted = -545.0;
  state.total_used = 52'868.0;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(state));

  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = cachedFor(kv, now);
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "fresh");
  EXPECT(!snapshot->balance_credits.has_value());
  EXPECT(!snapshot->baseline_credits.has_value());
  EXPECT(!snapshot->last_inferred_credit_credits.has_value());
  EXPECT(!snapshot->remaining_percent.has_value());
  EXPECT(!snapshot->used_percent.has_value());
  EXPECT_EQ(snapshot->unlimited_quota, true);
  EXPECT_EQ(snapshot->total_available, std::optional<double>(-53'413));
  EXPECT_EQ(snapshot->total_granted, std::optional<double>(-545));
  EXPECT_EQ(snapshot->total_used, std::optional<double>(52'868));
}

ASYNC_TEST("a cache peek without a KV or without retained state reports nothing") {
  rt::Task<std::optional<MeteredQuotaSnapshot>> withoutKv = cachedFor(nullptr, std::nullopt);
  const auto withoutKvValue = co_await std::move(withoutKv);
  EXPECT(!withoutKvValue.has_value());

  const auto kv = std::make_shared<MeteredKv>();
  rt::Task<std::optional<MeteredQuotaSnapshot>> empty = cachedFor(kv, 1);
  const auto emptyValue = co_await std::move(empty);
  EXPECT(!emptyValue.has_value());
}

ASYNC_TEST("a cache peek reports a KV read failure instead of throwing") {
  const auto kv = std::make_shared<MeteredKv>();
  kv->planReads(METERED_QUOTA_STATE_KEY, {"throw"});

  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = cachedFor(kv, 1);
  const auto readingValue = co_await std::move(reading);
  EXPECT(!readingValue.has_value());
}

ASYNC_TEST("a snapshot request refuses unusable credentials or a missing KV") {
  GetMeteredQuotaSnapshotOptions badOptions;
  badOptions.kv = std::shared_ptr<rt::Kv>();
  rt::Task<std::optional<MeteredQuotaSnapshot>> bad = getMeteredQuotaSnapshot(MeteredAccountCredentials{.apiKey = "bad key"}, std::move(badOptions));
  const auto badValue = co_await std::move(bad);
  EXPECT(!badValue.has_value());
  GetMeteredQuotaSnapshotOptions noKv;
  noKv.kv = std::shared_ptr<rt::Kv>();
  rt::Task<std::optional<MeteredQuotaSnapshot>> missing = snapshotFor(std::move(noKv));
  const auto missingValue = co_await std::move(missing);
  EXPECT(!missingValue.has_value());
}

ASYNC_TEST("a snapshot request survives a failed cache read and still refreshes") {
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 14'000'000;
  kv->planReads(METERED_QUOTA_STATE_KEY, {"throw"});
  auto calls = std::make_shared<std::vector<FetchCall>>();

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.now = fixedNow(now);
  options.fetcher = jsonFetcher(tokenUsageEnvelope(), calls);
  options.createLeaseOwner = owner("cache-read-failure-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "refreshed");
  EXPECT_EQ(calls->size(), std::size_t{1});
  EXPECT_EQ(snapshot->total_used, std::optional<double>(1'000));
}

ASYNC_TEST("a snapshot request survives a failed invalidation read and still refreshes") {
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 15'000'000;
  kv->planReads(METERED_QUOTA_INVALIDATION_KEY, {"throw"});
  MeteredQuotaState state = walletState();
  state.observed_at_ms = now - METERED_QUOTA_FRESH_MS;
  state.latest_refill_id = "0";
  kv->seed(METERED_QUOTA_STATE_KEY, stored(state));
  auto calls = std::make_shared<std::vector<FetchCall>>();

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.now = fixedNow(now);
  options.fetcher = jsonFetcher(tokenUsageEnvelope(), calls);
  options.createLeaseOwner = owner("invalidation-read-failure-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "refreshed");
  EXPECT_EQ(calls->size(), std::size_t{1});
}

ASYNC_TEST("a live lease serves the retained stale state immediately") {
  const auto kv = std::make_shared<MeteredKv>();
  const double observedAt = rt::dateNow() - METERED_QUOTA_FRESH_MS;
  MeteredQuotaState cached = walletState();
  cached.observed_at_ms = observedAt;
  cached.current_balance_quota = 25'000'000;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(cached));
  kv->seed(METERED_QUOTA_REFRESH_LEASE_KEY, lease("another-worker", rt::dateNow() + 60'000));

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.fetcher = rejectingFetcher("must not fetch");
  options.createLeaseOwner = owner("blocked-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "stale");
  EXPECT_EQ(snapshot->state.current_balance_quota, 25'000'000.0);
  EXPECT_EQ(snapshot->remaining_percent, std::optional<double>(50));
}

ASYNC_TEST("a refused refresh commit keeps the stored state and serves it as stale") {
  const auto kv = std::make_shared<MeteredKv>();
  const double observedAt = rt::dateNow() - METERED_QUOTA_FRESH_MS;
  MeteredQuotaState cached = walletState();
  cached.observed_at_ms = observedAt;
  cached.current_balance_quota = 20'000'000;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(cached));
  kv->conflictOnSet(METERED_QUOTA_STATE_KEY);

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.fetcher = jsonFetcher(tokenUsageEnvelope());
  options.createLeaseOwner = owner("losing-commit-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "stale");
  EXPECT_EQ(snapshot->state.current_balance_quota, 20'000'000.0);
  EXPECT(kv->value(METERED_QUOTA_STATE_KEY) == stored(cached));
  EXPECT(kv->value(METERED_QUOTA_REFRESH_LEASE_KEY).isNull());
  EXPECT_EQ(hasBalanceHistory(*kv), false);
}

ASYNC_TEST("a held refresh lease makes the caller wait for retained state") {
  const auto kv = std::make_shared<MeteredKv>();
  const double observedAt = rt::dateNow() - METERED_QUOTA_FRESH_MS;
  kv->seed(METERED_QUOTA_REFRESH_LEASE_KEY, lease("another-worker", rt::dateNow() + 60'000));
  MeteredQuotaState state = walletState();
  state.observed_at_ms = observedAt;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(state));
  kv->planReads(METERED_QUOTA_STATE_KEY, {"null"});

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.fetcher = rejectingFetcher("must not fetch");
  options.createLeaseOwner = owner("waiting-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "wait");
  EXPECT_EQ(snapshot->state.observed_at_ms, observedAt);
  EXPECT_EQ(snapshot->remaining_percent, std::optional<double>(100));
  EXPECT_EQ(kv->value(METERED_QUOTA_STATE_KEY).isNull(), false);
}

ASYNC_TEST("a held refresh lease with no retained state gives up after the cold wait") {
  const double startedAt = rt::dateNow();
  const auto kv = std::make_shared<MeteredKv>();
  kv->seed(METERED_QUOTA_REFRESH_LEASE_KEY, lease("another-worker", rt::dateNow() + 60'000));

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.fetcher = rejectingFetcher("must not fetch");
  options.createLeaseOwner = owner("cold-wait-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(!snapshot.has_value());
  EXPECT(rt::dateNow() - startedAt >= METERED_QUOTA_COLD_WAIT_MS);
  EXPECT_EQ(readsOf(*kv, METERED_QUOTA_STATE_KEY) > 10, true);
}

ASYNC_TEST("a snapshot request reports no state when the lease read and the cold wait fail") {
  const auto kv = std::make_shared<MeteredKv>();
  kv->planReads(METERED_QUOTA_REFRESH_LEASE_KEY, {"throw"});
  kv->planReads(METERED_QUOTA_STATE_KEY, {"null", "throw"});

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.fetcher = rejectingFetcher("must not fetch");
  options.createLeaseOwner = owner("unavailable-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(!snapshot.has_value());
}

ASYNC_TEST("a stale cached state is served while the refresh lease is lost during the fetch") {
  const auto kv = std::make_shared<MeteredKv>();
  const double observedAt = rt::dateNow() - METERED_QUOTA_FRESH_MS;
  MeteredQuotaState state = walletState();
  state.observed_at_ms = observedAt;
  state.current_balance_quota = 20'000'000;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(state));
  auto sawSignal = std::make_shared<bool>(false);

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.fetcher = rt::Fetch([kv, sawSignal](rt::RequestInfo, rt::RequestInit init) -> rt::Response {
    kv->seed(METERED_QUOTA_REFRESH_LEASE_KEY, lease("thief", rt::dateNow() + 60'000));
    *sawSignal = init.signal.has_value();
    return textResponse(rt::jsonStringify(tokenUsageEnvelope()), 200, "application/json");
  });
  options.createLeaseOwner = owner("losing-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(*sawSignal);
  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "stale");
  EXPECT_EQ(snapshot->state.current_balance_quota, 20'000'000.0);
  EXPECT_EQ(snapshot->state.observed_at_ms, observedAt);
}

ASYNC_TEST("a refresh whose retained state disappears falls back to the cached state") {
  const auto kv = std::make_shared<MeteredKv>();
  const double observedAt = rt::dateNow() - METERED_QUOTA_FRESH_MS;
  MeteredQuotaState state = walletState();
  state.observed_at_ms = observedAt;
  state.current_balance_quota = 30'000'000;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(state));
  // Reads: 1 cache probe, 2 refresh state entry, 3 replacement probe.
  kv->planReads(METERED_QUOTA_STATE_KEY, {"value", "value", "null"});

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.fetcher = rt::Fetch([kv](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    kv->seed(METERED_QUOTA_REFRESH_LEASE_KEY, lease("thief", rt::dateNow() + 60'000));
    return textResponse(rt::jsonStringify(tokenUsageEnvelope()), 200, "application/json");
  });
  options.createLeaseOwner = owner("losing-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "stale");
  EXPECT_EQ(snapshot->state.current_balance_quota, 30'000'000.0);
}

ASYNC_TEST("a refresh that loses its lease with no retained state reports nothing") {
  const auto kv = std::make_shared<MeteredKv>();
  kv->planReads(METERED_QUOTA_STATE_KEY, {"null", "null", "null"});

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.fetcher = rt::Fetch([kv](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    kv->seed(METERED_QUOTA_REFRESH_LEASE_KEY, lease("thief", rt::dateNow() + 60'000));
    return textResponse(rt::jsonStringify(tokenUsageEnvelope()), 200, "application/json");
  });
  options.createLeaseOwner = owner("losing-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(!snapshot.has_value());
}

ASYNC_TEST("a refresh keeps the lease it acquired and stores the observation with its retention TTL") {
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 16'000'000;
  auto calls = std::make_shared<std::vector<FetchCall>>();

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.now = fixedNow(now);
  options.fetcher = jsonFetcher(tokenUsageEnvelope(), calls);
  options.createLeaseOwner = owner("committing-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "refreshed");
  EXPECT(kv->value(METERED_QUOTA_REFRESH_LEASE_KEY).isNull());
  const std::optional<double>* ttl = kv->ttlOf.get(encodeKey(METERED_QUOTA_STATE_KEY));
  EXPECT(ttl != nullptr && *ttl == std::optional<double>(METERED_QUOTA_RETENTION_MS));
  EXPECT(kv->value(METERED_QUOTA_STATE_KEY) == stored(snapshot->state));
  EXPECT_EQ(calls->size(), std::size_t{1});
}

ASYNC_TEST("a failed refresh lease release never fails the answered snapshot") {
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 17'000'000;
  // Reads: 1 acquire probe, 2 lease re-check, 3 release probe.
  kv->planReads(METERED_QUOTA_REFRESH_LEASE_KEY, {"null", "value", "throw"});

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.now = fixedNow(now);
  options.fetcher = jsonFetcher(tokenUsageEnvelope());
  options.createLeaseOwner = owner("release-failure-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "refreshed");
  EXPECT_EQ(snapshot->total_used, std::optional<double>(1'000));
}

ASYNC_TEST("a failed balance-history write never fails the answered snapshot") {
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 18'000'000;
  rt::Task<std::optional<std::string>> fingerprinting = meterQuotaAccountFingerprint(credentials());
  const std::optional<std::string> fingerprint = co_await std::move(fingerprinting);
  EXPECT(fingerprint.has_value());
  const double bucketStart = rt::Math::floor(now / METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS) * METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS;
  kv->planReads(rt::KvKey{METERED_QUOTA_BALANCE_HISTORY_PREFIX, fingerprint.value_or(""), bucketStart}, {"throw"});

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.now = fixedNow(now);
  options.fetcher = jsonFetcher(tokenUsageEnvelope());
  options.createLeaseOwner = owner("history-failure-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(snapshot.has_value());
  if (!snapshot) co_return;
  EXPECT_EQ(snapshot->cache_state, "refreshed");
  const std::optional<double>* ttl = kv->ttlOf.get(encodeKey(METERED_QUOTA_STATE_KEY));
  EXPECT(ttl != nullptr && *ttl == std::optional<double>(METERED_QUOTA_RETENTION_MS));
}

ASYNC_TEST("a refresh failure with no cached state reports nothing") {
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 19'000'000;

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.now = fixedNow(now);
  options.fetcher = rejectingFetcher("outage");
  options.createLeaseOwner = owner("failing-owner");
  rt::Task<std::optional<MeteredQuotaSnapshot>> reading = snapshotFor(std::move(options));
  const std::optional<MeteredQuotaSnapshot> snapshot = co_await std::move(reading);

  EXPECT(!snapshot.has_value());
  EXPECT(kv->value(METERED_QUOTA_REFRESH_LEASE_KEY).isNull());
}

ASYNC_TEST("an aborted refresh rethrows instead of serving stale state") {
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 19'500'000;
  MeteredQuotaState state = walletState();
  state.observed_at_ms = now - METERED_QUOTA_FRESH_MS;
  kv->seed(METERED_QUOTA_STATE_KEY, stored(state));
  const rt::AbortController controller;
  controller.abort();

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.now = fixedNow(now);
  options.signal = controller.signal();
  options.fetcher = rejectingFetcher("aborted");
  options.createLeaseOwner = owner("aborted-owner");
  rt::Task<bool> rejecting = rejectsMatching(snapshotFor(std::move(options)), "aborted");
  const bool rejectingOk = co_await std::move(rejecting);
  EXPECT(rejectingOk);
}

// ---------------------------------------------------------------------------
// Invalidation
// ---------------------------------------------------------------------------

ASYNC_TEST("invalidation records one retention-bounded marker") {
  const auto kv = std::make_shared<MeteredKv>();
  GetCachedMeteredQuotaSnapshotOptions options;
  options.kv = kv;
  options.now = fixedNow(1'234.9);
  rt::Task<void> invalidating = invalidateMeteredQuotaSnapshot(std::move(options));
  co_await std::move(invalidating);

  EXPECT(kv->value(METERED_QUOTA_INVALIDATION_KEY) == kvOf(rt::jsonParse(R"({"invalidated_at_ms":1234})")));
  const std::optional<double>* ttl = kv->ttlOf.get(encodeKey(METERED_QUOTA_INVALIDATION_KEY));
  EXPECT(ttl != nullptr && *ttl == std::optional<double>(METERED_QUOTA_RETENTION_MS));
}

ASYNC_TEST("invalidation without a KV or with an invalid clock is refused") {
  GetCachedMeteredQuotaSnapshotOptions noKv;
  noKv.kv = std::shared_ptr<rt::Kv>();
  rt::Task<void> withoutKv = invalidateMeteredQuotaSnapshot(std::move(noKv));
  co_await std::move(withoutKv);

  const auto kv = std::make_shared<MeteredKv>();
  GetCachedMeteredQuotaSnapshotOptions badClock;
  badClock.kv = kv;
  badClock.now = fixedNow(rt::Number::NaN);
  rt::Task<bool> rejecting = rejectsMatching(invalidateMeteredQuotaSnapshot(std::move(badClock)), "Metered quota invalidation clock is invalid");
  const bool rejectingOk = co_await std::move(rejecting);
  EXPECT(rejectingOk);
  EXPECT_EQ(kv->has(METERED_QUOTA_INVALIDATION_KEY), false);
}

ASYNC_TEST("invalidation reports a refused KV commit") {
  const auto kv = std::make_shared<MeteredKv>();
  kv->conflictOnSet(METERED_QUOTA_INVALIDATION_KEY);

  GetCachedMeteredQuotaSnapshotOptions options;
  options.kv = kv;
  options.now = fixedNow(5);
  rt::Task<bool> rejecting = rejectsMatching(invalidateMeteredQuotaSnapshot(std::move(options)), "Deno KV could not invalidate the Metered quota snapshot");
  const bool rejectingOk = co_await std::move(rejecting);
  EXPECT(rejectingOk);
}

// ---------------------------------------------------------------------------
// Balance history
// ---------------------------------------------------------------------------

ASYNC_TEST("a balance sample is keyed by account and hour bucket") {
  const auto kv = std::make_shared<MeteredKv>();
  const MeteredQuotaState state = walletState();
  rt::Task<void> writing = writeMeteredQuotaBalanceSample(kv, state, 3'600'000 + 900, std::string("fingerprint-a"));
  co_await std::move(writing);

  const rt::KvKey key{METERED_QUOTA_BALANCE_HISTORY_PREFIX, "fingerprint-a", 3'600'000};
  EXPECT(kv->value(key) == kvOf(rt::jsonParse(R"({"v":1,"bucket_start_at_ms":3600000,"observed_at_ms":3600900,"balance_quota":50000000,
    "baseline_quota":50000000,"quota_per_credit":500000,"remaining_percent":100})")));
}

ASYNC_TEST("a token usage balance sample keeps the provider totals and no percentage") {
  const auto kv = std::make_shared<MeteredKv>();
  MeteredQuotaObservation input = observation();
  input.unlimited_quota = true;
  input.total_available = 9'000;
  input.total_granted = 10'000;
  input.total_used = 1'000;
  const MeteredQuotaState state = updateMeteredQuotaState(std::nullopt, input);
  rt::Task<void> writing = writeMeteredQuotaBalanceSample(kv, state, 7'200'000, std::string("fingerprint-a"));
  co_await std::move(writing);

  const rt::KvValue value = kv->value(rt::KvKey{METERED_QUOTA_BALANCE_HISTORY_PREFIX, "fingerprint-a", 7'200'000});
  EXPECT(value == kvOf(rt::jsonParse(R"({"v":1,"bucket_start_at_ms":7200000,"observed_at_ms":7200000,"balance_quota":0,"baseline_quota":0,
    "quota_per_credit":1,"remaining_percent":null,"unlimited_quota":true,"total_available":9000,"total_granted":10000,"total_used":1000})")));
}

ASYNC_TEST("a balance sample refuses an invalid clock or a missing account fingerprint") {
  const auto kv = std::make_shared<MeteredKv>();

  rt::Task<void> badClock = writeMeteredQuotaBalanceSample(kv, walletState(), -1, std::string("fingerprint-a"));
  co_await std::move(badClock);
  rt::Task<void> noFingerprint = writeMeteredQuotaBalanceSample(kv, walletState(), 3'600'000, std::nullopt);
  co_await std::move(noFingerprint);
  EXPECT_EQ(kv->stored.size(), std::size_t{0});
}

ASYNC_TEST("a balance sample never overwrites a newer observation in the same hour") {
  const auto kv = std::make_shared<MeteredKv>();
  const rt::KvKey key{METERED_QUOTA_BALANCE_HISTORY_PREFIX, "fingerprint-a", 3'600'000};
  MeteredQuotaBalanceSample newest = balanceSample();
  newest.observed_at_ms = 3'600'900;
  newest.balance_quota = 11;
  kv->seed(key, stored(newest));

  rt::Task<void> writing = writeMeteredQuotaBalanceSample(kv, walletState(), 3'600'500, std::string("fingerprint-a"));
  co_await std::move(writing);

  EXPECT(kv->value(key) == stored(newest));
}

ASYNC_TEST("a persistently contended balance history write fails loudly") {
  const auto kv = std::make_shared<MeteredKv>();
  const rt::KvKey key{METERED_QUOTA_BALANCE_HISTORY_PREFIX, "fingerprint-a", 3'600'000};
  kv->conflictOnSet(key);

  rt::Task<bool> rejecting =
    rejectsMatching(writeMeteredQuotaBalanceSample(kv, walletState(), 3'600'000, std::string("fingerprint-a")), "Metered quota balance history changed concurrently");
  const bool rejectingOk = co_await std::move(rejecting);
  EXPECT(rejectingOk);
}

ASYNC_TEST("a balance history read requires a KV and an account fingerprint") {
  ReadMeteredQuotaBalanceHistoryOptions withFingerprint{.sinceMs = 0, .nowMs = 10, .accountFingerprint = std::string("fingerprint-a")};
  rt::Task<std::vector<MeteredQuotaBalanceSample>> withoutKv = readMeteredQuotaBalanceHistory(nullptr, withFingerprint);
  const auto withoutKvValue = co_await std::move(withoutKv);
  EXPECT(withoutKvValue.empty());

  const auto kv = std::make_shared<MeteredKv>();
  ReadMeteredQuotaBalanceHistoryOptions withoutFingerprint{.sinceMs = 0, .nowMs = 10, .accountFingerprint = std::nullopt};
  rt::Task<std::vector<MeteredQuotaBalanceSample>> noAccount = readMeteredQuotaBalanceHistory(kv, withoutFingerprint);
  const auto noAccountValue = co_await std::move(noAccount);
  EXPECT(noAccountValue.empty());
}

ASYNC_TEST("a balance history read returns only this account's samples inside the window") {
  const auto kv = std::make_shared<MeteredKv>();
  const double bucket = METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS;
  for (const auto& [fingerprint, index] : std::vector<std::pair<std::string, double>>{{"fingerprint-a", 1}, {"fingerprint-a", 2}, {"fingerprint-a", 3}, {"fingerprint-b", 1}}) {
    MeteredQuotaBalanceSample sample = balanceSample();
    sample.bucket_start_at_ms = bucket * index;
    sample.observed_at_ms = bucket * index + 10;
    kv->seed(rt::KvKey{METERED_QUOTA_BALANCE_HISTORY_PREFIX, fingerprint, bucket * index}, stored(sample));
  }
  json::object invalid;
  invalid["v"] = 2;
  invalid["bucket_start_at_ms"] = rt::makeNumber(bucket * 4);
  kv->seed(rt::KvKey{METERED_QUOTA_BALANCE_HISTORY_PREFIX, "fingerprint-a", bucket * 4}, kvOf(invalid));

  ReadMeteredQuotaBalanceHistoryOptions options{.sinceMs = bucket + 1, .nowMs = bucket * 2 + 20, .accountFingerprint = std::string("fingerprint-a")};
  rt::Task<std::vector<MeteredQuotaBalanceSample>> reading = readMeteredQuotaBalanceHistory(kv, options);
  const std::vector<MeteredQuotaBalanceSample> samples = co_await std::move(reading);

  std::vector<double> observed;
  std::vector<double> buckets;
  for (const MeteredQuotaBalanceSample& sample : samples) {
    observed.push_back(sample.observed_at_ms);
    buckets.push_back(sample.bucket_start_at_ms);
  }
  EXPECT(observed == (std::vector<double>{bucket + 10, bucket * 2 + 10}));
  EXPECT(buckets == (std::vector<double>{bucket, bucket * 2}));
}

ASYNC_TEST("a balance history read honors the requested row limit") {
  const auto kv = std::make_shared<MeteredKv>();
  const double bucket = METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS;
  for (double index = 1; index <= 4; index += 1) {
    MeteredQuotaBalanceSample sample = balanceSample();
    sample.bucket_start_at_ms = bucket * index;
    sample.observed_at_ms = bucket * index + 5;
    kv->seed(rt::KvKey{METERED_QUOTA_BALANCE_HISTORY_PREFIX, "fingerprint-a", bucket * index}, stored(sample));
  }

  ReadMeteredQuotaBalanceHistoryOptions limitedOptions{.sinceMs = 0, .nowMs = bucket * 5, .accountFingerprint = std::string("fingerprint-a"), .limit = 2};
  rt::Task<std::vector<MeteredQuotaBalanceSample>> limitedRead = readMeteredQuotaBalanceHistory(kv, limitedOptions);
  const std::vector<MeteredQuotaBalanceSample> limited = co_await std::move(limitedRead);
  std::vector<double> buckets;
  for (const MeteredQuotaBalanceSample& sample : limited) buckets.push_back(sample.bucket_start_at_ms);
  EXPECT(buckets == (std::vector<double>{bucket, bucket * 2}));

  ReadMeteredQuotaBalanceHistoryOptions flooredOptions{.sinceMs = 0, .nowMs = bucket * 5, .accountFingerprint = std::string("fingerprint-a"), .limit = 0};
  rt::Task<std::vector<MeteredQuotaBalanceSample>> flooredRead = readMeteredQuotaBalanceHistory(kv, flooredOptions);
  const std::vector<MeteredQuotaBalanceSample> floored = co_await std::move(flooredRead);
  EXPECT_EQ(floored.size(), std::size_t{1});

  ReadMeteredQuotaBalanceHistoryOptions clampedOptions{.sinceMs = 0, .nowMs = bucket * 5, .accountFingerprint = std::string("fingerprint-a"), .limit = 5'000'000};
  rt::Task<std::vector<MeteredQuotaBalanceSample>> clampedRead = readMeteredQuotaBalanceHistory(kv, clampedOptions);
  const std::vector<MeteredQuotaBalanceSample> clamped = co_await std::move(clampedRead);
  EXPECT_EQ(clamped.size(), std::size_t{4});
}

TEST("resampling rejects unusable bucket or limit values") {
  const std::vector<MeteredQuotaBalanceSample> samples{balanceSample()};
  EXPECT(resampleMeteredQuotaBalanceHistory(samples, 0, 10).empty());
  EXPECT(resampleMeteredQuotaBalanceHistory(samples, -1, 10).empty());
  EXPECT(resampleMeteredQuotaBalanceHistory(samples, 1.5, 10).empty());
  EXPECT(resampleMeteredQuotaBalanceHistory(samples, 3'600'000, 0).empty());
  EXPECT(resampleMeteredQuotaBalanceHistory(samples, 3'600'000, -3).empty());
}

TEST("resampling keeps the newest observation in each bucket and the latest buckets") {
  const double hour = METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS;
  const auto sampleAt = [](double bucketStartAtMs, double observedAtMs, double balanceQuota) {
    MeteredQuotaBalanceSample sample = balanceSample();
    sample.bucket_start_at_ms = bucketStartAtMs;
    sample.observed_at_ms = observedAtMs;
    sample.balance_quota = balanceQuota;
    return sample;
  };
  const std::vector<MeteredQuotaBalanceSample> samples{
    sampleAt(hour, hour + 100, 1),
    sampleAt(hour, hour + 200, 2),
    sampleAt(hour * 2, hour * 2 + 100, 3),
    sampleAt(hour * 3, hour * 3 + 100, 4),
  };

  const std::vector<MeteredQuotaBalanceSample> resampled = resampleMeteredQuotaBalanceHistory(samples, hour, 2);
  std::vector<double> buckets;
  std::vector<double> balances;
  for (const MeteredQuotaBalanceSample& sample : resampled) {
    buckets.push_back(sample.bucket_start_at_ms);
    balances.push_back(sample.balance_quota);
  }
  EXPECT(buckets == (std::vector<double>{hour * 2, hour * 3}));
  EXPECT(balances == (std::vector<double>{3, 4}));

  const std::vector<MeteredQuotaBalanceSample> single = resampleMeteredQuotaBalanceHistory(samples, hour, 10);
  std::vector<double> singleBalances;
  for (const MeteredQuotaBalanceSample& sample : single) singleBalances.push_back(sample.balance_quota);
  EXPECT(singleBalances == (std::vector<double>{2, 3, 4}));
  EXPECT(!single.empty() && single[0].bucket_start_at_ms == hour);
  EXPECT(!single.empty() && single[0].observed_at_ms == hour + 200);
}

TEST("resampling breaks an observation-time tie by the source bucket timestamp") {
  const double hour = METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS;
  const double observedAt = hour * 5 + 30;
  MeteredQuotaBalanceSample newerBucket = balanceSample();
  newerBucket.bucket_start_at_ms = observedAt;
  newerBucket.observed_at_ms = observedAt;
  newerBucket.balance_quota = 1;
  MeteredQuotaBalanceSample olderBucket = balanceSample();
  olderBucket.bucket_start_at_ms = observedAt - 1;
  olderBucket.observed_at_ms = observedAt;
  olderBucket.balance_quota = 2;

  // The comparison runs against the normalized bucket start already stored, so
  // a later-iterated sample whose own bucket timestamp is above that normalized
  // start replaces the stored one. Both orders stay deterministic.
  const std::vector<MeteredQuotaBalanceSample> olderFirst = resampleMeteredQuotaBalanceHistory({olderBucket, newerBucket}, hour, 1);
  MeteredQuotaBalanceSample expectedOlderFirst = olderBucket;
  expectedOlderFirst.balance_quota = 1;
  expectedOlderFirst.bucket_start_at_ms = hour * 5;
  EXPECT(olderFirst.size() == 1 && olderFirst[0] == expectedOlderFirst);

  const std::vector<MeteredQuotaBalanceSample> newerFirst = resampleMeteredQuotaBalanceHistory({newerBucket, olderBucket}, hour, 1);
  MeteredQuotaBalanceSample expectedNewerFirst = olderBucket;
  expectedNewerFirst.bucket_start_at_ms = hour * 5;
  EXPECT(newerFirst.size() == 1 && newerFirst[0] == expectedNewerFirst);
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

ASYNC_TEST("diagnostics report an unconfigured Metered account without touching the network") {
  const rt::env::ScopedOverride restore{{std::string(METERED_API_KEY_ENV), std::string("")}};
  GetMeteredQuotaSnapshotOptions options;
  options.kv = std::shared_ptr<rt::Kv>();
  rt::Task<MeteredQuotaDiagnostics> reading = getMeteredQuotaDiagnostics(std::move(options));
  const MeteredQuotaDiagnostics diagnostics = co_await std::move(reading);
  EXPECT(sameJson(json::value_from(diagnostics), R"({"configured":false,"available":false,"cache_state":null,"confidence":null,"balance_credits":null,
    "baseline_credits":null,"remaining_percent":null,"used_percent":null,"observed_at_ms":null,"cycle_started_at_ms":null,"last_known_debits_credits":null,
    "last_inferred_credit_credits":null,"last_credit_at_ms":null,"latest_refill_id":null,"latest_refill_amount_credits":null,
    "latest_refill_completed_at_ms":null,"unlimited_quota":null,"total_available":null,"total_granted":null,"total_used":null})"));
}

ASYNC_TEST("diagnostics report a configured account with no reachable snapshot") {
  const rt::env::ScopedOverride restore{{std::string(METERED_API_KEY_ENV), credentials().apiKey}};
  GetMeteredQuotaSnapshotOptions options;
  options.kv = std::shared_ptr<rt::Kv>();
  rt::Task<MeteredQuotaDiagnostics> reading = getMeteredQuotaDiagnostics(std::move(options));
  const MeteredQuotaDiagnostics diagnostics = co_await std::move(reading);
  EXPECT_EQ(diagnostics.configured, true);
  EXPECT_EQ(diagnostics.available, false);
  EXPECT(!diagnostics.cache_state.has_value());
  EXPECT(diagnostics.total_used == nullptr);
}

ASYNC_TEST("diagnostics describe a wallet snapshot with confidence and debit history") {
  const rt::env::ScopedOverride restore{{std::string(METERED_API_KEY_ENV), credentials().apiKey}};
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 21'000'000;
  MeteredQuotaState state = walletState();
  state.observed_at_ms = now;
  state.current_balance_quota = 40'000'000;
  state.post_refill_baseline_quota = 50'000'000;
  state.last_known_debits_quota = 10'000'000;
  state.last_inferred_credit_quota = 5'000'000;
  state.last_credit_at_ms = now - 1'000;
  state.confidence = "inferred_adjustment";
  kv->seed(METERED_QUOTA_STATE_KEY, stored(state));

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.now = fixedNow(now);
  rt::Task<MeteredQuotaDiagnostics> reading = getMeteredQuotaDiagnostics(std::move(options));
  const MeteredQuotaDiagnostics diagnostics = co_await std::move(reading);

  EXPECT_EQ(diagnostics.configured, true);
  EXPECT_EQ(diagnostics.available, true);
  EXPECT_EQ(diagnostics.cache_state, std::optional<std::string>("fresh"));
  EXPECT_EQ(diagnostics.confidence, std::optional<std::string>("inferred_adjustment"));
  EXPECT_EQ(diagnostics.balance_credits, std::optional<double>(80));
  EXPECT_EQ(diagnostics.baseline_credits, std::optional<double>(100));
  EXPECT_EQ(diagnostics.remaining_percent, std::optional<double>(80));
  EXPECT_EQ(diagnostics.used_percent, std::optional<double>(20));
  EXPECT_EQ(diagnostics.observed_at_ms, std::optional<double>(now));
  EXPECT_EQ(diagnostics.cycle_started_at_ms, std::optional<double>(900'000));
  EXPECT_EQ(diagnostics.last_known_debits_credits, std::optional<double>(20));
  EXPECT_EQ(diagnostics.last_inferred_credit_credits, std::optional<double>(10));
  EXPECT_EQ(diagnostics.last_credit_at_ms, std::optional<double>(now - 1'000));
  EXPECT_EQ(diagnostics.latest_refill_id, std::optional<std::string>("refill-1"));
  EXPECT_EQ(diagnostics.latest_refill_amount_credits, std::optional<double>(100));
  EXPECT_EQ(diagnostics.latest_refill_completed_at_ms, std::optional<double>(900'000));
  EXPECT(diagnostics.unlimited_quota == rt::Nullable<bool>(false));
  EXPECT(diagnostics.total_available == nullptr);
  EXPECT(diagnostics.total_granted == nullptr);
  EXPECT(diagnostics.total_used == nullptr);
}

ASYNC_TEST("diagnostics describe a refreshed token usage snapshot without wallet fields") {
  const rt::env::ScopedOverride restore{{std::string(METERED_API_KEY_ENV), credentials().apiKey}};
  const auto kv = std::make_shared<MeteredKv>();
  const double now = 22'000'000;

  GetMeteredQuotaSnapshotOptions options = snapshotOptions(kv);
  options.now = fixedNow(now);
  options.fetcher = jsonFetcher(tokenUsageEnvelope({{"unlimited_quota", json::value(true)}, {"total_available", rt::makeNumber(9'000.0)}}));
  options.createLeaseOwner = owner("diagnostics-owner");
  rt::Task<MeteredQuotaDiagnostics> reading = getMeteredQuotaDiagnostics(std::move(options));
  const MeteredQuotaDiagnostics diagnostics = co_await std::move(reading);

  EXPECT_EQ(diagnostics.configured, true);
  EXPECT_EQ(diagnostics.available, true);
  EXPECT_EQ(diagnostics.cache_state, std::optional<std::string>("refreshed"));
  EXPECT(!diagnostics.confidence.has_value());
  EXPECT(!diagnostics.balance_credits.has_value());
  EXPECT(!diagnostics.baseline_credits.has_value());
  EXPECT(!diagnostics.remaining_percent.has_value());
  EXPECT(!diagnostics.used_percent.has_value());
  EXPECT_EQ(diagnostics.observed_at_ms, std::optional<double>(now));
  EXPECT(!diagnostics.cycle_started_at_ms.has_value());
  EXPECT(!diagnostics.last_known_debits_credits.has_value());
  EXPECT(!diagnostics.last_inferred_credit_credits.has_value());
  EXPECT(!diagnostics.last_credit_at_ms.has_value());
  EXPECT(diagnostics.unlimited_quota == rt::Nullable<bool>(true));
  EXPECT(diagnostics.total_available == rt::Nullable<double>(9'000.0));
  EXPECT(diagnostics.total_granted == rt::Nullable<double>(10'000.0));
  EXPECT(diagnostics.total_used == rt::Nullable<double>(1'000.0));
}

// ---------------------------------------------------------------------------
// Paid-fallback automatic backfill (src/paid-fallback/ledger-backfill.ts)
// ---------------------------------------------------------------------------

namespace {
  PaidFallbackRequestV3 settledRequestRow() {
    return PaidFallbackRequestV3{
      .v = 3,
      .key_id = "coverage-key",
      .request_id = "1",
      .policy_version = "v3",
      .route = "responses",
      .path = "/v1/responses",
      .model = "gpt-5-codex",
      .stream = true,
      .reasoning = std::string("high"),
      .provider = std::nullopt,
      .window_reset_at_ms = 60'000,
      .reserved_microcredits = 7,
      .quota_per_credit = 500'000,
      .provider_request_id = std::string("upstream-1"),
      .provider_quota = 12,
      .input_tokens = 100,
      .cached_input_tokens = 20.0,
      .output_tokens = 30,
      .dispatch_state = "dispatched",
      .terminal_state = "completed",
      .spend_microcredits = 7,
      .billing_state = "settled",
      .reconciliation_attempts = 1,
      .last_reconciliation_at_ms = 900,
      .dispatched_at_ms = 100,
      .terminal_at_ms = 200,
      .settled_at_ms = 300,
      .created_at_ms = 3'600'000,
      .updated_at_ms = 300,
    };
  }

  ledger_backfill::RunPaidFallbackBackfillV3Options runOptions(double nowMs, std::optional<bool> force = std::nullopt) {
    ledger_backfill::RunPaidFallbackBackfillV3Options options;
    options.nowMs = nowMs;
    options.force = force;
    return options;
  }

  rt::KvValue backfillState(double completedAtMs) { return kvOf(rt::jsonParse(R"({"v":1,"completed_at_ms":)" + rt::numberToString(completedAtMs) + "}")); }

  bool hasKeyContaining(const MeteredKv& kv, std::string_view fragment) {
    for (const auto& [encoded, entry] : kv.stored) {
      if (encoded.find(fragment) != std::string::npos) return true;
    }
    return false;
  }

  json::value automaticJson(const ledger_backfill::PaidFallbackAutomaticBackfillResult& result) { return json::value_from(result); }
}

ASYNC_TEST("the automatic backfill refuses an invalid clock") {
  const auto kv = std::make_shared<MeteredKv>();

  rt::Task<bool> negative = rejectsMatching(ledger_backfill::runPaidFallbackBackfillV3(kv, runOptions(-1)), "Paid fallback automatic backfill clock is invalid");
  const bool negativeOk = co_await std::move(negative);
  EXPECT(negativeOk);
  rt::Task<bool> notANumber =
    rejectsMatching(ledger_backfill::runPaidFallbackBackfillV3(kv, runOptions(rt::Number::NaN)), "Paid fallback automatic backfill clock is invalid");
  const bool notANumberOk = co_await std::move(notANumber);
  EXPECT(notANumberOk);
  EXPECT_EQ(kv->stored.size(), std::size_t{0});
}

ASYNC_TEST("a completed automatic backfill with no pending work is skipped") {
  const auto kv = std::make_shared<MeteredKv>();
  kv->seed(ledger_state::paidFallbackBackfillStateV3Key(), backfillState(5));

  rt::Task<ledger_backfill::PaidFallbackAutomaticBackfillResult> running = ledger_backfill::runPaidFallbackBackfillV3(kv, runOptions(10));
  const ledger_backfill::PaidFallbackAutomaticBackfillResult run = co_await std::move(running);

  EXPECT(sameJson(automaticJson(run), R"({"kind":"skipped","requests":null,"windows":null})"));
  EXPECT_EQ(kv->has(ledger_state::paidFallbackBackfillLeaseV3Key()), false);
  EXPECT_EQ(hasKeyContaining(*kv, "usage_rollup"), false);
}

ASYNC_TEST("a completed automatic backfill runs again when a settled row is unmarked") {
  const auto kv = std::make_shared<MeteredKv>();
  const double nowMs = 4'000'000;
  kv->seed(ledger_state::paidFallbackBackfillStateV3Key(), backfillState(5));
  const PaidFallbackRequestV3 row = settledRequestRow();
  const rt::KvKey requestKey = ledger_state::paidFallbackRequestV3Key(row.key_id, row.request_id);
  kv->seed(requestKey, stored(row));

  rt::Task<ledger_backfill::PaidFallbackAutomaticBackfillResult> running = ledger_backfill::runPaidFallbackBackfillV3(kv, runOptions(nowMs));
  const ledger_backfill::PaidFallbackAutomaticBackfillResult run = co_await std::move(running);

  EXPECT_EQ(run.kind, "completed");
  EXPECT(run.requests.has_value() && sameJson(json::value_from(*run.requests), R"({"scanned":1,"processed":1,"rollups_written":1,"failed":0,"truncated":false})"));
  EXPECT(run.windows.has_value() && sameJson(json::value_from(*run.windows), R"({"scanned":0,"rewritten":0,"truncated":false})"));
  EXPECT(kv->value(ledger_state::paidFallbackBackfillStateV3Key()) == backfillState(nowMs));

  const double bucketStartAtMs = rt::Math::floor(row.created_at_ms / rollups::PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS) * rollups::PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS;
  const rt::KvKey rollupKey = rollups::paidFallbackUsageRollupKey(bucketStartAtMs, row.model, "metered", rollups::paidFallbackUsageRollupShard(row.request_id));
  const json::value rollup = kv->value(rollupKey).toJson();
  EXPECT_EQ(rt::getNumber(rollup, "bucket_start_at_ms"), std::optional<double>(bucketStartAtMs));
  EXPECT(rt::getString(rollup, "model") == std::optional<std::string_view>(row.model));
  EXPECT(rt::getString(rollup, "provider") == std::optional<std::string_view>("metered"));
  EXPECT_EQ(rt::getNumber(rollup, "v"), std::optional<double>(1));
  EXPECT_EQ(rt::getNumber(rollup, "request_count"), std::optional<double>(1));
  EXPECT_EQ(rt::getNumber(rollup, "quota_sum"), std::optional<double>(12));
  EXPECT_EQ(rt::getNumber(rollup, "spend_microcredits"), std::optional<double>(7));
  EXPECT_EQ(rt::getNumber(rollup, "input_tokens"), std::optional<double>(100));
  EXPECT_EQ(rt::getNumber(rollup, "cached_input_tokens"), std::optional<double>(20));
  EXPECT_EQ(rt::getNumber(rollup, "output_tokens"), std::optional<double>(30));
  EXPECT_EQ(rt::getNumber(rollup, "first_request_at_ms"), std::optional<double>(row.created_at_ms));
  EXPECT_EQ(rt::getNumber(rollup, "last_request_at_ms"), std::optional<double>(row.created_at_ms));
  EXPECT_EQ(rt::getNumber(rollup, "updated_at_ms"), std::optional<double>(nowMs));

  const json::value rewritten = kv->value(requestKey).toJson();
  EXPECT_EQ(rt::getNumber(rewritten, "usage_rollup_at_ms"), std::optional<double>(nowMs));
  EXPECT_EQ(rt::getNumber(rewritten, "updated_at_ms"), std::optional<double>(nowMs));
  EXPECT_EQ(kv->has(ledger_state::paidFallbackBackfillLeaseV3Key()), false);
}

ASYNC_TEST("a held automatic backfill lease reports the run as busy") {
  const auto kv = std::make_shared<MeteredKv>();
  const double nowMs = 5'000'000;
  const PaidFallbackRequestV3 row = settledRequestRow();
  kv->seed(ledger_state::paidFallbackRequestV3Key(row.key_id, row.request_id), stored(row));
  kv->seed(ledger_state::paidFallbackBackfillLeaseV3Key(),
           kvOf(rt::jsonParse(R"({"token":"held-by-another-worker","expires_at_ms":)" + rt::numberToString(nowMs + 60'000) + "}")));

  rt::Task<ledger_backfill::PaidFallbackAutomaticBackfillResult> running = ledger_backfill::runPaidFallbackBackfillV3(kv, runOptions(nowMs));
  const ledger_backfill::PaidFallbackAutomaticBackfillResult run = co_await std::move(running);

  EXPECT(sameJson(automaticJson(run), R"({"kind":"busy","requests":null,"windows":null})"));
  EXPECT(rt::get(kv->value(ledger_state::paidFallbackRequestV3Key(row.key_id, row.request_id)).toJson(), "usage_rollup_at_ms") == nullptr);
}

ASYNC_TEST("a refused completion marker fails the automatic backfill loudly") {
  const auto kv = std::make_shared<MeteredKv>();
  const double nowMs = 6'000'000;
  kv->conflictOnSet(ledger_state::paidFallbackBackfillStateV3Key());

  rt::Task<bool> rejecting =
    rejectsMatching(ledger_backfill::runPaidFallbackBackfillV3(kv, runOptions(nowMs, true)), "Paid fallback automatic backfill state changed concurrently");
  const bool rejectingOk = co_await std::move(rejecting);
  EXPECT(rejectingOk);
  EXPECT_EQ(kv->has(ledger_state::paidFallbackBackfillLeaseV3Key()), false);
}

ASYNC_TEST("an automatic backfill with existing rollups and no marked work is skipped") {
  const auto kv = std::make_shared<MeteredKv>();
  kv->seed(ledger_state::paidFallbackBackfillStateV3Key(), backfillState(5));
  const rollups::PaidFallbackUsageRollup rollup = rollups::mergePaidFallbackUsageRollup(std::nullopt,
                                                                                       uos::authority::native_paid_contracts::PaidFallbackUsageRollupInput{
                                                                                         .bucket_start_at_ms = 0,
                                                                                         .model = "gpt-5-codex",
                                                                                         .provider = "metered",
                                                                                         .request_id = "1",
                                                                                         .quota = 1,
                                                                                         .input_tokens = 1,
                                                                                         .cached_input_tokens = std::nullopt,
                                                                                         .output_tokens = 1,
                                                                                         .spend_microcredits = 1,
                                                                                         .request_created_at_ms = 0,
                                                                                         .updated_at_ms = 5,
                                                                                       });
  kv->seed(rt::KvKey{rollups::PAID_FALLBACK_USAGE_ROLLUP_PREFIX, 0, "gpt-5-codex", "metered", 0}, stored(rollup));

  rt::Task<ledger_backfill::PaidFallbackAutomaticBackfillResult> running = ledger_backfill::runPaidFallbackBackfillV3(kv, runOptions(10));
  const ledger_backfill::PaidFallbackAutomaticBackfillResult run = co_await std::move(running);

  EXPECT(sameJson(automaticJson(run), R"({"kind":"skipped","requests":null,"windows":null})"));
}

ASYNC_TEST("a contended automatic backfill lease reports the run as busy") {
  const auto kv = std::make_shared<MeteredKv>();
  kv->conflictOnSet(ledger_state::paidFallbackBackfillLeaseV3Key());

  rt::Task<ledger_backfill::PaidFallbackAutomaticBackfillResult> running = ledger_backfill::runPaidFallbackBackfillV3(kv, runOptions(7'000'000, true));
  const ledger_backfill::PaidFallbackAutomaticBackfillResult run = co_await std::move(running);

  EXPECT(sameJson(automaticJson(run), R"({"kind":"busy","requests":null,"windows":null})"));
  EXPECT_EQ(kv->has(ledger_state::paidFallbackBackfillLeaseV3Key()), false);
}

ASYNC_TEST("an unreadable lease during release still reports the completed backfill") {
  const auto kv = std::make_shared<MeteredKv>();
  const double nowMs = 8'000'000;
  // Reads: 1 lease acquire probe, 2 release probe.
  kv->planReads(ledger_state::paidFallbackBackfillLeaseV3Key(), {"value", "throw"});

  rt::Task<ledger_backfill::PaidFallbackAutomaticBackfillResult> running = ledger_backfill::runPaidFallbackBackfillV3(kv, runOptions(nowMs, true));
  const ledger_backfill::PaidFallbackAutomaticBackfillResult run = co_await std::move(running);

  EXPECT_EQ(run.kind, "completed");
  EXPECT(kv->value(ledger_state::paidFallbackBackfillStateV3Key()) == backfillState(nowMs));
  EXPECT_EQ(kv->has(ledger_state::paidFallbackBackfillLeaseV3Key()), true);
}

ASYNC_TEST("a malformed reconciliation job message is dropped without touching the ledger") {
  const auto kv = std::make_shared<MeteredKv>();

  rt::Task<double> nullMessage = ledger_backfill::handlePaidFallbackReconciliationJobV3(rt::KvValue(nullptr), kv);
  const auto nullMessageValue = co_await std::move(nullMessage);
  EXPECT_EQ(nullMessageValue, 0.0);
  rt::Task<double> stringMessage = ledger_backfill::handlePaidFallbackReconciliationJobV3(rt::KvValue(std::string("key_id")), kv);
  const auto stringMessageValue = co_await std::move(stringMessage);
  EXPECT_EQ(stringMessageValue, 0.0);
  rt::Task<double> numericKey = ledger_backfill::handlePaidFallbackReconciliationJobV3(kvOf(rt::jsonParse(R"({"key_id":5})")), kv);
  const auto numericKeyValue = co_await std::move(numericKey);
  EXPECT_EQ(numericKeyValue, 0.0);
  EXPECT_EQ(kv->stored.size(), std::size_t{0});
}

ASYNC_TEST("enqueueing due reconciliation jobs requires an available KV") {
  const auto kv = std::make_shared<MeteredKv>();

  rt::Task<double> withoutKv = ledger_backfill::enqueueDuePaidFallbackReconciliationJobsV3(9'000'000, std::shared_ptr<rt::Kv>());
  const auto withoutKvValue = co_await std::move(withoutKv);
  EXPECT_EQ(withoutKvValue, 0.0);
  rt::Task<double> empty = ledger_backfill::enqueueDuePaidFallbackReconciliationJobsV3(9'000'000, kv);
  const auto emptyValue = co_await std::move(empty);
  EXPECT_EQ(emptyValue, 0.0);
  EXPECT_EQ(kv->stored.size(), std::size_t{0});
}
