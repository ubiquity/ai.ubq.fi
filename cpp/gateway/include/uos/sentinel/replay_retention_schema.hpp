#pragma once
// Port of src/sentinel/replay-retention-schema.ts.
/**
 * Shared retention schema for capture-owned storage accounting.
 *
 * This module owns the durable row/ledger shapes, key builders, reason codes,
 * test seams, ledger IO and the single-commit mutation helper. Both the runtime
 * retention module and the resumable bootstrap module import it; it never
 * imports either of them, so the retention split has no cycles.
 *
 * The ledger under SENTINEL_REPLAY_BUDGET_LEDGER_KEY is derived cached state.
 * Durable per-capture accounting rows are the source of truth, and every
 * accounting mutation writes the row change and its ledger delta in ONE
 * kv.atomic() commit that checks the exact versionstamp of both.
 */
// Rows read back from KV are `unknown` in the TS: the validators take an rt::KvValue. The rows are
// structs with a storage-only backing object so TypeScript spreads preserve undeclared members and their order.
#include "uos/sentinel/replay_limits.hpp"
#include "uos/sentinel/replay_model.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::sentinel::replay_retention_schema {
  inline const rt::KvKey SENTINEL_REPLAY_BUDGET_LEDGER_KEY{"uos_ai", "sentinel_replay", "v1", "budget"};
  /** Durable per-capture accounting rows; the 5th component is the captured-at millisecond. */
  inline const rt::KvKey SENTINEL_REPLAY_ACCOUNTING_PREFIX{"uos_ai", "sentinel_replay", "v1", "accounting"};
  /** Legacy pre-accounting-row reservations: read only to release their charge once, then deleted. */
  inline const rt::KvKey SENTINEL_REPLAY_RESERVATION_PREFIX{"uos_ai", "sentinel_replay", "v1", "reservation"};
  /** Bounded eviction/expiry tombstones consulted when a manifest is gone. */
  inline const rt::KvKey SENTINEL_REPLAY_EVICTION_PREFIX{"uos_ai", "sentinel_replay", "v1", "evicted"};
  inline constexpr double SENTINEL_REPLAY_RESERVATION_TTL_MS = 120'000;
  inline constexpr std::string_view SENTINEL_REPLAY_EVICTION_REASON = "storage_budget";
  inline constexpr std::string_view SENTINEL_REPLAY_EXPIRED_REASON = "payload_expired";
  inline constexpr std::string_view SENTINEL_REPLAY_STORAGE_FULL_REASON = "storage_full";
  inline constexpr std::string_view SENTINEL_REPLAY_ACCOUNTING_REASON = "storage_accounting_in_progress";
  inline constexpr std::string_view SENTINEL_REPLAY_STATUS_NOT_RETAINED = "status_not_retained";
  inline constexpr double SENTINEL_REPLAY_CLEAN_TARGET_RATIO = 0.9;
  /** Chunks written per bounded, fence-guarded staging batch. */
  inline constexpr double SENTINEL_REPLAY_STAGING_BATCH_CHUNKS = 12;
  inline constexpr double EVICTION_BATCH_RECORDS = 16;
  inline constexpr double EVICTION_MAX_CHUNK_DELETES = 512;
  // One legacy materialization adds two CAS checks plus the ledger fence. Bun's
  // native atomic backend accepts at most ten checks, so four rows (nine checks)
  // is the largest portable batch.
  inline constexpr double BOOTSTRAP_BATCH_ENTRIES = 4;
  // Owner rows (accounting + manifest) one slice of the chunk owner index reads.
  // Fixed, so no single owner read grows with retained captures.
  inline constexpr double BOOTSTRAP_OWNER_SCAN_ENTRIES = 64;
  // Longest a maintenance/admin batch waits on each call for its sweep's
  // background owner fill before leaving the sweep in place for a later batch.
  // Inference-path reservation and status admission use zero instead: on the VPS,
  // replay tasks can be awaited before client-visible failure responses.
  inline constexpr double BOOTSTRAP_OWNER_INDEX_WAIT_MS = 500;
  inline constexpr double RESERVATION_REAP_LIMIT = 8;
  inline constexpr double STATUS_PRUNE_BATCH = 16;
  inline constexpr double CAS_ATTEMPTS = 5;

  // "reserved" | "stored" | "evicting" | "revoked"
  using SentinelReplayAccountingState = std::string;
  // "evicted" | "expired"
  using SentinelReplayClaimKind = std::string;

  /** The durable per-capture accounting row. Never written with `expireIn`. */
  struct SentinelReplayAccountingRow {
    double version = 1;
    std::string capture_id;
    /** Empty only for a legacy manifest whose request owner was never recorded. */
    std::string request_id;
    std::string fingerprint;
    double bytes;
    SentinelReplayAccountingState state;
    double fence;
    double created_at_ms;
    /** Payload TTL: after this instant the reaper reclaims the charge as `expired`. */
    double expires_at_ms;
    /** Bounded status/tombstone TTL, independent of the payload TTL. */
    double status_expires_at_ms;
    /** Count of completed fence-guarded staging batches. */
    double stage;
    /** Cause recorded when this row moves to `evicting`. */
    std::optional<SentinelReplayClaimKind> claim_kind{};
    // Original stored properties survive spreads; strict DTO codecs and equality exclude this backing.
    std::optional<rt::KvObject> storedObject{};
    friend bool operator==(const SentinelReplayAccountingRow&, const SentinelReplayAccountingRow&);
  };

  /** A pre-accounting-row reservation; only ever read to release its charge once. */
  struct SentinelReplayLegacyReservation {
    double version = 1;
    std::string capture_id;
    std::string request_id;
    double bytes;
    double fence;
    std::string state;  // "reserved" | "revoked" | "published"
    double created_at_ms;
    double expires_at_ms;
    friend bool operator==(const SentinelReplayLegacyReservation&, const SentinelReplayLegacyReservation&) = default;
  };

  struct SentinelReplayBudgetLedger {
    double version = 1;
    double budget_bytes;
    double stored_bytes;
    double reserved_bytes;
    double records;
    /** Capture-owned status + eviction-tombstone rows, bounded by the metadata reserve. */
    double status_records;
    /** Approximate encoded bytes of those status/tombstone rows. */
    double metadata_bytes;
    double evicted_bytes;
    double evicted_records;
    double expired_bytes;
    double expired_records;
    double status_pruned_records;
    std::optional<double> last_eviction_at_ms;
    std::optional<double> last_warning_at_ms;
    std::optional<double> last_full_at_ms;
    std::optional<std::string> last_skip_reason;
    bool bootstrap_complete;
    /** "<prefix index>|<kv cursor>" while a resumable bootstrap sweep is in flight. */
    std::optional<std::string> bootstrap_cursor;
    /**
     * Random id the batch that starts a sweep stamps, so in-memory state built for
     * one sweep never serves another, even after the ledger is deleted. Absent on
     * ledgers written before it existed.
     */
    rt::Nullable<std::string> bootstrap_sweep{};
    /** Sticky fail-closed marker set when an in-scope row could not be accounted. */
    std::optional<std::string> accounting_error;
    bool over_budget;
    // Original stored properties survive spreads; strict DTO codecs and equality exclude this backing.
    std::optional<rt::KvObject> storedObject{};
    friend bool operator==(const SentinelReplayBudgetLedger&, const SentinelReplayBudgetLedger&);
  };

  struct SentinelReplayPublication {
    rt::KvKey ledger_key;
    std::optional<std::string> ledger_versionstamp;
    SentinelReplayBudgetLedger ledger;
    rt::KvKey accounting_key;
    std::optional<std::string> accounting_versionstamp;
    SentinelReplayAccountingRow accounting;
    /**
     * The exact request-status row the publication commit replaces. Its versionstamp
     * is checked with the ledger so the accounted metadata delta always describes
     * the row that was really replaced, never a row written by someone else.
     */
    rt::KvKey status_key;
    std::optional<std::string> status_versionstamp;
  };

  struct SentinelReplayAdmissionOk {
    static constexpr bool ok = true;
    rt::KvKey accounting_key;
    SentinelReplayAccountingRow accounting;
    double charge;
  };
  struct SentinelReplayAdmissionNotOk {
    static constexpr bool ok = false;
    std::string reason;  // SENTINEL_REPLAY_STORAGE_FULL_REASON | SENTINEL_REPLAY_ACCOUNTING_REASON
  };
  using SentinelReplayAdmission = std::variant<SentinelReplayAdmissionOk, SentinelReplayAdmissionNotOk>;

  struct SentinelReplayRetentionStatus {
    std::string state;  // "ok" | "unavailable"
    std::string scope;  // "capture_owned_kv_payload"
    std::optional<double> budget_bytes;
    std::optional<double> stored_bytes;
    std::optional<double> reserved_bytes;
    std::optional<double> records;
    std::optional<double> metadata_bytes;
    std::optional<double> status_records;
    std::optional<double> evicted_records;
    std::optional<double> evicted_bytes;
    std::optional<double> expired_records;
    std::optional<double> last_eviction_at_ms;
    std::optional<double> last_warning_at_ms;
    bool over_budget;
    bool near_capacity;
    bool accounting_complete;
    std::optional<std::string> accounting_error;
    std::optional<std::string> skipped_reason;
    friend bool operator==(const SentinelReplayRetentionStatus&, const SentinelReplayRetentionStatus&) = default;
  };

  // The stored rows and the admin status: value_from writes the members in declaration order;
  // value_to is strict.
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayAccountingRow& value);
  SentinelReplayAccountingRow tag_invoke(const json::value_to_tag<SentinelReplayAccountingRow>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayLegacyReservation& value);
  SentinelReplayLegacyReservation tag_invoke(const json::value_to_tag<SentinelReplayLegacyReservation>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayBudgetLedger& value);
  SentinelReplayBudgetLedger tag_invoke(const json::value_to_tag<SentinelReplayBudgetLedger>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayRetentionStatus& value);
  SentinelReplayRetentionStatus tag_invoke(const json::value_to_tag<SentinelReplayRetentionStatus>&, const json::value& value);

  // C++ storage views of validated rows; unlike strict JSON DTO codecs these retain non-JSON extra members.
  SentinelReplayAccountingRow toSentinelReplayAccountingRow(const rt::KvValue& value);
  SentinelReplayBudgetLedger toSentinelReplayBudgetLedger(const rt::KvValue& value);
  rt::KvValue sentinelReplayAccountingValue(const SentinelReplayAccountingRow& value);
  rt::KvValue sentinelReplayLedgerValue(const SentinelReplayBudgetLedger& value);

  // "delete" | "commit"
  using SentinelReplayRetentionFaultKind = std::string;
  // `((kind: "delete" | "commit") => boolean) | null`: an empty function is null.
  using SentinelReplayRetentionFaultInjector = std::function<bool(const SentinelReplayRetentionFaultKind& kind)>;

  /** Test seam: an injectable small budget; production always uses the fixed default. */
  // std::nullopt is null.
  void setSentinelReplayBudgetForTest(std::optional<double> bytes);

  /** Test seam: an injected payload-delete or final-commit failure. One-shot by construction. */
  void setSentinelReplayRetentionFaultsForTest(SentinelReplayRetentionFaultInjector injector);

  // Throws rt::JsError("injected_<kind>_failure") when the injector asks for it.
  void fault(const SentinelReplayRetentionFaultKind& kind);

  double sentinelReplayBudgetBytes();

  /**
   * Fixed 64 MiB metadata reserve inside the production 1 GiB budget, so payload
   * admissions may use at most `budget_bytes - reserve`. Budgets below 1 GiB are
   * only reachable through the injectable test seam, where the reserve scales as
   * budget/16 so both bounds stay exercisable; at exactly 1 GiB the reserve is
   * exactly SENTINEL_REPLAY_METADATA_RESERVE_BYTES.
   */
  double sentinelReplayMetadataReserve(double budgetBytes);

  /** Payload admissions may use at most this much of the budget. */
  double sentinelReplayPayloadBudgetBytes(double budgetBytes);

  rt::KvKey sentinelReplayAccountingKey(double capturedAtMs, const std::string& captureId);

  rt::KvKey sentinelReplayManifestKey(const replay_model::SentinelReplayManifest& manifest);

  /** The bounded eviction tombstone an owner lookup consults when a manifest is gone. */
  rt::KvKey sentinelReplayEvictionKey(const std::string& fingerprint);

  rt::KvKey sentinelReplayRequestStatusKey(const std::string& requestId);

  /** Conservative charge for a retained manifest; legacy rows predate stored_bytes. */
  double sentinelReplayManifestCharge(const replay_model::SentinelReplayManifest& manifest);

  // `counter(value: unknown)`: a non-negative safe integer number.
  bool counter(const rt::KvValue& value);
  bool counter(const rt::KvKeyPart& value);
  bool counter(const json::value& value);
  bool counter(double value);

  bool isAccountingRow(const rt::KvValue& value);

  /** The key shape must agree with the row it holds before anything derived is touched. */
  bool accountingKeyMatches(const rt::KvKey& key, const SentinelReplayAccountingRow& row);

  bool manifestKeyMatches(const rt::KvKey& key, const replay_model::SentinelReplayManifest& manifest);

  bool isLegacyReservation(const rt::KvValue& value);

  // JSON.stringify(value)'s UTF-8 length plus the row overhead; the overhead alone when
  // JSON.stringify throws (a BigInt anywhere).
  double sentinelReplayStatusMetadataBytes(const rt::KvValue& value);
  double sentinelReplayStatusMetadataBytes(const json::value& value);

  /**
   * Bytes an already-stored status/tombstone row contributes to the metadata bound.
   * A missing row contributes nothing, so replacing a row costs only its difference.
   */
  double storedStatusMetadataBytes(const rt::KvValue& value);
  double storedStatusMetadataBytes(const json::value& value);

  // `LedgerState`: `ledger` is present unless the kind is corrupt; an absent ledger has a null
  // versionstamp.
  enum class LedgerStateKind { absent, ok, corrupt };
  struct LedgerState {
    LedgerStateKind kind;
    std::optional<SentinelReplayBudgetLedger> ledger;
    std::optional<std::string> versionstamp;
  };

  [[nodiscard]] rt::Task<LedgerState> readLedger(rt::Kv& kv, double budgetBytes);

  SentinelReplayBudgetLedger withBudget(const SentinelReplayBudgetLedger& ledger, double budgetBytes);

  // `Readonly<{ ledger: SentinelReplayBudgetLedger; versionstamp: string | null }>`
  struct SentinelReplayLedgerSnapshot {
    SentinelReplayBudgetLedger ledger;
    std::optional<std::string> versionstamp;
  };

  /** Public read-only snapshot for the admin surface and tests; `null` means corrupt. */
  // The overload without `budgetBytes` is `undefined`.
  [[nodiscard]] rt::Task<std::optional<SentinelReplayLedgerSnapshot>> readSentinelReplayLedgerSnapshot(rt::Kv& kv);
  [[nodiscard]] rt::Task<std::optional<SentinelReplayLedgerSnapshot>> readSentinelReplayLedgerSnapshot(rt::Kv& kv, double budgetBytes);

  // `Readonly<{ deleted: number; remaining: number }>`
  struct DeleteChunkBatchResult {
    double deleted;
    double remaining;
    friend bool operator==(const DeleteChunkBatchResult&, const DeleteChunkBatchResult&) = default;
  };

  [[nodiscard]] rt::Task<DeleteChunkBatchResult> deleteChunkBatch(rt::Kv& kv, std::string captureId, double limit);

  [[nodiscard]] rt::Task<double> countChunks(rt::Kv& kv, std::string captureId);

  // `(ledger: SentinelReplayBudgetLedger) => SentinelReplayBudgetLedger`
  using LedgerMutation = std::function<SentinelReplayBudgetLedger(const SentinelReplayBudgetLedger& ledger)>;
  // `(operation: AtomicOperation) => AtomicOperation`
  using AtomicRowApplication = std::function<rt::AtomicOperation&(rt::AtomicOperation& operation)>;

  /** One atomic commit: the row change and the ledger delta it describes. */
  [[nodiscard]] rt::Task<bool> commitAccountingMutation(rt::Kv& kv, rt::KvKey key, std::optional<std::string> rowVersionstamp, LedgerMutation mutate,
                                                        AtomicRowApplication applyRow, double budgetBytes);

  /** Current retention status for the admin surface; `unavailable` is never reported as zero. */
  // The overload without `budgetBytes` is `undefined`.
  [[nodiscard]] rt::Task<SentinelReplayRetentionStatus> readSentinelReplayRetentionStatus(rt::Kv& kv);
  [[nodiscard]] rt::Task<SentinelReplayRetentionStatus> readSentinelReplayRetentionStatus(rt::Kv& kv, double budgetBytes);
}
