// Links: tests/sentinel_replay_retention_metadata_differential_test.cpp src/sentinel/replay_retention_metadata.cpp
//        src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_read.cpp src/sentinel/replay_retention_schema.cpp
//        src/sentinel/replay_model.cpp src/sentinel/replay_envelope.cpp src/sentinel/replay_observation.cpp
//        src/sentinel/upstream_capture.cpp src/sentinel/replay_limits.cpp src/sentinel/incident_outbox.cpp src/request.cpp
//        src/utils.cpp src/config.cpp src/release.cpp src/kv.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/value_compatibility.cpp
// Differential cases for src/sentinel/replay-retention-metadata.ts. Every case of its direct test,
// tests/sentinel-replay-retention-metadata.test.ts, drives src/sentinel/replay-retention.ts (admission, pruning and
// maintenance), not ported yet: "native over-cap request rows cannot hide corrupt tombstone pruning candidates",
// "native over-cap metadata makes bounded pruning progress without relaxing admission limits", "native TTL
// disappearance restores maintenance admission and complete metadata accounting", "a concurrent native status
// admission invalidates the whole metadata recount CAS", "corruption partial-list failure and an over-limit scan
// never lower metadata counters", "healthy admin polls inside the interval skip the metadata recount and the ledger
// write", "a removed status row converges at the next due poll while metadata pressure reclaims it at once", "a
// recount that loses to a concurrent writer, or a stale or corrupt marker, leaves the next poll due", "a due recount
// that meets a corrupt metadata row backs off for the interval instead of rescanning on every poll", "of two
// concurrent due recounts only the first to commit records the marker, so it never moves back". This file runs the
// module's own steps of those scenarios (recounts, markers, backoff, races, list/get failures, the over-limit witness)
// over rows written directly and the same fakes.
//
// sentinel_replay_retention_metadata_differential_vectors.inc was recorded from Bun 1.4.0 running the TS on the real
// native :memory: KV. Each step records its result, whether the ledger versionstamp moved, a view of the ledger and
// the marker and recount-failure rows; here the same steps run in the same order over rt::MemoryKv and each entry
// must serialize to the same JSON text.
#include "uos/sentinel/replay_limits.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_retention_bootstrap.hpp"
#include "uos/sentinel/replay_retention_metadata.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace limits = uos::sentinel::replay_limits;
  namespace model = uos::sentinel::replay_model;
  namespace bootstrap = uos::sentinel::replay_retention_bootstrap;
  namespace metadata = uos::sentinel::replay_retention_metadata;
  namespace schema = uos::sentinel::replay_retention_schema;

