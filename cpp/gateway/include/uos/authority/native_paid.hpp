#pragma once
// Port of src/authority/native-paid.ts.
// `unknown` parameters take a json::value (and a pointer overload, nullptr is undefined).
// The gate helpers whose TS parameter is `Readonly<{ value: PaidFallbackReconciliationGateV3 | null }>`
// but which validate that value at run time also accept a NativePaidRow<json::value>: the
// row as read from storage before any typed conversion.
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

namespace uos::authority::native_paid {
  inline constexpr double MAX_CAS_ATTEMPTS = 128;
  inline constexpr std::array<double, 6> RETRY_DELAYS_MS{5'000, 30'000, 120'000, 600'000, 3'600'000, 21'600'000};
  inline constexpr double UNRESOLVED_AFTER_MS = 24.0 * 60 * 60'000;
  inline constexpr double RECONCILIATION_LEASE_MS = 60'000;
  inline constexpr double PAID_FALLBACK_BACKFILL_LEASE_MS = 5.0 * 60'000;
  inline constexpr double PAID_FALLBACK_AUTOMATIC_BACKFILL_LIMIT = 5'000;

  /**
   * Raw paid-fallback request rows are retained for one year from request
   * creation, then expire. Research that needs older data reads the compact
   * per-hour usage rollups (paid_fallback_rollups.ts), which never expire.
   */
  inline constexpr double PAID_FALLBACK_REQUEST_LOG_RETENTION_MS = 365.0 * 24 * 60 * 60'000;

  double requestRowExpireIn(const native_paid_contracts::PaidFallbackRequestV3& row, double nowMs);

  // `value: unknown`: nullptr is undefined; `number` and `number | null` callers use the
  // double and optional overloads.
  bool isPositiveSafeInteger(const json::value& value);
  bool isPositiveSafeInteger(const json::value* value);
  bool isPositiveSafeInteger(double value);
  bool isPositiveSafeInteger(const std::optional<double>& value);

  /**
   * Window rows expire one year after their reset so they never outlive the
   * request rows that validate them; the per-request detail is gone by then.
   */
  double windowExpireIn(const native_paid_contracts::PaidFallbackWindowV3& window, double nowMs);

  // `value: unknown`: nullptr is undefined.
  bool isPaidFallbackWindowV3(const json::value& value);
  bool isPaidFallbackWindowV3(const json::value* value);

  native_paid_contracts::NativePaidKey paidFallbackWindowV3Key(std::string_view keyId, double resetAtMs);
  native_paid_contracts::NativePaidKey paidFallbackRequestV3Key(std::string_view keyId, std::string_view requestId);
  native_paid_contracts::NativePaidKey paidFallbackPendingV3Key(std::string_view keyId, std::string_view requestId);
  native_paid_contracts::NativePaidKey paidFallbackWindowV3Prefix(std::string_view keyId);
  native_paid_contracts::NativePaidKey paidFallbackRequestV3Prefix(std::string_view keyId);
  // [...PREFIX, "request"]
  inline const native_paid_contracts::NativePaidKey paidFallbackRequestV3GlobalPrefix{"uos_ai", "paid_fallback", "v3", "request"};
  // [...PREFIX, "window"]
  inline const native_paid_contracts::NativePaidKey paidFallbackWindowV3GlobalPrefix{"uos_ai", "paid_fallback", "v3", "window"};
  native_paid_contracts::NativePaidKey paidFallbackPendingV3Prefix(std::string_view keyId);
  // [...PREFIX, "pending"]
  inline const native_paid_contracts::NativePaidKey paidFallbackPendingV3GlobalPrefix{"uos_ai", "paid_fallback", "v3", "pending"};
  native_paid_contracts::NativePaidKey paidFallbackReconciliationGateV3Key();
  native_paid_contracts::NativePaidKey paidFallbackReconciliationLeaseV3Key(std::string_view keyId);
  native_paid_contracts::NativePaidKey paidFallbackBackfillCursorV3Key();
  native_paid_contracts::NativePaidKey paidFallbackBackfillWindowCursorV3Key();
  native_paid_contracts::NativePaidKey paidFallbackBackfillLeaseV3Key();
  native_paid_contracts::NativePaidKey paidFallbackBackfillStateV3Key();
  native_paid_contracts::NativePaidKey paidFallbackDeletionGuardV3Key(std::string_view keyId);

  // `value: unknown`: nullptr is undefined. The optional overload is the guard applied to a
  // typed `PaidFallbackReconciliationGateV3 | null` (nullopt is null).
  bool isPaidFallbackReconciliationGate(const json::value& value);
  bool isPaidFallbackReconciliationGate(const json::value* value);
  bool isPaidFallbackReconciliationGate(const std::optional<native_paid_contracts::PaidFallbackReconciliationGateV3>& value);

