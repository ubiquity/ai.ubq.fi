// Links: tests/sentinel_replay_staging_test.cpp src/sentinel/replay_store.cpp src/sentinel/replay_retention.cpp
//        src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_read.cpp
//        src/sentinel/replay_retention_schema.cpp src/sentinel/replay_model.cpp src/sentinel/replay_envelope.cpp
//        src/sentinel/replay_observation.cpp src/sentinel/upstream_capture.cpp src/sentinel/replay_limits.cpp src/sentinel/incident_outbox.cpp
//        src/request.cpp src/utils.cpp src/config.cpp src/release.cpp src/kv.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
// Port of tests/sentinel-replay-staging.test.ts. Storage is `openKv(":memory:")`: rt::MemoryKv. The TS Proxies over the
// store and its atomic operations are an rt::ForwardingKv / rt::ForwardingAtomicOperation pair that records the same
// calls; `assert.rejects(p, (error) => error === injected)` checks the rejection's identity (EXPECT_REJECTS_REASON).
#include "uos/sentinel/replay_limits.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "uos/sentinel/replay_retention.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"
#include "uos/sentinel/replay_store.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::sentinel::replay_model;
  using namespace uos::sentinel::replay_retention;
  using namespace uos::sentinel::replay_retention_schema;
  using uos::sentinel::replay_limits::SENTINEL_REPLAY_STORAGE_CHUNK_BYTES;
  using uos::sentinel::replay_observation::PersistDependencies;
  using uos::sentinel::replay_observation::SentinelReplayPersistResult;
  using uos::sentinel::replay_observation::SentinelReplayPersistResultIncomplete;
  using uos::sentinel::replay_observation::SentinelReplayPersistResultStored;
  using uos::sentinel::replay_read::decryptExportedSentinelReplay;
  using uos::sentinel::replay_read::EncryptedSentinelRequestReplays;
  using uos::sentinel::replay_read::listEncryptedSentinelReplaysByRequestId;
  using uos::sentinel::replay_store::persistEncryptedSentinelReplay;

  constexpr double NOW = 1'703'000'000'000;
  constexpr double BUDGET_BYTES = 16 * 1'024 * 1'024;

  double kvClock() { return rt::dateNow(); }

  const std::shared_ptr<rt::Bytes>& inputBody() {
    static const std::shared_ptr<rt::Bytes> cachedBody = [] {
      rt::Bytes random(1 * 1'024 * 1'024);
      for (std::size_t offset = 0; offset < random.size(); offset += 65'536) {
        rt::crypto::getRandomValues(std::span<std::uint8_t>(random).subspan(offset, std::min<std::size_t>(65'536, random.size() - offset)));
      }
      json::object body;
      body["model"] = "gpt-5.6-sol";
      body["blob"] = rt::toBase64(random);
      return std::make_shared<rt::Bytes>(rt::encodeUtf8(rt::jsonStringify(json::value(std::move(body)))));
    }();
    return cachedBody;
  }

  AcceptedSentinelReplayInput captureInput(const std::string& requestId) {
    return {
      .endpoint = "/v1/responses",
      .method = "POST",
      .body = inputBody(),
      .content_type = "application/json",
      .compatibility_headers = {},
      .request_id = requestId,
      .git_sha = std::string(40, 'a'),
      .deno_revision = "staging-fixture",
    };
  }

  SentinelFailureObservation failure() {
    return {
      .status = 502,
      .stream = false,
      .completed = false,
      .terminal_type = "http.error",
      .failure_kind = "upstream_timeout",
      .synthetic_terminal_type = std::nullopt,
      .provider_route = "chatgpt_codex",
    };
  }

  rt::Bytes randomKey() {
    rt::Bytes key(32);
    rt::crypto::getRandomValues(std::span<std::uint8_t>(key));
    return key;
  }

  // `Readonly<{ key: KvKey; bytes: number; ttl_ms: number | undefined }>`
  struct ChunkWrite {
    rt::KvKey key;
    double bytes;
    std::optional<double> ttl_ms;
  };
  // `Readonly<{ number: number; checks: readonly Check[]; chunks: readonly ChunkWrite[] }>`
  struct Batch {
    double number;
    std::vector<rt::AtomicCheck> checks;
    std::vector<ChunkWrite> chunks;
  };
  struct Observation {
    std::vector<Batch> batches;
    double direct_chunk_sets = 0;
    bool refuse_deletes = false;
    std::function<rt::Task<void>(Batch batch)> before;
    std::function<rt::Task<void>(Batch batch, rt::KvCommitResult result)> after;
  };

  bool isChunkKey(const rt::KvKey& key) {
    if (key.size() != SENTINEL_REPLAY_CHUNK_PREFIX.size() + 2) return false;
    for (std::size_t index = 0; index < SENTINEL_REPLAY_CHUNK_PREFIX.size(); ++index) {
      if (!key[index].isString() || key[index].string() != SENTINEL_REPLAY_CHUNK_PREFIX[index].string()) return false;
    }
    return true;
  }

  class ObservedAtomic final : public rt::ForwardingAtomicOperation {
  public:
    ObservedAtomic(std::unique_ptr<rt::AtomicOperation> operation, std::shared_ptr<Observation> observation)
        : rt::ForwardingAtomicOperation(std::move(operation)), observation_(std::move(observation)) {}
    using rt::ForwardingAtomicOperation::check;
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
      for (const rt::AtomicCheck& item : items) checks_.push_back(item);
      inner->check(std::move(items));
      return *this;
    }
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      if (isChunkKey(key)) {
        EXPECT(value.isBytes());
        // The caller clears chunk buffers later; retain byte lengths and keys now,
        // while native KV remains the source for actual stored/decrypted bytes.
        chunks_.push_back(ChunkWrite{.key = key, .bytes = static_cast<double>(value.asBytes().size()), .ttl_ms = options.expireIn});
      }
      inner->set(std::move(key), std::move(value), options);
      return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override {
      if (chunks_.empty()) {
        rt::KvCommitResult direct = co_await inner->commit();
        co_return direct;
      }
      const Batch batch{.number = static_cast<double>(observation_->batches.size() + 1), .checks = checks_, .chunks = chunks_};
      observation_->batches.push_back(batch);
      auto before = observation_->before(batch);
      co_await std::move(before);
      rt::KvCommitResult result = co_await inner->commit();
      auto after = observation_->after(batch, result);
      co_await std::move(after);
      co_return result;
    }

  private:
    std::shared_ptr<Observation> observation_;
    std::vector<rt::AtomicCheck> checks_;
    std::vector<ChunkWrite> chunks_;
  };

  rt::Task<void> refusedDelete() {
    throw rt::JsError("staging_fixture_delete_failure");
    co_return;
  }

  class ObservedKv final : public rt::ForwardingKv {
  public:
    ObservedKv(rt::Kv& kv, std::shared_ptr<Observation> observation) : rt::ForwardingKv(kv), observation_(std::move(observation)) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<ObservedAtomic>(inner.atomic(), observation_); }
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      if (isChunkKey(key)) observation_->direct_chunk_sets += 1;
      return inner.set(std::move(key), std::move(value), options);
    }
    rt::Task<void> delete_(rt::KvKey key) override {
      if (isChunkKey(key) && observation_->refuse_deletes) return refusedDelete();
      return inner.delete_(std::move(key));
    }

  private:
    std::shared_ptr<Observation> observation_;
  };

  std::shared_ptr<Observation> observationFor(std::function<rt::Task<void>(Batch)> before, std::function<rt::Task<void>(Batch, rt::KvCommitResult)> after) {
    auto observation = std::make_shared<Observation>();
    observation->before = std::move(before);
    observation->after = std::move(after);
    return observation;
  }

  rt::Task<SentinelReplayPersistResult> persist(std::shared_ptr<rt::Kv> kv, std::string requestId, rt::Bytes keyBytes) {
    const PersistDependencies dependencies{
      .kv = std::move(kv),
      .keyBytes = std::move(keyBytes),
      .now = [] { return NOW; },
      .randomUuid = [requestId] { return "capture-" + requestId; },
      .budgetBytes = BUDGET_BYTES,
    };
    const AcceptedSentinelReplayInput input = captureInput(requestId);
    const SentinelFailureObservation observed = failure();
    auto persisting = persistEncryptedSentinelReplay(input, observed, dependencies);
    SentinelReplayPersistResult result = co_await std::move(persisting);
    co_return result;
  }

  rt::Task<SentinelReplayBudgetLedger> ledgerOf(rt::Kv& kv) {
    auto reading = readSentinelReplayLedgerSnapshot(kv, BUDGET_BYTES);
    const std::optional<SentinelReplayLedgerSnapshot> snapshot = co_await std::move(reading);
    ASSERT(snapshot.has_value());
    co_return snapshot->ledger;
  }

  rt::Task<double> chunkCount(rt::Kv& kv, std::string captureId) {
    double count = 0;
    const rt::KvKey prefix{SENTINEL_REPLAY_CHUNK_PREFIX, captureId};
    rt::KvListSelector selector;
    selector.prefix = prefix;
    const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
    while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      EXPECT_EQ(entry->key.size(), prefix.size() + 1);
      count += 1;
    }
    co_return count;
  }

  bool isIncompleteStorageFull(const SentinelReplayPersistResult& result) {
    const auto* incomplete = std::get_if<SentinelReplayPersistResultIncomplete>(&result);
    return incomplete && incomplete->reason == SENTINEL_REPLAY_STORAGE_FULL_REASON;
  }

  rt::Task<void> resolved() { co_return; }
}

