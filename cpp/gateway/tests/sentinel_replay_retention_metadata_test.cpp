// Links: tests/sentinel_replay_retention_metadata_test.cpp src/sentinel/replay_retention.cpp src/sentinel/replay_retention_bootstrap.cpp
//        src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_read.cpp src/sentinel/replay_retention_schema.cpp
//        src/sentinel/replay_model.cpp src/sentinel/replay_envelope.cpp src/sentinel/replay_observation.cpp src/sentinel/upstream_capture.cpp
//        src/sentinel/replay_limits.cpp src/sentinel/incident_outbox.cpp src/request.cpp src/utils.cpp src/config.cpp src/release.cpp
//        src/kv.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
// Port of tests/sentinel-replay-retention-metadata.test.ts. Storage is `openKv(":memory:")`: rt::MemoryKv, whose expired
// rows disappear at its 60 s sweep as the native store's do; `waitForExpiry` polls in the test's fake time, so the
// native TTL wait costs no real time. The TS Proxies are rt::ForwardingKv / rt::ForwardingAtomicOperation subclasses; a
// replacement `list` (an async generator in the TS) is an rt::KvListIterator.
#include "uos/sentinel/replay_limits.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_retention.hpp"
#include "uos/sentinel/replay_retention_metadata.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <charconv>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::sentinel::replay_model;
  using namespace uos::sentinel::replay_retention;
  using namespace uos::sentinel::replay_retention_metadata;
  using namespace uos::sentinel::replay_retention_schema;
  using uos::sentinel::replay_limits::SENTINEL_REPLAY_MAX_STATUS_RECORDS;

  constexpr double NOW = 1'702'000'000'000;
  constexpr double BUDGET_BYTES = 1 * 1'024 * 1'024;

  double kvClock() { return rt::dateNow(); }

  rt::KvValue statusRow(const std::string& requestId) {
    return toKvValue(SentinelReplayCaptureStatusRow{
      .version = 1,
      .request_id = requestId,
      .status = "disabled",
      .reason = "metadata_fixture",
      .captured_at_ms = NOW,
      .manifest_key = std::nullopt,
      .fingerprint = std::nullopt,
      .expires_at_ms = std::nullopt,
    });
  }

  std::string tombstoneFingerprint(double index) {
    char hex[8];
    const auto end = std::to_chars(hex, hex + sizeof hex, static_cast<std::uint32_t>(index), 16).ptr;
    return rt::padStart(std::string(hex, end), 64, "0");
  }

  // `{ ...tombstoneRow(index), evicted_at_ms }`
  rt::KvValue tombstoneRow(double index, double evictedAtMs = NOW) {
    rt::KvObject row;
    row.set("version", rt::KvValue(1));
    row.set("fingerprint", rt::KvValue(tombstoneFingerprint(index)));
    row.set("request_id", rt::KvValue("metadata-tombstone-" + rt::numberToString(index)));
    row.set("evicted_at_ms", rt::KvValue(evictedAtMs));
    row.set("reason", rt::KvValue(SENTINEL_REPLAY_EXPIRED_REASON));
    return rt::KvValue(std::move(row));
  }

  rt::KvKey tombstoneKey(const rt::KvValue& row) { return rt::KvKey{SENTINEL_REPLAY_EVICTION_PREFIX, row["fingerprint"].asString()}; }

  rt::KvValue ledgerValue(const SentinelReplayBudgetLedger& ledger) { return rt::KvValue(json::value_from(ledger)); }

  rt::Task<SentinelReplayLedgerSnapshot> ledgerOf(rt::Kv& kv, double budgetBytes = BUDGET_BYTES) {
    auto reading = readSentinelReplayLedgerSnapshot(kv, budgetBytes);
    const std::optional<SentinelReplayLedgerSnapshot> snapshot = co_await std::move(reading);
    ASSERT(snapshot.has_value());
    co_return *snapshot;
  }

  bool sameSnapshot(const SentinelReplayLedgerSnapshot& left, const SentinelReplayLedgerSnapshot& right) {
    return left.ledger == right.ledger && left.versionstamp == right.versionstamp;
  }

  rt::Task<bool> admitStatus(rt::Kv& kv, std::string requestId, double ttlMs = 60'000) {
    const SentinelReplayStatusMetadataInput input{
      .key = sentinelReplayRequestStatusKey(requestId),
      .row = statusRow(requestId),
      .now_ms = NOW,
      .ttl_ms = ttlMs,
      .budget_bytes = BUDGET_BYTES,
    };
    auto admitting = admitSentinelReplayStatusMetadata(kv, input);
    const bool admitted = co_await std::move(admitting);
    co_return admitted;
  }

  /** KV expiry is earliest-after-TTL; observe native absence with a finite deadline rather than advancing a fake clock. */
  rt::Task<void> waitForExpiry(rt::Kv& kv, std::vector<rt::KvKey> keys) {
    // Deno 2.9.7 pins denokv_sqlite 0.14.0: its empty-DB collector fallback is 60s
    // plus 0..1s jitter, and later writes do not reschedule that deadline.
    const double started = rt::dateNow();
    const double deadline = started + 90'000;
    while (rt::dateNow() < deadline) {
      const std::vector<rt::KvEntryMaybe<>> entries = co_await kv.getMany(keys);
      bool absent = true;
      for (const rt::KvEntryMaybe<>& entry : entries) absent = absent && entry.value.isNull();
      if (absent) {
        json::object line;
        line["fixture"] = "native_metadata_ttl_absent";
        line["elapsed_ms"] = rt::makeNumber(rt::dateNow() - started);
        line["rows"] = rt::makeNumber(static_cast<double>(keys.size()));
        rt::console::info(rt::jsonStringify(json::value(std::move(line))));
        co_return;
      }
      co_await rt::sleep(250);
    }
    throw rt::JsError("native metadata TTL did not disappear within the fixture deadline");
  }

  rt::Task<std::vector<rt::KvKey>> expiringMetadata(rt::Kv& kv) {
    std::vector<rt::KvKey> keys;
    for (double index = 0; index < 6; index += 1) {
      const std::string requestId = "ttl-status-" + rt::numberToString(index);
      const bool admitted = co_await admitStatus(kv, requestId, 500);
      EXPECT_EQ(admitted, true);
      const rt::KvKey key = sentinelReplayRequestStatusKey(requestId);
      const rt::KvEntryMaybe<> stored = co_await kv.get(key);
      EXPECT(!stored.value.isNull());  // positive baseline: native KV really retained the TTL row
      keys.push_back(key);
    }
    const rt::KvValue row = tombstoneRow(1);
    const rt::KvKey key = tombstoneKey(row);
    const SentinelReplayStatusMetadataInput input{.key = key, .row = row, .now_ms = NOW, .ttl_ms = 500, .budget_bytes = BUDGET_BYTES};
    auto admitting = admitSentinelReplayStatusMetadata(kv, input);
    const bool admitted = co_await std::move(admitting);
    EXPECT_EQ(admitted, true);
    const rt::KvEntryMaybe<> stored = co_await kv.get(key);
    EXPECT(!stored.value.isNull());
    keys.push_back(key);
    const SentinelReplayLedgerSnapshot before = co_await ledgerOf(kv);
    EXPECT_EQ(before.ledger.status_records, 7.0);
    EXPECT(before.ledger.metadata_bytes + sentinelReplayStatusMetadataBytes(statusRow("ttl-after")) > sentinelReplayMetadataReserve(BUDGET_BYTES));
    co_return keys;
  }

  struct Metadata {
    double records;
    double bytes;
    friend bool operator==(const Metadata&, const Metadata&) = default;
  };
  std::string show(const Metadata& metadata) {
    return "{records: " + rt::numberToString(metadata.records) + ", bytes: " + rt::numberToString(metadata.bytes) + "}";
  }

  rt::Task<Metadata> liveMetadata(rt::Kv& kv) {
    double records = 0;
    double bytes = 0;
    const std::vector<rt::KvKey> prefixes{SENTINEL_REPLAY_REQUEST_PREFIX, SENTINEL_REPLAY_EVICTION_PREFIX};
    for (const rt::KvKey& prefix : prefixes) {
      rt::KvListSelector selector;
      selector.prefix = prefix;
      const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
      while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        records += 1;
        bytes += sentinelReplayStatusMetadataBytes(entry->value);
      }
    }
    co_return Metadata{.records = records, .bytes = bytes};
  }

  rt::Task<double> seedOverCapMetadata(rt::Kv& kv, double budgetBytes, double requestRecords = SENTINEL_REPLAY_MAX_STATUS_RECORDS) {
    const SentinelReplayMaintenanceOptions options{.now_ms = NOW, .budget_bytes = budgetBytes};
    auto maintaining = runSentinelReplayRetentionMaintenance(kv, options);
    co_await std::move(maintaining);
    const SentinelReplayLedgerSnapshot initial = co_await ledgerOf(kv, budgetBytes);
    double bytes = 0;
    for (double start = 0; start < requestRecords; start += 128) {
      const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      for (double index = start; index < rt::Math::min(start + 128, requestRecords); index += 1) {
        const std::string requestId = "legacy-status-" + rt::padStart(rt::numberToString(index), 5, "0");
        const rt::KvValue row = statusRow(requestId);
        operation->set(sentinelReplayRequestStatusKey(requestId), row);
        bytes += sentinelReplayStatusMetadataBytes(row);
      }
      const rt::KvCommitResult committed = co_await operation->commit();
      EXPECT_EQ(committed.ok, true);
    }
    const std::unique_ptr<rt::AtomicOperation> tombstones = kv.atomic();
    for (double index = 1; index <= 32; index += 1) {
      const rt::KvValue row = tombstoneRow(index, NOW - 1);
      tombstones->set(tombstoneKey(row), row);
      bytes += sentinelReplayStatusMetadataBytes(row);
    }
    const rt::KvCommitResult committed = co_await tombstones->commit();
    EXPECT_EQ(committed.ok, true);
    SentinelReplayBudgetLedger ledger = initial.ledger;
    ledger.status_records = requestRecords + 32;
    ledger.metadata_bytes = bytes;
    const rt::KvValue value = ledgerValue(ledger);
    co_await kv.set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, value);
    co_return bytes;
  }

  bool sameKey(const rt::KvKey& left, const rt::KvKey& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
      if (left[index].isBytes() || !(left[index] == right[index])) return false;
    }
    return true;
  }

  bool isMetadataSelector(const rt::KvListSelector& selector) {
    return selector.prefix && (sameKey(*selector.prefix, SENTINEL_REPLAY_REQUEST_PREFIX) || sameKey(*selector.prefix, SENTINEL_REPLAY_EVICTION_PREFIX));
  }

  // The `async function*` wrapper that counts every row it passes on.
  class CountingIterator final : public rt::KvListIterator {
  public:
    CountingIterator(std::unique_ptr<rt::KvListIterator> inner, std::shared_ptr<double> scanned) : inner_(std::move(inner)), scanned_(std::move(scanned)) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      std::optional<rt::KvEntry<>> entry = co_await inner_->next();
      if (entry) *scanned_ += 1;
      co_return entry;
    }
    std::string cursor() const override { return inner_->cursor(); }
    rt::Task<void> return_() override { return inner_->return_(); }

  private:
    std::unique_ptr<rt::KvListIterator> inner_;
    std::shared_ptr<double> scanned_;
  };

  /** Counts every status/tombstone row a caller reads through `list`; other prefixes pass through untouched. */
  class CountMetadataScans final : public rt::ForwardingKv {
  public:
    CountMetadataScans(rt::Kv& kv, std::shared_ptr<double> scanned) : rt::ForwardingKv(kv), scanned_(std::move(scanned)) {}
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      const bool counted = isMetadataSelector(selector);
      std::unique_ptr<rt::KvListIterator> iterator = inner.list(std::move(selector), std::move(options));
      if (!counted) return iterator;
      return std::make_unique<CountingIterator>(std::move(iterator), scanned_);
    }

  private:
    std::shared_ptr<double> scanned_;
  };

  struct Poll {
    double scanned;
    bool ledger_written;
    SentinelReplayRetentionStatus status;
  };

  /** One admin-poll maintenance pass, with its metadata rows scanned and whether it rewrote the ledger. */
  rt::Task<Poll> pollMaintenance(rt::Kv& kv, double nowMs, double budgetBytes) {
    const auto scanned = std::make_shared<double>(0);
    const SentinelReplayLedgerSnapshot before = co_await ledgerOf(kv, budgetBytes);
    CountMetadataScans counted(kv, scanned);
    const SentinelReplayMaintenanceOptions options{.now_ms = nowMs, .budget_bytes = budgetBytes};
    auto maintaining = runSentinelReplayRetentionMaintenance(counted, options);
    SentinelReplayRetentionStatus status = co_await std::move(maintaining);
    const SentinelReplayLedgerSnapshot after = co_await ledgerOf(kv, budgetBytes);
    co_return Poll{.scanned = *scanned, .ledger_written = after.versionstamp != before.versionstamp, .status = std::move(status)};
  }

  rt::Task<rt::KvValue> reconciledAt(rt::Kv& kv) {
    const rt::KvEntryMaybe<> entry = co_await kv.get(SENTINEL_REPLAY_METADATA_RECONCILED_KEY);
    co_return entry.value["reconciled_at_ms"];
  }

  // `observeCommits(kv, { before, after })`
  struct CommitObservation {
    std::function<rt::Task<void>()> before;
    std::function<void(const rt::KvCommitResult& result)> after;
  };

  class ObservedAtomic final : public rt::ForwardingAtomicOperation {
  public:
    ObservedAtomic(std::unique_ptr<rt::AtomicOperation> operation, std::shared_ptr<CommitObservation> observation)
        : rt::ForwardingAtomicOperation(std::move(operation)), observation_(std::move(observation)) {}
    using rt::ForwardingAtomicOperation::check;
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
      inner->check(std::move(items));
      return *this;
    }
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      inner->set(std::move(key), std::move(value), options);
      return *this;
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override {
      inner->delete_(std::move(key));
      return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override {
      auto before = observation_->before();
      co_await std::move(before);
      rt::KvCommitResult result = co_await inner->commit();
      observation_->after(result);
      co_return result;
    }

  private:
    std::shared_ptr<CommitObservation> observation_;
  };

  class ObserveCommits final : public rt::ForwardingKv {
  public:
    ObserveCommits(rt::Kv& kv, std::shared_ptr<CommitObservation> observation) : rt::ForwardingKv(kv), observation_(std::move(observation)) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<ObservedAtomic>(inner.atomic(), observation_); }

  private:
    std::shared_ptr<CommitObservation> observation_;
  };

  rt::Task<double> verifyOverCapCas(rt::Kv& kv, double budgetBytes) {
    const SentinelReplayLedgerSnapshot before = co_await ledgerOf(kv, budgetBytes);
    const rt::KvValue row = statusRow("legacy-status-00000");
    const rt::KvKey key = sentinelReplayRequestStatusKey(row["request_id"].asString());
    const rt::KvEntryMaybe<> entry = co_await kv.get(key);
    rt::KvValue updated = row;
    updated.asObject().set("reason", rt::KvValue("concurrent_metadata_fixture"));
    const double delta = sentinelReplayStatusMetadataBytes(updated) - sentinelReplayStatusMetadataBytes(row);
    const auto outcomes = std::make_shared<std::vector<bool>>();
    rt::Kv* store = &kv;
    const auto observation = std::make_shared<CommitObservation>();
    observation->before = [store, before, key, entry, updated, delta]() -> rt::Task<void> {
      SentinelReplayBudgetLedger next = before.ledger;
      next.metadata_bytes = before.ledger.metadata_bytes + delta;
      const rt::KvValue nextValue = ledgerValue(next);
      const std::unique_ptr<rt::AtomicOperation> operation = store->atomic();
      operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, before.versionstamp))
        .check(rt::AtomicCheck(key, entry.versionstamp))
        .set(key, updated)
        .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, nextValue);
      const rt::KvCommitResult result = co_await operation->commit();
      EXPECT_EQ(result.ok, true);
    };
    observation->after = [outcomes](const rt::KvCommitResult& result) { outcomes->push_back(result.ok); };
    ObserveCommits wrapped(kv, observation);
    auto pruning = pruneCaptureOwnedStatusMetadata(wrapped, NOW, 0, 0, 16, budgetBytes);
    const double pruned = co_await std::move(pruning);
    EXPECT_EQ(pruned, 0.0);
    EXPECT_EQ(*outcomes, std::vector<bool>{false});  // a native ledger race also rejects an over-cap scan before pruning
    const SentinelReplayLedgerSnapshot after = co_await ledgerOf(kv, budgetBytes);
    EXPECT_EQ(after.ledger.status_records, before.ledger.status_records);
    EXPECT_EQ(after.ledger.metadata_bytes, before.ledger.metadata_bytes + delta);
    EXPECT_EQ(after.ledger.status_pruned_records, 0.0);
    co_return delta;
  }
}