  // `entry: Readonly<{ value: PaidFallbackReconciliationGateV3 | null }>`
  native_paid_contracts::PaidFallbackReconciliationGateV3 paidFallbackReconciliationGateDueNow(
    const native_paid_contracts::NativePaidRow<native_paid_contracts::PaidFallbackReconciliationGateV3>& entry, double now);
  native_paid_contracts::PaidFallbackReconciliationGateV3 paidFallbackReconciliationGateDueNow(const native_paid_contracts::NativePaidRow<json::value>& entry, double now);

  bool paidFallbackReconciliationGateNeedsArm(const native_paid_contracts::NativePaidRow<native_paid_contracts::PaidFallbackReconciliationGateV3>& entry, double now);
  bool paidFallbackReconciliationGateNeedsArm(const native_paid_contracts::NativePaidRow<json::value>& entry, double now);

  /** True when any admission field fails its policy validation. */
  // `maximumExposure: number | null`
  bool isInvalidAdmissionPolicy(const native_paid_contracts::NativePaidAdmissionInput& input, bool unlimited, const std::optional<double>& maximumExposure);

  /** True when the row still needs settlement; a settled or unbilled row does not. */
  // `request: PaidFallbackRequestV3 | null`
  bool isBillableRequestV3(const std::optional<native_paid_contracts::PaidFallbackRequestV3>& request);

  // readonly ["uos_ai", "paid_fallback", "v3", "usage_rollup"]
  inline const native_paid_contracts::NativePaidKey PAID_FALLBACK_USAGE_ROLLUP_PREFIX{"uos_ai", "paid_fallback", "v3", "usage_rollup"};
  inline constexpr double PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS = 60.0 * 60 * 1'000;
  inline constexpr double PAID_FALLBACK_USAGE_ROLLUP_SHARD_COUNT = 16;

  /**
   * Deterministic shard for a request id: spreads concurrent settlements of one
   * model/provider across PAID_FALLBACK_USAGE_ROLLUP_SHARD_COUNT keys while
   * keeping every settlement of the same request on one key.
   */
  double paidFallbackUsageRollupShard(std::string_view requestId);

  /**
   * Providers whose usage the paid-fallback ledger already folds into these
   * rollups inside its settlement atomic. `PaidFallbackProvider` is exactly
   * `"metered" | "surplus"`, and both the live settlement
   * (`prepareSettlementRollupV3`) and the historical backfill
   * (`foldRequestRowIntoRollupV3`, where a legacy row without a provider label
   * defaults to `metered`) write rows under those two labels.
   * `recordTerminalUsageRollup` skips them: a terminal observation would
   * otherwise count the same request twice.
   */
  inline const std::unordered_set<std::string> PAID_FALLBACK_SETTLED_PROVIDERS{"metered", "surplus"};

  // `value: unknown`: nullptr is undefined.
  bool isPaidFallbackUsageRollup(const json::value& value);
  bool isPaidFallbackUsageRollup(const json::value* value);

  native_paid_contracts::NativePaidKey paidFallbackUsageRollupKey(double bucketStartAtMs, std::string_view model, std::string_view provider, double shard);

  /**
   * Merges one settled request into an existing/hour bucket. Pure so tests can
   * assert sums without a KV backend.
   */
  // `existing: PaidFallbackUsageRollup | null`
  native_paid_contracts::PaidFallbackUsageRollup mergePaidFallbackUsageRollup(const std::optional<native_paid_contracts::PaidFallbackUsageRollup>& existing,
                                                                              const native_paid_contracts::PaidFallbackUsageRollupInput& input);

  // `value: unknown`: nullptr is undefined.
  bool isBackfillableSettledPaidFallbackRequest(const json::value& value);
  bool isBackfillableSettledPaidFallbackRequest(const json::value* value);

  /** Admission reads stay native; this is the source decision for those exact named rows. */
  native_paid_contracts::NativePaidAdmissionPlan planPaidFallbackAdmissionV3(const native_paid_contracts::NativePaidAdmissionInput& input,
                                                                             const native_paid_contracts::NativePaidAdmissionSnapshot& snapshot,
                                                                             const native_paid_contracts::NativePaidAdmissionPolicy& policy, double now);

  // `NativePaidLifecyclePlan | null`
  std::optional<native_paid_contracts::NativePaidLifecyclePlan> planPaidFallbackLifecycleV3(const native_paid_contracts::PaidFallbackRequestV3& current,
                                                                                            const native_paid_contracts::PaidFallbackRequestLifecyclePatchV3& patch, double now);

  // `NativePaidReleasePlan | null`
  std::optional<native_paid_contracts::NativePaidReleasePlan> planPaidFallbackReleaseV3(const native_paid_contracts::NativePaidReleaseSnapshot& snapshot, bool allowDispatchIntent,
                                                                                        double now);

