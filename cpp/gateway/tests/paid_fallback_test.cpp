// Port of tests/paid-fallback.test.ts: every case, in the TS order.
// The TS file's MemoryKv fake is ported as MemoryKv below (rows keyed by JSON.stringify(key), counter
// versionstamps, expiry purged on access, sorted snapshot lists, the getCalls/listCalls/
// atomicCommitFailures counters, the one-shot beforeAtomicCommit hook and an enqueue that records
// messages or rejects with enqueueFailure); like the fake, each call settles one microtask after doing
// its work. One store serves every case, installed with setKvForTest before each case and removed
// after it, as the TS beforeEach/afterEach do. "concurrent paid fallback ledger patches retry without
// losing either update", ported first (wave 3), keeps its own wrapper: rt::MemoryKv (the store the fake
// stands in for) behind a forwarding wrapper that counts failed commits.
// createFetchMock(handler) is a scoped rt::GlobalFetchOverride; withMeteredApiKey is a scoped
// METERED_API_KEY. Credential isolation: every case that reaches the paid-fallback modules runs with
// HOME, CODEX_HOME, CODEX_AUTH_JSON_B64, METERED_API_KEY, OPENLUX_API_KEY and SURPLUS_API_KEY unset and
// config.codexAuthJsonB64 empty (the bun oracle ran under `env -i` with an empty HOME).
// C++-only step: the one-time paid-fallback backfill that an admission schedules runs on the case's
// loop, which dies with the case body, so every case that admits awaits it at its end
// (ledger_backfill::scheduledPaidFallbackBackfillForTest()). In the TS it runs on the shared event loop.
// Links: tests/paid_fallback_test.cpp src/analytics.cpp src/api_keys.cpp src/auth/relay.cpp src/authority/native_codex.cpp
//   src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
//   src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_contracts.cpp
//   src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp
//   src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp
//   src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp
//   src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp
//   src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp
//   src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
//   src/codex/routing_state.cpp src/config.cpp src/defaults.cpp src/http.cpp src/inference_deadline.cpp src/kv.cpp src/metered_quota.cpp
//   src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/optional_telemetry_queue.cpp
//   src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp
//   src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/rollups.cpp src/provider/anthropic_chat_responses.cpp
//   src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp
//   src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp
//   src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
//   src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
//   src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp
//   src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/metered.cpp src/provider/selection.cpp src/provider/surplus.cpp
//   src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/release.cpp src/responses_stream.cpp
//   src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp
//   src/runtime/websocket.cpp src/runtime_config.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
//   src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp src/utils.cpp
#include "uos/analytics.hpp"
#include "uos/api_keys.hpp"
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/paid_fallback/ledger_admission.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/types.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
using namespace uos::analytics;

namespace {
  // The TS fake's `atomicCommitFailures`: commits whose checks failed.
  class CountingOperation final : public rt::ForwardingAtomicOperation {
  public:
    CountingOperation(std::unique_ptr<rt::AtomicOperation> inner_, std::shared_ptr<int> failures_)
      : rt::ForwardingAtomicOperation(std::move(inner_)), failures(std::move(failures_)) {}

    rt::Task<rt::KvCommitResult> commit() override {
      rt::Task<rt::KvCommitResult> committing = rt::ForwardingAtomicOperation::commit();
      const rt::KvCommitResult result = co_await std::move(committing);
      if (!result.ok) *failures += 1;
      co_return result;
    }

  private:
    std::shared_ptr<int> failures;
  };

  class LedgerKv final : public rt::ForwardingKv {
  public:
    explicit LedgerKv(std::shared_ptr<rt::MemoryKv> store_) : rt::ForwardingKv(*store_), store(std::move(store_)) {}

    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<CountingOperation>(rt::ForwardingKv::atomic(), atomicCommitFailures); }

    int failures() const { return *atomicCommitFailures; }

  private:
    std::shared_ptr<rt::MemoryKv> store;
    std::shared_ptr<int> atomicCommitFailures = std::make_shared<int>(0);
  };

  std::shared_ptr<LedgerKv> clearedKv() {
    auto kv = std::make_shared<LedgerKv>(std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); }));
    uos::kv::setKvForTest(kv);
    return kv;
  }
}

namespace json = uos::json;
namespace index_ = uos::paid_fallback::index;
namespace ledger_admission = uos::paid_fallback::ledger_admission;
namespace ledger_backfill = uos::paid_fallback::ledger_backfill;
namespace ledger_state = uos::paid_fallback::ledger_state;
namespace contracts = uos::authority::native_paid_contracts;
using uos::api_keys::apiKeyHashKey;
using uos::api_keys::apiKeyIdKey;