ASYNC_TEST("native over-cap request rows cannot hide corrupt tombstone pruning candidates") {
  rt::MemoryKv kv(kvClock);
  const double budgetBytes = 1'024 * 1'024 * 1'024;
  auto seeding = seedOverCapMetadata(kv, budgetBytes, SENTINEL_REPLAY_MAX_STATUS_RECORDS + 1);
  co_await std::move(seeding);
  const rt::KvValue valid = tombstoneRow(33);
  struct Fixture {
    rt::KvKey key;
    rt::KvValue row;
  };
  rt::KvObject partial;
  partial.set("evicted_at_ms", rt::KvValue(NOW + 1));
  rt::KvValue mismatched = valid;
  mismatched.asObject().set("fingerprint", rt::KvValue(tombstoneFingerprint(34)));
  std::vector<Fixture> cases;
  cases.push_back(Fixture{.key = tombstoneKey(valid), .row = rt::KvValue(std::move(partial))});
  cases.push_back(Fixture{.key = tombstoneKey(valid), .row = mismatched});
  cases.push_back(Fixture{.key = rt::KvKey{SENTINEL_REPLAY_EVICTION_PREFIX, valid["fingerprint"].asString(), "extra"}, .row = valid});
  for (const Fixture& fixture : cases) {
    co_await kv.set(fixture.key, fixture.row);
    const SentinelReplayLedgerSnapshot current = co_await ledgerOf(kv, budgetBytes);
    SentinelReplayBudgetLedger counted = current.ledger;
    counted.status_records = current.ledger.status_records + 1;
    counted.metadata_bytes = current.ledger.metadata_bytes + sentinelReplayStatusMetadataBytes(fixture.row);
    const rt::KvValue countedValue = ledgerValue(counted);
    co_await kv.set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, countedValue);
    const SentinelReplayLedgerSnapshot before = co_await ledgerOf(kv, budgetBytes);
    const Metadata rowsBefore = co_await liveMetadata(kv);
    EXPECT_EQ(rowsBefore.records, SENTINEL_REPLAY_MAX_STATUS_RECORDS + 34);
    const rt::KvEntryMaybe<> corruptBefore = co_await kv.get(fixture.key);
    auto reconciling = reconcileSentinelReplayStatusMetadata(kv, budgetBytes);
    const std::string reconciled = co_await std::move(reconciling);
    EXPECT_EQ(reconciled, std::string("over_limit"));
    const SentinelReplayLedgerSnapshot afterScan = co_await ledgerOf(kv, budgetBytes);
    EXPECT(sameSnapshot(afterScan, before));  // the over-cap witness never writes partial totals
    auto pruning = pruneCaptureOwnedStatusMetadata(kv, NOW, 0, 0, 16, budgetBytes);
    const double pruned = co_await std::move(pruning);
    EXPECT_EQ(pruned, 0.0);
    const SentinelReplayLedgerSnapshot afterPrune = co_await ledgerOf(kv, budgetBytes);
    EXPECT(sameSnapshot(afterPrune, before));  // a corrupt candidate preserves the exact ledger and version
    const Metadata rowsAfter = co_await liveMetadata(kv);
    EXPECT_EQ(rowsAfter, rowsBefore);  // all native status and tombstone rows survive
    const rt::KvEntryMaybe<> corruptAfter = co_await kv.get(fixture.key);
    EXPECT(corruptAfter == corruptBefore);
    const rt::KvValue restored = ledgerValue(current.ledger);
    const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
    operation->delete_(fixture.key).set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, restored);
    co_await operation->commit();
  }
  kv.close();
}

