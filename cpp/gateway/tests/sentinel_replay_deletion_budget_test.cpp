// Links: tests/sentinel_replay_deletion_budget_test.cpp src/sentinel/replay_retention.cpp src/sentinel/replay_retention_bootstrap.cpp
//        src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_read.cpp src/sentinel/replay_retention_schema.cpp
//        src/sentinel/replay_model.cpp src/sentinel/replay_envelope.cpp src/sentinel/replay_observation.cpp src/sentinel/upstream_capture.cpp
//        src/sentinel/replay_limits.cpp src/sentinel/incident_outbox.cpp src/request.cpp src/utils.cpp src/config.cpp src/release.cpp
//        src/kv.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
// Port of tests/sentinel-replay-deletion-budget.test.ts. Storage is `openKv(":memory:")`: rt::MemoryKv. The three
// `for (const scenario of [...]) test(...)` cases are three tests sharing one scenario body.
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "uos/sentinel/replay_retention.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

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
  using namespace uos::sentinel::replay_retention_schema;
  using uos::sentinel::replay_read::isSentinelReplayManifest;

  constexpr double NOW = 1'700'000'000'000;
  constexpr double BUDGET_BYTES = 64 * 1'024 * 1'024;

  double kvClock() { return rt::dateNow(); }

  rt::KvValue keyValue(const rt::KvKey& key) {
    rt::KvArray parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(std::visit([](const auto& value) { return rt::KvValue(value); }, part.value));
    return rt::KvValue(std::move(parts));
  }

  struct Victim {
    SentinelReplayManifest manifest;
    SentinelReplayAccountingRow accounting;
    rt::KvKey manifestKey;
    SentinelReplayCaptureStatusRow status;
    rt::KvKey key;
  };

  /** Synthetic ciphertext rows exercise native deletion without encryption or provider calls. */
  Victim victimFor(double chunkCount, double index) {
    const std::string captureId = "deletion-budget-" + rt::numberToString(index);
    const std::string fingerprint = rt::repeat(rt::numberToString(index + 1), 64);
    const SentinelReplayManifest manifest{
      .version = 1,
      .capture_id = captureId,
      .fingerprint = fingerprint,
      .case_group_digest = std::string(64, 'a'),
      .captured_at_ms = NOW + index,
      .expires_at_ms = NOW + index + SENTINEL_REPLAY_TTL_MS,
      .algorithm = "AES-256-GCM",
      .compression = "gzip",
      .iv = "AAAAAAAAAAAAAAAA",
      .chunk_count = chunkCount,
      .ciphertext_bytes = chunkCount * SENTINEL_REPLAY_CHUNK_BYTES,
      .request_id = captureId,
    };
    EXPECT(isSentinelReplayManifest(rt::KvValue(json::value_from(manifest))));  // fixture manifest obeys the real multi-chunk size contract
    const SentinelReplayAccountingRow accounting{
      .version = 1,
      .capture_id = captureId,
      .request_id = captureId,
      .fingerprint = fingerprint,
      .bytes = sentinelReplayManifestCharge(manifest),
      .state = "stored",
      .fence = 1,
      .created_at_ms = manifest.captured_at_ms,
      .expires_at_ms = manifest.expires_at_ms,
      .status_expires_at_ms = NOW + index + SENTINEL_REPLAY_STATUS_TTL_MS,
      .stage = 1,
    };
    const rt::KvKey manifestKey = sentinelReplayManifestKey(manifest);
    const SentinelReplayCaptureStatusRow status{
      .version = 1,
      .request_id = captureId,
      .status = "ready",
      .reason = std::nullopt,
      .captured_at_ms = manifest.captured_at_ms,
      .manifest_key = manifestKey,
      .fingerprint = fingerprint,
      .expires_at_ms = manifest.expires_at_ms,
    };
    return {
      .manifest = manifest,
      .accounting = accounting,
      .manifestKey = manifestKey,
      .status = status,
      .key = sentinelReplayAccountingKey(accounting.created_at_ms, captureId),
    };
  }

  rt::Task<std::vector<Victim>> seedVictims(rt::Kv& kv, std::vector<double> chunkCounts) {
    std::vector<Victim> victims;
    for (std::size_t index = 0; index < chunkCounts.size(); ++index) victims.push_back(victimFor(chunkCounts[index], static_cast<double>(index)));
    const LedgerState state = co_await readLedger(kv, BUDGET_BYTES);
    if (state.kind == LedgerStateKind::corrupt) throw rt::JsError("empty fixture ledger is corrupt");
    for (const Victim& victim : victims) {
      for (double index = 0; index < victim.manifest.chunk_count; index += 1) {
        const rt::KvKey chunkKey{SENTINEL_REPLAY_CHUNK_PREFIX, victim.accounting.capture_id, index};
        const rt::KvValue chunk(rt::KvBytes(static_cast<std::size_t>(SENTINEL_REPLAY_CHUNK_BYTES), 0));
        co_await kv.set(chunkKey, chunk);
      }
      rt::KvObject dedupe;
      dedupe.set("manifest_key", keyValue(victim.manifestKey));
      const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->set(victim.key, rt::KvValue(json::value_from(victim.accounting)))
        .set(victim.manifestKey, rt::KvValue(json::value_from(victim.manifest)))
        .set(rt::KvKey{SENTINEL_REPLAY_DEDUPE_PREFIX, victim.accounting.fingerprint}, rt::KvValue(std::move(dedupe)))
        .set(sentinelReplayRequestStatusKey(victim.accounting.request_id), toKvValue(victim.status));
      const rt::KvCommitResult committed = co_await operation->commit();
      EXPECT_EQ(committed.ok, true);
    }
    SentinelReplayBudgetLedger ledger = *state.ledger;
    ledger.stored_bytes = 0;
    ledger.metadata_bytes = 0;
    for (const Victim& victim : victims) {
      ledger.stored_bytes += victim.accounting.bytes;
      ledger.metadata_bytes += sentinelReplayStatusMetadataBytes(toKvValue(victim.status));
    }
    ledger.records = static_cast<double>(victims.size());
    ledger.status_records = static_cast<double>(victims.size());
    ledger.bootstrap_complete = true;
    const rt::KvValue ledgerValue(json::value_from(ledger));
    co_await kv.set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue);
    co_return victims;
  }

  rt::Task<double> chunkCount(rt::Kv& kv, std::string captureId) {
    double count = 0;
    rt::KvListSelector selector;
    selector.prefix = rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, captureId};
    const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
    while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      if (entry->key.size() == SENTINEL_REPLAY_CHUNK_PREFIX.size() + 2) count += 1;
    }
    co_return count;
  }

  // `Promise.all(victims.map((victim) => chunkCount(kv, victim.accounting.capture_id)))`
  rt::Task<std::vector<double>> countsFor(rt::Kv& kv, const std::vector<Victim>& victims) {
    std::vector<rt::Promise<double>> counts;
    for (const Victim& victim : victims) counts.push_back(rt::Promise<double>(chunkCount(kv, victim.accounting.capture_id)));
    auto pending = rt::all(std::move(counts));
    std::vector<double> result = co_await pending.wait();
    co_return result;
  }

  rt::Task<SentinelReplayBudgetLedger> ledgerFor(rt::Kv& kv) {
    auto reading = readSentinelReplayLedgerSnapshot(kv, BUDGET_BYTES);
    const std::optional<SentinelReplayLedgerSnapshot> snapshot = co_await std::move(reading);
    if (!snapshot) throw rt::JsError("fixture ledger is corrupt");
    co_return snapshot->ledger;
  }

  struct Scenario {
    double limit;
    std::vector<double> remaining;
    double records;
  };

  rt::Task<void> evictionHonorsAggregateMaxChunkDeletes(Scenario scenario) {
    rt::MemoryKv kv(kvClock);
    const std::vector<double> counts{3, 3};
    auto seeding = seedVictims(kv, counts);
    const std::vector<Victim> victims = co_await std::move(seeding);
    auto baseline = countsFor(kv, victims);
    const std::vector<double> seeded = co_await std::move(baseline);
    EXPECT_EQ(seeded, counts);  // positive native-KV seed baseline

    const SentinelReplayBudgetLedger before = co_await ledgerFor(kv);
    const SentinelReplayEvictionOptions options{
      .target_bytes = 0,
      .max_chunk_deletes = scenario.limit,
      .now_ms = NOW + 10,
      .budget_bytes = BUDGET_BYTES,
    };
    auto evicting = evictSentinelReplays(kv, options);
    const SentinelReplayEviction result = co_await std::move(evicting);
    // Native row counts must prove the bound independently of the reported deletion count.
    auto surviving = countsFor(kv, victims);
    const std::vector<double> remaining = co_await std::move(surviving);
    EXPECT_EQ(remaining, scenario.remaining);  // actual surviving chunk rows
    EXPECT_EQ(result.chunks, scenario.limit);
    EXPECT_EQ(result.records, scenario.records);
    const double released = scenario.records == 0 ? 0 : victims[0].accounting.bytes;
    EXPECT_EQ(result.bytes, released);
    const SentinelReplayBudgetLedger after = co_await ledgerFor(kv);
    EXPECT_EQ(after.stored_bytes, before.stored_bytes - released);  // partial cleanup retains the entire remaining victim charge
    EXPECT_EQ(after.records, 2 - scenario.records);
    EXPECT_EQ(after.evicted_records, scenario.records);
    for (std::size_t index = 0; index < victims.size(); ++index) {
      const Victim& victim = victims[index];
      const rt::KvEntryMaybe<> accounting = co_await kv.get(victim.key);
      if (scenario.remaining[index] == 0) {
        EXPECT(accounting.value.isNull());
        const rt::KvEntryMaybe<> manifest = co_await kv.get(victim.manifestKey);
        EXPECT(manifest.value.isNull());
        const rt::KvEntryMaybe<> dedupe = co_await kv.get(rt::KvKey{SENTINEL_REPLAY_DEDUPE_PREFIX, victim.accounting.fingerprint});
        EXPECT(dedupe.value.isNull());
      } else {
        const std::string expectedState = scenario.remaining[index] == 3 ? "stored" : "evicting";
        EXPECT_EQ(accounting.value["state"], rt::KvValue(expectedState));
        EXPECT_EQ(accounting.value["bytes"], rt::KvValue(victim.accounting.bytes));
        const rt::KvEntryMaybe<> manifest = co_await kv.get(victim.manifestKey);
        EXPECT_EQ(manifest.value, rt::KvValue(json::value_from(victim.manifest)));
      }
    }
    if (scenario.limit == 0) EXPECT(after == before);  // zero budget leaves all accounting unchanged
    kv.close();
  }
}