// GCC 12.2 coroutine rule (PORTING.md): every awaited call is bound to a named Task first and its
// result to a named local before an assertion reads it.
#define AWAIT(name, expression)   \
  auto name##Task = (expression); \
  auto name = co_await std::move(name##Task)
// The same for a call whose result the case does not read.
#define AWAIT_DONE(name, expression) \
  auto name##Task = (expression);    \
  co_await std::move(name##Task)
// `assert.equal(actual, expected, message)`: EXPECT_EQ that also prints the TS message.
#define EXPECT_EQ_MESSAGE(actual, expected, message)                                                                                                      \
  do {                                                                                                                                                   \
    if (!((actual) == (expected)))                                                                                                                       \
      ::uos::rt::test::report(__FILE__, __LINE__,                                                                                                        \
                              std::string("expected " #actual " == " #expected " (") + (message) + ")\n      actual:   " + ::uos::rt::test::show(actual), false); \
  } while (0)

namespace {
  // ---- The TS file's MemoryKv ------------------------------------------------------------------
  // Rows keyed by `JSON.stringify(key)` in a Map, versionstamps from a counter padded to 20 digits
  // (delete takes one too), expiry purged with Date.now() before reads, list over a snapshot taken
  // at the call and sorted by the file's compareKeys, an enqueue that records clones or rejects with
  // `enqueueFailure`, the counters the cases read and a one-shot `beforeAtomicCommit` hook. Each call
  // does its work when it is made and settles one microtask later, as the fake's `Promise.resolve`
  // does. The fake has no getMany, sum, min, max or mutate: calling them throws TypeError, as calling
  // an undefined method does.

  // `encodeKey`: JSON.stringify(key).
  std::string encodeKey(const rt::KvKey& key) { return rt::kvJsonStringify(rt::kvValueFromKey(key)).value_or(std::string()); }

  // `compareKeyPart`: 0 for `===` parts, the difference of two numbers, otherwise String(part) in JS
  // `<` order.
  double compareKeyPart(const rt::KvKeyPart& left, const rt::KvKeyPart& right) {
    if (rt::kvKeyPartStrictEquals(left, right)) return 0;
    if (left.isNumber() && right.isNumber()) return left.number() - right.number();
    return rt::lessThan(left.toString(), right.toString()) ? -1 : 1;
  }

  double compareKeys(const rt::KvKey& left, const rt::KvKey& right) {
    const std::size_t length = std::max(left.size(), right.size());
    for (std::size_t index = 0; index < length; ++index) {
      if (index >= left.size()) return -1;
      if (index >= right.size()) return 1;
      const double comparison = compareKeyPart(left[index], right[index]);
      if (comparison != 0) return comparison;
    }
    return 0;
  }

  bool startsWithKey(const rt::KvKey& key, const rt::KvKey& prefix) {
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      if (index >= key.size() || !rt::kvKeyPartStrictEquals(key[index], prefix[index])) return false;
    }
    return true;
  }

  // The fake's list iterator: a generator over the snapshot (`cursor` is always "").
  class SnapshotIterator final : public rt::KvListIterator {
  public:
    explicit SnapshotIterator(std::vector<rt::KvEntry<>> entries_) : entries(std::move(entries_)) {}

    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      std::optional<rt::KvEntry<>> entry;
      if (!finished && index < entries.size()) entry = entries[index++];
      else finished = true;
      co_await rt::microtask();
      co_return entry;
    }
    std::string cursor() const override { return ""; }
    rt::Task<void> return_() override {
      finished = true;
      co_await rt::microtask();
    }

  private:
    std::vector<rt::KvEntry<>> entries;
    std::size_t index = 0;
    bool finished = false;
  };

  class MemoryKv final : public rt::Kv, public rt::KvEnqueue {
  public:
    struct StoredEntry {
      rt::KvKey key;
      rt::KvValue value;
      std::string versionstamp;
      std::optional<double> expiresAtMs;  // number | null
    };
    struct QueueMessage {
      rt::KvValue message;
      rt::KvEnqueueOptions options;
    };
    // One recorded atomic mutation: `{ type: "set", key, value, options }` or `{ type: "delete", key }`.
    struct Mutation {
      bool remove = false;
      rt::KvKey key;
      rt::KvValue value;
      std::optional<double> expireIn;
    };

    rt::OrderedMap<std::string, StoredEntry> entries;
    std::vector<QueueMessage> queueMessages;
    rt::Reason enqueueFailure;                  // Error | null (a null Reason is null)
    std::function<void()> beforeAtomicCommit;  // (() => void) | null
    int atomicCommitFailures = 0;
    int getCalls = 0;
    int listCalls = 0;

    using rt::Kv::get;
    using rt::Kv::getMany;

    void clear() {
      entries.clear();
      queueMessages.clear();
      enqueueFailure = nullptr;
      beforeAtomicCommit = nullptr;
      atomicCommitFailures = 0;
      getCalls = 0;
      listCalls = 0;
      version = 0;
    }

    std::optional<std::string> versionstamp(const rt::KvKey& key) {
      purgeExpired();
      const StoredEntry* entry = entries.get(encodeKey(key));
      if (entry == nullptr) return std::nullopt;
      return entry->versionstamp;
    }

    std::optional<double> expiration(const rt::KvKey& key) {
      purgeExpired();
      const StoredEntry* entry = entries.get(encodeKey(key));
      if (entry == nullptr) return std::nullopt;
      return entry->expiresAtMs;
    }

    // What `set` does before it returns its resolved promise (the hooks call it as `void memoryKv.set(...)`).
    rt::KvCommitResult setNow(const rt::KvKey& key, const rt::KvValue& value, rt::KvSetOptions options = {}) {
      const std::string stamp = nextVersionstamp();
      write(key, value, options.expireIn, stamp);
      return rt::KvCommitResult{.ok = true, .versionstamp = stamp};
    }

    // What `commit()` of the fake's atomic operation does before it returns its resolved promise.
    rt::KvCommitResult commitNow(const std::vector<rt::AtomicCheck>& checks, const std::vector<Mutation>& mutations) {
      if (beforeAtomicCommit) {
        const std::function<void()> hook = beforeAtomicCommit;
        hook();
      }
      beforeAtomicCommit = nullptr;
      purgeExpired();
      for (const rt::AtomicCheck& check : checks) {
        if (versionstamp(check.key) != check.versionstamp) {
          atomicCommitFailures += 1;
          return rt::KvCommitResult{.ok = false, .versionstamp = ""};
        }
      }
      const std::string stamp = nextVersionstamp();
      for (const Mutation& mutation : mutations) {
        if (mutation.remove) entries.delete_(encodeKey(mutation.key));
        else write(mutation.key, mutation.value, mutation.expireIn, stamp);
      }
      return rt::KvCommitResult{.ok = true, .versionstamp = stamp};
    }

    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions = {}) override {
      getCalls += 1;
      purgeExpired();
      rt::KvEntryMaybe<> result;
      result.key = key;
      result.value = rt::KvValue(nullptr);
      if (const StoredEntry* entry = entries.get(encodeKey(key))) {
        result.value = rt::kvStructuredClone(entry->value);
        result.versionstamp = entry->versionstamp;
      }
      co_await rt::microtask();
      co_return result;
    }

    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions = {}) override {
      throw rt::TypeError("kv.getMany is not a function");
      co_return std::vector<rt::KvEntryMaybe<>>();
    }

    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      const rt::KvCommitResult result = setNow(key, value, options);
      co_await rt::microtask();
      co_return result;
    }

    rt::Task<void> delete_(rt::KvKey key) override {
      purgeExpired();
      entries.delete_(encodeKey(key));
      nextVersionstamp();
      co_await rt::microtask();
    }

    rt::Task<void> enqueue(rt::KvValue message, rt::KvEnqueueOptions options) override {
      const rt::Reason failure = enqueueFailure;
      if (!failure) queueMessages.push_back(QueueMessage{.message = rt::kvStructuredClone(message), .options = std::move(options)});
      co_await rt::microtask();
      if (failure) std::rethrow_exception(failure);
    }

    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      listCalls += 1;
      purgeExpired();
      const rt::KvKey prefix = selector.prefix.value_or(rt::KvKey{});
      std::vector<const StoredEntry*> matched;
      for (const auto& [encoded, entry] : entries) {
        if (!startsWithKey(entry.key, prefix)) continue;
        if (selector.start && compareKeys(entry.key, *selector.start) < 0) continue;
        if (selector.end && compareKeys(entry.key, *selector.end) >= 0) continue;
        matched.push_back(&entry);
      }
      std::stable_sort(matched.begin(), matched.end(), [](const StoredEntry* left, const StoredEntry* right) { return compareKeys(left->key, right->key) < 0; });
      if (options.reverse) std::reverse(matched.begin(), matched.end());
      if (options.limit) {
        // `entries.slice(0, limit)`
        const double limit = std::isnan(*options.limit) ? 0 : std::trunc(*options.limit);
        const double size = static_cast<double>(matched.size());
        const double end = limit < 0 ? std::max(0.0, size + limit) : std::min(size, limit);
        matched.resize(static_cast<std::size_t>(end));
      }
      std::vector<rt::KvEntry<>> snapshot;
      for (const StoredEntry* entry : matched) snapshot.push_back(rt::KvEntry<>{entry->key, rt::kvStructuredClone(entry->value), entry->versionstamp});
      return std::make_unique<SnapshotIterator>(std::move(snapshot));
    }

    std::unique_ptr<rt::AtomicOperation> atomic() override;

    void close() override {}

  private:
    double version = 0;

    std::string nextVersionstamp() {
      version += 1;
      return rt::padStart(rt::numberToString(version), 20, "0");
    }

    void purgeExpired() {
      const double now = rt::dateNow();
      std::vector<std::string> expired;
      for (const auto& [encoded, entry] : entries) {
        if (entry.expiresAtMs && *entry.expiresAtMs <= now) expired.push_back(encoded);
      }
      for (const std::string& encoded : expired) entries.delete_(encoded);
    }

    void write(const rt::KvKey& key, const rt::KvValue& value, std::optional<double> expireIn, const std::string& stamp) {
      std::optional<double> expiresAtMs;
      if (expireIn) expiresAtMs = rt::dateNow() + rt::Math::max(0.0, *expireIn);
      entries.set(encodeKey(key), StoredEntry{.key = key, .value = rt::kvStructuredClone(value), .versionstamp = stamp, .expiresAtMs = expiresAtMs});
    }
  };

  class MemoryAtomicOperation final : public rt::AtomicOperation {
  public:
    explicit MemoryAtomicOperation(MemoryKv& kv_) : kv(kv_) {}
    using rt::AtomicOperation::check;
    using rt::AtomicOperation::max;
    using rt::AtomicOperation::min;
    using rt::AtomicOperation::sum;

    rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
      for (rt::AtomicCheck& item : items) checks.push_back(std::move(item));
      return *this;
    }
    rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("operation.mutate is not a function"); }
    rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.sum is not a function"); }
    rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.min is not a function"); }
    rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.max is not a function"); }
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      mutations.push_back(MemoryKv::Mutation{.remove = false, .key = std::move(key), .value = rt::kvStructuredClone(value), .expireIn = options.expireIn});
      return *this;
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override {
      mutations.push_back(MemoryKv::Mutation{.remove = true, .key = std::move(key), .value = rt::KvValue(), .expireIn = std::nullopt});
      return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override {
      const rt::KvCommitResult result = kv.commitNow(checks, mutations);
      co_await rt::microtask();
      co_return result;
    }

  private:
    MemoryKv& kv;
    std::vector<rt::AtomicCheck> checks;
    std::vector<MemoryKv::Mutation> mutations;
  };

  std::unique_ptr<rt::AtomicOperation> MemoryKv::atomic() { return std::make_unique<MemoryAtomicOperation>(*this); }

  // `const memoryKv = new MemoryKv(); const kv = memoryKv as unknown as Kv;`: one store for every case.
  const std::shared_ptr<MemoryKv>& store() {
    static const std::shared_ptr<MemoryKv> instance = std::make_shared<MemoryKv>();
    return instance;
  }
  MemoryKv& memoryKv() { return *store(); }
  std::shared_ptr<rt::Kv> kvHandle() { return store(); }
  // The `kv` argument of the `...(..., kv)` calls (`kvOverride?: Kv | null`).
  std::optional<std::shared_ptr<rt::Kv>> kvOverride() { return std::optional<std::shared_ptr<rt::Kv>>(kvHandle()); }

  // beforeEach `setKvForTest(kv)` / afterEach `setKvForTest(null)`, with the mandatory credential
  // isolation (no HOME, CODEX_HOME, CODEX_AUTH_JSON_B64 or seeded auth; the provider keys these
  // modules read unset, as in the `env -i` oracle run).
  struct Fixture {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt},           {"CODEX_HOME", std::nullopt},      {"CODEX_AUTH_JSON_B64", std::nullopt},
                                        {"METERED_API_KEY", std::nullopt}, {"OPENLUX_API_KEY", std::nullopt}, {"SURPLUS_API_KEY", std::nullopt}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    Fixture() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::kv::setKvForTest(kvHandle());
    }
    ~Fixture() {
      uos::kv::setKvForTest(nullptr);
      uos::config::config.codexAuthJsonB64 = savedSeed;
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
  };

  // `withMeteredApiKey(fn)`: METERED_API_KEY is "metered-test-key" for the scope.
  struct MeteredApiKey {
    rt::env::ScopedOverride key{{"METERED_API_KEY", std::string("metered-test-key")}};
  };

  // C++ only: the first admission in the process (or one after a run left work pending) schedules
  // the one-time backfill on the case's loop, which is destroyed with the body; the cases that admit
  // let it settle at their end (it never rejects).
  rt::Task<void> settleBackfill() {
    while (const std::optional<rt::Promise<void>> scheduled = ledger_backfill::scheduledPaidFallbackBackfillForTest()) co_await scheduled->wait();
  }

  // ---- fixtures of the TS file -----------------------------------------------------------------

  json::value number(double value) { return rt::makeNumber(value); }

  // `strictKeyRecord(overrides)`: `{ ...record, ...overrides }`.
  json::object strictKeyRecord(const json::object& overrides = {}) {
    const double now = rt::dateNow();
    json::object record;
    record["id"] = "key-paid";
    record["name"] = "Paid fallback test";
    record["prefix"] = "u_test";
    record["hash"] = "hash-paid";
    record["created_at_ms"] = number(now - 60'000);
    record["expires_at_ms"] = number(-1);
    record["revoked_at_ms"] = nullptr;
    record["usage_limit_requests"] = number(100);
    record["usage_requests"] = number(3);
    record["usage_reset_at_ms"] = number(now + 60 * 60 * 1000);
    record["window_ms"] = number(7 * 24 * 60 * 60 * 1000);
    record["paid_fallback_enabled"] = true;
    record["paid_fallback_limit_microcredits"] = number(5'000'000);
    record["paid_fallback_spent_microcredits"] = number(1'000'000);
    record["paid_fallback_reserved_microcredits"] = number(0);
    record["paid_fallback_reservation_request_id"] = nullptr;
    json::array models;
    models.push_back("gpt-5-codex");
    record["paid_fallback_model_ids"] = std::move(models);
    record["paid_fallback_quota_per_credit"] = number(500'000);
    json::object exposure;
    exposure["gpt-5-codex"] = number(250'000);
    record["paid_fallback_max_exposure_microcredits"] = std::move(exposure);
    record["paid_fallback_pricing_checked_at_ms"] = number(now - 30'000);
    for (const auto& member : overrides) rt::objectSet(record, member.key(), member.value());
    return record;
  }

  // `strictHashRecord(record)`
  json::object strictHashRecord(const json::object& record) {
    json::object hash;
    for (const std::string_view name : {"id", "expires_at_ms", "revoked_at_ms", "usage_limit_requests", "usage_requests", "usage_reset_at_ms", "window_ms",
                                        "paid_fallback_enabled", "paid_fallback_limit_microcredits", "paid_fallback_spent_microcredits",
                                        "paid_fallback_reserved_microcredits", "paid_fallback_reservation_request_id"}) {
      if (const json::value* member = rt::get(record, name)) hash[name] = *member;
    }
    return hash;
  }

  // `seedStrictKey(overrides)`
  rt::Task<json::object> seedStrictKey(json::object overrides) {
    const json::object record = strictKeyRecord(overrides);
    const std::string id(record.at("id").as_string());
    const std::string hash(record.at("hash").as_string());
    const rt::KvValue idRow(record);
    const rt::KvValue hashRow(strictHashRecord(record));
    AWAIT_DONE(idSet, memoryKv().set(apiKeyIdKey(id), idRow));
    AWAIT_DONE(hashSet, memoryKv().set(apiKeyHashKey(hash), hashRow));
    co_return record;
  }

  // `reservationInput(keyId, requestId, createdAtMs = Date.now())`
  index_::ReservePaidFallbackInput reservationInput(std::string keyId, std::string requestId, std::optional<double> createdAtMs = std::nullopt) {
    return index_::ReservePaidFallbackInput{
      .keyId = std::move(keyId),
      .requestId = std::move(requestId),
      .createdAtMs = createdAtMs.value_or(rt::dateNow()),
      .model = "gpt-5-codex",
      .route = "responses",
      .path = "/v1/responses",
      .stream = false,
      .reasoning = "high",
      .allowUnrosteredModel = std::nullopt,
      .reason = "primary_429",
    };
  }

  // `v3AdmissionInput(keyId, requestId)` without overrides; the cases assign their overrides.
  ledger_state::AdmissionInput v3AdmissionInput(std::string keyId, std::string requestId) {
    ledger_state::AdmissionInput input;
    input.keyId = std::move(keyId);
    input.requestId = std::move(requestId);
    input.createdAtMs = rt::dateNow();
    input.policyVersion = "policy-v3";
    input.limitMicrocredits = 1'000'000;
    input.maximumExposureMicrocredits = 250'000;
    input.initialSettledMicrocredits = 0;
    input.quotaPerCredit = 500'000;
    input.windowResetAtMs = rt::dateNow() + 60'000;
    input.model = "gpt-5-codex";
    input.route = "responses";
    input.path = "/v1/responses";
    input.stream = true;
    input.reasoning = "high";
    return input;
  }

  using AdmitResult = ledger_admission::AdmitPaidFallbackV3Result;

  std::string kindOf(const AdmitResult& decision) { return std::holds_alternative<ledger_admission::AdmitPaidFallbackV3ResultReserved>(decision) ? "reserved" : "blocked"; }

  std::string kindOf(const index_::PaidFallbackReservationDecision& decision) {
    if (std::holds_alternative<index_::PaidFallbackReservationDecisionReserved>(decision)) return "reserved";
    if (std::holds_alternative<index_::PaidFallbackReservationDecisionSkip>(decision)) return "skip";
    return "blocked";
  }

  std::vector<std::string> kindsOf(const std::vector<AdmitResult>& decisions) {
    std::vector<std::string> kinds;
    for (const AdmitResult& decision : decisions) kinds.push_back(kindOf(decision));
    return kinds;
  }

  // `decision.reservation` (the cases assert the kind first).
  contracts::PaidFallbackAdmissionV3 reservationOf(const AdmitResult& decision) { return std::get<ledger_admission::AdmitPaidFallbackV3ResultReserved>(decision).reservation; }
  index_::PaidFallbackReservation reservationOf(const index_::PaidFallbackReservationDecision& decision) {
    return std::get<index_::PaidFallbackReservationDecisionReserved>(decision).reservation;
  }

  // The decision as the TS object `assert.deepEqual` compares.
  json::value decisionJson(const AdmitResult& decision) {
    json::object out;
    if (const auto* reserved = std::get_if<ledger_admission::AdmitPaidFallbackV3ResultReserved>(&decision)) {
      out["kind"] = "reserved";
      out["reservation"] = json::value_from(reserved->reservation);
    } else {
      out["kind"] = "blocked";
      out["reason"] = json::string(std::get<ledger_admission::AdmitPaidFallbackV3ResultBlocked>(decision).reason);
    }
    return out;
  }

  json::value decisionJson(const index_::PaidFallbackReservationDecision& decision) {
    json::object out;
    if (const auto* reserved = std::get_if<index_::PaidFallbackReservationDecisionReserved>(&decision)) {
      out["kind"] = "reserved";
      out["reservation"] = json::value_from(reserved->reservation);
    } else if (const auto* skip = std::get_if<index_::PaidFallbackReservationDecisionSkip>(&decision)) {
      out["kind"] = "skip";
      out["reason"] = json::string(skip->reason);
    } else {
      const auto& blocked = std::get<index_::PaidFallbackReservationDecisionBlocked>(decision);
      out["kind"] = "blocked";
      out["reason"] = json::string(blocked.reason);
      out["reset_at_ms"] = blocked.reset_at_ms ? number(*blocked.reset_at_ms) : json::value(nullptr);
    }
    return out;
  }

  json::value deletionJson(const ledger_admission::PaidFallbackDeletionV3& deletion) {
    json::object out;
    if (const auto* deleted = std::get_if<ledger_admission::PaidFallbackDeletionV3Deleted>(&deletion)) {
      out["kind"] = "deleted";
      out["deleted_requests"] = number(deleted->deleted_requests);
      out["deleted_windows"] = number(deleted->deleted_windows);
      out["deleted_pending"] = number(deleted->deleted_pending);
      out["deleted_leases"] = number(deleted->deleted_leases);
    } else if (const auto* blocked = std::get_if<ledger_admission::PaidFallbackDeletionV3Blocked>(&deletion)) {
      out["kind"] = "blocked";
      out["outstanding"] = json::value_from(blocked->outstanding);
    } else {
      out["kind"] = "unavailable";
    }
    return out;
  }

  json::value outstandingJson(const std::optional<contracts::PaidFallbackOutstandingV3>& outstanding) {
    return outstanding ? json::value_from(*outstanding) : json::value(nullptr);
  }

  json::value deleted(double requests, double windows, double pending, double leases) {
    return deletionJson(ledger_admission::PaidFallbackDeletionV3Deleted{
      .deleted_requests = requests, .deleted_windows = windows, .deleted_pending = pending, .deleted_leases = leases});
  }

  json::value outstanding(double pendingRequests, double unresolvedRequests, double pendingMarkers, bool hasOutstanding) {
    return json::value_from(contracts::PaidFallbackOutstandingV3{
      .pending_requests = pendingRequests, .unresolved_requests = unresolvedRequests, .pending_markers = pendingMarkers, .has_outstanding = hasOutstanding});
  }

  json::value blockedDecision(std::string_view reason) {
    json::object out;
    out["kind"] = "blocked";
    out["reason"] = json::string(reason);
    return out;
  }

  // `{ next_due_at_ms: value }`
  rt::KvValue gateRow(std::optional<double> nextDueAtMs) {
    rt::KvObject gate;
    gate.set("next_due_at_ms", nextDueAtMs ? rt::KvValue(*nextDueAtMs) : rt::KvValue(nullptr));
    return rt::KvValue(std::move(gate));
  }

  // `{ created_at_ms, next_reconciliation_at_ms }`
  rt::KvValue pendingRow(double createdAtMs, double nextReconciliationAtMs) {
    rt::KvObject pending;
    pending.set("created_at_ms", rt::KvValue(createdAtMs));
    pending.set("next_reconciliation_at_ms", rt::KvValue(nextReconciliationAtMs));
    return rt::KvValue(std::move(pending));
  }

  // `typeof value`
  std::string typeOf(const rt::KvValue& value) { return std::string(rt::kvTypeof(value)); }

  // `Number(value)` for what these rows hold: a number, null (0), undefined (NaN), a boolean.
  double numberOf(const rt::KvValue& value) {
    if (value.isNumber()) return value.asNumber();
    if (value.isNull()) return 0;
    if (value.isBool()) return value.as<bool>() ? 1 : 0;
    return std::numeric_limits<double>::quiet_NaN();
  }

  // `lifecycle patch` of updatePaidFallbackRequestV3
  contracts::PaidFallbackRequestLifecyclePatchV3 dispatchedPatch(std::optional<std::string> providerRequestId) {
    contracts::PaidFallbackRequestLifecyclePatchV3 patch;
    if (providerRequestId) patch.provider_request_id = *providerRequestId;
    patch.dispatch_state = "dispatched";
    return patch;
  }

  // ---- `createFetchMock` handlers ----------------------------------------------------------------

  // A Metered token-log entry: `{ request_id, quota, prompt_tokens, completion_tokens, model_name,
  // created_at: Math.trunc(Date.now() / 1_000) }`, taken when the handler runs.
  json::object meteredLog(std::string_view requestId, double quota, double promptTokens, double completionTokens) {
    json::object log;
    log["request_id"] = json::string(requestId);
    log["quota"] = number(quota);
    log["prompt_tokens"] = number(promptTokens);
    log["completion_tokens"] = number(completionTokens);
    log["model_name"] = "gpt-5-codex";
    log["created_at"] = number(std::trunc(rt::dateNow() / 1'000));
    return log;
  }

  // `Response.json({ success: true, data })`
  rt::Response meteredLogs(json::array data) {
    json::object body;
    body["success"] = true;
    body["data"] = std::move(data);
    return rt::Response::json(body);
  }

  rt::Task<rt::Response> rejectedFetch(std::string message) {
    throw rt::JsError(message);
    co_return rt::Response();
  }

  // `createFetchMock(() => { calls += 1; return Promise.reject(new Error(message)); })`
  rt::Fetch rejectingFetch(std::shared_ptr<int> calls, std::string message) {
    return rt::Fetch([calls, message](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
      *calls += 1;
      return rejectedFetch(message);
    });
  }

  // `createFetchMock(() => Promise.resolve(Response.json({ success: true, data: entries() })))`
  rt::Fetch logsFetch(std::function<json::array()> entries) {
    return rt::Fetch([entries](rt::RequestInfo, rt::RequestInit) { return meteredLogs(entries()); });
  }
}

TEST("strict paid fallback policy accepts valid disabled history and unlimited reservation state") {
  json::object disabledOverrides;
  disabledOverrides["paid_fallback_enabled"] = false;
  disabledOverrides["paid_fallback_limit_microcredits"] = number(5'000'000);
  disabledOverrides["paid_fallback_spent_microcredits"] = number(1'250'000);
  disabledOverrides["paid_fallback_reserved_microcredits"] = number(0);
  disabledOverrides["paid_fallback_reservation_request_id"] = nullptr;
  const json::value disabledHistory = strictKeyRecord(disabledOverrides);
  EXPECT_EQ(index_::hasStrictPaidFallbackPolicy(disabledHistory), true);
  EXPECT_EQ(index_::hasStrictPaidFallbackKeyPolicy(disabledHistory), true);

  json::object neverEnabledOverrides;
  neverEnabledOverrides["paid_fallback_enabled"] = false;
  neverEnabledOverrides["paid_fallback_limit_microcredits"] = number(0);
  neverEnabledOverrides["paid_fallback_spent_microcredits"] = number(0);
  neverEnabledOverrides["paid_fallback_reserved_microcredits"] = number(0);
  neverEnabledOverrides["paid_fallback_reservation_request_id"] = nullptr;
  neverEnabledOverrides["paid_fallback_model_ids"] = json::array();
  neverEnabledOverrides["paid_fallback_quota_per_credit"] = number(0);
  neverEnabledOverrides["paid_fallback_pricing_checked_at_ms"] = nullptr;
  const json::value neverEnabled = strictKeyRecord(neverEnabledOverrides);
  EXPECT_EQ(index_::hasStrictPaidFallbackKeyPolicy(neverEnabled), true);

  json::object unlimitedOverrides;
  unlimitedOverrides["paid_fallback_limit_microcredits"] = number(-1);
  unlimitedOverrides["paid_fallback_spent_microcredits"] = number(50'000'000);
  unlimitedOverrides["paid_fallback_reserved_microcredits"] = number(0);
  unlimitedOverrides["paid_fallback_reservation_request_id"] = "request-unlimited";
  const json::value unlimitedReservation = strictKeyRecord(unlimitedOverrides);
  EXPECT_EQ(index_::hasStrictPaidFallbackPolicy(unlimitedReservation), true);
  EXPECT_EQ(index_::hasStrictPaidFallbackKeyPolicy(unlimitedReservation), true);
}

TEST("strict paid fallback policy rejects invalid values but ignores legacy counter relationships") {
  const double unsafe = rt::Number::MAX_SAFE_INTEGER + 1;
  const auto single = [](std::string_view key, json::value value) {
    json::object overrides;
    overrides[key] = std::move(value);
    return overrides;
  };
  const std::vector<json::object> invalidCommonPolicies{
    single("paid_fallback_spent_microcredits", number(-1)),
    single("paid_fallback_spent_microcredits", number(unsafe)),
    single("paid_fallback_reserved_microcredits", number(-1)),
    single("paid_fallback_reserved_microcredits", number(unsafe)),
    single("paid_fallback_limit_microcredits", number(0)),
    single("paid_fallback_limit_microcredits", number(-2)),
  };
  for (const json::object& overrides : invalidCommonPolicies) {
    EXPECT_EQ_MESSAGE(index_::hasStrictPaidFallbackPolicy(json::value(strictKeyRecord(overrides))), false, rt::jsonStringify(overrides));
  }

  std::vector<json::object> ignoredLegacyCounterRelationships;
  {
    json::object overrides;
    overrides["paid_fallback_reserved_microcredits"] = number(1);
    overrides["paid_fallback_reservation_request_id"] = nullptr;
    ignoredLegacyCounterRelationships.push_back(std::move(overrides));
  }
  {
    json::object overrides;
    overrides["paid_fallback_limit_microcredits"] = number(1'000'000);
    overrides["paid_fallback_spent_microcredits"] = number(900'000);
    overrides["paid_fallback_reserved_microcredits"] = number(100'001);
    overrides["paid_fallback_reservation_request_id"] = "request-over-cap";
    ignoredLegacyCounterRelationships.push_back(std::move(overrides));
  }
  {
    json::object overrides;
    overrides["paid_fallback_reserved_microcredits"] = number(0);
    overrides["paid_fallback_reservation_request_id"] = "request-empty-bounded-reservation";
    ignoredLegacyCounterRelationships.push_back(std::move(overrides));
  }
  {
    json::object overrides;
    overrides["paid_fallback_enabled"] = false;
    overrides["paid_fallback_reserved_microcredits"] = number(0);
    overrides["paid_fallback_reservation_request_id"] = "request-disabled";
    ignoredLegacyCounterRelationships.push_back(std::move(overrides));
  }
  {
    json::object overrides;
    overrides["paid_fallback_limit_microcredits"] = number(-1);
    overrides["paid_fallback_reserved_microcredits"] = number(1);
    overrides["paid_fallback_reservation_request_id"] = "request-unlimited-with-amount";
    ignoredLegacyCounterRelationships.push_back(std::move(overrides));
  }
  for (const json::object& overrides : ignoredLegacyCounterRelationships) {
    EXPECT_EQ_MESSAGE(index_::hasStrictPaidFallbackPolicy(json::value(strictKeyRecord(overrides))), true, rt::jsonStringify(overrides));
  }

  const std::vector<json::object> invalidEnabledKeyPolicies{
    single("paid_fallback_model_ids", json::array()),
    single("paid_fallback_quota_per_credit", number(0)),
    single("paid_fallback_quota_per_credit", number(unsafe)),
    single("paid_fallback_pricing_checked_at_ms", nullptr),
    single("paid_fallback_pricing_checked_at_ms", number(0)),
    single("paid_fallback_pricing_checked_at_ms", number(unsafe)),
  };
  for (const json::object& overrides : invalidEnabledKeyPolicies) {
    EXPECT_EQ_MESSAGE(index_::hasStrictPaidFallbackKeyPolicy(json::value(strictKeyRecord(overrides))), false, rt::jsonStringify(overrides));
  }
}

ASYNC_TEST("concurrent paid fallback ledger patches retry without losing either update") {
  const std::shared_ptr<LedgerKv> memoryKv = clearedKv();
  const std::shared_ptr<rt::Kv> kv = memoryKv;
  const std::string keyId = "key-concurrent-ledger";
  const std::string requestId = "request-concurrent-ledger";
  const double createdAtMs = rt::dateNow();

  ApiKeyRequestLogInput input{
    .id = requestId,
    .route = "responses",
    .path = "/v1/responses",
    .method = "POST",
    .status_code = 0,
    .stream = false,
    .created_at_ms = createdAtMs,
    .provider = "metered",
    .billing_status = "pending",
  };
  rt::Task<void> recording = recordApiKeyRequestLog(keyId, std::move(input), kv);
  co_await std::move(recording);

  ApiKeyRequestLogPatch providerPatch;
  providerPatch.provider_request_id = "provider-concurrent";
  ApiKeyRequestLogPatch completionPatch;
  completionPatch.status_code = 200;
  completionPatch.completed_at_ms = createdAtMs + 50;
  std::vector<rt::Promise<void>> updates;
  updates.emplace_back(updateApiKeyRequestLog(keyId, createdAtMs, requestId, providerPatch, kv));
  updates.emplace_back(updateApiKeyRequestLog(keyId, createdAtMs, requestId, completionPatch, kv));
  const auto both = rt::all(std::move(updates));
  co_await both.wait();

  EXPECT_EQ(memoryKv->failures(), 1);
  const rt::KvKey key = apiKeyRequestLogKey(keyId, createdAtMs, requestId);
  const rt::KvEntryMaybe<> stored = co_await memoryKv->get(key);
  EXPECT(stored.value["provider_request_id"] == rt::KvValue("provider-concurrent"));
  EXPECT(stored.value["status_code"] == rt::KvValue(200));
  EXPECT(stored.value["completed_at_ms"] == rt::KvValue(createdAtMs + 50));
  uos::kv::setKvForTest(nullptr);
}

ASYNC_TEST("V3 admits concurrent bounded requests without a single reservation slot") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-concurrent";
  const double resetAtMs = rt::dateNow() + 60'000;
  std::vector<rt::Promise<AdmitResult>> admissions;
  for (int index = 0; index < 100; ++index) {
    ledger_state::AdmissionInput input;
    input.keyId = keyId;
    input.requestId = "request-" + std::to_string(index);
    input.createdAtMs = rt::dateNow();
    input.policyVersion = "policy-v3";
    input.limitMicrocredits = 1'000'000;
    input.maximumExposureMicrocredits = 10'000;
    input.initialSettledMicrocredits = 0;
    input.quotaPerCredit = 500'000;
    input.windowResetAtMs = resetAtMs;
    input.model = "gpt-5-codex";
    input.route = "responses";
    input.path = "/v1/responses";
    input.stream = true;
    input.reasoning = "high";
    admissions.emplace_back(ledger_admission::admitPaidFallbackV3(std::move(input)));
  }
  const rt::Promise<std::vector<AdmitResult>> admitting = rt::all(std::move(admissions));
  const std::vector<AdmitResult> requests = co_await admitting.wait();
  EXPECT_EQ(kindsOf(requests), std::vector<std::string>(100, "reserved"));
  AWAIT(window, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, resetAtMs)));
  EXPECT_EQ(window.value["reserved_microcredits"], rt::KvValue(1'000'000));
  EXPECT_EQ(window.value["pending_count"], rt::KvValue(100));
  std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
  for (int index = 0; index < 100; ++index) reads.emplace_back(memory.get(ledger_state::paidFallbackRequestV3Key(keyId, "request-" + std::to_string(index))));
  const rt::Promise<std::vector<rt::KvEntryMaybe<>>> reading = rt::all(std::move(reads));
  const std::vector<rt::KvEntryMaybe<>> rows = co_await reading.wait();
  EXPECT_EQ(std::count_if(rows.begin(), rows.end(), [](const rt::KvEntryMaybe<>& entry) { return !entry.value.isNull(); }), 100);
  AWAIT(pending, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, "request-0")));
  EXPECT_EQ(typeOf(pending.value["next_reconciliation_at_ms"]), "number");
  co_await settleBackfill();
}

ASYNC_TEST("V3 idle reconciliation gate avoids pending scans and Metered billing reads") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  AWAIT_DONE(gateSet, memory.set(ledger_state::paidFallbackReconciliationGateV3Key(), gateRow(std::nullopt)));
  const auto fetchCalls = std::make_shared<int>(0);
  {
    const rt::GlobalFetchOverride fetch(rejectingFetch(fetchCalls, "idle reconciliation must not contact Metered"));
    AWAIT(reconciled, ledger_backfill::reconcileDuePaidFallbacksV3(rt::dateNow(), kvOverride()));
    EXPECT_EQ(reconciled, 0.0);
    EXPECT_EQ(memory.getCalls, 1);
    EXPECT_EQ(memory.listCalls, 0);
    EXPECT_EQ(*fetchCalls, 0);
    AWAIT(gate, memory.get(ledger_state::paidFallbackReconciliationGateV3Key()));
    EXPECT_EQ(gate.value, gateRow(std::nullopt));
  }
}

ASYNC_TEST("V3 reconciliation gate arms at dispatch boundaries and clears after settlement") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string providerRequestId = "provider-gate-dispatch";
  const auto fetchCalls = std::make_shared<int>(0);
  {
    const rt::GlobalFetchOverride fetch(rt::Fetch([fetchCalls, providerRequestId](rt::RequestInfo, rt::RequestInit) {
      *fetchCalls += 1;
      json::array data;
      data.push_back(meteredLog(providerRequestId, 10'000, 1, 2));
      return meteredLogs(std::move(data));
    }));
    const MeteredApiKey metered;
    ledger_state::AdmissionInput dispatchInput = v3AdmissionInput("gate-dispatch", "gate-dispatch-request");
    dispatchInput.dispatchIntent = true;
    dispatchInput.limitMicrocredits = -1;
    dispatchInput.maximumExposureMicrocredits = std::nullopt;
    AWAIT(admission, ledger_admission::admitPaidFallbackV3(std::move(dispatchInput)));
    ASSERT_EQ(kindOf(admission), "reserved");

    AWAIT(armed, memory.get(ledger_state::paidFallbackReconciliationGateV3Key()));
    EXPECT_EQ(typeOf(armed.value["next_due_at_ms"]), "number");

    AWAIT_DONE(dispatched, ledger_admission::updatePaidFallbackRequestV3(reservationOf(admission), dispatchedPatch(providerRequestId)));
    AWAIT(reconciled, ledger_backfill::reconcileDuePaidFallbacksV3(rt::dateNow() + 1'000, kvOverride()));
    EXPECT_EQ(reconciled, 1.0);
    EXPECT_EQ(*fetchCalls, 1);
    AWAIT(cleared, memory.get(ledger_state::paidFallbackReconciliationGateV3Key()));
    EXPECT_EQ(cleared.value, gateRow(std::nullopt));

    ledger_state::AdmissionInput transitionInput = v3AdmissionInput("gate-transition", "gate-transition-request");
    transitionInput.limitMicrocredits = -1;
    transitionInput.maximumExposureMicrocredits = std::nullopt;
    AWAIT(transition, ledger_admission::admitPaidFallbackV3(std::move(transitionInput)));
    ASSERT_EQ(kindOf(transition), "reserved");

    AWAIT(stillIdle, memory.get(ledger_state::paidFallbackReconciliationGateV3Key()));
    EXPECT_EQ(stillIdle.value, gateRow(std::nullopt));
    AWAIT_DONE(transitioned, ledger_admission::updatePaidFallbackRequestV3(reservationOf(transition), dispatchedPatch(providerRequestId)));
    AWAIT(rearmed, memory.get(ledger_state::paidFallbackReconciliationGateV3Key()));
    EXPECT_EQ(typeOf(rearmed.value["next_due_at_ms"]), "number");
  }
  co_await settleBackfill();
}

ASYNC_TEST("V3 missing reconciliation gate bootstraps the earliest legacy marker") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "gate-legacy";
  const std::string requestId = "gate-legacy-request";
  AWAIT(admission, ledger_admission::admitPaidFallbackV3(v3AdmissionInput(keyId, requestId)));
  ASSERT_EQ(kindOf(admission), "reserved");
  const double dueAtMs = rt::dateNow() + 60'000;
  AWAIT_DONE(pendingSet, memory.set(ledger_state::paidFallbackPendingV3Key(keyId, requestId), pendingRow(rt::dateNow(), dueAtMs)));
  const auto fetchCalls = std::make_shared<int>(0);
  {
    const rt::GlobalFetchOverride fetch(rejectingFetch(fetchCalls, "future legacy work must not contact Metered"));
    AWAIT(reconciled, ledger_backfill::reconcileDuePaidFallbacksV3(rt::dateNow(), kvOverride()));
    EXPECT_EQ(reconciled, 0.0);
    EXPECT_EQ(memory.listCalls, 1);
    EXPECT_EQ(*fetchCalls, 0);
    AWAIT(gate, memory.get(ledger_state::paidFallbackReconciliationGateV3Key()));
    EXPECT_EQ(gate.value, gateRow(dueAtMs));
  }
  co_await settleBackfill();
}

ASYNC_TEST("V3 concurrent dispatch-intent admissions retain every reservation and gate arm") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  std::vector<rt::Promise<AdmitResult>> admissions;
  for (int index = 0; index < 32; ++index) {
    ledger_state::AdmissionInput input = v3AdmissionInput("gate-concurrent", "gate-concurrent-" + std::to_string(index));
    input.limitMicrocredits = -1;
    input.maximumExposureMicrocredits = std::nullopt;
    input.dispatchIntent = true;
    admissions.emplace_back(ledger_admission::admitPaidFallbackV3(std::move(input)));
  }
  const rt::Promise<std::vector<AdmitResult>> admitting = rt::all(std::move(admissions));
  const std::vector<AdmitResult> decisions = co_await admitting.wait();
  EXPECT_EQ(kindsOf(decisions), std::vector<std::string>(32, "reserved"));
  std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
  for (int index = 0; index < 32; ++index) reads.emplace_back(memory.get(ledger_state::paidFallbackRequestV3Key("gate-concurrent", "gate-concurrent-" + std::to_string(index))));
  const rt::Promise<std::vector<rt::KvEntryMaybe<>>> reading = rt::all(std::move(reads));
  const std::vector<rt::KvEntryMaybe<>> requests = co_await reading.wait();
  EXPECT_EQ(std::count_if(requests.begin(), requests.end(), [](const rt::KvEntryMaybe<>& entry) { return !entry.value.isNull(); }), 32);
  AWAIT(gate, memory.get(ledger_state::paidFallbackReconciliationGateV3Key()));
  EXPECT_EQ(typeOf(gate.value["next_due_at_ms"]), "number");
  co_await settleBackfill();
}

ASYNC_TEST("paid fallback admission re-reads policy when a committed disable races its CAS") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  json::object seedOverrides;
  seedOverrides["id"] = "key-policy-cas-barrier";
  seedOverrides["hash"] = "hash-policy-cas-barrier";
  AWAIT(record, seedStrictKey(seedOverrides));
  const std::string keyId(record.at("id").as_string());
  const std::string requestId = "request-policy-cas-barrier";
  const auto policyMutationApplied = std::make_shared<bool>(false);
  json::object disabled = record;
  disabled["paid_fallback_enabled"] = false;
  disabled["paid_fallback_limit_microcredits"] = number(0);
  disabled["paid_fallback_model_ids"] = json::array();
  disabled["paid_fallback_quota_per_credit"] = number(0);
  disabled["paid_fallback_max_exposure_microcredits"] = json::object();
  disabled["paid_fallback_pricing_checked_at_ms"] = nullptr;
  memory.beforeAtomicCommit = [policyMutationApplied, keyId, disabled] {
    *policyMutationApplied = true;
    memoryKv().setNow(apiKeyIdKey(keyId), rt::KvValue(disabled));
  };

  {
    const MeteredApiKey metered;
    AWAIT(decision, index_::reservePaidFallback(reservationInput(keyId, requestId)));
    json::object skip;
    skip["kind"] = "skip";
    skip["reason"] = "disabled";
    EXPECT_EQ(decisionJson(decision), json::value(skip));
  }

  EXPECT_EQ(*policyMutationApplied, true);
  AWAIT(request, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, requestId)));
  EXPECT_EQ(request.value, rt::KvValue(nullptr));
  AWAIT(policy, memory.get(apiKeyIdKey(keyId)));
  EXPECT_EQ(policy.value["paid_fallback_enabled"], rt::KvValue(false));
  co_await settleBackfill();
}