ASYNC_TEST("native over-cap metadata makes bounded pruning progress without relaxing admission limits") {
  rt::MemoryKv kv(kvClock);
  const double budgetBytes = 1'024 * 1'024 * 1'024;
  auto seeding = seedOverCapMetadata(kv, budgetBytes);
  const double seededBytes = co_await std::move(seeding);
  const Metadata seeded = co_await liveMetadata(kv);
  EXPECT_EQ(seeded.records, SENTINEL_REPLAY_MAX_STATUS_RECORDS + 32);
  const SentinelReplayLedgerSnapshot beforeScan = co_await ledgerOf(kv, budgetBytes);
  auto reconciling = reconcileSentinelReplayStatusMetadata(kv, budgetBytes);
  const std::string reconciled = co_await std::move(reconciling);
  EXPECT_EQ(reconciled, std::string("over_limit"));
  const SentinelReplayLedgerSnapshot afterScan = co_await ledgerOf(kv, budgetBytes);
  EXPECT(sameSnapshot(afterScan, beforeScan));  // an over-cap scan checks the ledger without writing partial totals
  // Admin polls take the same over-cap witness once per interval, never every poll, and never write partial totals.
  const Poll witness = co_await pollMaintenance(kv, NOW + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS, budgetBytes);
  EXPECT_EQ(witness.scanned, SENTINEL_REPLAY_MAX_STATUS_RECORDS + 1);
  EXPECT_EQ(witness.ledger_written, false);
  const rt::KvValue marker = co_await reconciledAt(kv);
  EXPECT_EQ(marker, rt::KvValue(NOW + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS));
  const Poll quiet = co_await pollMaintenance(kv, NOW + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS + 30'000, budgetBytes);
  EXPECT_EQ(quiet.scanned, 0.0);
  const SentinelReplayLedgerSnapshot afterPolls = co_await ledgerOf(kv, budgetBytes);
  EXPECT(sameSnapshot(afterPolls, beforeScan));
  auto verifying = verifyOverCapCas(kv, budgetBytes);
  const double casDelta = co_await std::move(verifying);
  const double beforeBytes = seededBytes + casDelta;
  double removedBytes = 0;
  for (double pass = 0; pass < 2; pass += 1) {
    auto pruning = pruneCaptureOwnedStatusMetadata(kv, NOW + pass, 0, 0, 16, budgetBytes);
    const double pruned = co_await std::move(pruning);
    EXPECT_EQ(pruned, 16.0);
    for (double index = pass * 16 + 1; index <= (pass + 1) * 16; index += 1) {
      const rt::KvValue row = tombstoneRow(index, NOW - 1);
      const rt::KvEntryMaybe<> gone = co_await kv.get(tombstoneKey(row));
      EXPECT(gone.value.isNull());
      removedBytes += sentinelReplayStatusMetadataBytes(row);
    }
    const SentinelReplayLedgerSnapshot after = co_await ledgerOf(kv, budgetBytes);
    EXPECT_EQ(after.ledger.status_records, SENTINEL_REPLAY_MAX_STATUS_RECORDS + 32 - (pass + 1) * 16);
    EXPECT_EQ(after.ledger.metadata_bytes, beforeBytes - removedBytes);  // only committed row deletions change the charge, never a partial recount
    EXPECT_EQ(after.ledger.status_pruned_records, (pass + 1) * 16);
  }
  const rt::KvValue row = statusRow("legacy-after-pruning");
  // The 8 KiB per-row charge still exceeds the fixed 64 MiB reserve. An
  // admission makes one more bounded deletion and keeps refusing that bound.
  const SentinelReplayStatusMetadataInput input{
    .key = sentinelReplayRequestStatusKey("legacy-after-pruning"),
    .row = row,
    .now_ms = NOW + 2,
    .ttl_ms = 60'000,
    .budget_bytes = budgetBytes,
  };
  auto admitting = admitSentinelReplayStatusMetadata(kv, input);
  const bool admitted = co_await std::move(admitting);
  EXPECT_EQ(admitted, false);
  const SentinelReplayLedgerSnapshot after = co_await ledgerOf(kv, budgetBytes);
  const Metadata expected = co_await liveMetadata(kv);
  EXPECT_EQ(after.ledger.status_records, SENTINEL_REPLAY_MAX_STATUS_RECORDS - 1);
  EXPECT_EQ(after.ledger.status_records, expected.records);
  EXPECT_EQ(after.ledger.metadata_bytes, expected.bytes);
  EXPECT_EQ(after.ledger.status_pruned_records, 33.0);
  const rt::KvEntryMaybe<> refused = co_await kv.get(sentinelReplayRequestStatusKey("legacy-after-pruning"));
  EXPECT(refused.value.isNull());
  kv.close();
}

namespace {
  rt::Task<void> verifyMaintenanceExpiry(rt::Kv& kv) {
    auto expiring = expiringMetadata(kv);
    const std::vector<rt::KvKey> keys = co_await std::move(expiring);
    auto waiting = waitForExpiry(kv, keys);
    co_await std::move(waiting);
    const SentinelReplayLedgerSnapshot charged = co_await ledgerOf(kv);
    EXPECT_EQ(charged.ledger.status_records, 7.0);  // native TTL deletion leaves the old ledger charge until reconciliation
    const SentinelReplayMaintenanceOptions options{.now_ms = NOW + 1, .budget_bytes = BUDGET_BYTES};
    auto maintaining = runSentinelReplayRetentionMaintenance(kv, options);
    const SentinelReplayRetentionStatus maintained = co_await std::move(maintaining);
    EXPECT_EQ(maintained.status_records, std::optional<double>(0));
    EXPECT_EQ(maintained.metadata_bytes, std::optional<double>(0));
    const SentinelReplayLedgerSnapshot pruned = co_await ledgerOf(kv);
    EXPECT_EQ(pruned.ledger.status_pruned_records, 0.0);
    const bool admitted = co_await admitStatus(kv, std::string("ttl-after"));
    EXPECT_EQ(admitted, true);
    const SentinelReplayLedgerSnapshot after = co_await ledgerOf(kv);
    EXPECT_EQ(after.ledger.status_records, 1.0);
  }

  rt::Task<void> verifyAdmissionExpiry(rt::Kv& kv) {
    auto expiring = expiringMetadata(kv);
    const std::vector<rt::KvKey> keys = co_await std::move(expiring);
    auto waiting = waitForExpiry(kv, keys);
    co_await std::move(waiting);
    const bool admitted = co_await admitStatus(kv, std::string("ttl-after"));
    EXPECT_EQ(admitted, true);
    const SentinelReplayLedgerSnapshot after = co_await ledgerOf(kv);
    EXPECT_EQ(after.ledger.status_records, 1.0);
    EXPECT_EQ(after.ledger.metadata_bytes, sentinelReplayStatusMetadataBytes(statusRow("ttl-after")));
    EXPECT_EQ(after.ledger.status_pruned_records, 0.0);
  }

  rt::Task<void> verifyCompleteScan(rt::Kv& kv) {
    const double budgetBytes = 64 * 1'024 * 1'024;
    for (double index = 0; index < 129; index += 1) {
      const std::string requestId = "complete-status-" + rt::numberToString(index);
      const rt::KvValue status = statusRow(requestId);
      const rt::KvValue tombstone = tombstoneRow(index + 1);
      co_await kv.set(sentinelReplayRequestStatusKey(requestId), status);
      co_await kv.set(tombstoneKey(tombstone), tombstone);
    }
    const SentinelReplayMaintenanceOptions options{.now_ms = NOW, .budget_bytes = budgetBytes};
    auto maintaining = runSentinelReplayRetentionMaintenance(kv, options);
    SentinelReplayRetentionStatus maintained = co_await std::move(maintaining);
    for (int pass = 0; pass < 200 && !maintained.accounting_complete; pass += 1) {
      auto again = runSentinelReplayRetentionMaintenance(kv, options);
      maintained = co_await std::move(again);
    }
    EXPECT_EQ(maintained.accounting_complete, true);
    const rt::KvValue transient = statusRow("complete-expiring");
    const rt::KvKey transientKey = sentinelReplayRequestStatusKey("complete-expiring");
    const SentinelReplayStatusMetadataInput input{.key = transientKey, .row = transient, .now_ms = NOW, .ttl_ms = 500, .budget_bytes = budgetBytes};
    auto admitting = admitSentinelReplayStatusMetadata(kv, input);
    const bool admitted = co_await std::move(admitting);
    EXPECT_EQ(admitted, true);
    std::vector<rt::KvKey> keys;
    keys.push_back(transientKey);
    auto waiting = waitForExpiry(kv, keys);
    co_await std::move(waiting);
    const Metadata expected = co_await liveMetadata(kv);
    EXPECT_EQ(expected.records, 258.0);
    // Inside the interval the expired row keeps its conservative charge; the next due poll removes it.
    const SentinelReplayMaintenanceOptions insideOptions{.now_ms = NOW + 1, .budget_bytes = budgetBytes};
    auto insidePass = runSentinelReplayRetentionMaintenance(kv, insideOptions);
    const SentinelReplayRetentionStatus inside = co_await std::move(insidePass);
    EXPECT_EQ(inside.status_records, std::optional<double>(expected.records + 1));
    const SentinelReplayMaintenanceOptions dueOptions{.now_ms = NOW + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS, .budget_bytes = budgetBytes};
    auto duePass = runSentinelReplayRetentionMaintenance(kv, dueOptions);
    const SentinelReplayRetentionStatus after = co_await std::move(duePass);
    EXPECT_EQ(after.status_records, std::optional<double>(expected.records));
    EXPECT_EQ(after.metadata_bytes, std::optional<double>(expected.bytes));
    const SentinelReplayLedgerSnapshot ledger = co_await ledgerOf(kv, budgetBytes);
    EXPECT_EQ(ledger.ledger.status_pruned_records, 0.0);
  }
}

ASYNC_TEST("native TTL disappearance restores maintenance admission and complete metadata accounting") {
  // The three independent collectors start together, so genuine native TTL
  // acceptance costs one bounded fallback interval rather than three in series.
  rt::MemoryKv first(kvClock);
  rt::MemoryKv second(kvClock);
  rt::MemoryKv third(kvClock);
  std::vector<rt::Promise<void>> checks;
  checks.push_back(rt::Promise<void>(verifyMaintenanceExpiry(first)));
  checks.push_back(rt::Promise<void>(verifyAdmissionExpiry(second)));
  checks.push_back(rt::Promise<void>(verifyCompleteScan(third)));
  auto settling = rt::allSettled(std::move(checks));
  const std::vector<rt::SettledResult<void>> outcomes = co_await settling.wait();
  std::vector<std::string> failures;
  for (const rt::SettledResult<void>& outcome : outcomes) {
    if (outcome.status == "rejected") failures.push_back(rt::reasonToString(outcome.reason));
  }
  if (!failures.empty()) throw rt::JsError("native metadata TTL controls failed: " + rt::join(failures, "; "));
  first.close();
  second.close();
  third.close();
}

ASYNC_TEST("a concurrent native status admission invalidates the whole metadata recount CAS") {
  rt::MemoryKv kv(kvClock);
  const auto outcomes = std::make_shared<std::vector<bool>>();
  rt::Kv* store = &kv;
  const auto observation = std::make_shared<CommitObservation>();
  observation->before = [store]() -> rt::Task<void> {
    const bool admitted = co_await admitStatus(*store, std::string("added-during-recount"));
    EXPECT_EQ(admitted, true);
  };
  observation->after = [outcomes](const rt::KvCommitResult& result) { outcomes->push_back(result.ok); };
  ObserveCommits wrapped(kv, observation);
  const bool admitted = co_await admitStatus(kv, std::string("before-recount"));
  EXPECT_EQ(admitted, true);
  auto reconciling = reconcileSentinelReplayStatusMetadata(wrapped, BUDGET_BYTES);
  const std::string reconciled = co_await std::move(reconciling);
  EXPECT_EQ(reconciled, std::string("failed"));
  EXPECT_EQ(*outcomes, std::vector<bool>{false});  // the real native ledger version check rejects the stale scanned totals
  const Metadata expected = co_await liveMetadata(kv);
  const SentinelReplayLedgerSnapshot after = co_await ledgerOf(kv);
  EXPECT_EQ(expected.records, 2.0);
  EXPECT_EQ(after.ledger.status_records, expected.records);
  EXPECT_EQ(after.ledger.metadata_bytes, expected.bytes);
  auto again = reconcileSentinelReplayStatusMetadata(kv, BUDGET_BYTES);
  const std::string recounted = co_await std::move(again);
  EXPECT_EQ(recounted, std::string("reconciled"));
  kv.close();
}

namespace {
  // A store whose `list` is replaced (`withList(kv, list)`).
  using ListReplacement = std::function<std::unique_ptr<rt::KvListIterator>(rt::KvListSelector selector, rt::KvListOptions options)>;
  class WithList final : public rt::ForwardingKv {
  public:
    WithList(rt::Kv& kv, ListReplacement replacement) : rt::ForwardingKv(kv), replacement_(std::move(replacement)) {}
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      return replacement_(std::move(selector), std::move(options));
    }

  private:
    ListReplacement replacement_;
  };

  // `async function* interruptedList(kv, selector, options)`: the first real row, then an error.
  class InterruptedList final : public rt::KvListIterator {
  public:
    explicit InterruptedList(std::unique_ptr<rt::KvListIterator> inner) : inner_(std::move(inner)) {}
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
    std::string cursor() const override { return inner_->cursor(); }
    rt::Task<void> return_() override {
      if (done_) co_return;
      done_ = true;
      co_await inner_->return_();
    }

  private:
    std::unique_ptr<rt::KvListIterator> inner_;
    bool yielded_ = false;
    bool done_ = false;
  };

  // `async function* overLimitRows(limit, corruptWitness = false)`
  class OverLimitRows final : public rt::KvListIterator {
  public:
    OverLimitRows(double limit, bool corruptWitness) : limit_(limit), corruptWitness_(corruptWitness) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      if (done_ || !(index_ < limit_)) {
        done_ = true;
        co_return std::nullopt;
      }
      const std::string requestId = "scan-bound-" + rt::numberToString(index_);
      rt::KvValue value = statusRow(requestId);
      if (corruptWitness_ && index_ == SENTINEL_REPLAY_MAX_STATUS_RECORDS) value.asObject().set("request_id", rt::KvValue("wrong-witness-owner"));
      index_ += 1;
      // `yield await Promise.resolve({ ... })`
      co_await rt::microtask();
      co_return rt::KvEntry<>{.key = sentinelReplayRequestStatusKey(requestId), .value = std::move(value), .versionstamp = "fixture"};
    }
    std::string cursor() const override { return ""; }
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

  // `new Proxy(kv, { get })` whose `get` rejects.
  rt::Task<rt::KvEntryMaybe<>> unreadableGet() {
    throw rt::JsError("metadata_fixture_read_failure");
    co_return rt::KvEntryMaybe<>{};
  }
  class Unreadable final : public rt::ForwardingKv {
  public:
    explicit Unreadable(rt::Kv& kv) : rt::ForwardingKv(kv) {}
    using rt::ForwardingKv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey, rt::KvGetOptions = {}) override { return unreadableGet(); }
  };
}

