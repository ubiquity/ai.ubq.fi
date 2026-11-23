// Port of tests/native-paid-conformance.test.ts. node:assert/strict throws on the first failed
// assertion, so every assertion is an ASSERT. The file's exported runPaidConformanceVectors
// (SHARED_PAID_VECTORS) is tests/helpers/native_paid_conformance_vectors; the differential test
// compares every vector it returns with JSON.stringify of the same vectors under Bun.
#include "tests/helpers/native_paid_conformance_vectors.hpp"
#include "uos/authority/native_paid.hpp"
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace uos;
using namespace uos::authority::native_paid_contracts;
namespace paid = uos::authority::native_paid;
using namespace uos::tests::helpers::native_paid_conformance_vectors;

namespace {
  template <class Alternative, class Variant> const Alternative& kindOf(const Variant& value, const char* message) {
    const Alternative* alternative = std::get_if<Alternative>(&value);
    if (alternative == nullptr) throw rt::JsError(message);
    return *alternative;
  }
}

TEST("paid pure admission preserves bounded exposure, unlimited rows, H/deletion checks and policy transitions") {
  const PaidConformanceVectors v = runPaidConformanceVectors();
  ASSERT(std::holds_alternative<NativePaidAdmissionPlanWrite>(v.admitted));
  ASSERT(std::holds_alternative<NativePaidAdmissionPlanWrite>(v.partial));
  ASSERT(std::holds_alternative<NativePaidAdmissionPlanExhaustedPolicy>(v.exhausted));
  const NativePaidAdmissionPlanWrite& unlimited = kindOf<NativePaidAdmissionPlanWrite>(v.unlimited, "Missing write vector");
  ASSERT_EQ(std::get<NativePaidAdmissionPlanWrite>(v.partial).request.reserved_microcredits, 10.0);
  ASSERT(!unlimited.window);
  ASSERT_EQ(unlimited.request.reserved_microcredits, 0.0);
  ASSERT_EQ(std::get<NativePaidAdmissionPlanWrite>(v.admitted).checks, (std::vector<std::string>{"request", "deletionGuard", "policy", "window"}));
  ASSERT_EQ(v.invalidPolicy, true);
  ASSERT(v.deletionGuard == NativePaidAdmissionPlan(NativePaidAdmissionPlanBlocked{.reason = "invalid_policy"}));
  ASSERT(v.consumed == NativePaidAdmissionPlan(NativePaidAdmissionPlanBlocked{.reason = "invalid_policy"}));
  ASSERT(std::holds_alternative<NativePaidAdmissionPlanReserved>(v.reuse));
}

TEST("paid pure lifecycle keeps correlation sticky and refunds only definite no-fetch work") {
  const PaidConformanceVectors v = runPaidConformanceVectors();
  ASSERT(v.cancelled);
  ASSERT(v.cancelled->window);
  ASSERT(v.releaseIntentKnownNoFetch);
  ASSERT(!v.lifecycleReplay);
  ASSERT_EQ(v.cancelled->request.billing_state, "not_billed");
  ASSERT(!v.releaseIntentDefault);
  ASSERT_EQ(v.releaseIntentKnownNoFetch->request.billing_state, "not_billed");
  ASSERT(!v.releaseCorrelated);
  ASSERT(!v.releaseAfterSettlement);
  ASSERT_EQ(v.cancelled->checks, (std::vector<std::string>{"request", "gate", "window"}));
  ASSERT_EQ(v.cancelled->window->reserved_microcredits, 0.0);
}

