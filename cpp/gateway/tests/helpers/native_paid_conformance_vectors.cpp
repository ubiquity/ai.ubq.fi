#include "tests/helpers/native_paid_conformance_vectors.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace uos::tests::helpers::native_paid_conformance_vectors {
  using namespace authority::native_paid_contracts;
  namespace paid = authority::native_paid;

  namespace {
    template <class T> NativePaidRow<T> row(std::optional<T> value, std::optional<std::string> versionstamp = "opaque-revision") {
      return NativePaidRow<T>{.value = std::move(value), .versionstamp = std::move(versionstamp)};
    }
  }

  // BEGIN SHARED_PAID_VECTORS
  PaidConformanceVectors runPaidConformanceVectors() {
    const double now = 1'700'000'000'000;
    const NativePaidAdmissionInput input{
      .keyId = "key",
      .requestId = "request",
      .createdAtMs = now,
      .policyVersion = "policy-v3",
      .limitMicrocredits = 100,
      .maximumExposureMicrocredits = 80,
      .initialSettledMicrocredits = 0,
      .quotaPerCredit = 500'000,
      .windowResetAtMs = now + 60'000,
      .model = " model ",
      .route = "responses",
      .path = "/v1/responses",
      .stream = false,
      .reasoning = std::nullopt,
    };
    const NativePaidAdmissionSnapshot empty{
      .request = row<PaidFallbackRequestV3>(std::nullopt, std::nullopt),
      .window = row<PaidFallbackWindowV3>(std::nullopt, std::nullopt),
      .deletionGuard = row<PaidFallbackDeletionGuardV3>(std::nullopt, std::nullopt),
      .policyFence = NativePaidAdmissionSnapshot::PolicyFence{.versionstamp = "H:opaque"},
    };
    const NativePaidAdmissionPolicy policy{.unlimited = false, .maximumExposure = 80};
    const NativePaidAdmissionPlan admitted = paid::planPaidFallbackAdmissionV3(input, empty, policy, now);
    const NativePaidAdmissionPlanWrite* admittedWrite = std::get_if<NativePaidAdmissionPlanWrite>(&admitted);
    if (admittedWrite == nullptr || !admittedWrite->window) throw rt::JsError("Synthetic paid admission failed");
    // JSON.stringify({ input, empty, admitted }) before the other plans run.
    const NativePaidAdmissionInput inputBefore = input;
    const NativePaidAdmissionSnapshot emptyBefore = empty;
    const NativePaidAdmissionPlan admittedBefore = admitted;
    PaidFallbackWindowV3 saturatedWindow = *admittedWrite->window;
    saturatedWindow.settled_microcredits = 70;
    saturatedWindow.reserved_microcredits = 20;
    NativePaidAdmissionInput partialInput = input;
    partialInput.requestId = "partial";
    NativePaidAdmissionSnapshot saturated = empty;
    saturated.window = row<PaidFallbackWindowV3>(saturatedWindow);
    const NativePaidAdmissionPlan partial = paid::planPaidFallbackAdmissionV3(partialInput, saturated, policy, now);
    NativePaidAdmissionInput editedInput = input;
    editedInput.policyVersion = "edited";
    editedInput.limitMicrocredits = 50;
    const NativePaidAdmissionPlan exhausted = paid::planPaidFallbackAdmissionV3(editedInput, saturated, policy, now);
    const NativePaidRow<PaidFallbackPendingV3> pending = row<PaidFallbackPendingV3>(admittedWrite->pending);
    const NativePaidRow<PaidFallbackReconciliationGateV3> gate = row<PaidFallbackReconciliationGateV3>(PaidFallbackReconciliationGateV3{.next_due_at_ms = now + 100'000});
    const NativePaidRow<PaidFallbackRequestV3> request = row<PaidFallbackRequestV3>(admittedWrite->request);
    const std::optional<NativePaidLifecyclePlan> dispatched = paid::planPaidFallbackLifecycleV3(
      admittedWrite->request, PaidFallbackRequestLifecyclePatchV3{.provider = "metered", .provider_request_id = std::string("provider-id"), .dispatch_state = "dispatched"}, now + 1);
    if (!dispatched) throw rt::JsError("Synthetic paid dispatch failed");
    PaidFallbackRequestV3 metered = dispatched->request;
    metered.request_id = "client:metered";
    PaidFallbackRequestV3 surplus = dispatched->request;
    surplus.request_id = "client:surplus";
    surplus.provider = "surplus";
    surplus.provider_request_id = std::nullopt;
    const NativePaidProviderLog log{
      .request_id = "provider-id",
      .quota = 400'000,
      .prompt_tokens = 12,
      .cached_prompt_tokens = 3,
      .completion_tokens = 4,
      .model = "model",
      .created_at = now / 1000,
    };
    const NativePaidSettlementMetadata metadata{.correlation = "provider_request_id", .microcreditsPerCredit = 1'000'000};
    const NativePaidSettlementEvaluation meteredEvaluation = paid::evaluatePaidFallbackSettlementV3(metered, admittedWrite->pending, log, metadata);
    const NativePaidSettlementEvaluationSettle* meteredSettle = std::get_if<NativePaidSettlementEvaluationSettle>(&meteredEvaluation);
    if (meteredSettle == nullptr) throw rt::JsError("Synthetic Metered settlement failed");
    const paid::PaidFallbackSettlementRollupV3Result rollup = paid::paidFallbackSettlementRollupV3(metered, log, meteredSettle->spend, "provider_request_id", std::nullopt, now + 2);
    const NativePaidSettlementPlan settled = paid::planPaidFallbackSettlementRowsV3(
      NativePaidSettlementSnapshot{
        .request = row<PaidFallbackRequestV3>(metered),
        .pending = pending,
        .gate = gate,
        .window = row<PaidFallbackWindowV3>(admittedWrite->window),
        .rollup = row<PaidFallbackUsageRollup>(std::nullopt),
      },
      log, meteredSettle->spend, rollup.foldIntoRollup, now + 2);
    const std::optional<NativePaidReleasePlan> cancelled =
      paid::planPaidFallbackReleaseV3(NativePaidReleaseSnapshot{.request = request, .window = row<PaidFallbackWindowV3>(admittedWrite->window), .gate = gate}, false, now + 3);
    NativePaidAdmissionInput intentInput = input;
    intentInput.dispatchIntent = true;
    const NativePaidAdmissionPlan intent = paid::planPaidFallbackAdmissionV3(intentInput, empty, policy, now);
    const NativePaidAdmissionPlanWrite* intentWrite = std::get_if<NativePaidAdmissionPlanWrite>(&intent);
    if (intentWrite == nullptr) throw rt::JsError("Synthetic intent failed");
    PaidFallbackRequestV3 old = admittedWrite->request;
    old.created_at_ms = now - 24 * 60 * 60'000;
    old.reconciliation_attempts = 8;
    const NativePaidDeferralPlan deferred = paid::planPaidFallbackDeferralV3(old, admittedWrite->pending, now);
    const TerminalUsageRollupInput terminal{
      .model = " model ",
      .provider = "chatgpt_codex",
      .request_id = "terminal",
      .request_created_at_ms = now,
      .input_tokens = 12,
      .cached_input_tokens = std::nullopt,
      .output_tokens = 4,
    };
    const std::optional<NativePaidTerminalUsage> terminalUsage = paid::paidFallbackTerminalUsageV3(terminal);
    if (!terminalUsage) throw rt::JsError("Synthetic terminal usage failed");

    NativePaidAdmissionInput zeroLimit = input;
    zeroLimit.limitMicrocredits = 0;
    NativePaidAdmissionInput unlimitedInput = input;
    unlimitedInput.limitMicrocredits = -1;
    NativePaidAdmissionSnapshot guarded = empty;
    guarded.deletionGuard = row<PaidFallbackDeletionGuardV3>(PaidFallbackDeletionGuardV3{.created_at_ms = now});
    NativePaidAdmissionSnapshot reused = empty;
    reused.request = request;
    NativePaidAdmissionSnapshot consumedSnapshot = empty;
    consumedSnapshot.request = row<PaidFallbackRequestV3>(settled.request);
    NativePaidProviderLog otherLog = log;
    otherLog.request_id = "other";
    NativePaidProviderLog reservationLog = log;
    reservationLog.request_id = "gateway-reservation";
    NativePaidProviderLog infiniteLog = log;
    infiniteLog.quota = rt::Number::POSITIVE_INFINITY;
    PaidFallbackRequestV3 unmarked = settled.request;
    unmarked.usage_rollup_at_ms = rt::undefined;
    PaidFallbackRequestV3 legacy = metered;
    legacy.provider = std::nullopt;
    TerminalUsageRollupInput meteredTerminal = terminal;
    meteredTerminal.provider = "metered";
    TerminalUsageRollupInput surplusTerminal = terminal;
    surplusTerminal.provider = "surplus";
    TerminalUsageRollupInput unmeasuredTerminal = terminal;
    unmeasuredTerminal.input_tokens = std::nullopt;

    PaidConformanceVectors vectors{
      .admitted = admitted,
      .partial = partial,
      .exhausted = exhausted,
      .invalidPolicy = paid::isInvalidAdmissionPolicy(zeroLimit, false, 80),
      .unlimited = paid::planPaidFallbackAdmissionV3(unlimitedInput, empty, NativePaidAdmissionPolicy{.unlimited = true, .maximumExposure = std::nullopt}, now),
      .deletionGuard = paid::planPaidFallbackAdmissionV3(input, guarded, policy, now),
      .reuse = paid::planPaidFallbackAdmissionV3(input, reused, policy, now),
      .consumed = paid::planPaidFallbackAdmissionV3(input, consumedSnapshot, policy, now),
      .dispatched = *dispatched,
      .lifecycleReplay = paid::planPaidFallbackLifecycleV3(
        dispatched->request, PaidFallbackRequestLifecyclePatchV3{.provider = "surplus", .provider_request_id = std::string("replacement"), .dispatch_state = "reserved"}, now + 3),
      .cancelled = cancelled,
      .releaseIntentDefault = paid::planPaidFallbackReleaseV3(
        NativePaidReleaseSnapshot{.request = row<PaidFallbackRequestV3>(intentWrite->request), .window = row<PaidFallbackWindowV3>(intentWrite->window), .gate = gate}, false, now + 3),
      .releaseIntentKnownNoFetch = paid::planPaidFallbackReleaseV3(
        NativePaidReleaseSnapshot{.request = row<PaidFallbackRequestV3>(intentWrite->request), .window = row<PaidFallbackWindowV3>(intentWrite->window), .gate = gate}, true, now + 3),
      .releaseCorrelated = paid::planPaidFallbackReleaseV3(
        NativePaidReleaseSnapshot{.request = row<PaidFallbackRequestV3>(metered), .window = row<PaidFallbackWindowV3>(admittedWrite->window), .gate = gate}, true, now + 3),
      .releaseAfterSettlement = paid::planPaidFallbackReleaseV3(
        NativePaidReleaseSnapshot{.request = row<PaidFallbackRequestV3>(settled.request), .window = row<PaidFallbackWindowV3>(settled.window), .gate = gate}, true, now + 3),
      .meteredEvaluation = meteredEvaluation,
      .rollup = rollup,
      .settled = settled,
      .settlementReplay = paid::evaluatePaidFallbackSettlementV3(settled.request, admittedWrite->pending, log, metadata),
      .mismatchedMetered = paid::evaluatePaidFallbackSettlementV3(metered, admittedWrite->pending, otherLog, metadata),
      .missingPending = paid::evaluatePaidFallbackSettlementV3(metered, std::nullopt, log, metadata),
      .surplusExact = paid::evaluatePaidFallbackSettlementV3(surplus, admittedWrite->pending, reservationLog,
                                                             NativePaidSettlementMetadata{.correlation = "surplus_reservation", .microcreditsPerCredit = 1'000'000, .surplusBuyerCostMicro = 69}),
      .surplusZero = paid::evaluatePaidFallbackSettlementV3(surplus, admittedWrite->pending, log,
                                                            NativePaidSettlementMetadata{.correlation = "surplus_reservation", .microcreditsPerCredit = 1'000'000, .surplusBuyerCostMicro = 0}),
      .surplusCannotSettleMetered = paid::evaluatePaidFallbackSettlementV3(
        metered, admittedWrite->pending, log, NativePaidSettlementMetadata{.correlation = "surplus_reservation", .microcreditsPerCredit = 1'000'000, .surplusBuyerCostMicro = 69}),
      .invalidSpend = paid::evaluatePaidFallbackSettlementV3(metered, admittedWrite->pending, infiniteLog, metadata),
      .deferred = deferred,
      .deferralCleanup = paid::planPaidFallbackDeferralV3(settled.request, admittedWrite->pending, now),
      .deferralNoop = paid::planPaidFallbackDeferralV3(metered, std::nullopt, now),
      .originalExpiry =
        {
          paid::requestRowExpireIn(admittedWrite->request, now),
          paid::requestRowExpireIn(admittedWrite->request, now + 1),
          paid::windowExpireIn(*admittedWrite->window, now),
          paid::requestRowExpireIn(admittedWrite->request, now + paid::PAID_FALLBACK_REQUEST_LOG_RETENTION_MS + 1),
        },
      .gateDue =
        {
          paid::paidFallbackReconciliationGateDueNow(gate, now),
          paid::paidFallbackReconciliationGateDueNow(row<PaidFallbackReconciliationGateV3>(PaidFallbackReconciliationGateV3{.next_due_at_ms = now - 1}), now),
          paid::paidFallbackGateAfterScanV3(std::nullopt),
        },
      .guard = paid::planPaidFallbackDeletionGuardV3(std::nullopt, now),
      .guardReplay = paid::planPaidFallbackDeletionGuardV3(PaidFallbackDeletionGuardV3{.created_at_ms = now - 1}, now),
      .deletion = paid::planPaidFallbackDeletionV3(PaidFallbackOutstandingV3{.pending_requests = 0, .unresolved_requests = 1, .pending_markers = 1, .has_outstanding = true}),
      .leases =
        {
          paid::paidFallbackReconciliationLeaseV3("lease", now),
          paid::paidFallbackBackfillLeaseV3("backfill", now),
          paid::paidFallbackLeaseActiveV3(PaidFallbackReconciliationLeaseV3{.token = "lease", .expires_at_ms = now}, now),
          paid::paidFallbackLeaseOwnedV3(PaidFallbackReconciliationLeaseV3{.token = "old", .expires_at_ms = now}, "new"),
        },
      .backfill = paid::planPaidFallbackBackfillFoldV3(unmarked, std::nullopt, now + 3),
      .backfillPending = paid::planPaidFallbackBackfillFoldV3(admittedWrite->request, std::nullopt, now + 3),
      .legacyMetered = paid::paidFallbackRequestRollupIdentity(legacy),
      .terminalRollup = paid::paidFallbackTerminalRollupV3(terminal, *terminalUsage, "terminal", now),
      .terminalSkipped =
        {
          paid::paidFallbackTerminalUsageV3(meteredTerminal),
          paid::paidFallbackTerminalUsageV3(surplusTerminal),
          paid::paidFallbackTerminalUsageV3(unmeasuredTerminal),
        },
      .key = paid::paidFallbackRequestV3Key("key", "request"),
      .rollupKey = paid::paidFallbackUsageRollupKey(0, "model", "metered", paid::paidFallbackUsageRollupShard("request")),
      .backfillStates =
        {
          paid::paidFallbackRequestBackfillStateV3(std::nullopt),
          paid::paidFallbackRequestBackfillStateV3(admittedWrite->request),
          paid::paidFallbackRequestBackfillStateV3(settled.request),
        },
      .gateScan = paid::planPaidFallbackGateAfterScanV3(
        NativePaidRow<PaidFallbackReconciliationGateV3>{.value = PaidFallbackReconciliationGateV3{.next_due_at_ms = now}, .versionstamp = "opaque:not-a-counter"}, std::nullopt),
      .immutable = false,
    };
    vectors.immutable = inputBefore == input && emptyBefore == empty && admittedBefore == admitted;
    return vectors;
  }
  // END SHARED_PAID_VECTORS
}
