#pragma once
// Port of src/paid-fallback/ledger-backfill.ts.
// Paid-fallback ledger v3: backfill layer, split out of src/paid_fallback_ledger.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>; `kvOverride?: Kv | null` is
// std::optional<std::shared_ptr<rt::Kv>> (std::nullopt is undefined, which reads getKv(); a null
// pointer is null). `now = Date.now()` defaults and `options = {}` are overloads.
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <memory>
#include <optional>
#include <string>

namespace uos::paid_fallback::ledger_backfill {
  struct PaidFallbackRollupBackfillResult {
    double scanned;
    /** Rows this run applied (TTL rewrite and/or rollup merge) successfully. */
    double processed;
    double rollups_written;
    /**
     * Rows whose CAS commit failed (for example a live settlement updated the
     * same shard in between). The cursor is kept before them so a re-run
     * retries instead of losing the history.
     */
    double failed;
    /** True when the scan stopped at `limit` rows and a further run is needed. */
    bool truncated;
    friend bool operator==(const PaidFallbackRollupBackfillResult&, const PaidFallbackRollupBackfillResult&) = default;
  };

  // The `options` of backfillPaidFallbackUsageRollups: `Readonly<{ limit?: number; nowMs?: number }>`.
  struct BackfillPaidFallbackUsageRollupsOptions {
    std::optional<double> limit{};
    std::optional<double> nowMs{};
  };

  /**
   * One-time backfill that folds already-settled V3 rows into usage rollups and
   * applies the one-year raw-row TTL to pre-existing rows.
   *
   * Rollups are new with this feature: rows settled before deployment never
   * passed through the settlement hook, so without this pass the admin
   * projection would silently start from an empty history. It also fixes rows
   * that predate the TTL, because every request-row write now re-applies an
   * anchored expiry and these rows will not be rewritten by normal traffic.
   *
   * Idempotency: rows carry `usage_rollup_at_ms` set by the settlement write for
   * new traffic and by this backfill for historical rows, so a run can never
   * double-count usage already folded into a rollup. Run with a `limit` and
   * re-run until `truncated` is false; a persisted cursor resumes the next run
   * at the last committed row (the list `start` is inclusive) so repeated
   * batches stay O(remaining) rather than re-walking every already-processed
   * row. The walk itself is also scan-bounded so an invocation cannot exceed
   * its deadline without persisting forward progress.
   */
  rt::Task<PaidFallbackRollupBackfillResult> backfillPaidFallbackUsageRollups(std::shared_ptr<rt::Kv> kv);
  rt::Task<PaidFallbackRollupBackfillResult> backfillPaidFallbackUsageRollups(std::shared_ptr<rt::Kv> kv, BackfillPaidFallbackUsageRollupsOptions options);

  struct PaidFallbackWindowTtlBackfillResult {
    double scanned;
    double rewritten;
    bool truncated;
    friend bool operator==(const PaidFallbackWindowTtlBackfillResult&, const PaidFallbackWindowTtlBackfillResult&) = default;
  };

  // The `options` of backfillPaidFallbackWindowTtls: `Readonly<{ limit?: number; nowMs?: number }>`.
  struct BackfillPaidFallbackWindowTtlsOptions {
    std::optional<double> limit{};
    std::optional<double> nowMs{};
  };

  /**
   * Resumable sweep that applies the anchored one-year window TTL to existing
   * window rows. Window writes in live paths already carry the expiry; rows
   * written before this feature or restored by KV import do not, so they must
   * be rewritten in place. The rewrite is idempotent and each run bounds the
   * work, persisting its own cursor so repeated invocations complete the whole
   * prefix.
   */
  rt::Task<PaidFallbackWindowTtlBackfillResult> backfillPaidFallbackWindowTtls(std::shared_ptr<rt::Kv> kv);
  rt::Task<PaidFallbackWindowTtlBackfillResult> backfillPaidFallbackWindowTtls(std::shared_ptr<rt::Kv> kv, BackfillPaidFallbackWindowTtlsOptions options);

  struct PaidFallbackAutomaticBackfillResult {
    std::string kind;  // "completed" | "in_progress" | "skipped" | "busy"
    std::optional<PaidFallbackRollupBackfillResult> requests;
    std::optional<PaidFallbackWindowTtlBackfillResult> windows;
    friend bool operator==(const PaidFallbackAutomaticBackfillResult&, const PaidFallbackAutomaticBackfillResult&) = default;
  };

  // The `options` of runPaidFallbackBackfillV3: `Readonly<{ force?: boolean; limit?: number; nowMs?: number }>`.
  struct RunPaidFallbackBackfillV3Options {
    std::optional<bool> force{};
    std::optional<double> limit{};
    std::optional<double> nowMs{};
  };