  // `request: PaidFallbackRequestV3 | null`, `pending: PaidFallbackPendingV3 | null`. When
  // `reconciliation_attempts` is not a non-negative integer, RETRY_DELAYS_MS[...] is undefined in
  // the TS; `delay` is NaN here (and `next_reconciliation_at_ms` NaN in both).
  native_paid_contracts::NativePaidDeferralPlan planPaidFallbackDeferralV3(const std::optional<native_paid_contracts::PaidFallbackRequestV3>& request,
                                                                           const std::optional<native_paid_contracts::PaidFallbackPendingV3>& pending, double now);

  native_paid_contracts::NativePaidSettlementEvaluation evaluatePaidFallbackSettlementV3(const std::optional<native_paid_contracts::PaidFallbackRequestV3>& request,
                                                                                         const std::optional<native_paid_contracts::PaidFallbackPendingV3>& pending,
                                                                                         const native_paid_contracts::NativePaidProviderLog& providerLog,
                                                                                         const native_paid_contracts::NativePaidSettlementMetadata& metadata);

  // `correlation: NativePaidCorrelation = "provider_request_id"`
  native_paid_contracts::NativePaidRollupIdentity paidFallbackRequestRollupIdentity(const native_paid_contracts::PaidFallbackRequestV3& request,
                                                                                    std::string_view correlation = "provider_request_id");

  // The value paidFallbackSettlementRollupV3 returns: `Readonly<{ rollup: PaidFallbackUsageRollup; foldIntoRollup: boolean }>`.
  struct PaidFallbackSettlementRollupV3Result {
    native_paid_contracts::PaidFallbackUsageRollup rollup;
    bool foldIntoRollup;
    friend bool operator==(const PaidFallbackSettlementRollupV3Result&, const PaidFallbackSettlementRollupV3Result&) = default;
  };

  // `correlation: NativePaidCorrelation`, `existing: PaidFallbackUsageRollup | null`
  PaidFallbackSettlementRollupV3Result paidFallbackSettlementRollupV3(const native_paid_contracts::PaidFallbackRequestV3& request,
                                                                      const native_paid_contracts::NativePaidProviderLog& providerLog, double spend, std::string_view correlation,
                                                                      const std::optional<native_paid_contracts::PaidFallbackUsageRollup>& existing, double now);

  // Throws rt::JsError("Paid fallback settlement request is missing") when the snapshot has no request.
  native_paid_contracts::NativePaidSettlementPlan planPaidFallbackSettlementRowsV3(const native_paid_contracts::NativePaidSettlementSnapshot& snapshot,
                                                                                   const native_paid_contracts::NativePaidProviderLog& providerLog, double spend, bool foldIntoRollup,
                                                                                   double now);

  // The value planPaidFallbackBackfillFoldV3 returns:
  // `{ request: { ...request, updated_at_ms, usage_rollup_at_ms }, requestExpireIn, checks: string[], rollup: PaidFallbackUsageRollup | null }`.
  struct PaidFallbackBackfillFoldPlanV3 {
    native_paid_contracts::PaidFallbackRequestV3 request;
    double requestExpireIn;
    std::vector<std::string> checks;  // ("request" | "rollup")[]
    std::optional<native_paid_contracts::PaidFallbackUsageRollup> rollup;
    friend bool operator==(const PaidFallbackBackfillFoldPlanV3&, const PaidFallbackBackfillFoldPlanV3&) = default;
  };

  // `existing: PaidFallbackUsageRollup | null`
  PaidFallbackBackfillFoldPlanV3 planPaidFallbackBackfillFoldV3(const native_paid_contracts::PaidFallbackRequestV3& request,
                                                                const std::optional<native_paid_contracts::PaidFallbackUsageRollup>& existing, double now);

  bool paidFallbackBackfillRollupEligibleV3(const native_paid_contracts::PaidFallbackRequestV3& request);

  // `state: unknown, requestCursor: unknown, windowCursor: unknown`: nullptr is undefined (which is `!== null`).
  bool paidFallbackBackfillNeedsSweepV3(const json::value& state, const json::value& requestCursor, const json::value& windowCursor);
  bool paidFallbackBackfillNeedsSweepV3(const json::value* state, const json::value* requestCursor, const json::value* windowCursor);

  // `guard: PaidFallbackDeletionGuardV3 | null` -> `PaidFallbackDeletionGuardV3 | null`
  std::optional<native_paid_contracts::PaidFallbackDeletionGuardV3> planPaidFallbackDeletionGuardV3(
    const std::optional<native_paid_contracts::PaidFallbackDeletionGuardV3>& guard, double now);

