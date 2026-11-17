#pragma once
// Port of src/sentinel/replay-retention-metadata.ts.
/** Reconstruct TTL-bound metadata counters only from a complete, unchanged-ledger scan. */
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::sentinel::replay_retention_metadata {
  /** Durable marker of the last committed maintenance recount; it sits outside every counted prefix. */
  inline const rt::KvKey SENTINEL_REPLAY_METADATA_RECONCILED_KEY{"uos_ai", "sentinel_replay", "v1", "metadata_reconciled"};
  /**
   * Admin-poll maintenance recounts status/tombstone metadata at most once per
   * interval. Native TTL deletion only leaves the counters conservatively high, so
   * between recounts the admin status may overstate them by rows that already
   * expired; the first maintenance pass at least this long after the last
   * committed recount removes that residual charge. Admission and publish under
   * metadata pressure never wait for it: their pruning path recounts at once.
   */
  inline constexpr double SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS = 15 * 60 * 1'000;

  /** A maintenance recount: its time, and the marker version it read, which its commit checks. */
  // `marker_versionstamp: string | null` (std::nullopt is null).
  struct MaintenanceRecount {
    double at_ms;
    std::optional<std::string> marker_versionstamp;
  };

  // `entry: KvEntry<unknown>`. As in the TS, `prefix` is told apart by identity: only the
  // replay_model::SENTINEL_REPLAY_REQUEST_PREFIX object itself selects the status-row contract;
  // every other prefix (SENTINEL_REPLAY_EVICTION_PREFIX) selects the tombstone contract.
  bool metadataEntryMatches(const rt::KvEntry<>& entry, const rt::KvKey& prefix);

  /**
   * Deliberate metadata mutations change the ledger with their rows, so its version
   * must be captured BEFORE listing and checked AFTER both complete prefix scans.
   * Native TTL deletion can only make this count conservatively high; a later pass
   * removes that residual charge. Valid over-cap data permits bounded pruning only
   * after the same ledger CAS; its partial totals are never committed.
   * A partial/corrupt scan or CAS loss changes no counter.
   *
   * A recount that matches the ledger leaves the ledger unwritten, so an unchanged
   * pass never moves its version under concurrent CAS writers. For a maintenance
   * recount the same commit records the marker and checks that the marker is still
   * the version this pass read: a lost CAS therefore never records a recount that
   * did not land, and of two concurrent due passes only one records its time, so
   * the marker never moves back. A maintenance scan that meets a corrupt or
   * mismatched row records the failure under the same marker check.
   */
  // "reconciled" | "over_limit" | "failed"; the overload without `maintenance` (and std::nullopt)
  // is undefined.
  [[nodiscard]] rt::Task<std::string> reconcileSentinelReplayStatusMetadata(rt::Kv& kv, double budgetBytes);
  [[nodiscard]] rt::Task<std::string> reconcileSentinelReplayStatusMetadata(rt::Kv& kv, double budgetBytes, std::optional<MaintenanceRecount> maintenance);

  /**
   * The throttled recount for admin-poll maintenance. A valid marker inside the
   * interval, or a recorded scan failure inside it, defers the recount: the poll
   * reads those two rows instead of every status/tombstone row, and the recount
   * writes nothing. A missing or malformed row, one at least the interval old, one
   * from a clock that ran ahead of `nowMs`, or an invalid `nowMs` defers nothing.
   * A due recount that commits (including a valid over-cap witness) records the
   * marker. A lost CAS records nothing, so the next poll retries it; a scan that
   * meets a corrupt or mismatched row records the failure, so the next scan waits
   * one interval.
   */
  // "reconciled" | "over_limit" | "failed" | "not_due"
  [[nodiscard]] rt::Task<std::string> reconcileSentinelReplayStatusMetadataIfDue(rt::Kv& kv, double budgetBytes, double nowMs);
}