ASYNC_TEST("corruption partial-list failure and an over-limit scan never lower metadata counters") {
  rt::MemoryKv kv(kvClock);
  const bool admitted = co_await admitStatus(kv, std::string("preserved-row"));
  EXPECT_EQ(admitted, true);
  const SentinelReplayLedgerSnapshot before = co_await ledgerOf(kv);
  const rt::KvKey corruptKey = sentinelReplayRequestStatusKey("wrong-key-owner");
  const rt::KvValue corrupt = statusRow("different-owner");
  co_await kv.set(corruptKey, corrupt);
  auto reconciling = reconcileSentinelReplayStatusMetadata(kv, BUDGET_BYTES);
  const std::string reconciled = co_await std::move(reconciling);
  EXPECT_EQ(reconciled, std::string("failed"));
  const SentinelReplayLedgerSnapshot afterCorrupt = co_await ledgerOf(kv);
  EXPECT(sameSnapshot(afterCorrupt, before));
  auto pruning = pruneCaptureOwnedStatusMetadata(kv, NOW, SENTINEL_REPLAY_MAX_STATUS_RECORDS, 0, 16, BUDGET_BYTES);
  const double pruned = co_await std::move(pruning);
  EXPECT_EQ(pruned, 0.0);
  co_await kv.delete_(corruptKey);

  rt::Kv* store = &kv;
  WithList failed(kv, [store](rt::KvListSelector selector, rt::KvListOptions options) -> std::unique_ptr<rt::KvListIterator> {
    return std::make_unique<InterruptedList>(store->list(std::move(selector), std::move(options)));
  });
  auto reconcilingFailed = reconcileSentinelReplayStatusMetadata(failed, BUDGET_BYTES);
  const std::string failedResult = co_await std::move(reconcilingFailed);
  EXPECT_EQ(failedResult, std::string("failed"));
  auto pruningFailed = pruneCaptureOwnedStatusMetadata(failed, NOW, SENTINEL_REPLAY_MAX_STATUS_RECORDS, 0, 16, BUDGET_BYTES);
  const double prunedFailed = co_await std::move(pruningFailed);
  EXPECT_EQ(prunedFailed, 0.0);
  const SentinelReplayLedgerSnapshot afterFailed = co_await ledgerOf(kv);
  EXPECT(sameSnapshot(afterFailed, before));

  const auto requested = std::make_shared<std::vector<rt::KvListOptions>>();
  WithList overLimit(kv, [requested](rt::KvListSelector, rt::KvListOptions options) -> std::unique_ptr<rt::KvListIterator> {
    requested->push_back(options);
    return std::make_unique<OverLimitRows>(rt::Math::min(options.limit.value_or(0), SENTINEL_REPLAY_MAX_STATUS_RECORDS + 1), false);
  });
  auto reconcilingOver = reconcileSentinelReplayStatusMetadata(overLimit, BUDGET_BYTES);
  const std::string overResult = co_await std::move(reconcilingOver);
  EXPECT_EQ(overResult, std::string("over_limit"));
  const SentinelReplayLedgerSnapshot afterOver = co_await ledgerOf(kv);
  EXPECT(sameSnapshot(afterOver, before));
  ASSERT(!requested->empty());
  EXPECT((*requested)[0].consistency == std::optional<rt::KvConsistencyLevel>(rt::KvConsistencyLevel::strong));
  EXPECT_EQ((*requested)[0].limit, std::optional<double>(SENTINEL_REPLAY_MAX_STATUS_RECORDS + 1));
  WithList corruptWitness(kv, [](rt::KvListSelector, rt::KvListOptions options) -> std::unique_ptr<rt::KvListIterator> {
    return std::make_unique<OverLimitRows>(options.limit.value_or(0), true);
  });
  auto reconcilingWitness = reconcileSentinelReplayStatusMetadata(corruptWitness, BUDGET_BYTES);
  const std::string witnessResult = co_await std::move(reconcilingWitness);
  EXPECT_EQ(witnessResult, std::string("failed"));
  auto pruningWitness = pruneCaptureOwnedStatusMetadata(corruptWitness, NOW, SENTINEL_REPLAY_MAX_STATUS_RECORDS, 0, 16, BUDGET_BYTES);
  const double prunedWitness = co_await std::move(pruningWitness);
  EXPECT_EQ(prunedWitness, 0.0);
  const SentinelReplayLedgerSnapshot afterWitness = co_await ledgerOf(kv);
  EXPECT(sameSnapshot(afterWitness, before));
  Unreadable unreadable(kv);
  auto reconcilingUnreadable = reconcileSentinelReplayStatusMetadata(unreadable, BUDGET_BYTES);
  const std::string unreadableResult = co_await std::move(reconcilingUnreadable);
  EXPECT_EQ(unreadableResult, std::string("failed"));
  auto pruningUnreadable = pruneCaptureOwnedStatusMetadata(unreadable, NOW, SENTINEL_REPLAY_MAX_STATUS_RECORDS, 0, 16, BUDGET_BYTES);
  EXPECT_REJECTS_MESSAGE(std::move(pruningUnreadable), rt::JsError, "metadata_fixture_read_failure");
  const SentinelReplayLedgerSnapshot afterUnreadable = co_await ledgerOf(kv);
  EXPECT(sameSnapshot(afterUnreadable, before));
  SentinelReplayBudgetLedger negative = before.ledger;
  negative.status_records = -1;
  const rt::KvValue negativeValue = ledgerValue(negative);
  co_await kv.set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, negativeValue);
  auto reconcilingNegative = reconcileSentinelReplayStatusMetadata(kv, BUDGET_BYTES);
  const std::string negativeResult = co_await std::move(reconcilingNegative);
  EXPECT_EQ(negativeResult, std::string("failed"));
  auto pruningNegative = pruneCaptureOwnedStatusMetadata(kv, NOW, SENTINEL_REPLAY_MAX_STATUS_RECORDS, 0, 16, BUDGET_BYTES);
  const double prunedNegative = co_await std::move(pruningNegative);
  EXPECT_EQ(prunedNegative, 0.0);
  const rt::KvEntryMaybe<> stored = co_await kv.get(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  EXPECT_EQ(stored.value["status_records"], rt::KvValue(-1));
  kv.close();
}