#include "sentinel_replay_retention_metadata_differential_vectors.inc"

  constexpr double NOW = 1'702'000'000'000;
  constexpr double BUDGET = 1 * 1'024 * 1'024;
  constexpr double INTERVAL = metadata::SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS;

  double kvClock() { return rt::dateNow(); }

  const rt::KvKey& FAILED_KEY() {
    static const rt::KvKey key{"uos_ai", "sentinel_replay", "v1", "metadata_recount_failed"};
    return key;
  }

  const json::array& expectedTranscript() {
    static const json::value parsed = rt::jsonParse(METADATA_ORACLE_JSON);
    return parsed.at("transcript").as_array();
  }

  json::object object(std::initializer_list<std::pair<std::string_view, json::value>> members) {
    json::object out;
    for (const auto& [key, member] : members) rt::objectDefine(out, key, member);
    return out;
  }

  json::object statusRow(const std::string& requestId) {
    return object({{"version", 1},
                   {"request_id", json::string(requestId)},
                   {"status", "disabled"},
                   {"reason", "metadata_fixture"},
                   {"captured_at_ms", rt::makeNumber(NOW)},
                   {"manifest_key", nullptr},
                   {"fingerprint", nullptr},
                   {"expires_at_ms", nullptr}});
  }

  json::object tombstoneRow(double index, const std::string& requestId) {
    char hex[8];
    const auto end = std::to_chars(hex, hex + sizeof hex, static_cast<std::uint32_t>(index), 16).ptr;
    return object({{"version", 1},
                   {"fingerprint", json::string(rt::padStart(std::string(hex, end), 64, "0"))},
                   {"request_id", json::string(requestId)},
                   {"evicted_at_ms", rt::makeNumber(NOW)},
                   {"reason", json::string(std::string(schema::SENTINEL_REPLAY_EXPIRED_REASON))}});
  }

  rt::KvKey tombstoneKey(const json::object& row) { return rt::KvKey{schema::SENTINEL_REPLAY_EVICTION_PREFIX, std::string(row.at("fingerprint").as_string())}; }

  rt::Task<json::value> ledgerView(rt::Kv& kv) {
    const rt::KvEntryMaybe<> entry = co_await kv.get(schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
    if (entry.value.isNull()) co_return json::value(nullptr);
    json::object view;
    for (const std::string_view field : {"budget_bytes", "stored_bytes", "records", "status_records", "metadata_bytes", "status_pruned_records",
                                         "bootstrap_complete", "bootstrap_cursor", "accounting_error", "over_budget"}) {
      const rt::KvValue& member = entry.value[field];
      if (!member.isUndefined()) view[field] = member.toJson();
    }
    co_return json::value(std::move(view));
  }

  rt::Task<std::optional<std::string>> ledgerVersionstamp(rt::Kv& kv) {
    const rt::KvEntryMaybe<> entry = co_await kv.get(schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
    co_return entry.versionstamp;
  }

  rt::Task<json::value> rowJson(rt::Kv& kv, rt::KvKey key) {
    const rt::KvEntryMaybe<> entry = co_await kv.get(std::move(key));
    co_return entry.value.toJson();
  }

  // The transcript, compared entry by entry with the recorded one as it is built.
  class Transcript {
  public:
    explicit Transcript(rt::Kv& kv) : kv_(kv) {}
    rt::Task<void> step(std::string name, rt::Task<json::value> task) {
      const std::optional<std::string> before = co_await ledgerVersionstamp(kv_);
      json::object entry;
      entry["step"] = json::string(name);
      try {
        json::value result = co_await std::move(task);
        entry["ok"] = std::move(result);
      } catch (const rt::JsError& error) {
        entry["error"] = json::string(error.toString());
      }
      const std::optional<std::string> after = co_await ledgerVersionstamp(kv_);
      entry["ledger_written"] = before != after;
      json::value ledger = co_await ledgerView(kv_);
      entry["ledger"] = std::move(ledger);
      json::value marker = co_await rowJson(kv_, metadata::SENTINEL_REPLAY_METADATA_RECONCILED_KEY);
      entry["marker"] = std::move(marker);
      json::value failure = co_await rowJson(kv_, FAILED_KEY());
      entry["failure"] = std::move(failure);
      compare(json::value(std::move(entry)));
    }
    void finish() {
      if (index_ != expectedTranscript().size())
        rt::test::report(__FILE__, __LINE__, "ran " + std::to_string(index_) + " of " + std::to_string(expectedTranscript().size()) + " recorded steps", false);
    }

  private:
    void compare(const json::value& actual) {
      if (index_ >= expectedTranscript().size()) {
        rt::test::report(__FILE__, __LINE__, "unrecorded step " + rt::jsonStringify(actual), false);
        return;
      }
      const json::value& wanted = expectedTranscript().at(index_++);
      const std::string actualText = rt::jsonStringify(actual);
      const std::string wantedText = rt::jsonStringify(wanted);
      if (actualText != wantedText) {
        rt::test::report(__FILE__, __LINE__, "step " + std::string(wanted.at("step").as_string()) + "\n      actual:   " + actualText + "\n      expected: " + wantedText,
                         false);
      }
    }
    rt::Kv& kv_;
    std::size_t index_ = 0;
  };

#define STEP(name, task)                                                  \
  do {                                                                    \
    auto stepped_ = (task);                                               \
    auto recording_ = transcript.step((name), std::move(stepped_));       \
    co_await std::move(recording_);                                       \
  } while (false)

  rt::Task<json::value> textResult(rt::Task<std::string> task) {
    const std::string result = co_await std::move(task);
    co_return json::value(json::string(result));
  }
  rt::Task<json::value> reconcile(rt::Kv& kv, double budgetBytes) { return textResult(metadata::reconcileSentinelReplayStatusMetadata(kv, budgetBytes)); }
  rt::Task<json::value> reconcile(rt::Kv& kv, double budgetBytes, const metadata::MaintenanceRecount& maintenance) {
    return textResult(metadata::reconcileSentinelReplayStatusMetadata(kv, budgetBytes, maintenance));
  }
  rt::Task<json::value> ifDue(rt::Kv& kv, double budgetBytes, double nowMs) {
    return textResult(metadata::reconcileSentinelReplayStatusMetadataIfDue(kv, budgetBytes, nowMs));
  }
  rt::Task<json::value> resultOf(json::value result) { co_return result; }

  rt::Task<void> setValue(rt::Kv& kv, rt::KvKey key, rt::KvValue value) { co_await kv.set(std::move(key), std::move(value)); }

  bool checksLedger(const std::vector<rt::AtomicCheck>& checks) {
    for (const rt::AtomicCheck& check : checks) {
      if (check.key == schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY) return true;
    }
    return false;
  }

  using Race = std::function<rt::Task<void>()>;

  /** Runs `race` immediately before the first commit that checks the ledger. */
  class RaceFirstLedgerCommit : public rt::ForwardingKv {
  public:
    RaceFirstLedgerCommit(rt::Kv& kv, Race race) : rt::ForwardingKv(kv), race_(std::move(race)) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<Operation>(inner.atomic(), *this); }

  private:
    class Operation final : public rt::ForwardingAtomicOperation {
    public:
      Operation(std::unique_ptr<rt::AtomicOperation> operation, RaceFirstLedgerCommit& owner) : rt::ForwardingAtomicOperation(std::move(operation)), owner_(owner) {}
      using rt::ForwardingAtomicOperation::check;
      rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
        checksLedger_ = checksLedger_ || checksLedger(items);
        return rt::ForwardingAtomicOperation::check(std::move(items));
      }
      rt::Task<rt::KvCommitResult> commit() override {
        if (checksLedger_ && owner_.armed_) {
          owner_.armed_ = false;
          auto racing = owner_.race_();
          co_await std::move(racing);
        }
        auto committing = rt::ForwardingAtomicOperation::commit();
        rt::KvCommitResult result = co_await std::move(committing);
        co_return result;
      }

    private:
      RaceFirstLedgerCommit& owner_;
      bool checksLedger_ = false;
    };
    Race race_;
    bool armed_ = true;
  };

  // `(async function* () { ... yield await Promise.resolve({ key, value, versionstamp: "fixture" }) })()`
  class FabricatedIterator final : public rt::KvListIterator {
  public:
    FabricatedIterator(double limit, bool corruptWitness) : limit_(limit), corruptWitness_(corruptWitness) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      if (done_ || index_ >= limit_) {
        done_ = true;
        co_return std::nullopt;
      }
      const std::string requestId = "scan-bound-" + rt::numberToString(index_);
      json::object row = statusRow(requestId);
      if (corruptWitness_ && index_ == limits::SENTINEL_REPLAY_MAX_STATUS_RECORDS) rt::objectSet(row, "request_id", "wrong-witness-owner");
      index_ += 1;
      co_await rt::microtask();
      co_return rt::KvEntry<>{.key = schema::sentinelReplayRequestStatusKey(requestId), .value = rt::KvValue(row), .versionstamp = "fixture"};
    }
    std::string cursor() const override { throw rt::JsError("a generator has no cursor"); }
    rt::Task<void> return_() override {
      done_ = true;
      co_return;
    }

  private:
    double limit_;
    bool corruptWitness_;
    double index_ = 0;
    bool done_ = false;
  };

  json::value keyJson(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) parts.emplace_back(json::string(part.toString()));
    return parts;
  }

  /** A list that yields fabricated status rows, recording the options each call asked for. */
  class FabricatedListKv : public rt::ForwardingKv {
  public:
    FabricatedListKv(rt::Kv& kv, bool corruptWitness, json::array& requested) : rt::ForwardingKv(kv), corruptWitness_(corruptWitness), requested_(requested) {}
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      json::object request;
      request["prefix"] = selector.prefix ? keyJson(*selector.prefix) : json::value(nullptr);
      if (options.limit) request["limit"] = rt::makeNumber(*options.limit);
      if (options.consistency) request["consistency"] = *options.consistency == rt::KvConsistencyLevel::strong ? "strong" : "eventual";
      if (options.batchSize) request["batchSize"] = rt::makeNumber(*options.batchSize);
      requested_.push_back(std::move(request));
      const double limit = rt::Math::min(options.limit.value_or(0), limits::SENTINEL_REPLAY_MAX_STATUS_RECORDS + 1);
      return std::make_unique<FabricatedIterator>(limit, corruptWitness_);
    }

  private:
    bool corruptWitness_;
    json::array& requested_;
  };

  // `for await (const entry of kv.list(selector, options)) { yield entry; throw new Error("metadata_fixture_list_failure"); }`
  class InterruptedIterator final : public rt::KvListIterator {
  public:
    explicit InterruptedIterator(std::unique_ptr<rt::KvListIterator> inner) : inner_(std::move(inner)) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      if (done_) co_return std::nullopt;
      if (yielded_) {
        done_ = true;
        co_await inner_->return_();
        throw rt::JsError("metadata_fixture_list_failure");
      }
      std::optional<rt::KvEntry<>> entry = co_await inner_->next();
      if (!entry) {
        done_ = true;
        co_return std::nullopt;
      }
      yielded_ = true;
      co_return entry;
    }
    std::string cursor() const override { throw rt::JsError("a generator has no cursor"); }
    rt::Task<void> return_() override {
      if (!done_ && yielded_) co_await inner_->return_();
      done_ = true;
    }

  private:
    std::unique_ptr<rt::KvListIterator> inner_;
    bool yielded_ = false;
    bool done_ = false;
  };

  class InterruptedListKv : public rt::ForwardingKv {
  public:
    explicit InterruptedListKv(rt::Kv& kv) : rt::ForwardingKv(kv) {}
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      return std::make_unique<InterruptedIterator>(inner.list(std::move(selector), std::move(options)));
    }
  };

  class UnreadableKv : public rt::ForwardingKv {
  public:
    explicit UnreadableKv(rt::Kv& kv) : rt::ForwardingKv(kv) {}
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey, rt::KvGetOptions = {}) override {
      throw rt::JsError("metadata_fixture_read_failure");
      co_return rt::KvEntryMaybe<>{};
    }
  };

  class UnreadableManyKv : public rt::ForwardingKv {
  public:
    explicit UnreadableManyKv(rt::Kv& kv) : rt::ForwardingKv(kv) {}
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions = {}) override {
      throw rt::JsError("metadata_fixture_read_failure");
      co_return std::vector<rt::KvEntryMaybe<>>{};
    }
  };

  rt::KvValue objectWith(const rt::KvValue& value, std::string key, rt::KvValue member) {
    rt::KvObject object = value.asObject();
    object.set(std::move(key), std::move(member));
    return rt::KvValue(std::move(object));
  }
}