ASYNC_TEST("publication rejects a reservation expired during capture staging") {
  rt::MemoryKv kv(kvClock);
  const std::string requestId = "staging-expired-publication";
  auto wallNow = std::make_shared<double>(NOW);
  auto leaseReads = std::make_shared<int>(0);
  const std::function<double()> currentNow = [wallNow, leaseReads] {
    *leaseReads += 1;
    if (*leaseReads == 2) *wallNow = NOW + SENTINEL_REPLAY_RESERVATION_TTL_MS + 1;
    return *wallNow;
  };
  AcceptedSentinelReplayInput input = captureInput(requestId);
  input.body = std::make_shared<rt::Bytes>(rt::encodeUtf8("{}"));
  const PersistDependencies dependencies{
    .kv = std::shared_ptr<rt::Kv>(&kv, [](rt::Kv*) {}),
    .keyBytes = randomKey(),
    .now = [] { return NOW; },
    .currentNow = currentNow,
    .randomUuid = [requestId] { return "capture-" + requestId; },
    .budgetBytes = BUDGET_BYTES,
  };
  const SentinelFailureObservation observed = failure();
  auto persisting = persistEncryptedSentinelReplay(input, observed, dependencies);
  const SentinelReplayPersistResult result = co_await std::move(persisting);
  EXPECT(isIncompleteStorageFull(result));
  EXPECT_EQ(*leaseReads, 2);
  const double chunks = co_await chunkCount(kv, "capture-" + requestId);
  EXPECT_EQ(chunks, 0.0);
  const SentinelReplayBudgetLedger ledger = co_await ledgerOf(kv);
  EXPECT_EQ(ledger.reserved_bytes, 0.0);
  kv.close();
}