ASYNC_TEST("healthy admin polls inside the interval skip the metadata recount and the ledger write") {
  rt::MemoryKv kv(kvClock);
  const double budgetBytes = 1'024 * 1'024 * 1'024;
  // The analytics view requests /admin/errors?limit=1 every 30 seconds.
  const double pollIntervalMs = 30'000;
  // A representative retained set at the production budget: 4,096 status rows and 32 tombstones,
  // about 35 MiB of the 64 MiB metadata reserve, with the ledger already equal to the live rows.
  auto seeding = seedOverCapMetadata(kv, budgetBytes, 4'096);
  co_await std::move(seeding);
  const Metadata live = co_await liveMetadata(kv);
  EXPECT_EQ(live.records, 4'128.0);
  const double start = NOW + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS;
  const Poll first = co_await pollMaintenance(kv, start, budgetBytes);
  std::vector<Poll> inside;
  for (double at = start + pollIntervalMs; at < start + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS; at += pollIntervalMs) {
    Poll poll = co_await pollMaintenance(kv, at, budgetBytes);
    inside.push_back(std::move(poll));
  }
  const Poll due = co_await pollMaintenance(kv, start + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS, budgetBytes);
  double rowsScanned = first.scanned + due.scanned;
  double ledgerWrites = (first.ledger_written ? 1 : 0) + (due.ledger_written ? 1 : 0);
  for (const Poll& poll : inside) {
    rowsScanned += poll.scanned;
    ledgerWrites += poll.ledger_written ? 1 : 0;
  }
  json::object line;
  line["fixture"] = "metadata_recount_polls";
  line["retained_rows"] = rt::makeNumber(live.records);
  line["polls"] = rt::makeNumber(static_cast<double>(inside.size() + 2));
  line["rows_scanned"] = rt::makeNumber(rowsScanned);
  line["ledger_writes"] = rt::makeNumber(ledgerWrites);
  rt::console::info(rt::jsonStringify(json::value(std::move(line))));
  EXPECT_EQ(static_cast<double>(inside.size()), SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS / pollIntervalMs - 1);
  for (const Poll& poll : inside) {
    EXPECT_EQ(poll.scanned, 0.0);  // a healthy poll inside the interval reads no status or tombstone row
    EXPECT_EQ(poll.ledger_written, false);  // a healthy poll inside the interval never rewrites the ledger
    EXPECT(poll.status == first.status);  // the admin API reports the same retention status
  }
  EXPECT_EQ(first.scanned, live.records);  // a due poll recounts the whole retained set exactly once
  EXPECT_EQ(due.scanned, live.records);
  EXPECT_EQ(first.ledger_written, false);  // a recount that matches the ledger leaves its version alone
  EXPECT_EQ(due.ledger_written, false);
  EXPECT_EQ(first.status.status_records, std::optional<double>(live.records));
  EXPECT_EQ(first.status.metadata_bytes, std::optional<double>(live.bytes));
  const rt::KvValue marker = co_await reconciledAt(kv);
  EXPECT_EQ(marker, rt::KvValue(start + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS));
  kv.close();
}

ASYNC_TEST("a removed status row converges at the next due poll while metadata pressure reclaims it at once") {
  rt::MemoryKv kv(kvClock);
  const Poll initial = co_await pollMaintenance(kv, NOW, BUDGET_BYTES);
  EXPECT_EQ(initial.status.accounting_complete, true);
  std::vector<rt::KvKey> keys;
  for (double index = 0; index < 7; index += 1) {
    const std::string requestId = "pressure-status-" + rt::numberToString(index);
    const bool admitted = co_await admitStatus(kv, requestId, 3'600'000);
    EXPECT_EQ(admitted, true);
    keys.push_back(sentinelReplayRequestStatusKey(requestId));
  }
  const SentinelReplayLedgerSnapshot full = co_await ledgerOf(kv);
  EXPECT_EQ(full.ledger.status_records, 7.0);
  EXPECT(full.ledger.metadata_bytes + sentinelReplayStatusMetadataBytes(statusRow("pressure-after")) > sentinelReplayMetadataReserve(BUDGET_BYTES));
  // Deleting rows without touching the ledger is exactly what native TTL expiry leaves behind.
  for (std::size_t index = 0; index < 4; ++index) co_await kv.delete_(keys[index]);

  const Poll inside = co_await pollMaintenance(kv, NOW + 1, BUDGET_BYTES);
  EXPECT_EQ(inside.scanned, 0.0);
  EXPECT_EQ(inside.ledger_written, false);
  EXPECT_EQ(inside.status.status_records, std::optional<double>(7));  // inside the interval the vanished rows keep their conservative charge

  // Pressure does not wait for the interval: admission recounts before it would prune a live row.
  const bool admitted = co_await admitStatus(kv, std::string("pressure-after"), 3'600'000);
  EXPECT_EQ(admitted, true);
  const SentinelReplayLedgerSnapshot afterAdmission = co_await ledgerOf(kv);
  EXPECT_EQ(afterAdmission.ledger.status_records, 4.0);
  EXPECT_EQ(afterAdmission.ledger.status_pruned_records, 0.0);  // the stale charge was reclaimed without pruning a live row
  const Metadata liveAfterAdmission = co_await liveMetadata(kv);
  EXPECT_EQ(liveAfterAdmission, (Metadata{.records = afterAdmission.ledger.status_records, .bytes = afterAdmission.ledger.metadata_bytes}));

  co_await kv.delete_(keys[4]);
  const Poll stillInside = co_await pollMaintenance(kv, NOW + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS - 1, BUDGET_BYTES);
  EXPECT_EQ(stillInside.scanned, 0.0);
  EXPECT_EQ(stillInside.status.status_records, std::optional<double>(4));
  const Poll due = co_await pollMaintenance(kv, NOW + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS, BUDGET_BYTES);
  EXPECT_EQ(due.scanned, 3.0);
  EXPECT_EQ(due.ledger_written, true);
  EXPECT_EQ(due.status.status_records, std::optional<double>(3));  // the next due poll converges on the live rows
  const Metadata liveAfterDue = co_await liveMetadata(kv);
  EXPECT_EQ(liveAfterDue, (Metadata{.records = due.status.status_records.value_or(-1), .bytes = due.status.metadata_bytes.value_or(-1)}));
  kv.close();
}

namespace {
  bool checksLedgerKey(const std::vector<rt::AtomicCheck>& checks) {
    for (const rt::AtomicCheck& check : checks) {
      if (sameKey(check.key, SENTINEL_REPLAY_BUDGET_LEDGER_KEY)) return true;
    }
    return false;
  }

  struct RaceState {
    bool armed = true;
    std::function<rt::Task<void>()> race;
    std::shared_ptr<std::vector<bool>> outcomes;
  };

  class RacedAtomic final : public rt::ForwardingAtomicOperation {
  public:
    RacedAtomic(std::unique_ptr<rt::AtomicOperation> operation, std::shared_ptr<RaceState> state)
        : rt::ForwardingAtomicOperation(std::move(operation)), state_(std::move(state)) {}
    using rt::ForwardingAtomicOperation::check;
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
      checksLedger_ = checksLedger_ || checksLedgerKey(items);
      inner->check(std::move(items));
      return *this;
    }
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      inner->set(std::move(key), std::move(value), options);
      return *this;
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override {
      inner->delete_(std::move(key));
      return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override {
      if (!checksLedger_ || !state_->armed) {
        rt::KvCommitResult direct = co_await inner->commit();
        co_return direct;
      }
      state_->armed = false;
      auto racing = state_->race();
      co_await std::move(racing);
      rt::KvCommitResult result = co_await inner->commit();
      state_->outcomes->push_back(result.ok);
      co_return result;
    }

  private:
    std::shared_ptr<RaceState> state_;
    bool checksLedger_ = false;
  };

  /** Runs `race` immediately before the first commit that checks the ledger, and records that commit's outcome. */
  class RaceFirstLedgerCommit final : public rt::ForwardingKv {
  public:
    RaceFirstLedgerCommit(rt::Kv& kv, std::shared_ptr<RaceState> state) : rt::ForwardingKv(kv), state_(std::move(state)) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<RacedAtomic>(inner.atomic(), state_); }

  private:
    std::shared_ptr<RaceState> state_;
  };
}

ASYNC_TEST("a recount that loses to a concurrent writer, or a stale or corrupt marker, leaves the next poll due") {
  rt::MemoryKv kv(kvClock);
  co_await pollMaintenance(kv, NOW, BUDGET_BYTES);
  const rt::KvValue initialMarker = co_await reconciledAt(kv);
  EXPECT_EQ(initialMarker, rt::KvValue(NOW));
  const bool admitted = co_await admitStatus(kv, std::string("before-race"));
  EXPECT_EQ(admitted, true);
  const auto outcomes = std::make_shared<std::vector<bool>>();
  const double due = NOW + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS;
  rt::Kv* store = &kv;
  const auto state = std::make_shared<RaceState>();
  state->outcomes = outcomes;
  state->race = [store]() -> rt::Task<void> {
    const bool raced = co_await admitStatus(*store, std::string("during-race"));
    EXPECT_EQ(raced, true);
  };
  RaceFirstLedgerCommit raced(kv, state);
  const SentinelReplayMaintenanceOptions options{.now_ms = due, .budget_bytes = BUDGET_BYTES};
  auto maintaining = runSentinelReplayRetentionMaintenance(raced, options);
  co_await std::move(maintaining);
  EXPECT_EQ(*outcomes, std::vector<bool>{false});  // the ledger version check rejects the stale recount
  const rt::KvValue unchanged = co_await reconciledAt(kv);
  EXPECT_EQ(unchanged, rt::KvValue(NOW));  // a lost recount never records itself as done
  const SentinelReplayLedgerSnapshot raceLedger = co_await ledgerOf(kv);
  const Metadata liveAfterRace = co_await liveMetadata(kv);
  EXPECT_EQ(liveAfterRace, (Metadata{.records = 2, .bytes = raceLedger.ledger.metadata_bytes}));
  EXPECT_EQ(raceLedger.ledger.status_records, 2.0);  // the racing admission's own CAS kept the ledger exact

  const Poll retried = co_await pollMaintenance(kv, due + 1, BUDGET_BYTES);
  EXPECT_EQ(retried.scanned, 2.0);  // the next poll is still due and recounts
  const rt::KvValue retriedMarker = co_await reconciledAt(kv);
  EXPECT_EQ(retriedMarker, rt::KvValue(due + 1));
  const Poll deferred = co_await pollMaintenance(kv, due + 2, BUDGET_BYTES);
  EXPECT_EQ(deferred.scanned, 0.0);

  // A marker from a clock that ran ahead cannot postpone the recount indefinitely.
  rt::KvObject ahead;
  ahead.set("version", rt::KvValue(1));
  ahead.set("reconciled_at_ms", rt::KvValue(due + 10 * SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS));
  const rt::KvValue aheadValue(std::move(ahead));
  co_await kv.set(SENTINEL_REPLAY_METADATA_RECONCILED_KEY, aheadValue);
  const Poll afterAhead = co_await pollMaintenance(kv, due + 3, BUDGET_BYTES);
  EXPECT_EQ(afterAhead.scanned, 2.0);
  const rt::KvValue aheadMarker = co_await reconciledAt(kv);
  EXPECT_EQ(aheadMarker, rt::KvValue(due + 3));
  rt::KvObject negative;
  negative.set("version", rt::KvValue(1));
  negative.set("reconciled_at_ms", rt::KvValue(-1));
  rt::KvObject versioned;
  versioned.set("version", rt::KvValue(2));
  versioned.set("reconciled_at_ms", rt::KvValue(due));
  const std::vector<rt::KvValue> corrupts{rt::KvValue("not-a-marker"), rt::KvValue(std::move(negative)), rt::KvValue(std::move(versioned))};
  for (const rt::KvValue& corrupt : corrupts) {
    co_await kv.set(SENTINEL_REPLAY_METADATA_RECONCILED_KEY, corrupt);
    const Poll corrected = co_await pollMaintenance(kv, due + 4, BUDGET_BYTES);
    EXPECT_EQ(corrected.scanned, 2.0);  // a corrupt marker is treated as due
    const rt::KvValue correctedMarker = co_await reconciledAt(kv);
    EXPECT_EQ(correctedMarker, rt::KvValue(due + 4));
  }
  kv.close();
}

ASYNC_TEST("a due recount that meets a corrupt metadata row backs off for the interval instead of rescanning on every poll") {
  rt::MemoryKv kv(kvClock);
  const double pollIntervalMs = 30'000;
  co_await pollMaintenance(kv, NOW, BUDGET_BYTES);
  for (double index = 0; index < 3; index += 1) {
    const bool admitted = co_await admitStatus(kv, std::string("backoff-") + rt::numberToString(index), 3'600'000);
    EXPECT_EQ(admitted, true);
  }
  // Sorted after the three healthy rows, so a due scan reads all of them before it meets the corrupt one.
  const rt::KvKey corruptKey = sentinelReplayRequestStatusKey("zz-corrupt-owner");
  const rt::KvValue corrupt = statusRow("different-owner");
  co_await kv.set(corruptKey, corrupt);
  const double due = NOW + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS;
  const Poll failed = co_await pollMaintenance(kv, due, BUDGET_BYTES);
  EXPECT_EQ(failed.scanned, 4.0);  // the due recount reads up to the corrupt row
  EXPECT_EQ(failed.ledger_written, false);
  const rt::KvValue marker = co_await reconciledAt(kv);
  EXPECT_EQ(marker, rt::KvValue(NOW));  // a failed scan never records a recount

  std::vector<double> inside;
  for (double at = due + pollIntervalMs; at < due + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS; at += pollIntervalMs) {
    const Poll poll = co_await pollMaintenance(kv, at, BUDGET_BYTES);
    inside.push_back(poll.scanned);
  }
  EXPECT_EQ(static_cast<double>(inside.size()), SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS / pollIntervalMs - 1);
  EXPECT_EQ(inside, std::vector<double>(inside.size(), 0.0));  // polls inside the failure backoff do not repeat the scan

  const Poll retried = co_await pollMaintenance(kv, due + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS, BUDGET_BYTES);
  EXPECT_EQ(retried.scanned, 4.0);  // the scan is retried once the backoff ends
  co_await kv.delete_(corruptKey);
  const Poll quiet = co_await pollMaintenance(kv, due + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS + 1, BUDGET_BYTES);
  EXPECT_EQ(quiet.scanned, 0.0);
  const Poll recovered = co_await pollMaintenance(kv, due + 2 * SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS, BUDGET_BYTES);
  EXPECT_EQ(recovered.scanned, 3.0);  // after the repair the next due poll recounts the live rows
  const rt::KvValue recoveredMarker = co_await reconciledAt(kv);
  EXPECT_EQ(recoveredMarker, rt::KvValue(due + 2 * SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS));
  EXPECT_EQ(recovered.status.status_records, std::optional<double>(3));
  kv.close();
}

ASYNC_TEST("of two concurrent due recounts only the first to commit records the marker, so it never moves back") {
  rt::MemoryKv kv(kvClock);
  co_await pollMaintenance(kv, NOW, BUDGET_BYTES);
  const bool admitted = co_await admitStatus(kv, std::string("concurrent-recount"), 3'600'000);
  EXPECT_EQ(admitted, true);
  const double due = NOW + SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS;
  const double later = due + 5'000;
  const auto outcomes = std::make_shared<std::vector<bool>>();
  rt::Kv* store = &kv;
  // The earlier poll reads the marker, scans, and then loses the commit race to a later poll.
  const auto state = std::make_shared<RaceState>();
  state->outcomes = outcomes;
  state->race = [store, later]() -> rt::Task<void> {
    const SentinelReplayMaintenanceOptions options{.now_ms = later, .budget_bytes = BUDGET_BYTES};
    auto maintaining = runSentinelReplayRetentionMaintenance(*store, options);
    co_await std::move(maintaining);
    const rt::KvValue marker = co_await reconciledAt(*store);
    EXPECT_EQ(marker, rt::KvValue(later));
  };
  RaceFirstLedgerCommit raced(kv, state);
  const SentinelReplayMaintenanceOptions options{.now_ms = due, .budget_bytes = BUDGET_BYTES};
  auto maintaining = runSentinelReplayRetentionMaintenance(raced, options);
  co_await std::move(maintaining);
  EXPECT_EQ(*outcomes, std::vector<bool>{false});  // the stale poll's marker check rejects its commit
  const rt::KvValue marker = co_await reconciledAt(kv);
  EXPECT_EQ(marker, rt::KvValue(later));  // the marker keeps the later recount
  const Poll deferred = co_await pollMaintenance(kv, later + 1, BUDGET_BYTES);
  EXPECT_EQ(deferred.scanned, 0.0);  // and the committed recount still defers the next one
  kv.close();
}
