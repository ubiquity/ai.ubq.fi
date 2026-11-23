// Port of tests/paid-fallback-v3-cutover.test.ts.
// The TS file's MemoryKv fake is ported as MemoryKv below: rows in a Map keyed by JSON.stringify(key)
// (insertion order), counter versionstamps, `list` over the rows whose key starts with the prefix in
// that order (the fake ignores start, end and every option), get/set/atomic and nothing else (no
// delete, getMany, enqueue or expiry: a call to one of them throws TypeError, as calling an undefined
// method does). Like the fake, each call settles one microtask after doing its work. One store serves
// both cases and is installed with setKvForTest before each case and removed after it, as the TS
// beforeEach/afterEach do. createFetchMock(handler) is a scoped rt::GlobalFetchOverride; outside it
// fetch is rt's default (no transport: a call fails, where the TS would reach the network; the bun
// oracle run made no fetch). Credential isolation: HOME, CODEX_HOME, CODEX_AUTH_JSON_B64,
// METERED_API_KEY, OPENLUX_API_KEY and SURPLUS_API_KEY are unset and config.codexAuthJsonB64 is empty
// (the oracle ran under `env -i` with an empty HOME); the cases set METERED_API_KEY themselves.
// recordMeteredTerminal / recordMeteredAmbiguousFailure start the default terminal sweep
// (reconcileDuePaidFallbacksV3 in the background), as in the TS.
// C++-only step: the one-time paid-fallback backfill the first admission schedules runs on the case's
// loop, which dies with the case body, so both cases await it at their end
// (ledger_backfill::scheduledPaidFallbackBackfillForTest()). In the TS it runs on the shared event loop.
// Links: tests/paid_fallback_v3_cutover_test.cpp src/api_keys.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
#include "uos/api_keys.hpp"
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/index.hpp"
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
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace index_ = uos::paid_fallback::index;
namespace ledger_backfill = uos::paid_fallback::ledger_backfill;
namespace ledger_state = uos::paid_fallback::ledger_state;
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
#define EXPECT_EQ_MESSAGE(actual, expected, message)                                                                                                \
  do {                                                                                                                                             \
    if (!((actual) == (expected)))                                                                                                                 \
      ::uos::rt::test::report(__FILE__, __LINE__, std::string("expected " #actual " == " #expected " (") + (message) + ")\n      actual:   " + \
                                                    ::uos::rt::show(actual) + "\n      expected: " + ::uos::rt::show(expected), false);              \
  } while (0)

namespace {
  // `encodeKey`: JSON.stringify(key).
  std::string encodeKey(const rt::KvKey& key) { return rt::kvJsonStringify(rt::kvValueFromKey(key)).value_or(std::string()); }

  // `startsWithKey`: prefix.every((part, index) => key[index] === part)
  bool startsWithKey(const rt::KvKey& key, const rt::KvKey& prefix) {
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      if (index >= key.size() || !rt::kvKeyPartStrictEquals(key[index], prefix[index])) return false;
    }
    return true;
  }

  // The fake's list iterator: a generator over the rows matched at the call (`cursor` is always "").
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

  class MemoryKv final : public rt::Kv {
  public:
    struct StoredEntry {
      rt::KvKey key;
      rt::KvValue value;
      std::string versionstamp;
    };
    // One recorded atomic mutation: `{ type: "set", key, value }` or `{ type: "delete", key }`.
    struct Mutation {
      bool remove = false;
      rt::KvKey key;
      rt::KvValue value;
    };

    rt::OrderedMap<std::string, StoredEntry> entries;

    using rt::Kv::get;
    using rt::Kv::getMany;

    // What `commit()` of the fake's atomic operation does before it returns its resolved promise.
    rt::KvCommitResult commitNow(const std::vector<rt::AtomicCheck>& checks, const std::vector<Mutation>& mutations) {
      for (const rt::AtomicCheck& check : checks) {
        const StoredEntry* entry = entries.get(encodeKey(check.key));
        const std::optional<std::string> current = entry == nullptr ? std::nullopt : std::optional<std::string>(entry->versionstamp);
        if (current != check.versionstamp) return rt::KvCommitResult{.ok = false, .versionstamp = ""};
      }
      const std::string stamp = nextVersionstamp();
      for (const Mutation& mutation : mutations) {
        if (mutation.remove) entries.delete_(encodeKey(mutation.key));
        else entries.set(encodeKey(mutation.key), StoredEntry{.key = mutation.key, .value = rt::kvStructuredClone(mutation.value), .versionstamp = stamp});
      }
      return rt::KvCommitResult{.ok = true, .versionstamp = stamp};
    }

    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions = {}) override {
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

    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
      const std::string stamp = nextVersionstamp();
      entries.set(encodeKey(key), StoredEntry{.key = key, .value = rt::kvStructuredClone(value), .versionstamp = stamp});
      const rt::KvCommitResult result{.ok = true, .versionstamp = stamp};
      co_await rt::microtask();
      co_return result;
    }

    rt::Task<void> delete_(rt::KvKey) override {
      throw rt::TypeError("kv.delete is not a function");
      co_return;
    }

    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions = {}) override {
      const rt::KvKey prefix = selector.prefix.value_or(rt::KvKey{});
      std::vector<rt::KvEntry<>> matched;
      for (const auto& [encoded, entry] : entries) {
        if (startsWithKey(entry.key, prefix)) matched.push_back(rt::KvEntry<>{entry.key, rt::kvStructuredClone(entry.value), entry.versionstamp});
      }
      return std::make_unique<SnapshotIterator>(std::move(matched));
    }

    std::unique_ptr<rt::AtomicOperation> atomic() override;

    void close() override {}

  private:
    double version = 0;

    std::string nextVersionstamp() {
      version += 1;
      return rt::padStart(rt::numberToString(version), 20, "0");
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
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
      mutations.push_back(MemoryKv::Mutation{.remove = false, .key = std::move(key), .value = rt::kvStructuredClone(value)});
      return *this;
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override {
      mutations.push_back(MemoryKv::Mutation{.remove = true, .key = std::move(key), .value = rt::KvValue()});
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

  // `const memoryKv = new MemoryKv(); const kv = memoryKv as unknown as Kv;`
  const std::shared_ptr<MemoryKv>& store() {
    static const std::shared_ptr<MemoryKv> instance = std::make_shared<MemoryKv>();
    return instance;
  }
  MemoryKv& memoryKv() { return *store(); }
  std::optional<std::shared_ptr<rt::Kv>> kvOverride() { return std::optional<std::shared_ptr<rt::Kv>>(std::shared_ptr<rt::Kv>(store())); }

  // beforeEach `setKvForTest(kv)` / afterEach `setKvForTest(null)`, with the mandatory credential
  // isolation.
  struct Fixture {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt},           {"CODEX_HOME", std::nullopt},      {"CODEX_AUTH_JSON_B64", std::nullopt},
                                        {"METERED_API_KEY", std::nullopt}, {"OPENLUX_API_KEY", std::nullopt}, {"SURPLUS_API_KEY", std::nullopt}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    Fixture() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::kv::setKvForTest(store());
    }
    ~Fixture() {
      uos::kv::setKvForTest(nullptr);
      uos::config::config.codexAuthJsonB64 = savedSeed;
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
  };

  // C++ only: let the one-time backfill an admission scheduled settle before the case's loop dies.
  rt::Task<void> settleBackfill() {
    while (const std::optional<rt::Promise<void>> scheduled = ledger_backfill::scheduledPaidFallbackBackfillForTest()) co_await scheduled->wait();
  }

  const std::string keyId = "v3-cutover-key";
  const std::string keyHash = "v3-cutover-hash";
  // `legacyRequestLogPrefix`
  rt::KvKey legacyRequestLogPrefix() { return rt::KvKey{"ubq_ai", "api_keys", "request_log", keyId}; }

  json::value number(double value) { return rt::makeNumber(value); }

  // `countPrefix(prefix)`
  rt::Task<std::size_t> countPrefix(rt::KvKey prefix) {
    std::vector<rt::KvKey> keys;
    rt::KvListSelector selector;
    selector.prefix = std::move(prefix);
    const std::unique_ptr<rt::KvListIterator> iterator = memoryKv().list(selector);
    while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) keys.push_back(entry->key);
    co_return keys.size();
  }

  // `reservationInput(requestId, createdAtMs)`
  index_::ReservePaidFallbackInput reservationInput(std::string requestId, double createdAtMs) {
    return index_::ReservePaidFallbackInput{
      .keyId = keyId,
      .requestId = std::move(requestId),
      .createdAtMs = createdAtMs,
      .model = "gpt-5-codex",
      .route = "responses",
      .path = "/v1/responses",
      .stream = true,
      .reasoning = "high",
      .allowUnrosteredModel = std::nullopt,
      .reason = "primary_429",
    };
  }

  std::string kindOf(const index_::PaidFallbackReservationDecision& decision) {
    if (std::holds_alternative<index_::PaidFallbackReservationDecisionReserved>(decision)) return "reserved";
    if (std::holds_alternative<index_::PaidFallbackReservationDecisionSkip>(decision)) return "skip";
    return "blocked";
  }

  // `reserve(requestId, createdAtMs)`
  rt::Task<index_::PaidFallbackReservation> reserve(std::string requestId, double createdAtMs) {
    rt::Task<index_::PaidFallbackReservationDecision> reserving = index_::reservePaidFallback(reservationInput(std::move(requestId), createdAtMs));
    const index_::PaidFallbackReservationDecision decision = co_await std::move(reserving);
    ASSERT_EQ(kindOf(decision), "reserved");
    const index_::PaidFallbackReservation reservation = std::get<index_::PaidFallbackReservationDecisionReserved>(decision).reservation;
    co_return reservation;
  }

  // `readRequest(requestId)`: the stored PaidFallbackRequestV3 row (`assert.ok(entry.value)`).
  rt::Task<rt::KvValue> readRequest(std::string requestId) {
    const rt::KvKey key = ledger_state::paidFallbackRequestV3Key(keyId, requestId);
    rt::Task<rt::KvEntryMaybe<>> reading = memoryKv().get(key);
    const rt::KvEntryMaybe<> entry = co_await std::move(reading);
    ASSERT(entry.value.truthy());
    co_return entry.value;
  }

  // `assertLegacyStateUnchanged(expected)`
  rt::Task<void> assertLegacyStateUnchanged(json::object expected) {
    rt::Task<rt::KvEntryMaybe<>> idReading = memoryKv().get(apiKeyIdKey(keyId));
    const rt::KvEntryMaybe<> idEntry = co_await std::move(idReading);
    rt::Task<rt::KvEntryMaybe<>> hashReading = memoryKv().get(apiKeyHashKey(keyHash));
    const rt::KvEntryMaybe<> hashEntry = co_await std::move(hashReading);
    EXPECT_EQ(idEntry.value, rt::KvValue(expected));
    EXPECT_EQ(hashEntry.value["paid_fallback_spent_microcredits"], rt::KvValue(expected.at("paid_fallback_spent_microcredits")));
    EXPECT_EQ(hashEntry.value["paid_fallback_reserved_microcredits"], rt::KvValue(expected.at("paid_fallback_reserved_microcredits")));
    EXPECT_EQ(hashEntry.value["paid_fallback_reservation_request_id"], rt::KvValue(expected.at("paid_fallback_reservation_request_id")));
    rt::Task<std::size_t> counting = countPrefix(legacyRequestLogPrefix());
    const std::size_t legacyLogs = co_await std::move(counting);
    EXPECT_EQ(legacyLogs, std::size_t{0});
  }

  // The `ApiKeyRecord` literals of both cases (they differ in name and the legacy counters).
  json::object apiKeyRecord(double now, std::string_view name, double spent, double reserved, std::optional<std::string> reservationRequestId) {
    json::object record;
    record["id"] = keyId;
    record["name"] = json::string(name);
    record["prefix"] = "u_v3";
    record["hash"] = keyHash;
    record["created_at_ms"] = number(now - 60'000);
    record["expires_at_ms"] = number(-1);
    record["revoked_at_ms"] = nullptr;
    record["usage_limit_requests"] = number(100);
    record["usage_requests"] = number(0);
    record["usage_reset_at_ms"] = number(now + 60'000);
    record["window_ms"] = number(60'000);
    record["usage_quota_version"] = number(3);
    record["paid_fallback_enabled"] = true;
    record["paid_fallback_limit_microcredits"] = number(5'000'000);
    record["paid_fallback_spent_microcredits"] = number(spent);
    record["paid_fallback_reserved_microcredits"] = number(reserved);
    record["paid_fallback_reservation_request_id"] = reservationRequestId ? json::value(json::string(*reservationRequestId)) : json::value(nullptr);
    json::array models;
    models.push_back("gpt-5-codex");
    record["paid_fallback_model_ids"] = std::move(models);
    record["paid_fallback_quota_per_credit"] = number(500'000);
    json::object exposure;
    exposure["gpt-5-codex"] = number(250'000);
    record["paid_fallback_max_exposure_microcredits"] = std::move(exposure);
    record["paid_fallback_pricing_checked_at_ms"] = number(now - 30'000);
    return record;
  }

  // The hash row both cases write: the record's id, usage and legacy paid-fallback members.
  json::object hashRecordOf(const json::object& record) {
    json::object hash;
    for (const std::string_view name : {"id", "expires_at_ms", "revoked_at_ms", "usage_limit_requests", "usage_requests", "usage_reset_at_ms", "window_ms",
                                        "usage_quota_version", "paid_fallback_enabled", "paid_fallback_limit_microcredits", "paid_fallback_spent_microcredits",
                                        "paid_fallback_reserved_microcredits", "paid_fallback_reservation_request_id"}) {
      hash[name] = record.at(name);
    }
    return hash;
  }

  // `new Response(null, { status })`
  rt::Response emptyResponse(double status) {
    rt::ResponseInit init;
    init.status = status;
    return rt::Response(rt::BodyInit(nullptr), init);
  }

  index_::SurplusUsage surplusUsage(std::optional<double> input, std::optional<double> cached, std::optional<double> written, std::optional<double> output,
                                    rt::Nullable<double> buyerCostMicro = {}) {
    return index_::SurplusUsage{
      .input_tokens = input,
      .cached_input_tokens = cached,
      .cache_write_input_tokens = written,
      .output_tokens = output,
      .buyer_cost_micro = buyerCostMicro,
    };
  }

  index_::SurplusBillingPricing surplusPricing(double input, double output, std::optional<double> cacheRead, std::optional<double> cacheWrite,
                                               std::optional<bool> requireProviderCost = std::nullopt) {
    return index_::SurplusBillingPricing{
      .input_price_per_token = input,
      .output_price_per_token = output,
      .cache_read_price_per_token = cacheRead,
      .cache_write_price_per_token = cacheWrite,
      .require_provider_cost = requireProviderCost,
    };
  }

  rt::Task<rt::Response> rejectedFetch(std::string message) {
    throw rt::JsError(message);
    co_return rt::Response();
  }
}

ASYNC_TEST("Metered runtime lifecycle hard-cuts legacy counters and request logs in favor of V3") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  const double now = rt::dateNow();
  const json::object record = apiKeyRecord(now, "V3 cutover", 4'900'000, 200'000, std::string("legacy-stale-reservation"));
  const json::object hashRecord = hashRecordOf(record);
  AWAIT_DONE(idSet, memory.set(apiKeyIdKey(keyId), rt::KvValue(record)));
  AWAIT_DONE(hashSet, memory.set(apiKeyHashKey(keyHash), rt::KvValue(hashRecord)));

  {
    const rt::env::ScopedOverride meteredKey{{"METERED_API_KEY", std::string("test-metered-key")}};
    AWAIT(terminalReservation, reserve("terminal-missing-id", now));
    EXPECT_EQ(terminalReservation.reserved_microcredits, 250'000.0);
    EXPECT_EQ(terminalReservation.quota_used_percent, std::optional<double>(0));
    AWAIT_DONE(terminalUpstream, index_::recordMeteredUpstreamResponse(terminalReservation, emptyResponse(502), std::nullopt));
    AWAIT_DONE(terminalRecorded, index_::recordMeteredTerminal(terminalReservation, "incomplete"));
    AWAIT(terminal, readRequest("terminal-missing-id"));
    EXPECT_EQ(terminal["dispatch_state"], rt::KvValue("dispatched"));
    EXPECT_EQ(terminal["terminal_state"], rt::KvValue("incomplete"));
    EXPECT_EQ(terminal["provider_request_id"], rt::KvValue(nullptr));
    EXPECT_EQ(terminal["billing_state"], rt::KvValue("pending"));
    EXPECT_EQ(terminal["reserved_microcredits"], rt::KvValue(250'000));
    EXPECT_EQ(terminal["reconciliation_attempts"], rt::KvValue(1));
    AWAIT(terminalPending, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, terminal["request_id"].as<std::string>())));
    EXPECT(terminalPending.value.truthy());

    AWAIT(ambiguousReservation, reserve("ambiguous", now + 1));
    AWAIT_DONE(ambiguousRecorded, index_::recordMeteredAmbiguousFailure(ambiguousReservation));
    AWAIT(ambiguous, readRequest("ambiguous"));
    EXPECT_EQ(ambiguous["dispatch_state"], rt::KvValue("dispatched"));
    EXPECT_EQ(ambiguous["terminal_state"], rt::KvValue("ambiguous"));
    EXPECT_EQ(ambiguous["billing_state"], rt::KvValue("pending"));
    EXPECT_EQ(ambiguous["reserved_microcredits"], rt::KvValue(250'000));
    AWAIT(ambiguousPending, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, ambiguous["request_id"].as<std::string>())));
    EXPECT(ambiguousPending.value.truthy());

    AWAIT(dispatchedReservation, reserve("dispatched-http-error", now + 2));
    AWAIT_DONE(dispatchedUpstream,
               index_::recordMeteredUpstreamResponse(dispatchedReservation, emptyResponse(503), std::string("provider-request-id"), std::string("surplus")));
    AWAIT(dispatched, readRequest("dispatched-http-error"));
    EXPECT_EQ(dispatched["dispatch_state"], rt::KvValue("dispatched"));
    EXPECT_EQ(dispatched["terminal_state"], rt::KvValue("pending"));
    EXPECT_EQ(dispatched["provider_request_id"], rt::KvValue("provider-request-id"));
    EXPECT_EQ(dispatched["provider"], rt::KvValue("surplus"));
    EXPECT_EQ(dispatched["billing_state"], rt::KvValue("pending"));
    EXPECT_EQ(dispatched["reserved_microcredits"], rt::KvValue(250'000));

    AWAIT(cancelledReservation, reserve("cancelled-before-dispatch", now + 3));
    AWAIT_DONE(cancelledRecorded, index_::recordMeteredUndispatchedCancellation(cancelledReservation));
    AWAIT(cancelled, readRequest("cancelled-before-dispatch"));
    EXPECT_EQ(cancelled["dispatch_state"], rt::KvValue("not_dispatched"));
    EXPECT_EQ(cancelled["terminal_state"], rt::KvValue("cancelled"));
    EXPECT_EQ(cancelled["billing_state"], rt::KvValue("not_billed"));
    EXPECT_EQ(cancelled["spend_microcredits"], rt::KvValue(0));
    AWAIT(cancelledPending, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, cancelled["request_id"].as<std::string>())));
    EXPECT_EQ(cancelledPending.value, rt::KvValue(nullptr));

    AWAIT(window, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, terminalReservation.window_reset_at_ms)));
    EXPECT_EQ(window.value["settled_microcredits"], rt::KvValue(0));
    EXPECT_EQ(window.value["reserved_microcredits"], rt::KvValue(750'000));
    EXPECT_EQ(window.value["pending_count"], rt::KvValue(3));
    AWAIT_DONE(legacy, assertLegacyStateUnchanged(record));
  }
  co_await settleBackfill();
}

ASYNC_TEST("Surplus usage settles cache read and write pricing, while incomplete usage stays pending") {
  const Fixture fixture;
  MemoryKv& memory = memoryKv();
  memory.entries.clear();
  const double now = rt::dateNow();
  const json::object record = apiKeyRecord(now, "Surplus billing", 0, 0, std::nullopt);
  AWAIT_DONE(idSet, memory.set(apiKeyIdKey(keyId), rt::KvValue(record)));
  AWAIT_DONE(hashSet, memory.set(apiKeyHashKey(keyHash), rt::KvValue(hashRecordOf(record))));

  {
    const rt::env::ScopedOverride meteredKey{{"METERED_API_KEY", std::string("test-metered-key")}};
    AWAIT(settledReservation, reserve("surplus-settled", now));
    AWAIT_DONE(settledUpstream, index_::recordMeteredUpstreamResponse(settledReservation, emptyResponse(200), std::nullopt, std::string("surplus")));
    AWAIT_DONE(settledTerminal, index_::recordMeteredTerminal(settledReservation, "completed", std::string("surplus")));
    const std::string surplusSettlementId = "surplus:" + settledReservation.request_id;
    const index_::SurplusUsage cachedUsage = surplusUsage(100, 20, 30, 10);
    const index_::SurplusBillingPricing cachedPricing = surplusPricing(0.000001, 0.000003, 0.0000001, 0.000002);
    AWAIT_DONE(firstSettlement, index_::recordSurplusUsage(settledReservation, surplusSettlementId, "gpt-5-codex", cachedUsage, cachedPricing));
    AWAIT(settled, readRequest("surplus-settled"));
    EXPECT_EQ(settled["provider_request_id"], rt::KvValue(nullptr));
    EXPECT_EQ(settled["billing_state"], rt::KvValue("settled"));
    EXPECT_EQ(settled["provider_quota"], rt::KvValue(86));
    EXPECT_EQ(settled["spend_microcredits"], rt::KvValue(172));
    AWAIT(settledWindowBeforeReplay, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, settledReservation.window_reset_at_ms)));
    AWAIT_DONE(replayedSettlement, index_::recordSurplusUsage(settledReservation, surplusSettlementId, "gpt-5-codex", cachedUsage, cachedPricing));
    AWAIT(settledAfterReplay, readRequest("surplus-settled"));
    AWAIT(settledWindowAfterReplay, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, settledReservation.window_reset_at_ms)));
    EXPECT_EQ(settledAfterReplay["spend_microcredits"], settled["spend_microcredits"]);
    EXPECT_EQ(settledWindowAfterReplay.value["settled_microcredits"], settledWindowBeforeReplay.value["settled_microcredits"]);
    EXPECT_EQ(settledWindowAfterReplay.value["reserved_microcredits"], settledWindowBeforeReplay.value["reserved_microcredits"]);

    // Real native Surplus measurements must override the catalog estimate.
    // An odd 69-microcredit charge cannot round through 500,000 quota/credit
    // into 70 microcredits, and an explicitly free completion stays free.
    struct NativeFixture {
      std::string id;
      double cost;
      std::optional<double> cached;
      std::optional<double> written;
    };
    const std::vector<NativeFixture> nativeFixtures{
      {.id = "surplus-native-fill", .cost = 1056, .cached = std::nullopt, .written = 1028},
      {.id = "surplus-native-read", .cost = 69, .cached = 1028, .written = std::nullopt},
      {.id = "surplus-native-free", .cost = 0, .cached = std::nullopt, .written = std::nullopt},
    };
    for (std::size_t index = 0; index < nativeFixtures.size(); ++index) {
      const NativeFixture& item = nativeFixtures[index];
      AWAIT(reservation, reserve(item.id, now));
      AWAIT_DONE(upstream, index_::recordMeteredUpstreamResponse(reservation, emptyResponse(200), std::nullopt, std::string("surplus")));
      AWAIT_DONE(terminal, index_::recordMeteredTerminal(reservation, "completed", std::string("surplus")));
      const index_::SurplusUsage usage = surplusUsage(1043, item.cached, item.written, 4, item.cost);
      const index_::SurplusBillingPricing pricing = surplusPricing(0.000001, 0.000003, 0.0000001, 0.000002);
      AWAIT_DONE(settlement, index_::recordSurplusUsage(reservation, "surplus:" + item.id, "gpt-5-codex", usage, pricing));
      AWAIT(actual, readRequest(item.id));
      EXPECT_EQ(actual["billing_state"], rt::KvValue("settled"));
      EXPECT_EQ(actual["spend_microcredits"], rt::KvValue(item.cost));
      EXPECT_EQ_MESSAGE(actual["cached_input_tokens"], item.cached ? rt::KvValue(*item.cached) : rt::KvValue(nullptr),
                        "unknown cache reads must stay unknown in the authoritative settlement");
      AWAIT(windowBeforeReplay, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, reservation.window_reset_at_ms)));
      AWAIT_DONE(replay, index_::recordSurplusUsage(reservation, "surplus:" + item.id, "gpt-5-codex", usage, pricing));
      AWAIT(windowAfterReplay, memory.get(ledger_state::paidFallbackWindowV3Key(keyId, reservation.window_reset_at_ms)));
      AWAIT(replayed, readRequest(item.id));
      EXPECT_EQ(replayed["spend_microcredits"], rt::KvValue(item.cost));
      EXPECT_EQ(windowAfterReplay.value["settled_microcredits"], windowBeforeReplay.value["settled_microcredits"]);
      EXPECT_EQ(windowAfterReplay.value["reserved_microcredits"], windowBeforeReplay.value["reserved_microcredits"]);
    }

    const std::vector<rt::Nullable<double>> invalidCosts{-1.0, 0.1, rt::Number::MAX_SAFE_INTEGER + 1, nullptr};
    for (std::size_t index = 0; index < invalidCosts.size(); ++index) {
      const rt::Nullable<double>& buyerCostMicro = invalidCosts[index];
      const std::string id = "surplus-native-invalid-" + (buyerCostMicro.isNull() ? std::string("null") : rt::numberToString(*buyerCostMicro));
      AWAIT(reservation, reserve(id, now));
      AWAIT_DONE(upstream, index_::recordMeteredUpstreamResponse(reservation, emptyResponse(200), std::nullopt, std::string("surplus")));
      AWAIT_DONE(terminal, index_::recordMeteredTerminal(reservation, "completed", std::string("surplus")));
      const index_::SurplusUsage usage = surplusUsage(1043, 1028, std::nullopt, 4, buyerCostMicro);
      const index_::SurplusBillingPricing pricing = surplusPricing(0.000001, 0.000003, std::nullopt, std::nullopt);
      AWAIT_DONE(settlement, index_::recordSurplusUsage(reservation, "surplus:" + id, "gpt-5-codex", usage, pricing));
      AWAIT(actual, readRequest(id));
      EXPECT_EQ_MESSAGE(actual["billing_state"], rt::KvValue("pending"), "a malformed authoritative charge must not fall back to guessed pricing");
      EXPECT_EQ(actual["spend_microcredits"], rt::KvValue(nullptr));
    }

    AWAIT(nativeUnmeasured, reserve("surplus-native-unmeasured-cost", now));
    AWAIT_DONE(unmeasuredUpstream, index_::recordMeteredUpstreamResponse(nativeUnmeasured, emptyResponse(200), std::nullopt, std::string("surplus")));
    AWAIT_DONE(unmeasuredTerminal, index_::recordMeteredTerminal(nativeUnmeasured, "completed", std::string("surplus")));
    const index_::SurplusUsage unmeasuredUsage = surplusUsage(1043, std::nullopt, 1028, 4);
    const index_::SurplusBillingPricing unmeasuredPricing = surplusPricing(0.000001, 0.000003, std::nullopt, 0.000002, true);
    AWAIT_DONE(unmeasuredSettlement, index_::recordSurplusUsage(nativeUnmeasured, "surplus:native-unmeasured", "gpt-5-codex", unmeasuredUsage, unmeasuredPricing));
    AWAIT(unmeasuredBilling, readRequest(nativeUnmeasured.request_id));
    EXPECT_EQ_MESSAGE(unmeasuredBilling["billing_state"], rt::KvValue("pending"), "native gross cache writes must not use legacy additive pricing");
    AWAIT(unmeasuredSpend, readRequest(nativeUnmeasured.request_id));
    EXPECT_EQ(unmeasuredSpend["spend_microcredits"], rt::KvValue(nullptr));

    AWAIT(incompleteReservation, reserve("surplus-incomplete", now + 1));
    AWAIT_DONE(incompleteUpstream, index_::recordMeteredUpstreamResponse(incompleteReservation, emptyResponse(200), std::string("surplus-incomplete-request"),
                                                                         std::string("surplus")));
    AWAIT_DONE(incompleteTerminal, index_::recordMeteredTerminal(incompleteReservation, "completed", std::string("surplus")));
    const index_::SurplusUsage incompleteUsage = surplusUsage(std::nullopt, std::nullopt, std::nullopt, 10);
    const index_::SurplusBillingPricing incompletePricing = surplusPricing(0.000001, 0.000003, std::nullopt, std::nullopt);
    AWAIT_DONE(incompleteSettlement,
               index_::recordSurplusUsage(incompleteReservation, "surplus-incomplete-request", "gpt-5-codex", incompleteUsage, incompletePricing));

    const auto meteredLogCalls = std::make_shared<int>(0);
    {
      const rt::GlobalFetchOverride fetch(rt::Fetch([meteredLogCalls](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
        *meteredLogCalls += 1;
        return rejectedFetch("Surplus reconciliation must not call Metered logs");
      }));
      AWAIT_DONE(reconciled, ledger_backfill::reconcilePaidFallbackV3(keyId, rt::dateNow(), kvOverride()));
    }
    AWAIT(incomplete, readRequest("surplus-incomplete"));
    EXPECT_EQ(*meteredLogCalls, 0);
    EXPECT_EQ(incomplete["billing_state"], rt::KvValue("pending"));
    EXPECT_EQ(incomplete["spend_microcredits"], rt::KvValue(nullptr));
    AWAIT(incompletePending, memory.get(ledger_state::paidFallbackPendingV3Key(keyId, incomplete["request_id"].as<std::string>())));
    EXPECT(incompletePending.value.truthy());

    AWAIT(meteredReservation, reserve("metered-must-ignore-surplus-cost", now));
    AWAIT_DONE(meteredUpstream,
               index_::recordMeteredUpstreamResponse(meteredReservation, emptyResponse(200), std::string("metered-request"), std::string("metered")));
    AWAIT_DONE(meteredTerminal, index_::recordMeteredTerminal(meteredReservation, "completed", std::string("metered")));
    const index_::SurplusUsage meteredUsage = surplusUsage(1043, 1028, std::nullopt, 4, 69.0);
    const index_::SurplusBillingPricing meteredPricing = surplusPricing(0.000001, 0.000003, std::nullopt, std::nullopt);
    AWAIT_DONE(meteredSettlement, index_::recordSurplusUsage(meteredReservation, "surplus:must-not-match-metered", "gpt-5-codex", meteredUsage, meteredPricing));
    AWAIT(meteredBilling, readRequest(meteredReservation.request_id));
    EXPECT_EQ(meteredBilling["billing_state"], rt::KvValue("pending"));
    AWAIT(meteredSpend, readRequest(meteredReservation.request_id));
    EXPECT_EQ(meteredSpend["spend_microcredits"], rt::KvValue(nullptr));
  }
  co_await settleBackfill();
}