ASYNC_TEST("replay-retention-metadata matches the Bun transcript") {
  rt::MemoryKv kv(kvClock);
  Transcript transcript(kv);
  STEP("reconcile absent ledger", reconcile(kv, BUDGET));
  const std::vector<std::string> requestIds{"metadata-a", "metadata-b", "metadata-c"};
  for (const std::string& id : requestIds) {
    const rt::KvValue row(statusRow(id));
    co_await setValue(kv, schema::sentinelReplayRequestStatusKey(id), row);
  }
  const json::object firstTombstone = tombstoneRow(1, "");
  const rt::KvValue firstTombstoneValue(firstTombstone);
  co_await setValue(kv, tombstoneKey(firstTombstone), firstTombstoneValue);
  const json::object secondTombstone = tombstoneRow(2, "metadata-tombstone-2");
  const rt::KvValue secondTombstoneValue(secondTombstone);
  co_await setValue(kv, tombstoneKey(secondTombstone), secondTombstoneValue);
  STEP("reconcile before bootstrap", reconcile(kv, BUDGET));
  STEP("bootstrap", rt::async([&]() -> rt::Task<json::value> {
         for (int call = 0; call < 50; call += 1) {
           const std::optional<schema::SentinelReplayBudgetLedger> ledger = co_await bootstrap::bootstrapBeforeMutation(kv, BUDGET, NOW);
           if (ledger) co_return json::value(true);
         }
         co_return json::value(false);
       }));
  STEP("reconcile matching", reconcile(kv, BUDGET));
  const rt::KvKey expiredKey = schema::sentinelReplayRequestStatusKey("metadata-b");
  co_await kv.delete_(expiredKey);
  STEP("reconcile after expiry", reconcile(kv, BUDGET));
  STEP("reconcile other budget", reconcile(kv, BUDGET * 2));
  STEP("reconcile back to budget", reconcile(kv, BUDGET));
  const rt::KvKey corruptKey = schema::sentinelReplayRequestStatusKey("zz-corrupt-owner");
  const rt::KvValue corruptRow(statusRow("different-owner"));
  co_await setValue(kv, corruptKey, corruptRow);
  STEP("reconcile corrupt row", reconcile(kv, BUDGET));
  STEP("reconcile corrupt row in maintenance", reconcile(kv, BUDGET, metadata::MaintenanceRecount{.at_ms = NOW + 5, .marker_versionstamp = std::nullopt}));
  STEP("due inside the failure backoff", ifDue(kv, BUDGET, NOW + 6));
  co_await kv.delete_(corruptKey);
  STEP("repaired inside the failure backoff", ifDue(kv, BUDGET, NOW + 7));
  STEP("due after the failure backoff", ifDue(kv, BUDGET, NOW + 5 + INTERVAL));
  STEP("due inside the interval", ifDue(kv, BUDGET, NOW + 6 + INTERVAL));
  STEP("due at the interval end", ifDue(kv, BUDGET, NOW + 5 + 2 * INTERVAL - 1));
  STEP("due one interval later", ifDue(kv, BUDGET, NOW + 5 + 2 * INTERVAL));
  const rt::KvValue aheadMarker(object({{"version", 1}, {"reconciled_at_ms", rt::makeNumber(NOW + 100 * INTERVAL)}}));
  co_await setValue(kv, metadata::SENTINEL_REPLAY_METADATA_RECONCILED_KEY, aheadMarker);
  STEP("marker from a clock ahead", ifDue(kv, BUDGET, NOW + 3 * INTERVAL));
  const std::vector<std::pair<std::string, json::value>> markers{
    {"text marker", "not-a-marker"},
    {"negative marker", object({{"version", 1}, {"reconciled_at_ms", -1}})},
    {"fractional marker", object({{"version", 1}, {"reconciled_at_ms", rt::makeNumber(NOW + 3 * INTERVAL + 0.5)}})},
    {"version 2 marker", object({{"version", 2}, {"reconciled_at_ms", rt::makeNumber(NOW + 3 * INTERVAL)}})},
    {"array marker", json::array{1, rt::makeNumber(NOW + 3 * INTERVAL)}},
  };
  for (const auto& [label, marker] : markers) {
    const rt::KvValue markerValue(marker);
    co_await setValue(kv, metadata::SENTINEL_REPLAY_METADATA_RECONCILED_KEY, markerValue);
    STEP("due with a " + label, ifDue(kv, BUDGET, NOW + 3 * INTERVAL + 1));
  }
  const rt::KvValue recordedFailure(object({{"version", 1}, {"failed_at_ms", rt::makeNumber(NOW + 3 * INTERVAL + 2)}}));
  co_await setValue(kv, FAILED_KEY(), recordedFailure);
  STEP("due with a recorded failure", ifDue(kv, BUDGET, NOW + 3 * INTERVAL + 3));
  const rt::KvValue sameTimeFailure(object({{"version", 1}, {"failed_at_ms", rt::makeNumber(NOW + 3 * INTERVAL + 2)}, {"extra", true}}));
  co_await setValue(kv, FAILED_KEY(), sameTimeFailure);
  STEP("due with a failure at the same time", ifDue(kv, BUDGET, NOW + 3 * INTERVAL + 2));
  const rt::KvValue aheadFailure(object({{"version", 1}, {"failed_at_ms", rt::makeNumber(NOW + 3 * INTERVAL + 9)}}));
  co_await setValue(kv, FAILED_KEY(), aheadFailure);
  STEP("due with a failure from a clock ahead", ifDue(kv, BUDGET, NOW + 3 * INTERVAL + 4));
  co_await kv.delete_(FAILED_KEY());
  const std::vector<std::pair<std::string, double>> nows{{"fractional now", 1.5}, {"negative now", -1}, {"unsafe now", std::ldexp(1.0, 60)}};
  for (const auto& [label, nowMs] : nows) STEP("due at a " + label, ifDue(kv, BUDGET, nowMs));

  // Ledger states that refuse a recount.
  const rt::KvEntryMaybe<> healthyEntry = co_await kv.get(schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  const rt::KvValue healthy = healthyEntry.value;
  const rt::KvValue erroredLedger = objectWith(healthy, "accounting_error", rt::KvValue("invalid_in_scope_row"));
  co_await setValue(kv, schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY, erroredLedger);
  STEP("reconcile with an accounting error", reconcile(kv, BUDGET));
  const rt::KvValue incompleteLedger = objectWith(healthy, "bootstrap_complete", rt::KvValue(false));
  co_await setValue(kv, schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY, incompleteLedger);
  STEP("reconcile before bootstrap completes", reconcile(kv, BUDGET, metadata::MaintenanceRecount{.at_ms = NOW, .marker_versionstamp = std::nullopt}));
  const rt::KvValue corruptLedger = objectWith(healthy, "status_records", rt::KvValue(-1));
  co_await setValue(kv, schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY, corruptLedger);
  STEP("reconcile with a corrupt ledger", reconcile(kv, BUDGET));
  const rt::KvValue staleLedger = objectWith(objectWith(healthy, "status_records", rt::KvValue(0)), "metadata_bytes", rt::KvValue(0));
  co_await setValue(kv, schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY, staleLedger);
  STEP("reconcile a stale ledger", reconcile(kv, BUDGET));

  // A concurrent ledger write between the scan and the commit.
  const rt::KvEntryMaybe<> marker = co_await kv.get(metadata::SENTINEL_REPLAY_METADATA_RECONCILED_KEY);
  const rt::KvValue restoredRow(statusRow("metadata-b"));
  co_await setValue(kv, expiredKey, restoredRow);
  rt::Kv& store = kv;
  RaceFirstLedgerCommit ledgerRace(kv, [&store]() -> rt::Task<void> {
    return rt::async([&store]() -> rt::Task<void> {
      const rt::KvEntryMaybe<> current = co_await store.get(schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
      const rt::KvValue raced = objectWith(current.value, "last_skip_reason", rt::KvValue("racing_writer"));
      co_await store.set(schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY, raced);
    });
  });
  STEP("reconcile losing its ledger check",
       reconcile(ledgerRace, BUDGET, metadata::MaintenanceRecount{.at_ms = NOW + 4 * INTERVAL, .marker_versionstamp = marker.versionstamp}));
  RaceFirstLedgerCommit markerRace(kv, [&store]() -> rt::Task<void> {
    return rt::async([&store]() -> rt::Task<void> {
      const rt::KvValue moved(object({{"version", 1}, {"reconciled_at_ms", rt::makeNumber(NOW + 4 * INTERVAL + 1)}}));
      co_await store.set(metadata::SENTINEL_REPLAY_METADATA_RECONCILED_KEY, moved);
    });
  });
  STEP("reconcile losing its marker check",
       reconcile(markerRace, BUDGET, metadata::MaintenanceRecount{.at_ms = NOW + 4 * INTERVAL, .marker_versionstamp = marker.versionstamp}));
  STEP("reconcile after the races", reconcile(kv, BUDGET));

  // List fakes: fabricated over-limit rows, a corrupt witness, an interrupted list.
  json::array requested;
  FabricatedListKv overLimit(kv, false, requested);
  STEP("reconcile over the limit", reconcile(overLimit, BUDGET));
  STEP("reconcile over the limit in maintenance", rt::async([&]() -> rt::Task<json::value> {
         const rt::KvEntryMaybe<> current = co_await kv.get(metadata::SENTINEL_REPLAY_METADATA_RECONCILED_KEY);
         const metadata::MaintenanceRecount maintenance{.at_ms = NOW + 5 * INTERVAL, .marker_versionstamp = current.versionstamp};
         auto reconciling = reconcile(overLimit, BUDGET, maintenance);
         json::value result = co_await std::move(reconciling);
         co_return result;
       }));
  FabricatedListKv corruptWitness(kv, true, requested);
  STEP("reconcile with a corrupt witness", reconcile(corruptWitness, BUDGET));
  STEP("requested list options", resultOf(json::value(requested)));
  InterruptedListKv interrupted(kv);
  STEP("reconcile with an interrupted list", reconcile(interrupted, BUDGET));
  UnreadableKv unreadable(kv);
  STEP("reconcile with an unreadable ledger", reconcile(unreadable, BUDGET));
  UnreadableManyKv unreadableMany(kv);
  STEP("due with unreadable markers", ifDue(unreadableMany, BUDGET, NOW + 6 * INTERVAL));

  // metadataEntryMatches.
  const rt::KvKey& request = model::SENTINEL_REPLAY_REQUEST_PREFIX;
  const rt::KvKey& eviction = schema::SENTINEL_REPLAY_EVICTION_PREFIX;
  const rt::KvKey copiedRequest = request;
  const json::object tombstone = tombstoneRow(9, "metadata-tombstone-9");
  const std::string fingerprint(tombstone.at("fingerprint").as_string());
  const json::object noRequestTombstone = tombstoneRow(9, "");
  const auto with = [](json::object row, std::string_view key, json::value member) {
    rt::objectSet(row, key, std::move(member));
    return row;
  };
  const std::vector<std::tuple<std::string, rt::KvKey, json::value, const rt::KvKey*>> cases{
    {"status row", rt::KvKey{request, "row-a"}, statusRow("row-a"), &request},
    {"status row under another id", rt::KvKey{request, "row-b"}, statusRow("row-a"), &request},
    {"status row with an extra key part", rt::KvKey{request, "row-a", "extra"}, statusRow("row-a"), &request},
    {"status row under a short key", request, statusRow("row-a"), &request},
    {"status row under another prefix", rt::KvKey{eviction, "row-a"}, statusRow("row-a"), &request},
    {"status row checked against a copied prefix", rt::KvKey{request, "row-a"}, statusRow("row-a"), &copiedRequest},
    {"invalid status row", rt::KvKey{request, "row-a"}, with(statusRow("row-a"), "status", "unknown-status"), &request},
    {"tombstone", rt::KvKey{eviction, fingerprint}, tombstone, &eviction},
    {"tombstone without a request", rt::KvKey{eviction, fingerprint}, noRequestTombstone, &eviction},
    {"tombstone under another fingerprint", rt::KvKey{eviction, std::string(64, '0')}, tombstone, &eviction},
    {"tombstone with a bad request id", rt::KvKey{eviction, fingerprint}, with(tombstone, "request_id", "bad id"), &eviction},
    {"tombstone with a fractional time", rt::KvKey{eviction, fingerprint}, with(tombstone, "evicted_at_ms", 1.5), &eviction},
    {"tombstone with another reason", rt::KvKey{eviction, fingerprint}, with(tombstone, "reason", "fixture"), &eviction},
    {"tombstone with an evicted reason", rt::KvKey{eviction, fingerprint},
     with(tombstone, "reason", json::string(std::string(schema::SENTINEL_REPLAY_EVICTION_REASON))), &eviction},
    {"tombstone with an upper-case fingerprint", rt::KvKey{eviction, rt::toUpperCase(fingerprint)},
     with(tombstone, "fingerprint", json::string(rt::toUpperCase(fingerprint))), &eviction},
    {"tombstone version 2", rt::KvKey{eviction, fingerprint}, with(tombstone, "version", 2), &eviction},
    {"tombstone that is an array", rt::KvKey{eviction, fingerprint}, json::array{tombstone}, &eviction},
    {"tombstone checked as a status row", rt::KvKey{eviction, fingerprint}, tombstone, &request},
  };
  for (const auto& [label, key, value, prefix] : cases) {
    const rt::KvEntry<> entry{.key = key, .value = rt::KvValue(value), .versionstamp = "fixture"};
    STEP("matches " + label, resultOf(json::value(metadata::metadataEntryMatches(entry, *prefix))));
  }
  kv.close();
  transcript.finish();
}
