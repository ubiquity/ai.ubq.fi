#pragma once
// Port of src/sentinel/replay-retention.ts.
/**
 * Durable, concurrency-safe retention accounting for capture-owned data.
 *
 * One fixed per-host budget covers the encoded KV payload this feature retains
 * (base64-expanded ciphertext chunks, metadata, status/dedupe/index row
 * overhead) plus in-progress reservations. It is not a bound on the shared
 * SQLite database, WAL or unrelated namespaces.
 *
 * Source of truth: one durable accounting row per capture under
 * ["uos_ai","sentinel_replay","v1","accounting", captured_at_ms, capture_id].
 * The timestamp-first key order IS the oldest-first index; there is no second
 * index namespace. Accounting rows carry NO `expireIn`: they are the durable
 * record of retained bytes and a row stops existing only through one atomic
 * commit that deletes it and decrements the ledger in the same operation.
 *
 * The ledger under SENTINEL_REPLAY_BUDGET_LEDGER_KEY is derived cached state and
 * is never the source of truth. Every accounting mutation (admit, publish,
 * revoke, release, evict, expire, status admit/prune) is ONE kv.atomic() commit
 * that checks the exact versionstamp of BOTH the accounting/status row it
 * mutates AND the ledger, and writes both. A ledger delta is never applied in a
 * commit separate from the row change it describes.
 *
 * State machine:
 *   reserved --publish--> stored --evict--> (row deleted, ledger stored_bytes-=,
 *                                            records-=, evicted_*+=)
 *   reserved --revoke--> revoked --release--> (row deleted, reserved_bytes-=)
 *   stored --expire--> (row deleted, records-=, NO evicted_* increment)
 *
 * This module owns the runtime path only; the durable row/ledger schema lives in
 * replay-retention-schema.ts and the resumable pre-accounting sweep lives in
 * replay-retention-bootstrap.ts, so neither is duplicated here.
 *
 * Fencing: a writer advances `stage` through a CAS that requires
 * state==="reserved", its own fence and an unexpired row before each bounded
 * chunk batch; a reaper revoking the same row increments the fence, so a paused
 * writer can never advance again and can never publish.
 */