ASYNC_TEST("paid fallback admission reconciles a due reservation and fails closed while exposure is unresolved") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "admission-due-reconcile";
  const std::string providerRequestId = "provider-admission-due";
  const auto providerLogs = std::make_shared<json::array>();
  const auto fetchCalls = std::make_shared<int>(0);
  {
    const rt::GlobalFetchOverride fetch(rt::Fetch([fetchCalls, providerLogs](rt::RequestInfo, rt::RequestInit) {
      *fetchCalls += 1;
      return meteredLogs(*providerLogs);
    }));
    const MeteredApiKey metered;
    json::object seedOverrides;
    seedOverrides["id"] = keyId;
    seedOverrides["paid_fallback_limit_microcredits"] = number(500'000);
    AWAIT_DONE(seeded, seedStrictKey(seedOverrides));
    AWAIT(first, index_::reservePaidFallback(reservationInput(keyId, keyId + "-first")));
    ASSERT_EQ(kindOf(first), "reserved");
    AWAIT(second, index_::reservePaidFallback(reservationInput(keyId, keyId + "-second")));
    EXPECT_EQ(kindOf(second), "reserved");
    AWAIT_DONE(dispatched, ledger_admission::updatePaidFallbackRequestV3(reservationOf(first), dispatchedPatch(providerRequestId)));
    AWAIT(blocked, index_::reservePaidFallback(reservationInput(keyId, keyId + "-third")));
    ASSERT_EQ(kindOf(blocked), "blocked");
    EXPECT_EQ(std::get<index_::PaidFallbackReservationDecisionBlocked>(blocked).reason, "limit_exceeded");
    EXPECT_EQ(*fetchCalls, 1);
    providerLogs->push_back(meteredLog(providerRequestId, 10'000, 1, 2));
    const contracts::PaidFallbackAdmissionV3 firstReservation = reservationOf(first);
    AWAIT_DONE(pendingSet, memory.set(ledger_state::paidFallbackPendingV3Key(keyId, firstReservation.request_id),
                                      pendingRow(firstReservation.created_at_ms, rt::dateNow() - 1)));
    AWAIT_DONE(gateSet, memory.set(ledger_state::paidFallbackReconciliationGateV3Key(), gateRow(0)));
    AWAIT(third, index_::reservePaidFallback(reservationInput(keyId, keyId + "-third")));
    ASSERT_EQ(kindOf(third), "reserved");
    EXPECT_EQ(reservationOf(third).reserved_microcredits, 230'000.0);
    AWAIT(settled, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, firstReservation.request_id)));
    EXPECT_EQ(settled.value["spend_microcredits"], rt::KvValue(20'000));
  }
  co_await settleBackfill();
}

ASYNC_TEST("V3 unlimited admission writes independent rows without a shared window") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-unlimited";
  const double resetAtMs = rt::dateNow() + 60'000;
  std::vector<rt::Promise<AdmitResult>> admissions;
  for (int index = 0; index < 100; ++index) {
    ledger_state::AdmissionInput input;
    input.keyId = keyId;
    input.requestId = "unlimited-" + std::to_string(index);
    input.createdAtMs = rt::dateNow();
    input.policyVersion = "policy-v3";
    input.limitMicrocredits = -1;
    input.maximumExposureMicrocredits = std::nullopt;
    input.initialSettledMicrocredits = 0;
    input.quotaPerCredit = 500'000;
    input.windowResetAtMs = resetAtMs;
    input.model = "gpt-5-codex";
    input.route = "responses";
    input.path = "/v1/responses";
    input.stream = true;
    input.reasoning = "high";
    admissions.emplace_back(ledger_admission::admitPaidFallbackV3(std::move(input)));
  }
  const rt::Promise<std::vector<AdmitResult>> admitting = rt::all(std::move(admissions));
  const std::vector<AdmitResult> decisions = co_await admitting.wait();
  EXPECT_EQ(kindsOf(decisions), std::vector<std::string>(100, "reserved"));
  AWAIT(window, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, resetAtMs)));
  EXPECT_EQ(window.value, rt::KvValue(nullptr));
  co_await settleBackfill();
}