namespace {
  rt::Task<void> pausedBatchRejected(double pausedBatch) {
    rt::MemoryKv kv(kvClock);
    const std::string requestId = "staging-paused-" + rt::numberToString(pausedBatch);
    const std::string captureId = "capture-" + requestId;
    const rt::KvKey accountingKey = sentinelReplayAccountingKey(NOW, captureId);
    auto revoked = std::make_shared<bool>(false);
    auto inspected = std::make_shared<bool>(false);
    rt::Kv* store = &kv;
    const std::shared_ptr<Observation> observation = observationFor(
      [store, pausedBatch, captureId, accountingKey, revoked](Batch batch) -> rt::Task<void> {
        if (batch.number != pausedBatch) co_return;
        const double staged = co_await chunkCount(*store, captureId);
        EXPECT_EQ(staged, pausedBatch == 1 ? 0.0 : 12.0);
        const rt::KvEntryMaybe<> entry = co_await store->get(accountingKey);
        ASSERT(!entry.value.isNull());
        EXPECT_EQ(entry.value["state"], rt::KvValue("reserved"));
        const SentinelReplayAccountingRow row = json::value_to<SentinelReplayAccountingRow>(entry.value.toJson());
        const SentinelReplayAbandonOptions options{.now_ms = NOW, .budget_bytes = BUDGET_BYTES};
        auto abandoning = abandonSentinelReplayAccounting(*store, row, accountingKey, options);
        const SentinelReplayAbandonment cleanup = co_await std::move(abandoning);
        EXPECT_EQ(cleanup.released, true);
        const SentinelReplayBudgetLedger ledger = co_await ledgerOf(*store);
        EXPECT_EQ(ledger.reserved_bytes, 0.0);
        const double remaining = co_await chunkCount(*store, captureId);
        EXPECT_EQ(remaining, 0.0);
        *revoked = true;
      },
      [store, pausedBatch, captureId, accountingKey, inspected](Batch batch, rt::KvCommitResult result) -> rt::Task<void> {
        if (batch.number != pausedBatch) co_return;
        EXPECT_EQ(result.ok, false);  // native accounting-row CAS must reject before cleanup can conceal the race
        const rt::KvEntryMaybe<> entry = co_await store->get(accountingKey);
        EXPECT(entry.value.isNull());
        const double reappeared = co_await chunkCount(*store, captureId);
        EXPECT_EQ(reappeared, 0.0);  // no chunk may reappear before the writer receives the failed commit
        *inspected = true;
      });
    auto persisting = persist(std::make_shared<ObservedKv>(kv, observation), requestId, randomKey());
    const SentinelReplayPersistResult result = co_await std::move(persisting);
    EXPECT(isIncompleteStorageFull(result));
    EXPECT_EQ(*revoked, true);
    EXPECT_EQ(*inspected, true);
    EXPECT_EQ(observation->direct_chunk_sets, 0.0);
    EXPECT_EQ(static_cast<double>(observation->batches.size()), pausedBatch);
    const SentinelReplayBudgetLedger ledger = co_await ledgerOf(kv);
    EXPECT_EQ(ledger.reserved_bytes, 0.0);
    kv.close();
  }
}