  rt::Task<PaidFallbackAutomaticBackfillResult> runPaidFallbackBackfillV3(std::shared_ptr<rt::Kv> kv);
  rt::Task<PaidFallbackAutomaticBackfillResult> runPaidFallbackBackfillV3(std::shared_ptr<rt::Kv> kv, RunPaidFallbackBackfillV3Options options);

  // `export { paidFallbackBackfillAttempted, paidFallbackBackfillRetryPending, schedulePaidFallbackBackfill }`:
  // live `let` bindings, defined in the .cpp (false until the first schedule) and read by
  // ledger-admission.
  extern bool paidFallbackBackfillAttempted;
  extern bool paidFallbackBackfillRetryPending;

  // `kvOverride?: Kv`: the overload without it (and a null pointer) resolves the shared KV.
  void schedulePaidFallbackBackfill();
  void schedulePaidFallbackBackfill(std::shared_ptr<rt::Kv> kvOverride);

  // C++ test seam (no TS twin): the in-flight scheduled backfill (`scheduledPaidFallbackBackfill`),
  // std::nullopt when none runs, so an ASYNC_TEST can let it settle before its loop is destroyed.
  // The promise never rejects.
  std::optional<rt::Promise<void>> scheduledPaidFallbackBackfillForTest();

  // The `options` of reconcilePaidFallbackV3: `Readonly<{ skipGateRecompute?: boolean }>`.
  struct ReconcilePaidFallbackV3Options {
    std::optional<bool> skipGateRecompute{};
  };

  rt::Task<double> reconcilePaidFallbackV3(std::string keyId);
  rt::Task<double> reconcilePaidFallbackV3(std::string keyId, double now);
  rt::Task<double> reconcilePaidFallbackV3(std::string keyId, double now, std::optional<std::shared_ptr<rt::Kv>> kvOverride);
  rt::Task<double> reconcilePaidFallbackV3(std::string keyId, double now, std::optional<std::shared_ptr<rt::Kv>> kvOverride, ReconcilePaidFallbackV3Options options);

  // Throws rt::JsError "Paid fallback reconciliation gate changed concurrently." as the TS does.
  rt::Task<double> reconcileDuePaidFallbacksV3();
  rt::Task<double> reconcileDuePaidFallbacksV3(double now);
  rt::Task<double> reconcileDuePaidFallbacksV3(double now, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  // `terminalState: PaidFallbackRequestV3["terminal_state"]`, `provider?: PaidFallbackProvider`.
  rt::Task<double> markPaidFallbackTerminalV3(authority::native_paid_contracts::PaidFallbackAdmissionV3 reservation, std::string terminalState);
  rt::Task<double> markPaidFallbackTerminalV3(authority::native_paid_contracts::PaidFallbackAdmissionV3 reservation, std::string terminalState,
                                              ::uos::types::PaidFallbackProvider provider);

  rt::Task<double> recordPaidFallbackTerminalV3(authority::native_paid_contracts::PaidFallbackAdmissionV3 reservation, std::string terminalState);
  rt::Task<double> recordPaidFallbackTerminalV3(authority::native_paid_contracts::PaidFallbackAdmissionV3 reservation, std::string terminalState,
                                                ::uos::types::PaidFallbackProvider provider);

  // `message: unknown`: a queue message as delivered (the value ledger-state's enqueue wrote).
  rt::Task<double> handlePaidFallbackReconciliationJobV3(rt::KvValue message);
  rt::Task<double> handlePaidFallbackReconciliationJobV3(rt::KvValue message, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  rt::Task<double> enqueueDuePaidFallbackReconciliationJobsV3();
  rt::Task<double> enqueueDuePaidFallbackReconciliationJobsV3(double now);
  rt::Task<double> enqueueDuePaidFallbackReconciliationJobsV3(double now, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  // ---- JSON conversions of the snake_case records (members in declaration order) ----------
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackRollupBackfillResult& value);
  PaidFallbackRollupBackfillResult tag_invoke(const json::value_to_tag<PaidFallbackRollupBackfillResult>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackWindowTtlBackfillResult& value);
  PaidFallbackWindowTtlBackfillResult tag_invoke(const json::value_to_tag<PaidFallbackWindowTtlBackfillResult>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackAutomaticBackfillResult& value);
  PaidFallbackAutomaticBackfillResult tag_invoke(const json::value_to_tag<PaidFallbackAutomaticBackfillResult>&, const json::value& value);
}
