#pragma once
// Port of src/paid-fallback/ledger-state.ts: the paid-fallback V3 ledger's shared state helpers
// (key layout re-exported from src/authority/native-paid.ts, KV resolution, the reconciliation
// gate recompute, the reconciliation queue, and the operator projections).
// `kv: Kv` parameters are a non-null std::shared_ptr<rt::Kv>; `kvOverride?: Kv | null` is
// std::optional<std::shared_ptr<rt::Kv>> (std::nullopt is undefined, a null pointer is null).
// Stored rows the TS reads as `PaidFallbackRequestV3` / `PaidFallbackPendingV3` /
// `PaidFallbackWindowV3` are converted with their strict value_to (a malformed row throws
// rt::TypeError where the TS would read whatever is stored), except the provider usage and
// outstanding projections, which consume raw stored fields as the TS does.
#include "uos/authority/native_paid.hpp"
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::paid_fallback::ledger_state {
  // `export { ... } from "../authority/native-paid.ts"`
  using authority::native_paid::isPaidFallbackWindowV3;
  using authority::native_paid::PAID_FALLBACK_REQUEST_LOG_RETENTION_MS;
  using authority::native_paid::paidFallbackBackfillCursorV3Key;
  using authority::native_paid::paidFallbackBackfillLeaseV3Key;
  using authority::native_paid::paidFallbackBackfillStateV3Key;
  using authority::native_paid::paidFallbackBackfillWindowCursorV3Key;
  using authority::native_paid::paidFallbackDeletionGuardV3Key;
  using authority::native_paid::paidFallbackPendingV3Key;
  using authority::native_paid::paidFallbackReconciliationGateV3Key;
  using authority::native_paid::paidFallbackReconciliationLeaseV3Key;
  using authority::native_paid::paidFallbackRequestV3GlobalPrefix;
  using authority::native_paid::paidFallbackRequestV3Key;
  using authority::native_paid::paidFallbackWindowV3Key;
  using authority::native_paid::requestRowExpireIn;

  // `export type { ... } from "../authority/native-paid-contracts.ts"`
  using authority::native_paid_contracts::PaidFallbackAdmissionV3;
  using authority::native_paid_contracts::PaidFallbackOutstandingV3;
  using authority::native_paid_contracts::PaidFallbackReconciliationGateV3;
  using authority::native_paid_contracts::PaidFallbackWindowProjectionV3;

  // `AdmissionInput = NativePaidAdmissionInput & Readonly<{ policyCheck?: Readonly<{ key: KvKey; versionstamp: string | null }> }>`
  struct AdmissionInput : authority::native_paid_contracts::NativePaidAdmissionInput {
    // Native routing owns the immutable authenticated H-row key and revision.
    std::optional<rt::AtomicCheck> policyCheck{};
  };

  // `resolveKv(kvOverride: Kv | null | undefined): Promise<Kv | null>`: getKv() when undefined.
  rt::Task<std::shared_ptr<rt::Kv>> resolveKv(std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  // `PendingMarkerScan = Readonly<{ entries: readonly KvEntry<PaidFallbackPendingV3>[]; earliest_due_at_ms: number | null }>`
  struct PendingMarkerScan {
    std::vector<rt::KvEntry<authority::native_paid_contracts::PaidFallbackPendingV3>> entries;
    std::optional<double> earliest_due_at_ms;
  };

  rt::Task<PendingMarkerScan> scanPaidFallbackPendingMarkers(std::shared_ptr<rt::Kv> kv);

  // `Promise<number | null>`; throws rt::JsError "Paid fallback reconciliation gate changed
  // concurrently." after MAX_CAS_ATTEMPTS lost commits.
  rt::Task<std::optional<double>> recomputePaidFallbackReconciliationGateV3(std::shared_ptr<rt::Kv> kv);

  // ---- `kv.enqueue` -------------------------------------------------------------------------
  // rt::Kv has no queue (docs/rt/kv.md). The TS probes the store for an `enqueue` method; a C++
  // store or test double that has one also derives from KvEnqueue, and a store without it makes
  // enqueuePaidFallbackReconciliationJob return false, as `typeof enqueue !== "function"` does.
  // The capability and its options are rt's (rt::SqliteKv and the guard over it have it).

  // The @deno/kv `enqueue` options (`{ delay?, keysIfUndelivered?, backoffSchedule? }`).
  using rt::KvEnqueueOptions;
  // `kv.enqueue(message, options)`; the caller ignores the result.
  using rt::KvEnqueue;

  // `enqueuePaidFallbackReconciliationJob(kv, keyId, delay = 0)`: enqueues `{ key_id }` with the
  // reconciliation backoff schedule; false when the store has no queue or the enqueue fails.
  rt::Task<bool> enqueuePaidFallbackReconciliationJob(std::shared_ptr<rt::Kv> kv, std::string keyId);
  rt::Task<bool> enqueuePaidFallbackReconciliationJob(std::shared_ptr<rt::Kv> kv, std::string keyId, double delay);

  // `listPaidFallbackRequestsV3(keyId, limit = 100, kvOverride?)`: newest first, at most 1,000.
  rt::Task<std::vector<types::PaidFallbackRequestV3>> listPaidFallbackRequestsV3(std::string keyId);
  rt::Task<std::vector<types::PaidFallbackRequestV3>> listPaidFallbackRequestsV3(std::string keyId, double limit);
  rt::Task<std::vector<types::PaidFallbackRequestV3>> listPaidFallbackRequestsV3(std::string keyId, double limit, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  // `Readonly<Record<PaidFallbackProvider, PaidFallbackProviderUsageV3>>`: one member per
  // provider, in the order the TS object literal creates them.
  struct PaidFallbackProviderUsageByProviderV3 {
    json::object metered;
    json::object surplus;
    friend bool operator==(const PaidFallbackProviderUsageByProviderV3&, const PaidFallbackProviderUsageByProviderV3&) = default;
  };

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackProviderUsageByProviderV3& value);
  PaidFallbackProviderUsageByProviderV3 tag_invoke(const json::value_to_tag<PaidFallbackProviderUsageByProviderV3>&, const json::value& value);

  /**
   * Returns current-window usage split by the actual paid provider. The shared
   * admission window remains provider-neutral; this projection is only an
   * operator view and does not affect quota decisions.
   */
  rt::Task<PaidFallbackProviderUsageByProviderV3> getPaidFallbackProviderUsageV3(std::string keyId, double windowResetAtMs);
  rt::Task<PaidFallbackProviderUsageByProviderV3> getPaidFallbackProviderUsageV3(std::string keyId, double windowResetAtMs,
                                                                                 std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  // `PaidFallbackWindowProjectionV3 | null`
  rt::Task<std::optional<PaidFallbackWindowProjectionV3>> getPaidFallbackWindowProjectionV3(std::string keyId, double windowResetAtMs, double limitMicrocredits);
  rt::Task<std::optional<PaidFallbackWindowProjectionV3>> getPaidFallbackWindowProjectionV3(std::string keyId, double windowResetAtMs, double limitMicrocredits,
                                                                                            std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  // `PaidFallbackOutstandingV3 | null`
  rt::Task<std::optional<PaidFallbackOutstandingV3>> getPaidFallbackOutstandingV3(std::string keyId);
  rt::Task<std::optional<PaidFallbackOutstandingV3>> getPaidFallbackOutstandingV3(std::string keyId, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  // `export { ... }` (the module's second export list)
  using authority::native_paid::isPaidFallbackReconciliationGate;
  using authority::native_paid::MAX_CAS_ATTEMPTS;
  using authority::native_paid::PAID_FALLBACK_AUTOMATIC_BACKFILL_LIMIT;
  using authority::native_paid::PAID_FALLBACK_BACKFILL_LEASE_MS;
  using authority::native_paid::paidFallbackPendingV3GlobalPrefix;
  using authority::native_paid::paidFallbackPendingV3Prefix;
  using authority::native_paid::paidFallbackReconciliationGateDueNow;
  using authority::native_paid::paidFallbackReconciliationGateNeedsArm;
  using authority::native_paid::paidFallbackRequestV3Prefix;
  using authority::native_paid::paidFallbackWindowV3GlobalPrefix;
  using authority::native_paid::paidFallbackWindowV3Prefix;
  using authority::native_paid::RECONCILIATION_LEASE_MS;
  using authority::native_paid::windowExpireIn;

  // `export type { ... }`
  using authority::native_paid_contracts::PaidFallbackBackfillLeaseV3;
  using authority::native_paid_contracts::PaidFallbackBackfillStateV3;
  using authority::native_paid_contracts::PaidFallbackDeletionGuardV3;
  using authority::native_paid_contracts::PaidFallbackPendingV3;
  using authority::native_paid_contracts::PaidFallbackReconciliationLeaseV3;
}