ASYNC_TEST("first and second chunk transactions reject a released reservation before writer cleanup") {
  const std::vector<double> pausedBatches{1, 2};
  for (const double pausedBatch : pausedBatches) {
    auto running = pausedBatchRejected(pausedBatch);
    co_await std::move(running);
  }
}

ASYNC_TEST("a capture larger than 12 chunks uses bounded checked transactions and decrypts unchanged") {
  rt::MemoryKv kv(kvClock);
  const std::string requestId = "staging-roundtrip";
  const rt::Bytes keyBytes = randomKey();
  const rt::Bytes original = *inputBody();
  const rt::KvKey accountingKey = sentinelReplayAccountingKey(NOW, "capture-" + requestId);
  rt::Kv* store = &kv;
  const std::shared_ptr<Observation> observation = observationFor(
    [store, accountingKey](Batch batch) -> rt::Task<void> {
      const rt::KvEntryMaybe<> current = co_await store->get(accountingKey);
      EXPECT_EQ(batch.checks.size(), std::size_t{1});
      if (!batch.checks.empty()) {
        EXPECT_EQ(batch.checks[0].key, accountingKey);
        EXPECT_EQ(batch.checks[0].versionstamp, current.versionstamp);
      }
      EXPECT(batch.chunks.size() <= 12);
      double sum = 0;
      for (const ChunkWrite& chunk : batch.chunks) sum += chunk.bytes;
      EXPECT(sum <= 576 * 1'024);
      for (const ChunkWrite& chunk : batch.chunks) {
        EXPECT(chunk.bytes <= SENTINEL_REPLAY_STORAGE_CHUNK_BYTES);
        EXPECT_EQ(chunk.ttl_ms, std::optional<double>(SENTINEL_REPLAY_TTL_MS));
      }
    },
    [](Batch, rt::KvCommitResult result) -> rt::Task<void> {
      EXPECT_EQ(result.ok, true);
      return resolved();
    });
  auto persisting = persist(std::make_shared<ObservedKv>(kv, observation), requestId, keyBytes);
  const SentinelReplayPersistResult result = co_await std::move(persisting);
  const auto* stored = std::get_if<SentinelReplayPersistResultStored>(&result);
  ASSERT(stored != nullptr);
  EXPECT(stored->manifest.chunk_count > 12);
  EXPECT(observation->batches.size() > 1);
  EXPECT_EQ(observation->direct_chunk_sets, 0.0);
  EXPECT_EQ(SENTINEL_REPLAY_STAGING_BATCH_CHUNKS, 12.0);
  auto exporting = listEncryptedSentinelReplaysByRequestId(kv, requestId, NOW);
  const EncryptedSentinelRequestReplays exported = co_await std::move(exporting);
  ASSERT_EQ(exported.captures.size(), std::size_t{1});
  auto decrypting = decryptExportedSentinelReplay(exported.captures[0], keyBytes);
  const SentinelReplayPlaintext decrypted = co_await std::move(decrypting);
  EXPECT(*decrypted.body == original);
  const SentinelReplayBudgetLedger ledger = co_await ledgerOf(kv);
  EXPECT_EQ(ledger.reserved_bytes, 0.0);
  EXPECT_EQ(ledger.records, 1.0);
  kv.close();
}

namespace {
  rt::Task<void> laterChunkCommitException(bool refuseDeletes) {
    rt::MemoryKv kv(kvClock);
    const std::string requestId = std::string("staging-commit-failure-") + (refuseDeletes ? "true" : "false");
    const std::string captureId = "capture-" + requestId;
    const rt::KvKey accountingKey = sentinelReplayAccountingKey(NOW, captureId);
    const rt::Reason injected = rt::makeReason(rt::JsError("staging_fixture_commit_failure"));
    const std::shared_ptr<Observation> observation = observationFor(
      [injected](Batch batch) -> rt::Task<void> {
        if (batch.number == 2) std::rethrow_exception(injected);
        co_return;
      },
      [](Batch, rt::KvCommitResult) -> rt::Task<void> { return resolved(); });
    observation->refuse_deletes = refuseDeletes;
    auto persisting = persist(std::make_shared<ObservedKv>(kv, observation), requestId, randomKey());
    EXPECT_REJECTS_REASON(std::move(persisting), injected);
    EXPECT_EQ(observation->batches.size(), std::size_t{2});
    const double chunks = co_await chunkCount(kv, captureId);
    const rt::KvEntryMaybe<> row = co_await kv.get(accountingKey);
    const SentinelReplayBudgetLedger ledger = co_await ledgerOf(kv);
    if (refuseDeletes) {
      EXPECT_EQ(chunks, 12.0);
      EXPECT_EQ(row.value["state"], rt::KvValue("revoked"));
      EXPECT_EQ(rt::KvValue(ledger.reserved_bytes), row.value["bytes"]);
    } else {
      EXPECT_EQ(chunks, 0.0);
      EXPECT(row.value.isNull());
      EXPECT_EQ(ledger.reserved_bytes, 0.0);
    }
    observation->refuse_deletes = false;
    const SentinelReplayMaintenanceOptions options{.now_ms = NOW + 1, .budget_bytes = BUDGET_BYTES};
    auto maintaining = runSentinelReplayRetentionMaintenance(kv, options);
    co_await std::move(maintaining);
    const double drained = co_await chunkCount(kv, captureId);
    EXPECT_EQ(drained, 0.0);
    const SentinelReplayBudgetLedger after = co_await ledgerOf(kv);
    EXPECT_EQ(after.reserved_bytes, 0.0);
    const rt::KvEntryMaybe<> gone = co_await kv.get(accountingKey);
    EXPECT(gone.value.isNull());
    kv.close();
  }
}

ASYNC_TEST("a later chunk commit exception preserves its error and cleanup retains charge until native chunks are gone") {
  const std::vector<bool> refusals{false, true};
  for (const bool refuseDeletes : refusals) {
    auto running = laterChunkCommitException(refuseDeletes);
    co_await std::move(running);
  }
}

namespace {
  rt::KvValue withStoredExtras(const rt::KvValue& fields) {
    rt::KvObject object;
    object.set("extra_first", rt::KvValue("retained"));
    for (const auto& [key, value] : fields.asObject()) object.set(key, value);
    object.set("extra_bytes", rt::KvValue(rt::KvBytes{0, 255}));
    object.set("extra_bigint", rt::KvValue(rt::BigInt(7)));
    object.set("extra_undefined", rt::KvValue());
    return rt::KvValue(std::move(object));
  }

  std::vector<std::string> storedKeys(const rt::KvValue& value) {
    std::vector<std::string> keys;
    for (const auto& [key, _] : value.asObject()) keys.push_back(key);
    return keys;
  }

  void expectStoredExtras(const rt::KvValue& value, const std::vector<std::string>& originalKeys) {
    EXPECT_EQ(value["extra_first"], rt::KvValue("retained"));
    EXPECT_EQ(value["extra_bytes"], rt::KvValue(rt::KvBytes{0, 255}));
    EXPECT_EQ(value["extra_bigint"], rt::KvValue(rt::BigInt(7)));
    ASSERT(value.isObject());
    EXPECT(value.asObject().contains("extra_undefined"));
    EXPECT(value["extra_undefined"].isUndefined());
    EXPECT_EQ(storedKeys(value), originalKeys);
  }
}

// Differential: retained_fields_oracle.ts, Bun 1.4.0, 2026-11-16T19:10:11Z.
ASYNC_TEST("staging preserves unknown stored values and property order through the TypeScript row spread") {
  rt::MemoryKv kv(kvClock);
  const SentinelReplayAccountingRow row{
    .version = 1, .capture_id = "extra-row", .request_id = "extra-request", .fingerprint = std::string(64, 'a'),
    .bytes = 100, .state = "reserved", .fence = 1, .created_at_ms = 1000, .expires_at_ms = 2000, .status_expires_at_ms = 3000, .stage = 0,
  };
  const rt::KvValue original = withStoredExtras(rt::KvValue(json::value_from(row)));
  const rt::KvKey key = sentinelReplayAccountingKey(1000, "extra-row");
  auto seeding = kv.set(key, original);
  co_await std::move(seeding);
  const SentinelReplayStagingFenceInput input{.accounting_key = key, .fence = 1, .now_ms = 1500, .budget_bytes = 65536};
  auto advancing = advanceSentinelReplayStagingFence(kv, input);
  const SentinelReplayStagingFence result = co_await std::move(advancing);
  ASSERT(std::holds_alternative<SentinelReplayStagingFenceOk>(result));
  const rt::KvEntryMaybe<> stored = co_await kv.get(key);
  EXPECT_EQ(stored.value["stage"], rt::KvValue(1));
  EXPECT_EQ(stored.value["bytes"], rt::KvValue(100));
  EXPECT_EQ(stored.value["state"], rt::KvValue("reserved"));
  expectStoredExtras(stored.value, storedKeys(original));
  kv.close();
}

ASYNC_TEST("ledger reads and atomic mutations preserve unknown stored values while strict DTOs stay declared") {
  rt::MemoryKv kv(kvClock);
  const LedgerState empty = co_await readLedger(kv, 65536);
  const rt::KvValue original = withStoredExtras(rt::KvValue(json::value_from(*empty.ledger)));
  auto seeding = kv.set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, original);
  co_await std::move(seeding);
  const LedgerState read = co_await readLedger(kv, 65536);
  ASSERT(read.kind == LedgerStateKind::ok);
  EXPECT_EQ(*read.ledger, *empty.ledger);
  const json::value dto = json::value_from(*read.ledger);
  EXPECT_EQ(rt::hasOwn(dto.as_object(), "extra_first"), false);
  EXPECT_EQ(rt::hasOwn(dto.as_object(), "extra_bigint"), false);
  const LedgerMutation mutate = [](const SentinelReplayBudgetLedger& ledger) {
    SentinelReplayBudgetLedger next = ledger;
    next.reserved_bytes += 9;
    return next;
  };
  const AtomicRowApplication apply = [](rt::AtomicOperation& operation) -> rt::AtomicOperation& { return operation; };
  const rt::KvKey rowKey{"fixture-absent-row"};
  auto committing = commitAccountingMutation(kv, rowKey, std::nullopt, mutate, apply, 65536);
  const bool committed = co_await std::move(committing);
  EXPECT_EQ(committed, true);
  const rt::KvEntryMaybe<> stored = co_await kv.get(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  EXPECT_EQ(stored.value["reserved_bytes"], rt::KvValue(9));
  EXPECT_EQ(stored.value["stored_bytes"], rt::KvValue(0));
  expectStoredExtras(stored.value, storedKeys(original));
  kv.close();
}

namespace {
  // A pre-existing stored record may contain valid fields plus opaque extras; inject those at its first read.
  class ExtraOnReadKv final : public rt::ForwardingKv {
  public:
    explicit ExtraOnReadKv(rt::Kv& store) : rt::ForwardingKv(store) {}
    std::optional<std::vector<std::string>> accountingKeys;
    std::optional<std::vector<std::string>> ledgerKeys;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      auto reading = inner.get(key, options);
      rt::KvEntryMaybe<> entry = co_await std::move(reading);
      if (entry.value.isObject() && key == SENTINEL_REPLAY_BUDGET_LEDGER_KEY && !ledgerKeys) {
        entry.value = withStoredExtras(entry.value);
        ledgerKeys = storedKeys(entry.value);
      } else if (entry.value.isObject() && !accountingKeys && key.size() == 6 && key[3].isString() && key[3].string() == "accounting") {
        entry.value = withStoredExtras(entry.value);
        accountingKeys = storedKeys(entry.value);
      }
      co_return entry;
    }
  };
}

