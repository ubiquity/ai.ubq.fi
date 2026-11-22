#include "uos/sentinel/replay_retention_bootstrap.hpp"
#include "uos/rt/memory.hpp"

#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"
#include "uos/sentinel/replay_read.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/weak.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uos::sentinel::replay_retention_bootstrap {
  namespace {
    using replay_model::SENTINEL_REPLAY_CHUNK_PREFIX;
    using replay_model::SENTINEL_REPLAY_DEDUPE_PREFIX;
    using replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX;
    using replay_model::SENTINEL_REPLAY_REQUEST_PREFIX;
    using replay_model::SENTINEL_REPLAY_STATUS_TTL_MS;
    using replay_model::SentinelReplayManifest;
    using replay_retention_schema::accountingKeyMatches;
    using replay_retention_schema::BOOTSTRAP_BATCH_ENTRIES;
    using replay_retention_schema::BOOTSTRAP_OWNER_INDEX_WAIT_MS;
    using replay_retention_schema::BOOTSTRAP_OWNER_SCAN_ENTRIES;
    using replay_retention_schema::counter;
    using replay_retention_schema::EVICTION_MAX_CHUNK_DELETES;
    using replay_retention_schema::fault;
    using replay_retention_schema::isAccountingRow;
    using replay_retention_schema::isLegacyReservation;
    using replay_retention_schema::LedgerState;
    using replay_retention_schema::LedgerStateKind;
    using replay_retention_schema::manifestKeyMatches;
    using replay_retention_schema::readLedger;
    using replay_retention_schema::SENTINEL_REPLAY_ACCOUNTING_PREFIX;
    using replay_retention_schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY;
    using replay_retention_schema::SENTINEL_REPLAY_EVICTION_PREFIX;
    using replay_retention_schema::SENTINEL_REPLAY_RESERVATION_PREFIX;
    using replay_retention_schema::sentinelReplayAccountingKey;
    using replay_retention_schema::sentinelReplayManifestCharge;
    using replay_retention_schema::sentinelReplayPayloadBudgetBytes;
    using replay_retention_schema::sentinelReplayStatusMetadataBytes;
    using replay_retention_schema::SentinelReplayAccountingRow;
    using replay_retention_schema::SentinelReplayBudgetLedger;
    using replay_retention_schema::SentinelReplayLegacyReservation;
    using replay_retention_schema::withBudget;

    // `typeof value === "object" && value !== null`
    bool isObjectLike(const rt::KvValue& value) {
      return !(value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt() || value.is<rt::KvHole>());
    }

    // The typed view of a value isAccountingRow accepted.
    SentinelReplayAccountingRow accountingRowOf(const rt::KvValue& value) {
      return replay_retention_schema::toSentinelReplayAccountingRow(value);
    }

    // `key.join("\u0000")`
    std::string joinedKey(const rt::KvKey& key) {
      std::vector<std::string> parts;
      parts.reserve(key.size());
      for (const rt::KvKeyPart& part : key) parts.push_back(part.toString());
      return rt::join(parts, std::string(1, '\0'));
    }
  }

  rt::Task<void> releaseLegacyReservation(rt::Kv& kv, rt::KvKey key, SentinelReplayLegacyReservation reservation, double budgetBytes) {
    if (reservation.state == "published") {
      co_await kv.delete_(key);
      co_return;
    }
    auto deleting = replay_retention_schema::deleteChunkBatch(kv, reservation.capture_id, EVICTION_MAX_CHUNK_DELETES);
    const replay_retention_schema::DeleteChunkBatchResult cleanup = co_await std::move(deleting);
    if (cleanup.remaining > 0) co_return;
    const rt::KvEntryMaybe<> entry = co_await kv.get(key);
    if (!isLegacyReservation(entry.value)) co_return;
    const double bytes = reservation.bytes;
    replay_retention_schema::LedgerMutation mutate = [bytes](const SentinelReplayBudgetLedger& ledger) {
      SentinelReplayBudgetLedger next = ledger;
      next.reserved_bytes = rt::Math::max(0, ledger.reserved_bytes - bytes);
      return next;
    };
    replay_retention_schema::AtomicRowApplication applyRow = [key](rt::AtomicOperation& operation) -> rt::AtomicOperation& { return operation.delete_(key); };
    auto committing = replay_retention_schema::commitAccountingMutation(kv, key, entry.versionstamp, std::move(mutate), std::move(applyRow), budgetBytes);
    co_await std::move(committing);
  }

  namespace {
    /**
     * Prefixes in capture-owned key order. Built lazily: replay-store.ts imports the
     * retention modules, so a top-level array here would read model or store
     * bindings before that module body has initialized them. The order is
     * significant — "accounting" sorts before "manifest", so the accounting sweep is
     * always complete before a manifest is checked for an existing accounting row.
     */
    // The prefixes are the exported constant objects themselves: the TS tells them apart by identity.
    using PrefixList = std::array<const rt::KvKey*, 7>;
    PrefixList bootstrapPrefixes() {
      return {
        &SENTINEL_REPLAY_ACCOUNTING_PREFIX,
        &SENTINEL_REPLAY_CHUNK_PREFIX,
        &SENTINEL_REPLAY_DEDUPE_PREFIX,
        &SENTINEL_REPLAY_EVICTION_PREFIX,
        &SENTINEL_REPLAY_MANIFEST_PREFIX,
        &SENTINEL_REPLAY_REQUEST_PREFIX,
        &SENTINEL_REPLAY_RESERVATION_PREFIX,
      };
    }

    // `prefixes[index]`, undefined (nullptr) outside the list.
    const rt::KvKey* prefixAt(const PrefixList& prefixes, double index) {
      if (!(index >= 0 && index < static_cast<double>(prefixes.size()))) return nullptr;
      return prefixes[static_cast<std::size_t>(index)];
    }

    // `prefixes.indexOf(prefix)`
    double indexOfPrefix(const PrefixList& prefixes, const rt::KvKey* prefix) {
      for (std::size_t index = 0; index < prefixes.size(); ++index) {
        if (prefixes[index] == prefix) return static_cast<double>(index);
      }
      return -1;
    }

    struct BootstrapMaterialization {
      rt::KvKey accounting_key;
      rt::KvKey manifest_key;
      std::string manifest_versionstamp;
      SentinelReplayAccountingRow row;
    };

    struct BootstrapDelta {
      double stored_bytes;
      double reserved_bytes;
      double records;
      double status_records;
      double metadata_bytes;
      bool invalid;
      double legacy_reaped;
      std::vector<BootstrapMaterialization> materializations;
    };

    // `Partial<BootstrapDelta>`
    struct BootstrapDeltaPatch {
      std::optional<double> stored_bytes{};
      std::optional<double> reserved_bytes{};
      std::optional<double> records{};
      std::optional<double> status_records{};
      std::optional<double> metadata_bytes{};
      std::optional<bool> invalid{};
      std::optional<double> legacy_reaped{};
      std::optional<std::vector<BootstrapMaterialization>> materializations{};
    };

    BootstrapDelta emptyDelta() {
      return {
        .stored_bytes = 0,
        .reserved_bytes = 0,
        .records = 0,
        .status_records = 0,
        .metadata_bytes = 0,
        .invalid = false,
        .legacy_reaped = 0,
        .materializations = {},
      };
    }

    BootstrapDelta addDelta(const BootstrapDelta& delta, const BootstrapDeltaPatch& patch) {
      std::vector<BootstrapMaterialization> materializations = delta.materializations;
      if (patch.materializations) materializations.insert(materializations.end(), patch.materializations->begin(), patch.materializations->end());
      return {
        .stored_bytes = delta.stored_bytes + patch.stored_bytes.value_or(0),
        .reserved_bytes = delta.reserved_bytes + patch.reserved_bytes.value_or(0),
        .records = delta.records + patch.records.value_or(0),
        .status_records = delta.status_records + patch.status_records.value_or(0),
        .metadata_bytes = delta.metadata_bytes + patch.metadata_bytes.value_or(0),
        .invalid = delta.invalid || patch.invalid.value_or(false),
        .legacy_reaped = delta.legacy_reaped + patch.legacy_reaped.value_or(0),
        .materializations = std::move(materializations),
      };
    }

    // `Readonly<{ index: number; inner: string | null }>`
    struct ParsedCursor {
      double index;
      std::optional<std::string> inner;
    };

    ParsedCursor parseCursor(const std::optional<std::string>& cursor) {
      if (!cursor) return {.index = 0, .inner = std::nullopt};
      const std::int64_t separator = rt::indexOf(*cursor, "|");
      if (separator < 0) return {.index = 0, .inner = std::nullopt};
      const double index = rt::parseInt(rt::slice(*cursor, 0, separator), 10);
      std::string inner = rt::slice(*cursor, separator + 1);
      return {
        .index = rt::Number::isSafeInteger(index) && index >= 0 ? index : 0,
        .inner = inner.empty() ? std::nullopt : std::optional<std::string>(std::move(inner)),
      };
    }

    /** Accounting rows own the durable charge, so the sweep counts them directly. */
    BootstrapDelta bootstrapAccountingDelta(const rt::KvKey& key, const rt::KvValue& value, const BootstrapDelta& delta) {
      if (!isAccountingRow(value)) return addDelta(delta, {.invalid = true});
      const SentinelReplayAccountingRow row = accountingRowOf(value);
      if (!accountingKeyMatches(key, row)) return addDelta(delta, {.invalid = true});
      if (row.state == "stored" || row.state == "evicting") return addDelta(delta, {.stored_bytes = row.bytes, .records = 1});
      return addDelta(delta, {.reserved_bytes = row.bytes});
    }

    /** A legacy manifest gains its durable charge owner in the same commit as the bootstrap ledger delta. */
    rt::Task<BootstrapDelta> bootstrapManifestDelta(rt::Kv& kv, rt::KvEntry<> entry, BootstrapDelta delta) {
      const rt::KvKey& key = entry.key;
      const rt::KvValue& value = entry.value;
      if (!replay_read::isSentinelReplayManifest(value)) co_return addDelta(delta, {.invalid = true});
      const SentinelReplayManifest manifest = replay_read::toSentinelReplayManifest(value);
      if (!manifestKeyMatches(key, manifest)) co_return addDelta(delta, {.invalid = true});
      const rt::KvKey accountingKey = sentinelReplayAccountingKey(manifest.captured_at_ms, manifest.capture_id);
      const rt::KvEntryMaybe<> accounting = co_await kv.get(accountingKey);
      if (!accounting.value.isNull()) {
        const bool matches = isAccountingRow(accounting.value) && accountingKeyMatches(accountingKey, accountingRowOf(accounting.value)) &&
                             accounting.value["fingerprint"].asString() == manifest.fingerprint;
        co_return matches ? delta : addDelta(delta, {.invalid = true});
      }
      const std::string accountingText = joinedKey(accountingKey);
      const BootstrapMaterialization* pending = nullptr;
      for (const BootstrapMaterialization& candidate : delta.materializations) {
        if (joinedKey(candidate.accounting_key) == accountingText) {
          pending = &candidate;
          break;
        }
      }
      if (pending) co_return pending->row.fingerprint == manifest.fingerprint ? delta : addDelta(delta, {.invalid = true});
      const double bytes = sentinelReplayManifestCharge(manifest);
      SentinelReplayAccountingRow row{
        .version = 1,
        .capture_id = manifest.capture_id,
        .request_id = manifest.request_id.value_or(""),
        .fingerprint = manifest.fingerprint,
        .bytes = bytes,
        .state = "stored",
        .fence = 1,
        .created_at_ms = manifest.captured_at_ms,
        .expires_at_ms = manifest.expires_at_ms,
        .status_expires_at_ms = manifest.captured_at_ms + SENTINEL_REPLAY_STATUS_TTL_MS,
        .stage = 0,
      };
      std::vector<BootstrapMaterialization> materializations;
      materializations.push_back(
        BootstrapMaterialization{.accounting_key = accountingKey, .manifest_key = key, .manifest_versionstamp = entry.versionstamp, .row = std::move(row)});
      co_return addDelta(delta, {.stored_bytes = bytes, .records = 1, .materializations = std::move(materializations)});
    }

    /** Request status rows are capture-owned metadata and consume the metadata reserve. */
    BootstrapDelta bootstrapStatusDelta(const rt::KvValue& value, const BootstrapDelta& delta) {
      if (!replay_observation::isSentinelReplayCaptureStatusRow(value)) return addDelta(delta, {.invalid = true});
      return addDelta(delta, {.status_records = 1, .metadata_bytes = sentinelReplayStatusMetadataBytes(value)});
    }

    /** Eviction and expiry tombstones are capture-owned metadata too. */
    BootstrapDelta bootstrapTombstoneDelta(const rt::KvValue& value, const BootstrapDelta& delta) {
      // `value as { fingerprint?: unknown; reason?: unknown } | null`
      if (!isObjectLike(value) || !value["fingerprint"].isString() || !value["reason"].isString()) {
        return addDelta(delta, {.invalid = true});
      }
      return addDelta(delta, {.status_records = 1, .metadata_bytes = sentinelReplayStatusMetadataBytes(value)});
    }

    /** Legacy reservation rows are read only to release their own charge once. */
    rt::Task<BootstrapDelta> bootstrapLegacyDelta(rt::Kv& kv, rt::KvKey key, rt::KvValue value, BootstrapDelta delta, double budgetBytes) {
      if (!isLegacyReservation(value)) co_return addDelta(delta, {.invalid = true});
      auto releasing = releaseLegacyReservation(kv, key, toSentinelReplayLegacyReservation(value), budgetBytes);
      co_await std::move(releasing);
      co_return addDelta(delta, {.legacy_reaped = 1});
    }

    /**
     * Capture ids of every valid accounting row and manifest, for one sweep. While
     * the ledger is incomplete no capture gains its first owner: admission requires
     * completed accounting, and publication and this sweep's manifest phase add
     * rows only for a capture that already has one. Each sweep carries the random
     * id its first batch stamped (`bootstrap_sweep`), so an index filled at any
     * point of a sweep holds every owner that existed when a later batch of that
     * same sweep read its ledger, whichever cursor either was at, and it never
     * serves another sweep, not even one started after the ledger was deleted. No
     * index is built for an id until a ledger carries it: a batch that reaches the
     * chunk phase while stamping a new id commits the id first, and then runs one
     * more pass, so a small store still finishes in one call and the stamping
     * commit never waits on a fill.
     *
     * A batch can still outlive that read: another caller may finish the sweep, and
     * an admission may then reserve and stage a capture this index never saw. Every
     * commit that creates an owner (admission, publication, materialization) also
     * writes the ledger, and an ownerless chunk is deleted only in a commit that
     * checks the ledger versionstamp the batch read, so such a delete fails instead.
     * The same check makes the batch's own ledger commit fail, so the failed delete
     * never persists as an invalid row.
     *
     * A handle fills at most one index at a time, in the background: one slice of
     * BOOTSTRAP_OWNER_SCAN_ENTRIES rows after another, from the first batch that
     * needs it until both owner prefixes are read, yielding between slices and
     * stopping if the ledger no longer carries its unfinished sweep. Progress
     * depends neither on further requests nor on any batch's commit, and concurrent
     * batches share it instead of each starting their own. Inference-path batches
     * start the fill without waiting: on the VPS, replay tasks can precede client-visible
     * failure responses. Maintenance and admin batches wait for it at most
     * BOOTSTRAP_OWNER_INDEX_WAIT_MS on every call while it fills. A batch whose wait
     * expires judges no chunk and writes no ledger. The index lives in memory, so a
     * restarted process starts one new fill. A complete index is only read.
     *
     * Restoring an older copy of the ledger alone, rather than of the whole store,
     * brings back a sweep id a running process may still hold an index for, so
     * restart every process after such a restore. Without a restart, a retired
     * stand-in can also stall that restored sweep indefinitely, refusing admissions.
     */
    // One mutable index shared by the map, the fill and every batch that judges with it; the TS
    // compares it by identity (`chunkOwnerIndexes.get(kv) === index`).
    struct ChunkOwnerIndex {
      const std::string sweep;
      /** A stand-in for a sweep whose chunk phase a commit on this handle has passed: it never fills. */
      const bool retired;
      double prefix;
      std::optional<std::string> scan;
      std::unordered_set<std::string> owners;
      rt::Promise<void> filled;
      // `failure: Readonly<{ error: unknown }> | null`
      std::optional<rt::Reason> failure;
      std::size_t ownerBytes = 0;
      rt::memory::ScopedCache memory{rt::memory::Cache::sentinel_replay_owners};
    };
    using ChunkOwnerIndexHandle = std::shared_ptr<ChunkOwnerIndex>;

    // `const chunkOwnerIndexes = new WeakMap<Kv, ChunkOwnerIndex>()`
    rt::WeakMap<rt::Kv, ChunkOwnerIndexHandle>& chunkOwnerIndexes() {
      static rt::WeakMap<rt::Kv, ChunkOwnerIndexHandle> indexes;
      return indexes;
    }

    // `chunkOwnerIndexes.get(kv)`; a null handle is undefined.
    ChunkOwnerIndexHandle heldIndex(const rt::Kv& kv) {
      const ChunkOwnerIndexHandle* held = chunkOwnerIndexes().find(kv);
      return held ? *held : nullptr;
    }

    using OwnerPrefixList = std::array<const rt::KvKey*, 2>;
    OwnerPrefixList ownerIndexPrefixes() { return {&SENTINEL_REPLAY_ACCOUNTING_PREFIX, &SENTINEL_REPLAY_MANIFEST_PREFIX}; }
    bool isOwnerIndexComplete(const ChunkOwnerIndex& index) { return index.prefix >= static_cast<double>(ownerIndexPrefixes().size()); }
    ChunkOwnerIndexHandle emptyChunkOwnerIndex(std::string sweep, bool retired) {
      return std::make_shared<ChunkOwnerIndex>(ChunkOwnerIndex{
        .sweep = std::move(sweep),
        .retired = retired,
        .prefix = 0,
        .scan = std::nullopt,
        .owners = {},
        .filled = rt::Promise<void>::resolve(),
        .failure = std::nullopt,
      });
    }
  }

  void forgetChunkOwnerIndex(const rt::Kv& kv) { chunkOwnerIndexes().delete_(kv); }

  namespace {
    // `chunkOwnerIndexes.get(kv) === index` for a fill running in the background: false once the
    // handle it was started on is gone (the TS closure would keep that handle alive).
    bool holds(const rt::Kv& kv, const std::weak_ptr<const void>& alive, const ChunkOwnerIndexHandle& index) {
      return !alive.expired() && heldIndex(kv) == index;
    }

    std::optional<std::string> ownerCaptureId(const rt::KvKey* prefix, const rt::KvEntry<>& entry) {
      if (prefix == &SENTINEL_REPLAY_ACCOUNTING_PREFIX) {
        if (!isAccountingRow(entry.value)) return std::nullopt;
        const SentinelReplayAccountingRow row = accountingRowOf(entry.value);
        return accountingKeyMatches(entry.key, row) ? std::optional<std::string>(row.capture_id) : std::nullopt;
      }
      if (!replay_read::isSentinelReplayManifest(entry.value)) return std::nullopt;
      const SentinelReplayManifest manifest = replay_read::toSentinelReplayManifest(entry.value);
      return manifestKeyMatches(entry.key, manifest) ? std::optional<std::string>(manifest.capture_id) : std::nullopt;
    }

    /** Read at most BOOTSTRAP_OWNER_SCAN_ENTRIES owner rows; true once both owner prefixes are indexed. */
    rt::Task<bool> advanceChunkOwnerIndex(rt::Kv& kv, std::weak_ptr<const void> alive, ChunkOwnerIndexHandle index) {
      const OwnerPrefixList ownerPrefixes = ownerIndexPrefixes();
      double budget = BOOTSTRAP_OWNER_SCAN_ENTRIES;
      while (index->prefix < static_cast<double>(ownerPrefixes.size()) && budget > 0) {
        if (alive.expired()) co_return false;
        const rt::KvKey* prefix = ownerPrefixes[static_cast<std::size_t>(index->prefix)];
        rt::KvListSelector selector;
        selector.prefix = *prefix;
        rt::KvListOptions options;
        options.limit = budget;
        options.cursor = index->scan;
        const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector), std::move(options));
        double read = 0;
        while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
          read += 1;
          const std::optional<std::string> owner = ownerCaptureId(prefix, *entry);
          if (owner && index->owners.insert(*owner).second) index->ownerBytes += sizeof(*owner) + rt::memory::bytes(*owner);
        }
        if (read < budget) {
          index->prefix += 1;
          index->scan = std::nullopt;
        } else {
          index->scan = iterator->cursor();
        }
        budget -= read;
      }
      index->memory.set(index->owners.size(), index->ownerBytes + index->owners.bucket_count() * sizeof(void*));
      co_return index->prefix >= static_cast<double>(ownerPrefixes.size());
    }

    /** The id of the sweep in flight, or null for a sweep not started yet or one stamped before sweeps had ids. */
    std::optional<std::string> durableSweep(const SentinelReplayBudgetLedger& ledger) {
      if (!ledger.bootstrap_cursor) return std::nullopt;
      return ledger.bootstrap_sweep.isNullish() ? std::nullopt : std::optional<std::string>(*ledger.bootstrap_sweep);
    }

    /** Paced slices, only while this handle and the durable unfinished sweep still need the index. */
    rt::Task<void> fillChunkOwnerIndex(rt::Kv& kv, std::weak_ptr<const void> alive, ChunkOwnerIndexHandle index, double budgetBytes) {
      while (holds(kv, alive, index)) {
        const LedgerState state = co_await readLedger(kv, budgetBytes);
        if (!holds(kv, alive, index)) co_return;
        if (state.kind != LedgerStateKind::ok || state.ledger->bootstrap_complete || durableSweep(*state.ledger) != index->sweep) {
          forgetChunkOwnerIndex(kv);
          co_return;
        }
        auto advancing = advanceChunkOwnerIndex(kv, alive, index);
        const bool complete = co_await std::move(advancing);
        if (complete) co_return;
        // Yield the main thread and storage between bounded slices, even without more requests.
        co_await rt::sleep(1);
      }
    }

    // `fillChunkOwnerIndex(kv, index, budgetBytes).catch((error) => { ... })`: the promise kept as
    // `filled`. `.catch` settles the promise it returns one turn after the fill settles, on either
    // path, and runs its callback in that turn.
    rt::Task<void> fillChunkOwnerIndexCaught(rt::Kv& kv, std::weak_ptr<const void> alive, ChunkOwnerIndexHandle index, double budgetBytes) {
      rt::Reason failure;
      try {
        auto filling = fillChunkOwnerIndex(kv, alive, index, budgetBytes);
        co_await std::move(filling);
      } catch (...) {
        failure = std::current_exception();
      }
      co_await rt::microtask();
      if (!failure) co_return;
      index->failure = failure;
      if (holds(kv, alive, index)) chunkOwnerIndexes().delete_(kv);
    }

    /**
     * Hold a new index for this sweep, replacing one held for another, and start
     * filling it. A failed fill is dropped, so the next batch that needs an index
     * starts another, and the batches waiting on it see the error.
     */
    ChunkOwnerIndexHandle startChunkOwnerIndex(rt::Kv& kv, const std::string& sweep, double budgetBytes) {
      ChunkOwnerIndexHandle index = emptyChunkOwnerIndex(sweep, false);
      chunkOwnerIndexes().set(kv, index);
      index->filled = rt::Promise<void>(fillChunkOwnerIndexCaught(kv, kv.weakIdentity(), index, budgetBytes));
      return index;
    }

    /** Resolve once `settled` does or `ms` have passed, whichever is first. */
    rt::Task<void> settledWithin(rt::Promise<void> settled, double ms) {
      const rt::Resolvers<void> elapsed = rt::withResolvers<void>();
      const rt::Resolve<void> resolve = elapsed.resolve;
      const rt::Timeout timer = rt::setTimeout([resolve] { resolve(); }, ms);
      std::vector<rt::Promise<void>> racers{settled, elapsed.promise};
      rt::Reason failure;
      try {
        co_await rt::race(std::move(racers)).wait();
      } catch (...) {
        failure = std::current_exception();
      }
      rt::clearTimeout(timer);
      elapsed.resolve();
      if (failure) std::rethrow_exception(failure);
    }

    rt::Task<bool> chunkRemains(rt::Kv& kv, std::optional<std::string> cursor) {
      rt::KvListSelector selector;
      selector.prefix = SENTINEL_REPLAY_CHUNK_PREFIX;
      rt::KvListOptions options;
      options.limit = 1;
      options.cursor = std::move(cursor);
      const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector), std::move(options));
      const std::optional<rt::KvEntry<>> next = co_await iterator->next();
      co_return next.has_value();
    }

    // `Readonly<{ ready: boolean; index: ChunkOwnerIndex | null }>`; a null handle is null.
    struct PreparedChunkOwners {
      bool ready;
      ChunkOwnerIndexHandle index;
    };

    /**
     * The owner index a chunk page is judged against. A chunk prefix with nothing
     * left needs none. Otherwise the page starts the sweep's index and, if allowed,
     * waits for it. The page is not ready while the sweep's id is not durable yet,
     * while that index is still filling after the bounded wait, or when the sweep's
     * chunk phase is already past on this handle.
     */
    rt::Task<PreparedChunkOwners> prepareChunkOwners(rt::Kv& kv, std::optional<std::string> sweep, std::optional<std::string> cursor, double budgetBytes,
                                                     double ownerIndexWaitMs) {
      const ChunkOwnerIndexHandle held = heldIndex(kv);
      if (sweep && held && held->sweep == *sweep && isOwnerIndexComplete(*held)) co_return PreparedChunkOwners{.ready = true, .index = held};
      auto probing = chunkRemains(kv, cursor);
      const bool remains = co_await std::move(probing);
      if (!remains) co_return PreparedChunkOwners{.ready = true, .index = nullptr};
      if (!sweep) co_return PreparedChunkOwners{.ready = false, .index = nullptr};
      const ChunkOwnerIndexHandle current = heldIndex(kv);
      const ChunkOwnerIndexHandle index = current && current->sweep == *sweep ? current : startChunkOwnerIndex(kv, *sweep, budgetBytes);
      if (ownerIndexWaitMs > 0 && !index->retired && !isOwnerIndexComplete(*index)) {
        auto waiting = settledWithin(index->filled, ownerIndexWaitMs);
        co_await std::move(waiting);
      }
      if (index->failure) rt::rethrow(*index->failure);
      if (isOwnerIndexComplete(*index)) co_return PreparedChunkOwners{.ready = true, .index = index};
      co_return PreparedChunkOwners{.ready = false, .index = nullptr};
    }

    /**
     * Reclaim staged chunks that have no durable manifest or accounting owner. A
     * null index means no chunk remained when this pass began, so a chunk seen now
     * was staged since by a capture whose reservation already owns it. The delete
     * also checks the ledger versionstamp this batch read: any owner created since
     * wrote the ledger, so the delete fails rather than reclaim a live payload. A
     * chunk that another batch sharing the index already reclaimed is gone, not
     * invalid.
     */
    rt::Task<BootstrapDelta> bootstrapChunkDelta(rt::Kv& kv, rt::KvEntry<> entry, BootstrapDelta delta, ChunkOwnerIndexHandle owners,
                                                 std::optional<std::string> ledgerVersionstamp) {
      const rt::KvKey& key = entry.key;
      if (key.size() != SENTINEL_REPLAY_CHUNK_PREFIX.size() + 2 || !key[4].isString() || !counter(key[5])) co_return addDelta(delta, {.invalid = true});
      if (!owners || owners->owners.contains(key[4].string())) co_return delta;
      fault("delete");
      const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(rt::AtomicCheck(key, entry.versionstamp)).check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerVersionstamp)).delete_(key);
      const rt::KvCommitResult deleted = co_await operation->commit();
      if (deleted.ok) co_return delta;
      const rt::KvEntryMaybe<> current = co_await kv.get(key);
      co_return !current.versionstamp ? delta : addDelta(delta, {.invalid = true});
    }

    /** A dedupe row must reference a well-formed manifest key. */
    BootstrapDelta bootstrapDedupeDelta(const rt::KvValue& value, const BootstrapDelta& delta) {
      // `value as { manifest_key?: unknown } | null`
      if (!isObjectLike(value) || !value["manifest_key"].isArray() || value["manifest_key"].asArray().size() != 7) {
        return addDelta(delta, {.invalid = true});
      }
      return delta;
    }

    /**
     * Fold one scanned in-scope row into the running bootstrap delta. Rows that do
     * not match their own prefix's contract are counted as invalid rather than
     * trusted or deleted on a guess, which keeps discovery fail-closed.
     */
    rt::Task<BootstrapDelta> bootstrapEntryDelta(rt::Kv& kv, const rt::KvKey* prefix, rt::KvEntry<> entry, BootstrapDelta delta, double budgetBytes,
                                                 ChunkOwnerIndexHandle owners, std::optional<std::string> ledgerVersionstamp) {
      if (prefix == &SENTINEL_REPLAY_ACCOUNTING_PREFIX) co_return bootstrapAccountingDelta(entry.key, entry.value, delta);
      if (prefix == &SENTINEL_REPLAY_MANIFEST_PREFIX) {
        auto folding = bootstrapManifestDelta(kv, entry, delta);
        BootstrapDelta next = co_await std::move(folding);
        co_return next;
      }
      if (prefix == &SENTINEL_REPLAY_REQUEST_PREFIX) co_return bootstrapStatusDelta(entry.value, delta);
      if (prefix == &SENTINEL_REPLAY_EVICTION_PREFIX) co_return bootstrapTombstoneDelta(entry.value, delta);
      if (prefix == &SENTINEL_REPLAY_RESERVATION_PREFIX) {
        auto folding = bootstrapLegacyDelta(kv, entry.key, entry.value, delta, budgetBytes);
        BootstrapDelta next = co_await std::move(folding);
        co_return next;
      }
      if (prefix == &SENTINEL_REPLAY_CHUNK_PREFIX) {
        auto folding = bootstrapChunkDelta(kv, entry, delta, owners, ledgerVersionstamp);
        BootstrapDelta next = co_await std::move(folding);
        co_return next;
      }
      co_return bootstrapDedupeDelta(entry.value, delta);
    }

    /** Outcome of sweeping one bootstrap prefix within the remaining entry budget. */
    // `cursor: string | null`
    struct BootstrapSweep {
      BootstrapDelta delta;
      double scanned;
      std::optional<std::string> cursor;
      bool exhausted;
    };

    /**
     * Sweep one prefix up to `budget` entries. `exhausted` means the sweep consumed
     * its whole budget mid-prefix, or is still waiting for the chunk owner index,
     * so the caller must resume from `cursor` at the same prefix instead of advancing.
     */
    rt::Task<BootstrapSweep> sweepBootstrapPrefix(rt::Kv& kv, const rt::KvKey* prefix, std::optional<std::string> cursor, double budget, BootstrapDelta delta,
                                                  double budgetBytes, std::optional<std::string> sweep, std::optional<std::string> ledgerVersionstamp,
                                                  double ownerIndexWaitMs) {
      ChunkOwnerIndexHandle owners;
      if (prefix == &SENTINEL_REPLAY_CHUNK_PREFIX) {
        // Judge no chunk until every owner is indexed; the cursor stays here meanwhile.
        auto preparing = prepareChunkOwners(kv, sweep, cursor, budgetBytes, ownerIndexWaitMs);
        const PreparedChunkOwners prepared = co_await std::move(preparing);
        if (!prepared.ready) co_return BootstrapSweep{.delta = std::move(delta), .scanned = 0, .cursor = std::move(cursor), .exhausted = true};
        owners = prepared.index;
      }
      rt::KvListSelector selector;
      selector.prefix = *prefix;
      rt::KvListOptions options;
      options.limit = budget;
      options.cursor = cursor;
      const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector), std::move(options));
      double scanned = 0;
      for (;;) {
        std::optional<rt::KvEntry<>> entry = co_await iterator->next();
        if (!entry) break;
        scanned += 1;
        rt::Reason failure;
        try {
          auto folding = bootstrapEntryDelta(kv, prefix, std::move(*entry), delta, budgetBytes, owners, ledgerVersionstamp);
          delta = co_await std::move(folding);
        } catch (...) {
          failure = std::current_exception();
        }
        if (failure) {
          co_await iterator->return_();
          std::rethrow_exception(failure);
        }
        if (scanned >= budget) {
          std::string next = iterator->cursor();
          co_await iterator->return_();
          co_return BootstrapSweep{.delta = std::move(delta), .scanned = scanned, .cursor = std::move(next), .exhausted = true};
        }
      }
      co_return BootstrapSweep{.delta = std::move(delta), .scanned = scanned, .cursor = std::nullopt, .exhausted = false};
    }

    /**
     * After a committed batch: a finished sweep leaves no index held, and a sweep
     * past its chunk phase judges no chunk again, so its index gives way to a
     * stand-in that keeps a batch still at an older chunk cursor from refilling it.
     */
    void settleChunkOwnerIndex(rt::Kv& kv, const std::string& sweep, double phase) {
      const PrefixList prefixes = bootstrapPrefixes();
      if (phase >= static_cast<double>(prefixes.size())) {
        forgetChunkOwnerIndex(kv);
        return;
      }
      const ChunkOwnerIndexHandle held = heldIndex(kv);
      if (phase > indexOfPrefix(prefixes, &SENTINEL_REPLAY_CHUNK_PREFIX) && !(held && held->sweep == sweep && held->retired)) {
        chunkOwnerIndexes().set(kv, emptyChunkOwnerIndex(sweep, true));
      }
    }

    rt::KvValue ledgerValue(const SentinelReplayBudgetLedger& ledger) { return replay_retention_schema::sentinelReplayLedgerValue(ledger); }
    rt::KvValue accountingValue(const SentinelReplayAccountingRow& row) { return replay_retention_schema::sentinelReplayAccountingValue(row); }

    /**
     * Commit the batch's ledger and materialized accounting rows, each checked
     * against what the batch read. Returns the new ledger versionstamp, or null
     * when another write won.
     */
    rt::Task<std::optional<std::string>> commitBootstrapBatch(rt::Kv& kv, std::optional<std::string> versionstamp, BootstrapDelta delta,
                                                              SentinelReplayBudgetLedger next) {
      fault("commit");
      const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, versionstamp));
      for (const BootstrapMaterialization& materialization : delta.materializations) {
        operation->check(rt::AtomicCheck(materialization.accounting_key, std::nullopt))
          .check(rt::AtomicCheck(materialization.manifest_key, materialization.manifest_versionstamp))
          .set(materialization.accounting_key, accountingValue(materialization.row));
      }
      operation->set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue(next));
      const rt::KvCommitResult committed = co_await operation->commit();
      co_return committed.ok ? std::optional<std::string>(committed.versionstamp) : std::nullopt;
    }

    /** A batch that stamped a new id and stopped at the chunk phase, its entries unspent, did so only to make that id durable. */
    bool stampedBeforeChunks(const std::optional<std::string>& durable, double phase, double scanned) {
      return !durable && prefixAt(bootstrapPrefixes(), phase) == &SENTINEL_REPLAY_CHUNK_PREFIX && scanned < BOOTSTRAP_BATCH_ENTRIES;
    }

    // `Readonly<{ delta: BootstrapDelta; scanned: number; index: number; inner: string | null }>`
    struct BootstrapBatch {
      BootstrapDelta delta;
      double scanned;
      double index;
      std::optional<std::string> inner;
    };

    /** Sweep from the cursor, prefix by prefix, until the batch's entries are spent, a prefix is left mid-way, or every prefix is done. */
    rt::Task<BootstrapBatch> sweepBootstrapBatch(rt::Kv& kv, std::optional<std::string> from, std::optional<std::string> sweep,
                                                 std::optional<std::string> versionstamp, double budgetBytes, double ownerIndexWaitMs) {
      const ParsedCursor parsed = parseCursor(from);
      double index = parsed.index;
      std::optional<std::string> inner = parsed.inner;
      const PrefixList prefixes = bootstrapPrefixes();
      BootstrapDelta delta = emptyDelta();
      double scanned = 0;
      while (index < static_cast<double>(prefixes.size()) && scanned < BOOTSTRAP_BATCH_ENTRIES) {
        auto sweeping = sweepBootstrapPrefix(kv, prefixAt(prefixes, index), inner, BOOTSTRAP_BATCH_ENTRIES - scanned, delta, budgetBytes, sweep, versionstamp,
                                             ownerIndexWaitMs);
        BootstrapSweep swept = co_await std::move(sweeping);
        delta = std::move(swept.delta);
        scanned += swept.scanned;
        if (swept.exhausted) {
          // Keep this prefix and its cursor: the next pass resumes exactly here.
          co_return BootstrapBatch{.delta = std::move(delta), .scanned = scanned, .index = index, .inner = std::move(swept.cursor)};
        }
        index += 1;
        inner = std::nullopt;
      }
      co_return BootstrapBatch{.delta = std::move(delta), .scanned = scanned, .index = index, .inner = std::move(inner)};
    }
  }

  rt::Task<SentinelReplayBudgetLedger> runBootstrapBatch(rt::Kv& kv, SentinelReplayBudgetLedger ledger, std::optional<std::string> versionstamp,
                                                         double budgetBytes, double nowMs) {
    auto running = runBootstrapBatch(kv, std::move(ledger), std::move(versionstamp), budgetBytes, nowMs, BOOTSTRAP_OWNER_INDEX_WAIT_MS);
    SentinelReplayBudgetLedger result = co_await std::move(running);
    co_return result;
  }

  rt::Task<SentinelReplayBudgetLedger> runBootstrapBatch(rt::Kv& kv, SentinelReplayBudgetLedger ledger, std::optional<std::string> versionstamp,
                                                         double budgetBytes, double nowMs, double ownerIndexWaitMs) {
    // A batch that starts a sweep, or continues one stamped before sweeps had
    // ids, stamps a new id, and judges no chunk until a ledger carries it.
    const std::optional<std::string> durable = durableSweep(ledger);
    // A fresh sweep clears the sticky error so a removed corrupt row can resolve it.
    std::optional<std::string> error = !ledger.bootstrap_cursor ? std::nullopt : ledger.accounting_error;
    // A failed full sweep retained its deltas. Rebuild live counters on restart;
    // cursor continuations still accumulate, and cumulative history stays intact.
    // (`live` is emptyDelta() or the ledger: only its five live counters are read.)
    const bool rebuild = !ledger.bootstrap_cursor && ledger.accounting_error;
    const double liveStoredBytes = rebuild ? 0 : ledger.stored_bytes;
    const double liveReservedBytes = rebuild ? 0 : ledger.reserved_bytes;
    const double liveRecords = rebuild ? 0 : ledger.records;
    const double liveStatusRecords = rebuild ? 0 : ledger.status_records;
    const double liveMetadataBytes = rebuild ? 0 : ledger.metadata_bytes;
    auto sweeping = sweepBootstrapBatch(kv, ledger.bootstrap_cursor, durable, versionstamp, budgetBytes, ownerIndexWaitMs);
    const BootstrapBatch batch = co_await std::move(sweeping);
    const bool complete = batch.index >= static_cast<double>(bootstrapPrefixes().size());
    const std::optional<std::string> cursor =
      complete ? std::nullopt : std::optional<std::string>(rt::numberToString(batch.index) + "|" + batch.inner.value_or(""));
    // Still waiting for its sweep's owner index, the batch scanned and moved
    // nothing, so it writes no ledger that could cost a judging batch its commit.
    if (batch.scanned == 0 && durable && cursor == ledger.bootstrap_cursor) co_return ledger;
    if (batch.delta.invalid) error = "invalid_in_scope_row";
    const std::string sweep = durable ? *durable : rt::crypto::randomUUID();
    const double storedBytes = liveStoredBytes + batch.delta.stored_bytes;
    SentinelReplayBudgetLedger next = ledger;
    next.budget_bytes = budgetBytes;
    next.stored_bytes = storedBytes;
    next.reserved_bytes = liveReservedBytes + batch.delta.reserved_bytes;
    next.records = liveRecords + batch.delta.records;
    next.status_records = liveStatusRecords + batch.delta.status_records;
    next.metadata_bytes = liveMetadataBytes + batch.delta.metadata_bytes;
    next.bootstrap_complete = complete && !error;
    next.bootstrap_cursor = cursor;
    next.bootstrap_sweep = sweep;
    next.accounting_error = error;
    next.over_budget = storedBytes > sentinelReplayPayloadBudgetBytes(budgetBytes);
    next.last_warning_at_ms = storedBytes > sentinelReplayPayloadBudgetBytes(budgetBytes) ? std::optional<double>(nowMs) : ledger.last_warning_at_ms;
    auto committing = commitBootstrapBatch(kv, versionstamp, batch.delta, next);
    const std::optional<std::string> committed = co_await std::move(committing);
    if (committed) {
      settleChunkOwnerIndex(kv, sweep, batch.index);
      // With the id now durable, one more pass may judge chunks in this same call.
      if (stampedBeforeChunks(durable, batch.index, batch.scanned)) {
        auto again = runBootstrapBatch(kv, next, committed, budgetBytes, nowMs, ownerIndexWaitMs);
        SentinelReplayBudgetLedger result = co_await std::move(again);
        co_return result;
      }
      co_return next;
    }
    // Another write won. The owner index belongs to the sweep, not to this batch, so it stays as it was.
    const LedgerState reread = co_await readLedger(kv, budgetBytes);
    if (reread.kind == LedgerStateKind::corrupt) co_return ledger;
    co_return withBudget(*reread.ledger, budgetBytes);
  }

  rt::Task<std::optional<SentinelReplayBudgetLedger>> bootstrapBeforeMutation(rt::Kv& kv, double budgetBytes, double nowMs) {
    auto bootstrapping = bootstrapBeforeMutation(kv, budgetBytes, nowMs, BOOTSTRAP_OWNER_INDEX_WAIT_MS);
    std::optional<SentinelReplayBudgetLedger> result = co_await std::move(bootstrapping);
    co_return result;
  }

  rt::Task<std::optional<SentinelReplayBudgetLedger>> bootstrapBeforeMutation(rt::Kv& kv, double budgetBytes, double nowMs, double ownerIndexWaitMs) {
    const LedgerState state = co_await readLedger(kv, budgetBytes);
    if (state.kind == LedgerStateKind::corrupt) co_return std::nullopt;
    const SentinelReplayBudgetLedger ledger = withBudget(*state.ledger, budgetBytes);
    if (ledger.bootstrap_complete) {
      // Admissions may create owners from here on, so no earlier index can be trusted again.
      forgetChunkOwnerIndex(kv);
      co_return ledger;
    }
    auto running = runBootstrapBatch(kv, ledger, state.versionstamp, budgetBytes, nowMs, ownerIndexWaitMs);
    const SentinelReplayBudgetLedger bootstrapped = co_await std::move(running);
    if (bootstrapped.bootstrap_complete) co_return bootstrapped;
    co_return std::nullopt;
  }

  SentinelReplayLegacyReservation toSentinelReplayLegacyReservation(const rt::KvValue& value) {
    const rt::KvValue& requestId = value["request_id"];
    const rt::KvValue& createdAtMs = value["created_at_ms"];
    const rt::KvValue& expiresAtMs = value["expires_at_ms"];
    constexpr double absent = std::numeric_limits<double>::quiet_NaN();
    return {
      .version = value["version"].asNumber(),
      .capture_id = value["capture_id"].asString(),
      .request_id = requestId.isString() ? requestId.asString() : std::string(),
      .bytes = value["bytes"].asNumber(),
      .fence = value["fence"].asNumber(),
      .state = value["state"].asString(),
      .created_at_ms = createdAtMs.isNumber() ? createdAtMs.asNumber() : absent,
      .expires_at_ms = expiresAtMs.isNumber() ? expiresAtMs.asNumber() : absent,
    };
  }
}