TEST("paid pure settlement keeps Metered and Surplus correlation, actual overspend and terminal independence") {
  const PaidConformanceVectors v = runPaidConformanceVectors();
  ASSERT(v.settled.window);
  ASSERT(v.meteredEvaluation == NativePaidSettlementEvaluation(NativePaidSettlementEvaluationSettle{.spend = 800'000}));
  ASSERT(v.surplusExact == NativePaidSettlementEvaluation(NativePaidSettlementEvaluationSettle{.spend = 69}));
  ASSERT(v.surplusZero == NativePaidSettlementEvaluation(NativePaidSettlementEvaluationSettle{.spend = 0}));
  ASSERT(v.mismatchedMetered == NativePaidSettlementEvaluation(NativePaidSettlementEvaluationNoop{}));
  ASSERT(v.surplusCannotSettleMetered == NativePaidSettlementEvaluation(NativePaidSettlementEvaluationNoop{}));
  ASSERT(v.missingPending == NativePaidSettlementEvaluation(NativePaidSettlementEvaluationNoop{}));
  ASSERT(v.invalidSpend == NativePaidSettlementEvaluation(NativePaidSettlementEvaluationDefer{}));
  ASSERT_EQ(v.settled.request.terminal_state, "pending");
  ASSERT_EQ(v.settled.request.terminal_at_ms, std::nullopt);
  ASSERT_EQ(v.settled.window->settled_microcredits, 800'000.0);
  ASSERT_EQ(v.settled.window->reserved_microcredits, 0.0);
  ASSERT(v.settlementReplay == NativePaidSettlementEvaluation(NativePaidSettlementEvaluationCleanup{}));
  ASSERT_EQ(v.settled.checks, (std::vector<std::string>{"request", "pending", "rollup", "gate", "window"}));
  ASSERT_EQ(v.rollup.rollup.provider, "metered");
}

TEST("paid pure deferral retains unresolved exposure and preserves the original cleanup check set") {
  const PaidConformanceVectors v = runPaidConformanceVectors();
  const NativePaidDeferralPlanWrite& deferred = kindOf<NativePaidDeferralPlanWrite>(v.deferred, "Missing deferral vector");
  ASSERT_EQ(deferred.request.billing_state, "unresolved");
  ASSERT_EQ(deferred.request.reserved_microcredits, 80.0);
  ASSERT_EQ(deferred.delay, 21'600'000.0);
  ASSERT_EQ(deferred.request.terminal_state, "pending");
  const NativePaidDeferralPlan expectedCleanup = NativePaidDeferralPlanCleanup{.checks = {"pending", "gate"}};
  ASSERT(v.deferralCleanup == expectedCleanup);
  ASSERT(v.deferralNoop == NativePaidDeferralPlan(NativePaidDeferralPlanNoop{.checks = {}}));
  ASSERT(std::holds_alternative<paid::PaidFallbackDeletionPlanV3Blocked>(v.deletion));
}

TEST("paid pure retention, permanent guards, leases and rollup facts retain native identities") {
  const PaidConformanceVectors v = runPaidConformanceVectors();
  ASSERT(v.guard);
  ASSERT(v.backfill.rollup);
  const double year = paid::PAID_FALLBACK_REQUEST_LOG_RETENTION_MS;
  ASSERT_EQ(v.originalExpiry, (std::vector<double>{year, year - 1, year + 60'000, 1}));
  ASSERT_EQ(v.guardReplay, std::nullopt);
  ASSERT_EQ(v.guard->created_at_ms, 1'700'000'000'000.0);
  ASSERT_EQ(v.backfillPending.rollup, std::nullopt);
  ASSERT_EQ(v.backfill.rollup->request_count, 1.0);
  ASSERT_EQ(v.backfill.rollup->spend_microcredits, 800'000.0);
  ASSERT_EQ(v.legacyMetered.provider, "metered");
  ASSERT(v.terminalSkipped == (std::vector<std::optional<NativePaidTerminalUsage>>{std::nullopt, std::nullopt, std::nullopt}));
  ASSERT_EQ(v.terminalRollup.quota, 0.0);
  ASSERT_EQ(v.terminalRollup.cached_input_tokens, std::nullopt);
  ASSERT_EQ(v.backfillStates, (std::vector<std::string>{"missing", "fold", "retention"}));
  ASSERT(v.gateScan == (paid::PaidFallbackGateAfterScanPlanV3{.gate = {.next_due_at_ms = std::nullopt}, .checks = {"gate"}, .versionstamp = "opaque:not-a-counter"}));
  ASSERT_EQ(v.immutable, true);
  ASSERT_EQ((std::vector<bool>{std::get<2>(v.leases), std::get<3>(v.leases)}), (std::vector<bool>{false, false}));
  ASSERT_EQ(v.key, (NativePaidKey{"uos_ai", "paid_fallback", "v3", "request", "key", "request"}));
}