ASYNC_TEST("publication, eviction claims and metadata recounts retain opaque stored row members") {
  rt::MemoryKv kv(kvClock);
  const auto wrapped = std::make_shared<ExtraOnReadKv>(kv);
  const std::string requestId = "extra-publication";
  const rt::KvKey key = sentinelReplayAccountingKey(NOW, "capture-" + requestId);
  auto persisting = persist(wrapped, requestId, randomKey());
  const SentinelReplayPersistResult result = co_await std::move(persisting);
  ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(result));
  ASSERT(wrapped->accountingKeys.has_value());
  ASSERT(wrapped->ledgerKeys.has_value());
  const rt::KvEntryMaybe<> published = co_await kv.get(key);
  const rt::KvEntryMaybe<> ledger = co_await kv.get(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  EXPECT_EQ(published.value["state"], rt::KvValue("stored"));
  expectStoredExtras(published.value, *wrapped->accountingKeys);
  expectStoredExtras(ledger.value, *wrapped->ledgerKeys);
  setSentinelReplayRetentionFaultsForTest([](const std::string& kind) { return kind == "delete"; });
  auto clearFault = rt::scopeExit([] { setSentinelReplayRetentionFaultsForTest({}); });
  const SentinelReplayMaintenanceOptions pressure{.now_ms = NOW + 1, .budget_bytes = 65536};
  auto evicting = runSentinelReplayRetentionMaintenance(kv, pressure);
  EXPECT_REJECTS_MESSAGE(std::move(evicting), rt::JsError, "injected_delete_failure");
  const rt::KvEntryMaybe<> claimed = co_await kv.get(key);
  EXPECT_EQ(claimed.value["state"], rt::KvValue("evicting"));
  EXPECT_EQ(claimed.value["claim_kind"], rt::KvValue("evicted"));
  std::vector<std::string> claimedKeys = *wrapped->accountingKeys;
  claimedKeys.push_back("claim_kind");
  expectStoredExtras(claimed.value, claimedKeys);
  const rt::KvEntryMaybe<> recounted = co_await kv.get(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  expectStoredExtras(recounted.value, *wrapped->ledgerKeys);
  setSentinelReplayRetentionFaultsForTest({});
  auto finishing = runSentinelReplayRetentionMaintenance(kv, pressure);
  co_await std::move(finishing);
  const rt::KvEntryMaybe<> removed = co_await kv.get(key);
  EXPECT(removed.value.isNull());
  const rt::KvEntryMaybe<> finalLedger = co_await kv.get(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  expectStoredExtras(finalLedger.value, *wrapped->ledgerKeys);
  kv.close();
}

ASYNC_TEST("reservation revocation and release preserve opaque accounting and ledger members") {
  rt::MemoryKv kv(kvClock);
  const LedgerState empty = co_await readLedger(kv, BUDGET_BYTES);
  SentinelReplayBudgetLedger ledger = *empty.ledger;
  ledger.bootstrap_complete = true;
  ledger.reserved_bytes = 100;
  const rt::KvValue originalLedger = withStoredExtras(rt::KvValue(json::value_from(ledger)));
  const SentinelReplayAccountingRow row{
    .version = 1, .capture_id = "extra-revocation", .request_id = "extra-revocation", .fingerprint = std::string(64, 'b'),
    .bytes = 100, .state = "reserved", .fence = 1, .created_at_ms = NOW, .expires_at_ms = NOW + 120000,
    .status_expires_at_ms = NOW + SENTINEL_REPLAY_STATUS_TTL_MS, .stage = 0,
  };
  const rt::KvValue originalRow = withStoredExtras(rt::KvValue(json::value_from(row)));
  const rt::KvKey key = sentinelReplayAccountingKey(NOW, row.capture_id);
  const rt::KvValue chunk("x");
  auto seedingLedger = kv.set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, originalLedger);
  co_await std::move(seedingLedger);
  auto seedingRow = kv.set(key, originalRow);
  co_await std::move(seedingRow);
  auto seedingChunk = kv.set(rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, row.capture_id, 0}, chunk);
  co_await std::move(seedingChunk);
  auto seedingSecondChunk = kv.set(rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, row.capture_id, 1}, chunk);
  co_await std::move(seedingSecondChunk);
  const SentinelReplayAbandonOptions paused{.now_ms = NOW, .max_chunk_deletes = 1, .budget_bytes = BUDGET_BYTES};
  auto abandoning = abandonSentinelReplayAccounting(kv, row, key, paused);
  const SentinelReplayAbandonment held = co_await std::move(abandoning);
  EXPECT_EQ(held.revoked, true);
  EXPECT_EQ(held.released, false);
  const rt::KvEntryMaybe<> revoked = co_await kv.get(key);
  EXPECT_EQ(revoked.value["state"], rt::KvValue("revoked"));
  EXPECT_EQ(revoked.value["fence"], rt::KvValue(2));
  expectStoredExtras(revoked.value, storedKeys(originalRow));
  const SentinelReplayAccountingRow revokedRow = toSentinelReplayAccountingRow(revoked.value);
  const SentinelReplayAbandonOptions resumed{.now_ms = NOW, .max_chunk_deletes = 10, .budget_bytes = BUDGET_BYTES};
  auto releasing = abandonSentinelReplayAccounting(kv, revokedRow, key, resumed);
  const SentinelReplayAbandonment released = co_await std::move(releasing);
  EXPECT_EQ(released.released, true);
  const rt::KvEntryMaybe<> finalLedger = co_await kv.get(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  EXPECT_EQ(finalLedger.value["reserved_bytes"], rt::KvValue(0));
  expectStoredExtras(finalLedger.value, storedKeys(originalLedger));
  kv.close();
}
