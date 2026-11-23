#pragma once
// Port of src/sentinel/replay-retention-bootstrap.ts.
/**
 * Resumable bootstrap accounting for capture-owned storage.
 *
 * The sweep counts every in-scope row itself, so while it is pending no other
 * writer may also count a row, or the same row is charged twice; every counter
 * mutation is gated behind `bootstrapBeforeMutation`. It counts scanned entries
 * rather than only valid records: an invalid or unreadable in-scope row marks
 * accounting incomplete so new admissions stay refused instead of trusting a
 * zero. Legacy reservation rows are read only to release their charge once.
 *
 * A chunk key names only its capture, while owner keys lead with a timestamp,
 * so a chunk's owner has no point lookup. Chunks are judged against a
 * process-local index of owner capture ids for the current sweep, filled in
 * the background in fixed slices of BOOTSTRAP_OWNER_SCAN_ENTRIES rows, so each
 * owner read stays bounded, the reads stay linear over the sweep instead of
 * one full scan per chunk. Inference-path batches never wait for the fill;
 * maintenance and admin batches wait only briefly. An ownerless chunk is
 * deleted only in a commit that also checks the ledger versionstamp its batch
 * read, so an owner published after that read keeps its payload.
 */
// The process-local owner index is keyed by the handle's identity (rt::WeakMap<rt::Kv, ...>, the
// TS WeakMap<Kv, ...>): every handle object, including a forwarding wrapper, holds its own. The
// background fill keeps a reference to the handle it was started on and stops at its next check
// once that handle is destroyed (the TS closure keeps it alive instead); the handle must outlive
// any read or commit of the fill still in flight.
#include "uos/sentinel/replay_retention_schema.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::sentinel::replay_retention_bootstrap {
  /** Legacy reservation release: read once, delete chunks, then delete the row. */
  [[nodiscard]] rt::Task<void> releaseLegacyReservation(rt::Kv& kv, rt::KvKey key, replay_retention_schema::SentinelReplayLegacyReservation reservation,
                                                        double budgetBytes);

  /** Hold no owner index on this handle. A fill still running for one stops before its next slice. */
  void forgetChunkOwnerIndex(const rt::Kv& kv);

  /**
   * One bounded, resumable bootstrap sweep across EVERY capture-owned prefix that
   * holds retained bytes or metadata. It counts scanned entries, not just valid
   * records: an invalid or unreadable in-scope row marks accounting incomplete so
   * new admissions stay refused instead of trusting a zero.
   */
  // `versionstamp: string | null` (std::nullopt is null); the overload without `ownerIndexWaitMs`
  // waits BOOTSTRAP_OWNER_INDEX_WAIT_MS.
  [[nodiscard]] rt::Task<replay_retention_schema::SentinelReplayBudgetLedger> runBootstrapBatch(rt::Kv& kv,
                                                                                               replay_retention_schema::SentinelReplayBudgetLedger ledger,
                                                                                               std::optional<std::string> versionstamp, double budgetBytes,
                                                                                               double nowMs);
  [[nodiscard]] rt::Task<replay_retention_schema::SentinelReplayBudgetLedger> runBootstrapBatch(rt::Kv& kv,
                                                                                               replay_retention_schema::SentinelReplayBudgetLedger ledger,
                                                                                               std::optional<std::string> versionstamp, double budgetBytes,
                                                                                               double nowMs, double ownerIndexWaitMs);

  /**
   * Ensure the resumable sweep has finished before any counter mutation. The sweep
   * counts every in-scope row itself, so while it is pending no other writer may
   * also count a row, or the same row is charged twice. Returns the fresh ledger
   * when accounting is authoritative, and null when the ledger is corrupt or the
   * sweep is still incomplete, in which case no counter may move.
   */
  // std::nullopt is null; the overload without `ownerIndexWaitMs` waits BOOTSTRAP_OWNER_INDEX_WAIT_MS.
  [[nodiscard]] rt::Task<std::optional<replay_retention_schema::SentinelReplayBudgetLedger>> bootstrapBeforeMutation(rt::Kv& kv, double budgetBytes,
                                                                                                                     double nowMs);
  [[nodiscard]] rt::Task<std::optional<replay_retention_schema::SentinelReplayBudgetLedger>> bootstrapBeforeMutation(rt::Kv& kv, double budgetBytes,
                                                                                                                     double nowMs, double ownerIndexWaitMs);

  // ---- C++ addition ---------------------------------------------------------------------------
  // The typed view of a stored value isLegacyReservation accepted, for releaseLegacyReservation
  // (replay-retention's legacy reaper passes the rows it lists). isLegacyReservation checks only
  // version, capture_id, bytes, fence and state, the members the release reads; `request_id`
  // reads as "" and `created_at_ms` / `expires_at_ms` as NaN when absent or of another type.
  replay_retention_schema::SentinelReplayLegacyReservation toSentinelReplayLegacyReservation(const rt::KvValue& value);
}