// Every function takes the store as `rt::Kv&` (as the sibling retention modules do); the store
// must outlive the returned task. Rows read back from KV are validated with the schema's
// validators and then read member by member, so an unrelated extra member of any kind is ignored
// as the TS ignores it; rewritten stored rows preserve those extras and their order through the
// schema storage helpers. The `Readonly<{...}>` inputs are the option structs below, members in TS order, `?:`
// members std::nullopt when undefined. The exported functions taking a struct are plain
// functions returning the task, so `co_await f(kv, options)` with a named `options` is safe under
// the GCC 12.2 coroutine rules (never pass a braced temporary inside the co_await).
#include "uos/sentinel/replay_retention_schema.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::sentinel::replay_retention {
  // `input: Readonly<{ key: KvKey; row: unknown; now_ms: number; ttl_ms: number; budget_bytes?: number }>`
  struct SentinelReplayStatusMetadataInput {
    rt::KvKey key;
    rt::KvValue row;
    double now_ms;
    double ttl_ms;
    std::optional<double> budget_bytes{};
  };

  /**
   * Admit one bounded status/tombstone row through a CAS on the ledger's
   * status_records/metadata_bytes counters. When either bound is exceeded the
   * oldest capture-owned status/tombstone rows are pruned first.
   */
  [[nodiscard]] rt::Task<bool> admitSentinelReplayStatusMetadata(rt::Kv& kv, const SentinelReplayStatusMetadataInput& input);

  /** Prune the oldest capture-owned status/tombstone rows until `extra` fits the bound. */
  // The overload without `budgetBytesOverride` (and std::nullopt) is undefined.
  [[nodiscard]] rt::Task<double> pruneCaptureOwnedStatusMetadata(rt::Kv& kv, double nowMs, double extraRecords, double extraBytes, double maxDeletes);
  [[nodiscard]] rt::Task<double> pruneCaptureOwnedStatusMetadata(rt::Kv& kv, double nowMs, double extraRecords, double extraBytes, double maxDeletes,
                                                                 std::optional<double> budgetBytesOverride);

  // `input: Readonly<{ accounting_key: KvKey; fence: number; now_ms: number; budget_bytes?: number }>`
  struct SentinelReplayStagingFenceInput {
    rt::KvKey accounting_key;
    double fence;
    double now_ms;
    std::optional<double> budget_bytes{};
  };

  // `Readonly<{ ok: true; stage: number; versionstamp: string }> | Readonly<{ ok: false; reason: "revoked" | "expired" | "missing" }>`
  struct SentinelReplayStagingFenceOk {
    static constexpr bool ok = true;
    double stage;
    std::string versionstamp;
    friend bool operator==(const SentinelReplayStagingFenceOk&, const SentinelReplayStagingFenceOk&) = default;
  };
  struct SentinelReplayStagingFenceNotOk {
    static constexpr bool ok = false;
    std::string reason;  // "revoked" | "expired" | "missing"
    friend bool operator==(const SentinelReplayStagingFenceNotOk&, const SentinelReplayStagingFenceNotOk&) = default;
  };
  using SentinelReplayStagingFence = std::variant<SentinelReplayStagingFenceOk, SentinelReplayStagingFenceNotOk>;

  /** Fenced staging: advance `stage` before a batch of chunks may be written. */
  [[nodiscard]] rt::Task<SentinelReplayStagingFence> advanceSentinelReplayStagingFence(rt::Kv& kv, const SentinelReplayStagingFenceInput& input);

  // `options` of prepareSentinelReplayPublication.
  struct SentinelReplayPublicationOptions {
    /** Frozen capture time used to keep published expiry aligned with the manifest. */
    double now_ms;
    /** Fresh wall-clock time used only to validate the reservation lease. */
    double lease_now_ms;
    rt::KvKey status_key;
    std::optional<double> status_bytes{};
    std::optional<double> budget_bytes{};
  };

  /**
   * Read the fresh ledger and accounting row for a publish attempt. Returns null
   * unless the row is still `reserved` at the writer's fence, unexpired, and the
   * actual charge fits both the row's own bound and the payload budget.
   */
  // std::nullopt is null.
  [[nodiscard]] rt::Task<std::optional<replay_retention_schema::SentinelReplayPublication>> prepareSentinelReplayPublication(
    rt::Kv& kv, const replay_retention_schema::SentinelReplayAccountingRow& accounting, const rt::KvKey& accountingKey, double actualCharge,
    const SentinelReplayPublicationOptions& options);

  // `options: Readonly<{ now_ms: number; max_chunk_deletes?: number; budget_bytes?: number }>`
  struct SentinelReplayAbandonOptions {
    double now_ms;
    std::optional<double> max_chunk_deletes{};
    std::optional<double> budget_bytes{};
  };

  // `Readonly<{ revoked: boolean; deleted_chunks: number; released: boolean; remaining: number }>`
  struct SentinelReplayAbandonment {
    bool revoked;
    double deleted_chunks;
    bool released;
    double remaining;
    friend bool operator==(const SentinelReplayAbandonment&, const SentinelReplayAbandonment&) = default;
  };

  /**
   * Revoke a reservation or resume an `evicting` row: fence it, delete its staged
   * chunks in bounded batches, and release the charge ONLY when a fresh list over
   * the chunk prefix returns zero entries. A partial deletion leaves the row
   * revoked/evicting with its charge held for a later pass.
   */
  [[nodiscard]] rt::Task<SentinelReplayAbandonment> abandonSentinelReplayAccounting(rt::Kv& kv,
                                                                                    const replay_retention_schema::SentinelReplayAccountingRow& accounting,
                                                                                    const rt::KvKey& accountingKey, const SentinelReplayAbandonOptions& options);

  // `options` of evictSentinelReplays.
  struct SentinelReplayEvictionOptions {
    double target_bytes;
    std::optional<double> target_records{};
    std::optional<double> max_records{};
    std::optional<double> max_chunk_deletes{};
    double now_ms;
    std::optional<double> budget_bytes{};
  };

  // `Readonly<{ records: number; bytes: number; chunks: number }>`
  struct SentinelReplayEviction {
    double records;
    double bytes;
    double chunks;
    friend bool operator==(const SentinelReplayEviction&, const SentinelReplayEviction&) = default;
  };

  /**
   * Evict oldest stored captures until the ledger reaches its byte and record
   * targets. Each victim is claimed by exactly one caller before anything is
   * deleted, and its charge is released only when its chunk prefix is proven empty.
   */
  [[nodiscard]] rt::Task<SentinelReplayEviction> evictSentinelReplays(rt::Kv& kv, const SentinelReplayEvictionOptions& options);

  // `options: Readonly<{ now_ms: number; max_records?: number; budget_bytes?: number }>`
  struct SentinelReplayReclaimOptions {
    double now_ms;
    std::optional<double> max_records{};
    std::optional<double> budget_bytes{};
  };

  // `Readonly<{ records: number; bytes: number }>`
  struct SentinelReplayReclamation {
    double records;
    double bytes;
    friend bool operator==(const SentinelReplayReclamation&, const SentinelReplayReclamation&) = default;
  };

  /**
   * TTL reclamation: accounting rows outlive their payload TTL, so a maintenance
   * pass claims rows whose payload already expired and releases the charge with
   * records-1 and NO evicted_* increment, reporting `expired`/`payload_expired`.
   */
  [[nodiscard]] rt::Task<SentinelReplayReclamation> reclaimExpiredSentinelReplays(rt::Kv& kv, const SentinelReplayReclaimOptions& options);

  // `input` of reserveSentinelReplayCapacity.
  struct SentinelReplayCapacityInput {
    std::string capture_id;
    std::string request_id;
    double plaintext_bytes;
    double metadata_bytes;
    std::string fingerprint;
    double now_ms;
    std::optional<double> budget_bytes{};
  };

  /**
   * Reserve capacity for one capture before any chunk is written: the accounting
   * row (state=reserved) and the ledger's reserved_bytes increment commit
   * together. Refuses with storage_full when bounded cleanup cannot admit it.
   */
  [[nodiscard]] rt::Task<replay_retention_schema::SentinelReplayAdmission> reserveSentinelReplayCapacity(rt::Kv& kv, const SentinelReplayCapacityInput& input);

  // `options: Readonly<{ now_ms: number; budget_bytes?: number }>`
  struct SentinelReplayMaintenanceOptions {
    double now_ms;
    std::optional<double> budget_bytes{};
  };

  /** Bounded maintenance: reap, bootstrap, reclaim expired payloads, evict if needed, and recount metadata when due. */
  [[nodiscard]] rt::Task<replay_retention_schema::SentinelReplayRetentionStatus> runSentinelReplayRetentionMaintenance(
    rt::Kv& kv, const SentinelReplayMaintenanceOptions& options);

  // `export { readSentinelReplayRetentionStatus } from "./replay-retention-schema.ts";`
  using replay_retention_schema::readSentinelReplayRetentionStatus;
}