  // The value planPaidFallbackDeletionV3 returns:
  // `{ kind: "unavailable" } | { kind: "blocked"; outstanding: PaidFallbackOutstandingV3 } | { kind: "delete" }`.
  struct PaidFallbackDeletionPlanV3Unavailable {
    static constexpr std::string_view kind = "unavailable";
    friend bool operator==(const PaidFallbackDeletionPlanV3Unavailable&, const PaidFallbackDeletionPlanV3Unavailable&) = default;
  };
  struct PaidFallbackDeletionPlanV3Blocked {
    static constexpr std::string_view kind = "blocked";
    native_paid_contracts::PaidFallbackOutstandingV3 outstanding;
    friend bool operator==(const PaidFallbackDeletionPlanV3Blocked&, const PaidFallbackDeletionPlanV3Blocked&) = default;
  };
  struct PaidFallbackDeletionPlanV3Delete {
    static constexpr std::string_view kind = "delete";
    friend bool operator==(const PaidFallbackDeletionPlanV3Delete&, const PaidFallbackDeletionPlanV3Delete&) = default;
  };
  using PaidFallbackDeletionPlanV3 = std::variant<PaidFallbackDeletionPlanV3Unavailable, PaidFallbackDeletionPlanV3Blocked, PaidFallbackDeletionPlanV3Delete>;

  // `outstanding: PaidFallbackOutstandingV3 | null`
  PaidFallbackDeletionPlanV3 planPaidFallbackDeletionV3(const std::optional<native_paid_contracts::PaidFallbackOutstandingV3>& outstanding);

  // `lease: PaidFallbackReconciliationLeaseV3 | null`; the backfill lease has the same shape and is
  // passed by the backfill callers.
  bool paidFallbackLeaseActiveV3(const std::optional<native_paid_contracts::PaidFallbackReconciliationLeaseV3>& lease, double now);
  bool paidFallbackLeaseActiveV3(const std::optional<native_paid_contracts::PaidFallbackBackfillLeaseV3>& lease, double now);

  native_paid_contracts::PaidFallbackReconciliationLeaseV3 paidFallbackReconciliationLeaseV3(std::string token, double now);

  native_paid_contracts::PaidFallbackBackfillLeaseV3 paidFallbackBackfillLeaseV3(std::string token, double now);

  bool paidFallbackLeaseOwnedV3(const std::optional<native_paid_contracts::PaidFallbackReconciliationLeaseV3>& lease, std::string_view token);
  bool paidFallbackLeaseOwnedV3(const std::optional<native_paid_contracts::PaidFallbackBackfillLeaseV3>& lease, std::string_view token);

  // `earliestDueAtMs: number | null`
  native_paid_contracts::PaidFallbackReconciliationGateV3 paidFallbackGateAfterScanV3(std::optional<double> earliestDueAtMs);

  // `NativePaidTerminalUsage | null`
  std::optional<native_paid_contracts::NativePaidTerminalUsage> paidFallbackTerminalUsageV3(const native_paid_contracts::TerminalUsageRollupInput& input);

  native_paid_contracts::PaidFallbackUsageRollupInput paidFallbackTerminalRollupV3(const native_paid_contracts::TerminalUsageRollupInput& input,
                                                                                   const native_paid_contracts::NativePaidTerminalUsage& usage, std::string_view requestId, double now);

  /** A marked row is a retention rewrite only; it must never fold twice. */
  // `request: PaidFallbackRequestV3 | null` -> `"missing" | "retention" | "fold"`
  std::string paidFallbackRequestBackfillStateV3(const std::optional<native_paid_contracts::PaidFallbackRequestV3>& request);

  // The value planPaidFallbackGateAfterScanV3 returns:
  // `{ gate: PaidFallbackReconciliationGateV3; checks: readonly ["gate"]; versionstamp: string | null }`.
  struct PaidFallbackGateAfterScanPlanV3 {
    native_paid_contracts::PaidFallbackReconciliationGateV3 gate;
    std::vector<std::string> checks{"gate"};  // readonly ["gate"]
    std::optional<std::string> versionstamp;
    friend bool operator==(const PaidFallbackGateAfterScanPlanV3&, const PaidFallbackGateAfterScanPlanV3&) = default;
  };

  // `gate: Readonly<{ value: PaidFallbackReconciliationGateV3 | null; versionstamp: string | null }>`, `earliestDueAtMs: number | null`
  PaidFallbackGateAfterScanPlanV3 planPaidFallbackGateAfterScanV3(const native_paid_contracts::NativePaidRow<native_paid_contracts::PaidFallbackReconciliationGateV3>& gate,
                                                                  std::optional<double> earliestDueAtMs);
  PaidFallbackGateAfterScanPlanV3 planPaidFallbackGateAfterScanV3(const native_paid_contracts::NativePaidRow<json::value>& gate, std::optional<double> earliestDueAtMs);
}