ASYNC_TEST("V3 terminal reconciliation settles a pending request exactly once without KV queues") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-terminal";
  const std::string requestId = "gateway-terminal";
  const std::string providerRequestId = "provider-terminal";
  const double resetAtMs = rt::dateNow() + 60'000;
  const double createdAtMs = rt::dateNow();
  {
    const rt::GlobalFetchOverride fetch(logsFetch([providerRequestId] {
      json::array data;
      data.push_back(meteredLog(providerRequestId, 123'456, 40, 60));
      return data;
    }));
    const MeteredApiKey metered;
    ledger_state::AdmissionInput input;
    input.keyId = keyId;
    input.requestId = requestId;
    input.createdAtMs = createdAtMs;
    input.policyVersion = "policy-v3";
    input.limitMicrocredits = 1'000'000;
    input.maximumExposureMicrocredits = 250'000;
    input.initialSettledMicrocredits = 0;
    input.quotaPerCredit = 500'000;
    input.windowResetAtMs = resetAtMs;
    input.model = "gpt-5-codex";
    input.route = "responses";
    input.path = "/v1/responses";
    input.stream = true;
    input.reasoning = "high";
    AWAIT(decision, ledger_admission::admitPaidFallbackV3(std::move(input)));
    ASSERT_EQ(kindOf(decision), "reserved");

    AWAIT_DONE(dispatched, ledger_admission::updatePaidFallbackRequestV3(reservationOf(decision), dispatchedPatch(providerRequestId)));

    AWAIT(firstTerminal, ledger_backfill::recordPaidFallbackTerminalV3(reservationOf(decision), "completed"));
    EXPECT_EQ(firstTerminal, 1.0);
    AWAIT(secondTerminal, ledger_backfill::recordPaidFallbackTerminalV3(reservationOf(decision), "completed"));
    EXPECT_EQ(secondTerminal, 0.0);

    AWAIT(request, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, requestId)));
    EXPECT_EQ(request.value["billing_state"], rt::KvValue("settled"));
    EXPECT_EQ(request.value["terminal_state"], rt::KvValue("completed"));
    EXPECT_EQ(request.value["spend_microcredits"], rt::KvValue(246'912));
    EXPECT_EQ(request.value["provider_quota"], rt::KvValue(123'456));
    EXPECT_EQ(request.value["input_tokens"], rt::KvValue(40));
    EXPECT_EQ(request.value["output_tokens"], rt::KvValue(60));
    EXPECT_EQ(typeOf(request.value["dispatched_at_ms"]), "number");
    EXPECT_EQ(typeOf(request.value["terminal_at_ms"]), "number");
    EXPECT_EQ(typeOf(request.value["settled_at_ms"]), "number");
    AWAIT(window, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, resetAtMs)));
    EXPECT_EQ(window.value["reserved_microcredits"], rt::KvValue(0));
    EXPECT_EQ(window.value["settled_microcredits"], rt::KvValue(246'912));
    EXPECT_EQ(window.value["pending_count"], rt::KvValue(0));
    AWAIT(pending, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, requestId)));
    EXPECT_EQ(pending.value, rt::KvValue(nullptr));
  }
  co_await settleBackfill();
}

ASYNC_TEST("V3 reconciliation records billing facts without manufacturing a terminal event") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-out-of-order";
  const std::string requestId = "gateway-out-of-order";
  const std::string providerRequestId = "provider-out-of-order";
  const rt::KvKey requestKey = ledger_state::paidFallbackRequestV3Key(keyId, requestId);
  const auto injectedContention = std::make_shared<bool>(false);
  {
    const rt::GlobalFetchOverride fetch(rt::Fetch([injectedContention, requestKey, providerRequestId](rt::RequestInfo, rt::RequestInit) {
      if (!*injectedContention) {
        *injectedContention = true;
        memoryKv().beforeAtomicCommit = [requestKey] {
          const MemoryKv::StoredEntry* existing = memoryKv().entries.get(encodeKey(requestKey));
          if (existing == nullptr) throw rt::JsError("missing request for contention injection");
          rt::KvObject bumped = existing->value.as<rt::KvObject>();
          bumped.set("updated_at_ms", rt::KvValue(numberOf(existing->value["updated_at_ms"]) + 1));
          memoryKv().setNow(requestKey, rt::KvValue(std::move(bumped)));
        };
      }
      json::array data;
      data.push_back(meteredLog(providerRequestId, 50'000, 12, 34));
      return meteredLogs(std::move(data));
    }));
    const MeteredApiKey metered;
    AWAIT(decision, ledger_admission::admitPaidFallbackV3(v3AdmissionInput(keyId, requestId)));
    ASSERT_EQ(kindOf(decision), "reserved");

    AWAIT_DONE(dispatched, ledger_admission::updatePaidFallbackRequestV3(reservationOf(decision), dispatchedPatch(providerRequestId)));

    AWAIT(reconciled, ledger_backfill::reconcilePaidFallbackV3(keyId, rt::dateNow() + 100, kvOverride()));
    EXPECT_EQ(reconciled, 1.0);
    AWAIT(settledBeforeTerminal, memory.get(requestKey));
    EXPECT_EQ(settledBeforeTerminal.value["billing_state"], rt::KvValue("settled"));
    EXPECT_EQ(settledBeforeTerminal.value["terminal_state"], rt::KvValue("pending"));
    EXPECT_EQ(settledBeforeTerminal.value["provider_quota"], rt::KvValue(50'000));
    EXPECT_EQ(settledBeforeTerminal.value["input_tokens"], rt::KvValue(12));
    EXPECT_EQ(settledBeforeTerminal.value["output_tokens"], rt::KvValue(34));
    EXPECT_EQ(settledBeforeTerminal.value["spend_microcredits"], rt::KvValue(100'000));
    EXPECT_EQ(memory.atomicCommitFailures, 1);

    AWAIT(terminal, ledger_backfill::recordPaidFallbackTerminalV3(reservationOf(decision), "ambiguous"));
    EXPECT_EQ(terminal, 0.0);
    AWAIT(afterTerminal, memory.get(requestKey));
    EXPECT_EQ(afterTerminal.value["terminal_state"], rt::KvValue("ambiguous"));
    EXPECT_EQ(typeOf(afterTerminal.value["terminal_at_ms"]), "number");
    EXPECT_EQ(afterTerminal.value["spend_microcredits"], rt::KvValue(100'000));
    AWAIT(again, ledger_backfill::reconcilePaidFallbackV3(keyId, rt::dateNow() + 200, kvOverride()));
    EXPECT_EQ(again, 0.0);
  }
  co_await settleBackfill();
}

ASYNC_TEST("V3 terminal delivery expedites a request deferred before provider billing appeared") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-terminal-expedite";
  const std::string requestId = "gateway-terminal-expedite";
  const std::string providerRequestId = "provider-terminal-expedite";
  const auto billingVisible = std::make_shared<bool>(false);
  {
    const rt::GlobalFetchOverride fetch(logsFetch([billingVisible, providerRequestId] {
      json::array data;
      if (*billingVisible) data.push_back(meteredLog(providerRequestId, 25'000, 5, 6));
      return data;
    }));
    const MeteredApiKey metered;
    AWAIT(decision, ledger_admission::admitPaidFallbackV3(v3AdmissionInput(keyId, requestId)));
    ASSERT_EQ(kindOf(decision), "reserved");

    AWAIT_DONE(dispatched, ledger_admission::updatePaidFallbackRequestV3(reservationOf(decision), dispatchedPatch(providerRequestId)));
    const double deferredAtMs = rt::dateNow() + 100;
    AWAIT(deferredCount, ledger_backfill::reconcilePaidFallbackV3(keyId, deferredAtMs, kvOverride()));
    EXPECT_EQ(deferredCount, 0.0);
    AWAIT(deferred, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, requestId)));
    EXPECT_EQ(deferred.value["next_reconciliation_at_ms"], rt::KvValue(deferredAtMs + 5'000));
    AWAIT(futureGate, memory.get(ledger_state::paidFallbackReconciliationGateV3Key()));
    EXPECT_EQ(numberOf(futureGate.value["next_due_at_ms"]) > deferredAtMs, true);

    *billingVisible = true;
    AWAIT_DONE(marked, ledger_backfill::markPaidFallbackTerminalV3(reservationOf(decision), "completed"));
    AWAIT(dueGate, memory.get(ledger_state::paidFallbackReconciliationGateV3Key()));
    EXPECT_EQ(numberOf(dueGate.value["next_due_at_ms"]) <= rt::dateNow(), true);
    AWAIT(reconciled, ledger_backfill::reconcilePaidFallbackV3(keyId, rt::dateNow(), kvOverride()));
    EXPECT_EQ(reconciled, 1.0);
    AWAIT(settled, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, requestId)));
    EXPECT_EQ(settled.value["billing_state"], rt::KvValue("settled"));
    EXPECT_EQ(settled.value["terminal_state"], rt::KvValue("completed"));
    EXPECT_EQ(settled.value["spend_microcredits"], rt::KvValue(50'000));
    EXPECT_EQ(settled.value["reconciliation_attempts"], rt::KvValue(2));
  }
  co_await settleBackfill();
}

ASYNC_TEST("V3 expedite versions an already-due gate against stale recomputation") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-expedite-gate-version";
  const std::string requestId = "expedite-gate-version";
  AWAIT(decision, ledger_admission::admitPaidFallbackV3(v3AdmissionInput(keyId, requestId)));
  ASSERT_EQ(kindOf(decision), "reserved");

  const rt::KvKey gateKey = ledger_state::paidFallbackReconciliationGateV3Key();
  const rt::KvKey pendingKey = ledger_state::paidFallbackPendingV3Key(keyId, requestId);
  const double now = rt::dateNow();
  AWAIT_DONE(gateSet, memory.set(gateKey, gateRow(now)));
  AWAIT_DONE(pendingSet, memory.set(pendingKey, pendingRow(now, now + 60'000)));
  const std::optional<std::string> before = memory.versionstamp(gateKey);

  AWAIT_DONE(marked, ledger_backfill::markPaidFallbackTerminalV3(reservationOf(decision), "completed"));

  EXPECT(memory.versionstamp(gateKey) != before);
  AWAIT(gate, memory.get(gateKey));
  EXPECT_EQ(numberOf(gate.value["next_due_at_ms"]) <= rt::dateNow(), true);
  AWAIT(pending, memory.get(pendingKey));
  EXPECT_EQ(numberOf(pending.value["next_reconciliation_at_ms"]) <= rt::dateNow(), true);
  co_await settleBackfill();
}

ASYNC_TEST("V3 bounded policy edits preserve exposure, admit concurrently, and retain final-call overshoot") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-policy-edit";
  const double resetAtMs = rt::dateNow() + 60'000;
  ledger_state::AdmissionInput firstInput = v3AdmissionInput(keyId, "policy-first");
  firstInput.policyVersion = "policy-v1";
  firstInput.windowResetAtMs = resetAtMs;
  firstInput.maximumExposureMicrocredits = 400'000;
  AWAIT(first, ledger_admission::admitPaidFallbackV3(std::move(firstInput)));
  ASSERT_EQ(kindOf(first), "reserved");

  EXPECT_EQ(reservationOf(first).quota_used_percent, std::optional<double>(0));

  std::vector<rt::Promise<AdmitResult>> admissions;
  for (const char* requestId : {"policy-second", "policy-third"}) {
    ledger_state::AdmissionInput input = v3AdmissionInput(keyId, requestId);
    input.policyVersion = "policy-v2";
    input.limitMicrocredits = 600'000;
    input.windowResetAtMs = resetAtMs;
    input.maximumExposureMicrocredits = 200'000;
    admissions.emplace_back(ledger_admission::admitPaidFallbackV3(std::move(input)));
  }
  const rt::Promise<std::vector<AdmitResult>> admitting = rt::all(std::move(admissions));
  const std::vector<AdmitResult> concurrent = co_await admitting.wait();
  std::vector<std::string> kinds = kindsOf(concurrent);
  std::sort(kinds.begin(), kinds.end(), [](const std::string& a, const std::string& b) { return rt::localeCompare(a, b) < 0; });
  EXPECT_EQ(kinds, (std::vector<std::string>{"blocked", "reserved"}));
  const auto secondDecision = std::find_if(concurrent.begin(), concurrent.end(), [](const AdmitResult& decision) { return kindOf(decision) == "reserved"; });
  if (secondDecision == concurrent.end()) throw rt::JsError("expected concurrent reservation");
  const contracts::PaidFallbackAdmissionV3 second = reservationOf(*secondDecision);

  ledger_state::AdmissionInput loweredInput = v3AdmissionInput(keyId, "policy-lowered");
  loweredInput.policyVersion = "policy-v3";
  loweredInput.limitMicrocredits = 500'000;
  loweredInput.windowResetAtMs = resetAtMs;
  loweredInput.maximumExposureMicrocredits = 200'000;
  AWAIT(lowered, ledger_admission::admitPaidFallbackV3(std::move(loweredInput)));
  EXPECT_EQ(decisionJson(lowered), blockedDecision("limit_exceeded"));
  AWAIT(transitioned, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, resetAtMs)));
  EXPECT_EQ(transitioned.value["policy_version"], rt::KvValue("policy-v3"));
  EXPECT_EQ(transitioned.value["limit_microcredits"], rt::KvValue(500'000));
  EXPECT_EQ(transitioned.value["reserved_microcredits"], rt::KvValue(600'000));

  rt::OrderedMap<std::string, std::string> providerIds;
  providerIds.set(reservationOf(first).request_id, "provider-policy-first");
  providerIds.set(second.request_id, "provider-policy-second");
  std::vector<rt::Promise<void>> updates;
  const std::vector<contracts::PaidFallbackAdmissionV3> dispatchedReservations{reservationOf(first), second};
  for (const contracts::PaidFallbackAdmissionV3& reservation : dispatchedReservations) {
    const std::string* providerRequestId = providerIds.get(reservation.request_id);
    if (providerRequestId == nullptr) throw rt::JsError("missing provider request id fixture for " + reservation.request_id);
    contracts::PaidFallbackRequestLifecyclePatchV3 patch = dispatchedPatch(*providerRequestId);
    patch.terminal_state = "completed";
    updates.emplace_back(ledger_admission::updatePaidFallbackRequestV3(reservation, std::move(patch)));
  }
  const rt::Promise<void> updating = rt::all(std::move(updates));
  co_await updating.wait();
  {
    const rt::GlobalFetchOverride fetch(logsFetch([] {
      json::array data;
      data.push_back(meteredLog("provider-policy-first", 200'000, 10, 20));
      data.push_back(meteredLog("provider-policy-second", 150'000, 20, 30));
      return data;
    }));
    const MeteredApiKey metered;
    AWAIT(reconciled, ledger_backfill::reconcilePaidFallbackV3(keyId, rt::dateNow() + 100, kvOverride()));
    EXPECT_EQ(reconciled, 2.0);
  }

  AWAIT(overshot, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, resetAtMs)));
  EXPECT_EQ(overshot.value["settled_microcredits"], rt::KvValue(700'000));
  EXPECT_EQ(overshot.value["reserved_microcredits"], rt::KvValue(0));
  EXPECT_EQ(overshot.value["pending_count"], rt::KvValue(0));
  ledger_state::AdmissionInput afterInput = v3AdmissionInput(keyId, "policy-after-overshoot");
  afterInput.policyVersion = "policy-v3";
  afterInput.limitMicrocredits = 500'000;
  afterInput.windowResetAtMs = resetAtMs;
  AWAIT(afterOvershoot, ledger_admission::admitPaidFallbackV3(std::move(afterInput)));
  EXPECT_EQ(decisionJson(afterOvershoot), blockedDecision("limit_exceeded"));
  AWAIT(projection, ledger_state::getPaidFallbackWindowProjectionV3(keyId, resetAtMs, 500'000, kvOverride()));
  ASSERT(projection.has_value());
  const contracts::PaidFallbackWindowProjectionV3 expected{
    .key_id = keyId,
    .policy_version = "policy-v3",
    .window_reset_at_ms = resetAtMs,
    .limit_microcredits = 500'000,
    .settled_microcredits = 700'000,
    .reserved_microcredits = 0,
    .pending_count = 0,
    .updated_at_ms = overshot.value["updated_at_ms"].isNumber() ? std::optional<double>(overshot.value["updated_at_ms"].asNumber()) : std::nullopt,
  };
  EXPECT_EQ(json::value_from(*projection), json::value_from(expected));
  co_await settleBackfill();
}

ASYNC_TEST("V3 due scanning honors leases and backoff while retaining unresolved exposure after 24 hours") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-unresolved";
  const std::string requestId = "missing-provider-id";
  const double createdAtMs = rt::dateNow();
  const double resetAtMs = createdAtMs + 60'000;
  ledger_state::AdmissionInput input = v3AdmissionInput(keyId, requestId);
  input.createdAtMs = createdAtMs;
  input.windowResetAtMs = resetAtMs;
  input.maximumExposureMicrocredits = 300'000;
  AWAIT(decision, ledger_admission::admitPaidFallbackV3(std::move(input)));
  ASSERT_EQ(kindOf(decision), "reserved");

  const double firstAttemptAtMs = rt::dateNow() + 100;
  rt::KvObject heldLease;
  heldLease.set("token", rt::KvValue("held-lease"));
  heldLease.set("expires_at_ms", rt::KvValue(firstAttemptAtMs + 60'000));
  const rt::KvSetOptions leaseExpiry{.expireIn = 60'000};
  AWAIT_DONE(leaseSet, memory.set(ledger_state::paidFallbackReconciliationLeaseV3Key(keyId), rt::KvValue(std::move(heldLease)), leaseExpiry));
  AWAIT(leased, ledger_backfill::reconcileDuePaidFallbacksV3(firstAttemptAtMs, kvOverride()));
  EXPECT_EQ(leased, 0.0);
  AWAIT(untouched, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, requestId)));
  EXPECT_EQ(untouched.value["reconciliation_attempts"], rt::KvValue(0));

  AWAIT_DONE(leaseDeleted, memory.delete_(ledger_state::paidFallbackReconciliationLeaseV3Key(keyId)));
  AWAIT(firstAttempt, ledger_backfill::reconcileDuePaidFallbacksV3(firstAttemptAtMs, kvOverride()));
  EXPECT_EQ(firstAttempt, 0.0);
  AWAIT(attempted, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, requestId)));
  EXPECT_EQ(attempted.value["reconciliation_attempts"], rt::KvValue(1));
  AWAIT(pending, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, requestId)));
  EXPECT_EQ(pending.value["next_reconciliation_at_ms"], rt::KvValue(firstAttemptAtMs + 5'000));

  AWAIT(early, ledger_backfill::reconcileDuePaidFallbacksV3(firstAttemptAtMs + 4'999, kvOverride()));
  EXPECT_EQ(early, 0.0);
  AWAIT(notRetried, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, requestId)));
  EXPECT_EQ(notRetried.value["reconciliation_attempts"], rt::KvValue(1));
  AWAIT(secondAttempt, ledger_backfill::reconcileDuePaidFallbacksV3(firstAttemptAtMs + 5'000, kvOverride()));
  EXPECT_EQ(secondAttempt, 0.0);
  AWAIT(backedOff, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, requestId)));
  EXPECT_EQ(backedOff.value["next_reconciliation_at_ms"], rt::KvValue(firstAttemptAtMs + 35'000));

  const double unresolvedAtMs = createdAtMs + 24 * 60 * 60'000 + 1;
  AWAIT(unresolvedCount, ledger_backfill::reconcileDuePaidFallbacksV3(unresolvedAtMs, kvOverride()));
  EXPECT_EQ(unresolvedCount, 0.0);
  AWAIT(request, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, requestId)));
  EXPECT_EQ(request.value["billing_state"], rt::KvValue("unresolved"));
  EXPECT_EQ(request.value["terminal_state"], rt::KvValue("pending"));
  EXPECT_EQ(request.value["reconciliation_attempts"], rt::KvValue(3));
  AWAIT(retained, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, requestId)));
  EXPECT_EQ(!retained.value.isNull(), true);
  AWAIT(window, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, resetAtMs)));
  EXPECT_EQ(window.value["reserved_microcredits"], rt::KvValue(300'000));
  EXPECT_EQ(window.value["pending_count"], rt::KvValue(1));
  AWAIT(outstandingNow, ledger_state::getPaidFallbackOutstandingV3(keyId, kvOverride()));
  EXPECT_EQ(outstandingJson(outstandingNow), outstanding(0, 1, 1, true));
  AWAIT(deletion, ledger_admission::deletePaidFallbackStateV3(keyId, kvOverride()));
  EXPECT_EQ(std::holds_alternative<ledger_admission::PaidFallbackDeletionV3Blocked>(deletion), true);
  co_await settleBackfill();
}

ASYNC_TEST("V3 unlimited projection and newest-first history support safe settled-state deletion") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-unlimited-projection";
  const double resetAtMs = rt::dateNow() + 60'000;
  const double createdAtMs = rt::dateNow();
  ledger_state::AdmissionInput olderInput = v3AdmissionInput(keyId, "unlimited-older");
  olderInput.createdAtMs = createdAtMs;
  olderInput.limitMicrocredits = -1;
  olderInput.maximumExposureMicrocredits = std::nullopt;
  olderInput.windowResetAtMs = resetAtMs;
  AWAIT(older, ledger_admission::admitPaidFallbackV3(std::move(olderInput)));
  ledger_state::AdmissionInput newerInput = v3AdmissionInput(keyId, "unlimited-newer");
  newerInput.createdAtMs = createdAtMs + 10;
  newerInput.limitMicrocredits = -1;
  newerInput.maximumExposureMicrocredits = std::nullopt;
  newerInput.windowResetAtMs = resetAtMs;
  AWAIT(newer, ledger_admission::admitPaidFallbackV3(std::move(newerInput)));
  ASSERT_EQ(kindOf(older), "reserved");
  ASSERT_EQ(kindOf(newer), "reserved");

  contracts::PaidFallbackRequestLifecyclePatchV3 olderPatch = dispatchedPatch(std::string("provider-unlimited-older"));
  olderPatch.terminal_state = "completed";
  contracts::PaidFallbackRequestLifecyclePatchV3 newerPatch = dispatchedPatch(std::string("provider-unlimited-newer"));
  newerPatch.terminal_state = "completed";
  std::vector<rt::Promise<void>> updates;
  updates.emplace_back(ledger_admission::updatePaidFallbackRequestV3(reservationOf(older), std::move(olderPatch)));
  updates.emplace_back(ledger_admission::updatePaidFallbackRequestV3(reservationOf(newer), std::move(newerPatch)));
  const rt::Promise<void> updating = rt::all(std::move(updates));
  co_await updating.wait();
  {
    const rt::GlobalFetchOverride fetch(logsFetch([] {
      json::array data;
      data.push_back(meteredLog("provider-unlimited-older", 10'000, 1, 2));
      data.push_back(meteredLog("provider-unlimited-newer", 15'000, 3, 4));
      return data;
    }));
    const MeteredApiKey metered;
    AWAIT(reconciled, ledger_backfill::reconcilePaidFallbackV3(keyId, rt::dateNow() + 100, kvOverride()));
    EXPECT_EQ(reconciled, 2.0);
  }

  AWAIT(window, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, resetAtMs)));
  EXPECT_EQ(window.value, rt::KvValue(nullptr));
  AWAIT(projection, ledger_state::getPaidFallbackWindowProjectionV3(keyId, resetAtMs, -1, kvOverride()));
  AWAIT(newest, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, "unlimited-newer")));
  ASSERT(projection.has_value());
  const contracts::PaidFallbackWindowProjectionV3 expected{
    .key_id = keyId,
    .policy_version = "policy-v3",
    .window_reset_at_ms = resetAtMs,
    .limit_microcredits = -1,
    .settled_microcredits = 50'000,
    .reserved_microcredits = 0,
    .pending_count = 0,
    .updated_at_ms = newest.value["updated_at_ms"].isNumber() ? std::optional<double>(newest.value["updated_at_ms"].asNumber()) : std::nullopt,
  };
  EXPECT_EQ(json::value_from(*projection), json::value_from(expected));
  AWAIT(history, ledger_state::listPaidFallbackRequestsV3(keyId, 1, kvOverride()));
  std::vector<std::string> historyIds;
  for (const uos::types::PaidFallbackRequestV3& request : history) historyIds.push_back(request.request_id);
  EXPECT_EQ(historyIds, std::vector<std::string>{"unlimited-newer"});
  AWAIT(settledOutstanding, ledger_state::getPaidFallbackOutstandingV3(keyId, kvOverride()));
  EXPECT_EQ(outstandingJson(settledOutstanding), outstanding(0, 0, 0, false));
  rt::KvObject staleLease;
  staleLease.set("token", rt::KvValue("stale-cleanup-lease"));
  staleLease.set("expires_at_ms", rt::KvValue(rt::dateNow() - 1));
  AWAIT_DONE(leaseSet, memory.set(ledger_state::paidFallbackReconciliationLeaseV3Key(keyId), rt::KvValue(std::move(staleLease))));
  AWAIT(firstDeletion, ledger_admission::deletePaidFallbackStateV3(keyId, kvOverride()));
  EXPECT_EQ(deletionJson(firstDeletion), deleted(2, 0, 0, 1));
  AWAIT(emptied, ledger_state::listPaidFallbackRequestsV3(keyId, 100, kvOverride()));
  EXPECT_EQ(emptied.size(), std::size_t{0});
  AWAIT(guard, memory.get(ledger_state::paidFallbackDeletionGuardV3Key(keyId)));
  EXPECT_EQ(!guard.value.isNull(), true);
  AWAIT(secondDeletion, ledger_admission::deletePaidFallbackStateV3(keyId, kvOverride()));
  EXPECT_EQ(deletionJson(secondDeletion), deleted(0, 0, 0, 0));
  ledger_state::AdmissionInput staleInput = v3AdmissionInput(keyId, "stale-after-deletion");
  staleInput.limitMicrocredits = -1;
  staleInput.maximumExposureMicrocredits = std::nullopt;
  staleInput.windowResetAtMs = resetAtMs;
  AWAIT(stale, ledger_admission::admitPaidFallbackV3(std::move(staleInput)));
  EXPECT_EQ(decisionJson(stale), blockedDecision("invalid_policy"));
  co_await settleBackfill();
}

ASYNC_TEST("V3 window rollover isolates prior exposure from new admissions") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-window-rollover";
  const double firstResetAtMs = rt::dateNow() + 60'000;
  const double secondResetAtMs = firstResetAtMs + 60'000;
  ledger_state::AdmissionInput firstInput = v3AdmissionInput(keyId, "rollover-first");
  firstInput.windowResetAtMs = firstResetAtMs;
  firstInput.initialSettledMicrocredits = 900'000;
  firstInput.maximumExposureMicrocredits = 250'000;
  AWAIT(first, ledger_admission::admitPaidFallbackV3(std::move(firstInput)));
  ledger_state::AdmissionInput secondInput = v3AdmissionInput(keyId, "rollover-second");
  secondInput.windowResetAtMs = secondResetAtMs;
  secondInput.initialSettledMicrocredits = 0;
  secondInput.maximumExposureMicrocredits = 250'000;
  AWAIT(second, ledger_admission::admitPaidFallbackV3(std::move(secondInput)));
  ASSERT_EQ(kindOf(first), "reserved");
  ASSERT_EQ(kindOf(second), "reserved");

  EXPECT_EQ(reservationOf(first).reserved_microcredits, 100'000.0);
  EXPECT_EQ(reservationOf(first).quota_used_percent, std::optional<double>(90));
  EXPECT_EQ(reservationOf(second).reserved_microcredits, 250'000.0);
  EXPECT_EQ(reservationOf(second).quota_used_percent, std::optional<double>(0));
  AWAIT(firstWindow, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, firstResetAtMs)));
  AWAIT(secondWindow, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, secondResetAtMs)));
  EXPECT_EQ(firstWindow.value["settled_microcredits"], rt::KvValue(900'000));
  EXPECT_EQ(firstWindow.value["reserved_microcredits"], rt::KvValue(100'000));
  EXPECT_EQ(secondWindow.value["settled_microcredits"], rt::KvValue(0));
  EXPECT_EQ(secondWindow.value["reserved_microcredits"], rt::KvValue(250'000));
  co_await settleBackfill();
}

ASYNC_TEST("V3 deletion guard linearizes against an in-flight admission") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-deletion-race";
  memory.beforeAtomicCommit = [keyId] {
    rt::KvObject guard;
    guard.set("created_at_ms", rt::KvValue(rt::dateNow()));
    memoryKv().setNow(ledger_state::paidFallbackDeletionGuardV3Key(keyId), rt::KvValue(std::move(guard)));
  };
  AWAIT(decision, ledger_admission::admitPaidFallbackV3(v3AdmissionInput(keyId, "stale-in-flight-request")));
  EXPECT_EQ(decisionJson(decision), blockedDecision("invalid_policy"));
  EXPECT_EQ(memory.atomicCommitFailures, 1);
  AWAIT(request, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, "stale-in-flight-request")));
  EXPECT_EQ(request.value, rt::KvValue(nullptr));
  AWAIT(outstandingNow, ledger_state::getPaidFallbackOutstandingV3(keyId, kvOverride()));
  EXPECT_EQ(outstandingJson(outstandingNow), outstanding(0, 0, 0, false));
  co_await settleBackfill();
}

ASYNC_TEST("V3 undispatched release is idempotent and cannot erase dispatched exposure") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-release";
  const double resetAtMs = rt::dateNow() + 60'000;
  AWAIT_DONE(gateSet, memory.set(ledger_state::paidFallbackReconciliationGateV3Key(), gateRow(rt::dateNow() + 60'000)));
  ledger_state::AdmissionInput releasedInput = v3AdmissionInput(keyId, "release-before-dispatch");
  releasedInput.windowResetAtMs = resetAtMs;
  AWAIT(released, ledger_admission::admitPaidFallbackV3(std::move(releasedInput)));
  ASSERT_EQ(kindOf(released), "reserved");

  AWAIT_DONE(firstRelease, ledger_admission::releaseUndispatchedPaidFallbackV3(reservationOf(released)));
  AWAIT_DONE(secondRelease, ledger_admission::releaseUndispatchedPaidFallbackV3(reservationOf(released)));
  AWAIT(releasedRequest, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, reservationOf(released).request_id)));
  EXPECT_EQ(releasedRequest.value["billing_state"], rt::KvValue("not_billed"));
  EXPECT_EQ(releasedRequest.value["dispatch_state"], rt::KvValue("not_dispatched"));
  EXPECT_EQ(releasedRequest.value["terminal_state"], rt::KvValue("cancelled"));
  EXPECT_EQ(typeOf(releasedRequest.value["terminal_at_ms"]), "number");
  AWAIT(gate, memory.get(ledger_state::paidFallbackReconciliationGateV3Key()));
  EXPECT_EQ(numberOf(gate.value["next_due_at_ms"]) <= rt::dateNow(), true);

  ledger_state::AdmissionInput dispatchedInput = v3AdmissionInput(keyId, "release-after-dispatch");
  dispatchedInput.windowResetAtMs = resetAtMs;
  AWAIT(dispatched, ledger_admission::admitPaidFallbackV3(std::move(dispatchedInput)));
  ASSERT_EQ(kindOf(dispatched), "reserved");

  AWAIT_DONE(update, ledger_admission::updatePaidFallbackRequestV3(reservationOf(dispatched), dispatchedPatch(std::nullopt)));
  AWAIT_DONE(lateRelease, ledger_admission::releaseUndispatchedPaidFallbackV3(reservationOf(dispatched)));
  AWAIT(dispatchedRequest, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, reservationOf(dispatched).request_id)));
  EXPECT_EQ(dispatchedRequest.value["billing_state"], rt::KvValue("pending"));
  EXPECT_EQ(dispatchedRequest.value["dispatch_state"], rt::KvValue("dispatched"));
  AWAIT(dispatchedPending, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, reservationOf(dispatched).request_id)));
  EXPECT_EQ(!dispatchedPending.value.isNull(), true);

  ledger_state::AdmissionInput prefetchInput = v3AdmissionInput(keyId, "release-prefetch-intent");
  prefetchInput.windowResetAtMs = resetAtMs;
  prefetchInput.dispatchIntent = true;
  AWAIT(prefetch, ledger_admission::admitPaidFallbackV3(std::move(prefetchInput)));
  ASSERT_EQ(kindOf(prefetch), "reserved");

  AWAIT(dispatchIntent, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, reservationOf(prefetch).request_id)));
  EXPECT_EQ(dispatchIntent.value["dispatch_state"], rt::KvValue("dispatched"));
  EXPECT_EQ(typeOf(dispatchIntent.value["dispatched_at_ms"]), "number");
  AWAIT_DONE(prefetchRelease, ledger_admission::releasePaidFallbackBeforeProviderFetchV3(reservationOf(prefetch)));
  AWAIT(prefetchRequest, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, reservationOf(prefetch).request_id)));
  EXPECT_EQ(prefetchRequest.value["billing_state"], rt::KvValue("not_billed"));
  EXPECT_EQ(prefetchRequest.value["dispatch_state"], rt::KvValue("not_dispatched"));
  EXPECT_EQ(prefetchRequest.value["terminal_state"], rt::KvValue("cancelled"));
  co_await settleBackfill();
}

ASYNC_TEST("V3 queue delivery coalesces due rows by key and duplicate delivery is idempotent") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-queue-coalesced";
  const double resetAtMs = rt::dateNow() + 60'000;
  ledger_state::AdmissionInput firstInput = v3AdmissionInput(keyId, "queue-first");
  firstInput.windowResetAtMs = resetAtMs;
  AWAIT(first, ledger_admission::admitPaidFallbackV3(std::move(firstInput)));
  ledger_state::AdmissionInput secondInput = v3AdmissionInput(keyId, "queue-second");
  secondInput.windowResetAtMs = resetAtMs;
  AWAIT(second, ledger_admission::admitPaidFallbackV3(std::move(secondInput)));
  ASSERT_EQ(kindOf(first), "reserved");
  ASSERT_EQ(kindOf(second), "reserved");

  EXPECT_EQ(memory.queueMessages.size(), std::size_t{0});
  AWAIT_DONE(firstUpdate, ledger_admission::updatePaidFallbackRequestV3(reservationOf(first), dispatchedPatch(std::string("provider-queue-first"))));
  AWAIT_DONE(secondUpdate, ledger_admission::updatePaidFallbackRequestV3(reservationOf(second), dispatchedPatch(std::string("provider-queue-second"))));
  AWAIT(queued, ledger_backfill::enqueueDuePaidFallbackReconciliationJobsV3(rt::dateNow() + 1, kvOverride()));
  EXPECT_EQ(queued, 1.0);
  std::vector<rt::KvValue> messages;
  for (const MemoryKv::QueueMessage& queuedMessage : memory.queueMessages) messages.push_back(queuedMessage.message);
  rt::KvObject job;
  job.set("key_id", rt::KvValue(keyId));
  EXPECT_EQ(messages, std::vector<rt::KvValue>{rt::KvValue(job)});

  {
    const rt::GlobalFetchOverride fetch(logsFetch([] {
      json::array data;
      data.push_back(meteredLog("provider-queue-first", 10'000, 1, 2));
      return data;
    }));
    const MeteredApiKey metered;
    AWAIT(handled, ledger_backfill::handlePaidFallbackReconciliationJobV3(rt::KvValue(job), kvOverride()));
    EXPECT_EQ(handled, 1.0);
    AWAIT(duplicate, ledger_backfill::handlePaidFallbackReconciliationJobV3(rt::KvValue(job), kvOverride()));
    EXPECT_EQ(duplicate, 0.0);
  }
  AWAIT(firstRequest, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, "queue-first")));
  AWAIT(secondRequest, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, "queue-second")));
  EXPECT_EQ(firstRequest.value["billing_state"], rt::KvValue("settled"));
  EXPECT_EQ(secondRequest.value["billing_state"], rt::KvValue("pending"));
  EXPECT_EQ(secondRequest.value["reconciliation_attempts"], rt::KvValue(1));
  co_await settleBackfill();
}

ASYNC_TEST("V3 queue enqueue failure does not roll back durable reconciliation backoff") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-queue-failure";
  const std::string requestId = "queue-failure-request";
  AWAIT(decision, ledger_admission::admitPaidFallbackV3(v3AdmissionInput(keyId, requestId)));
  ASSERT_EQ(kindOf(decision), "reserved");

  AWAIT_DONE(dispatched, ledger_admission::updatePaidFallbackRequestV3(reservationOf(decision), dispatchedPatch(std::string("provider-queue-failure"))));
  {
    const rt::GlobalFetchOverride fetch(logsFetch([] { return json::array(); }));
    memory.enqueueFailure = rt::makeReason(rt::JsError("queue unavailable"));
    const MeteredApiKey metered;
    AWAIT(reconciled, ledger_backfill::reconcilePaidFallbackV3(keyId, rt::dateNow() + 100, kvOverride()));
    EXPECT_EQ(reconciled, 0.0);
  }
  AWAIT(request, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, requestId)));
  AWAIT(pending, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, requestId)));
  EXPECT_EQ(request.value["reconciliation_attempts"], rt::KvValue(1));
  EXPECT_EQ(pending.value["next_reconciliation_at_ms"], rt::KvValue(numberOf(request.value["last_reconciliation_at_ms"]) + 5'000));
  co_await settleBackfill();
}

ASYNC_TEST("V3 unresolved rows remain queue-reconcilable when late provider billing appears") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const std::string keyId = "v3-unresolved-late";
  const std::string requestId = "unresolved-late-request";
  const std::string providerRequestId = "provider-unresolved-late";
  ledger_state::AdmissionInput input = v3AdmissionInput(keyId, requestId);
  input.createdAtMs = rt::dateNow() - 24 * 60 * 60'000 - 1;
  AWAIT(decision, ledger_admission::admitPaidFallbackV3(std::move(input)));
  ASSERT_EQ(kindOf(decision), "reserved");

  AWAIT_DONE(dispatched, ledger_admission::updatePaidFallbackRequestV3(reservationOf(decision), dispatchedPatch(providerRequestId)));
  {
    const rt::GlobalFetchOverride fetch(logsFetch([] { return json::array(); }));
    const MeteredApiKey metered;
    AWAIT(reconciled, ledger_backfill::reconcilePaidFallbackV3(keyId, rt::dateNow() + 100, kvOverride()));
    EXPECT_EQ(reconciled, 0.0);
  }
  AWAIT(unresolved, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, requestId)));
  EXPECT_EQ(unresolved.value["billing_state"], rt::KvValue("unresolved"));
  AWAIT(pending, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, requestId)));
  if (pending.value.isNull()) throw rt::JsError("expected a pending reconciliation record");
  rt::KvObject due = pending.value.as<rt::KvObject>();
  due.set("next_reconciliation_at_ms", rt::KvValue(rt::dateNow() - 1));
  AWAIT_DONE(pendingSet, memory.set(ledger_state::paidFallbackPendingV3Key(keyId, requestId), rt::KvValue(std::move(due))));
  {
    const rt::GlobalFetchOverride fetch(logsFetch([providerRequestId] {
      json::array data;
      data.push_back(meteredLog(providerRequestId, 20'000, 1, 2));
      return data;
    }));
    const MeteredApiKey metered;
    rt::KvObject job;
    job.set("key_id", rt::KvValue(keyId));
    AWAIT(handled, ledger_backfill::handlePaidFallbackReconciliationJobV3(rt::KvValue(std::move(job)), kvOverride()));
    EXPECT_EQ(handled, 1.0);
  }
  AWAIT(settled, memory.get(ledger_state::paidFallbackRequestV3Key(keyId, requestId)));
  EXPECT_EQ(settled.value["billing_state"], rt::KvValue("settled"));
  AWAIT(cleared, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, requestId)));
  EXPECT_EQ(cleared.value, rt::KvValue(nullptr));
  co_await settleBackfill();
}

ASYNC_TEST("disabled paid fallback does not rewrite an expired usage window") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.clear();
  const double expiredResetAtMs = rt::dateNow() - 1;
  json::object seedOverrides;
  seedOverrides["id"] = "key-disabled-expired";
  seedOverrides["hash"] = "hash-disabled-expired";
  seedOverrides["paid_fallback_enabled"] = false;
  seedOverrides["usage_reset_at_ms"] = number(expiredResetAtMs);
  AWAIT(record, seedStrictKey(seedOverrides));
  const std::string id(record.at("id").as_string());

  AWAIT(decision, index_::reservePaidFallback(reservationInput(id, "request-disabled")));
  json::object skip;
  skip["kind"] = "skip";
  skip["reason"] = "disabled";
  EXPECT_EQ(decisionJson(decision), json::value(skip));
  AWAIT(stored, memory.get(apiKeyIdKey(id)));
  EXPECT_EQ(stored.value["usage_reset_at_ms"], rt::KvValue(expiredResetAtMs));
  EXPECT_EQ(stored.value["paid_fallback_reservation_request_id"], rt::KvValue(nullptr));
  co_await settleBackfill();
}