ASYNC_TEST("eviction honors aggregate max_chunk_deletes=0 across multiple native-KV victims") {
  const Scenario scenario{.limit = 0, .remaining = {3, 3}, .records = 0};
  auto running = evictionHonorsAggregateMaxChunkDeletes(scenario);
  co_await std::move(running);
}

ASYNC_TEST("eviction honors aggregate max_chunk_deletes=1 across multiple native-KV victims") {
  const Scenario scenario{.limit = 1, .remaining = {2, 3}, .records = 0};
  auto running = evictionHonorsAggregateMaxChunkDeletes(scenario);
  co_await std::move(running);
}

ASYNC_TEST("eviction honors aggregate max_chunk_deletes=5 across multiple native-KV victims") {
  const Scenario scenario{.limit = 5, .remaining = {0, 1}, .records = 1};
  auto running = evictionHonorsAggregateMaxChunkDeletes(scenario);
  co_await std::move(running);
}

ASYNC_TEST("default eviction deletes at most 512 native chunks and later maintenance releases the partial charge once") {
  rt::MemoryKv kv(kvClock);
  const std::vector<double> counts{257, 257};
  auto seeding = seedVictims(kv, counts);
  const std::vector<Victim> victims = co_await std::move(seeding);
  auto baseline = countsFor(kv, victims);
  const std::vector<double> seeded = co_await std::move(baseline);
  EXPECT_EQ(seeded, counts);  // positive native-KV seed baseline

  const SentinelReplayBudgetLedger before = co_await ledgerFor(kv);
  const SentinelReplayEvictionOptions options{.target_bytes = 0, .now_ms = NOW + 10, .budget_bytes = BUDGET_BYTES};
  auto evicting = evictSentinelReplays(kv, options);
  const SentinelReplayEviction result = co_await std::move(evicting);
  auto afterEviction = countsFor(kv, victims);
  const std::vector<double> remaining = co_await std::move(afterEviction);
  EXPECT_EQ(remaining, (std::vector<double>{0, 2}));  // default 512 is one aggregate allowance
  EXPECT_EQ(result.chunks, 512.0);
  EXPECT_EQ(result.records, 1.0);
  EXPECT_EQ(result.bytes, victims[0].accounting.bytes);
  const SentinelReplayBudgetLedger partial = co_await ledgerFor(kv);
  EXPECT_EQ(partial.stored_bytes, victims[1].accounting.bytes);
  EXPECT_EQ(partial.evicted_bytes, victims[0].accounting.bytes);
  const rt::KvEntryMaybe<> accounting = co_await kv.get(victims[1].key);
  EXPECT_EQ(accounting.value["state"], rt::KvValue("evicting"));
  EXPECT_EQ(accounting.value["bytes"], rt::KvValue(victims[1].accounting.bytes));
  EXPECT_EQ(accounting.value["claim_kind"], rt::KvValue("evicted"));

  // Maintenance resumes the budget claim after its payload TTL has passed.
  const SentinelReplayMaintenanceOptions late{.now_ms = NOW + SENTINEL_REPLAY_TTL_MS + 11, .budget_bytes = BUDGET_BYTES};
  auto maintaining = runSentinelReplayRetentionMaintenance(kv, late);
  co_await std::move(maintaining);
  auto afterMaintenance = countsFor(kv, victims);
  const std::vector<double> drained = co_await std::move(afterMaintenance);
  EXPECT_EQ(drained, (std::vector<double>{0, 0}));  // the bounded continuation deletes only the two remaining chunks
  const rt::KvEntryMaybe<> resumed = co_await kv.get(victims[1].key);
  EXPECT(resumed.value.isNull());
  const SentinelReplayBudgetLedger finished = co_await ledgerFor(kv);
  EXPECT_EQ(finished.stored_bytes, 0.0);
  EXPECT_EQ(finished.records, 0.0);
  EXPECT_EQ(finished.evicted_records, 2.0);
  EXPECT_EQ(finished.evicted_bytes, before.stored_bytes);
  const SentinelReplayMaintenanceOptions again{.now_ms = NOW + 12, .budget_bytes = BUDGET_BYTES};
  auto repeating = runSentinelReplayRetentionMaintenance(kv, again);
  co_await std::move(repeating);
  const SentinelReplayBudgetLedger repeated = co_await ledgerFor(kv);
  EXPECT_EQ(repeated.stored_bytes, finished.stored_bytes);
  EXPECT_EQ(repeated.records, finished.records);
  EXPECT_EQ(repeated.evicted_records, finished.evicted_records);
  EXPECT_EQ(repeated.evicted_bytes, finished.evicted_bytes);  // the same charge is never released twice
  kv.close();
}
