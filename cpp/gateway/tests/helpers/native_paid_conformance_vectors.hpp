#pragma once
// The exported runPaidConformanceVectors of tests/native-paid-conformance.test.ts (its
// SHARED_PAID_VECTORS block), shared by tests/native_paid_conformance_test.cpp and
// tests/native_paid_differential_test.cpp. The TS returns an object literal; this struct has
// its members in the same order.
#include "uos/authority/native_paid.hpp"
#include "uos/authority/native_paid_contracts.hpp"

#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace uos::tests::helpers::native_paid_conformance_vectors {
  /** This exact synthetic vector function executes against the same source in Bun and real workerd. */
  struct PaidConformanceVectors {
    authority::native_paid_contracts::NativePaidAdmissionPlan admitted;
    authority::native_paid_contracts::NativePaidAdmissionPlan partial;
    authority::native_paid_contracts::NativePaidAdmissionPlan exhausted;
    bool invalidPolicy;
    authority::native_paid_contracts::NativePaidAdmissionPlan unlimited;
    authority::native_paid_contracts::NativePaidAdmissionPlan deletionGuard;
    authority::native_paid_contracts::NativePaidAdmissionPlan reuse;
    authority::native_paid_contracts::NativePaidAdmissionPlan consumed;
    authority::native_paid_contracts::NativePaidLifecyclePlan dispatched;
    std::optional<authority::native_paid_contracts::NativePaidLifecyclePlan> lifecycleReplay;
    std::optional<authority::native_paid_contracts::NativePaidReleasePlan> cancelled;
    std::optional<authority::native_paid_contracts::NativePaidReleasePlan> releaseIntentDefault;
    std::optional<authority::native_paid_contracts::NativePaidReleasePlan> releaseIntentKnownNoFetch;
    std::optional<authority::native_paid_contracts::NativePaidReleasePlan> releaseCorrelated;
    std::optional<authority::native_paid_contracts::NativePaidReleasePlan> releaseAfterSettlement;
    authority::native_paid_contracts::NativePaidSettlementEvaluation meteredEvaluation;
    authority::native_paid::PaidFallbackSettlementRollupV3Result rollup;
    authority::native_paid_contracts::NativePaidSettlementPlan settled;
    authority::native_paid_contracts::NativePaidSettlementEvaluation settlementReplay;
    authority::native_paid_contracts::NativePaidSettlementEvaluation mismatchedMetered;
    authority::native_paid_contracts::NativePaidSettlementEvaluation missingPending;
    authority::native_paid_contracts::NativePaidSettlementEvaluation surplusExact;
    authority::native_paid_contracts::NativePaidSettlementEvaluation surplusZero;
    authority::native_paid_contracts::NativePaidSettlementEvaluation surplusCannotSettleMetered;
    authority::native_paid_contracts::NativePaidSettlementEvaluation invalidSpend;
    authority::native_paid_contracts::NativePaidDeferralPlan deferred;
    authority::native_paid_contracts::NativePaidDeferralPlan deferralCleanup;
    authority::native_paid_contracts::NativePaidDeferralPlan deferralNoop;
    std::vector<double> originalExpiry;
    std::vector<authority::native_paid_contracts::PaidFallbackReconciliationGateV3> gateDue;
    std::optional<authority::native_paid_contracts::PaidFallbackDeletionGuardV3> guard;
    std::optional<authority::native_paid_contracts::PaidFallbackDeletionGuardV3> guardReplay;
    authority::native_paid::PaidFallbackDeletionPlanV3 deletion;
    // [reconciliation lease, backfill lease, active, owned]
    std::tuple<authority::native_paid_contracts::PaidFallbackReconciliationLeaseV3, authority::native_paid_contracts::PaidFallbackBackfillLeaseV3, bool, bool> leases;
    authority::native_paid::PaidFallbackBackfillFoldPlanV3 backfill;
    authority::native_paid::PaidFallbackBackfillFoldPlanV3 backfillPending;
    authority::native_paid_contracts::NativePaidRollupIdentity legacyMetered;
    authority::native_paid_contracts::PaidFallbackUsageRollupInput terminalRollup;
    std::vector<std::optional<authority::native_paid_contracts::NativePaidTerminalUsage>> terminalSkipped;
    authority::native_paid_contracts::NativePaidKey key;
    authority::native_paid_contracts::NativePaidKey rollupKey;
    std::vector<std::string> backfillStates;
    authority::native_paid::PaidFallbackGateAfterScanPlanV3 gateScan;
    bool immutable;
  };

  PaidConformanceVectors runPaidConformanceVectors();
}
