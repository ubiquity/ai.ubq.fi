// Differential cases for src/authority/native-paid.ts recorded from Bun 1.4.0 (see
// native_paid_differential_vectors.inc). Each table case converts its JSON arguments to the C++
// parameter types, calls the port and compares the result with JSON.stringify of the TS result as
// JSON data: member order is not compared (structs write their members in declaration order,
// docs/PORT-NOTES.md). Where the port has several overloads for one TS function (unknown values,
// raw and typed gate rows, the two lease types) every overload must give the recorded result.
// The last tests pin inputs JSON cannot carry (undefined, NaN, Infinity, -0).
#include "tests/helpers/native_paid_conformance_vectors.hpp"
#include "uos/authority/native_paid.hpp"
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <cmath>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace uos;
using namespace uos::authority::native_paid_contracts;
namespace paid = uos::authority::native_paid;
namespace shared = uos::tests::helpers::native_paid_conformance_vectors;

namespace {
#include "native_paid_differential_vectors.inc"

  constexpr double now = 1'700'000'000'000;

  // ---- JSON arguments -> C++ parameters ------------------------------------------------------
  std::string text(const json::value& value) { return json::value_to<std::string>(value); }
  double number(const json::value& value) { return rt::getNumber(value).value(); }
  std::optional<double> numberOrNull(const json::value& value) { return value.is_null() ? std::nullopt : std::optional<double>(number(value)); }
  std::optional<std::string> textOrNull(const json::value& value) { return value.is_null() ? std::nullopt : std::optional<std::string>(text(value)); }
  template <class T> std::optional<T> recordOrNull(const json::value& value) {
    if (value.is_null()) return std::nullopt;
    return json::value_to<T>(value);
  }

  // `row(value, versionstamp)` of the TS cases: `{ value: T | null, versionstamp: string | null }`.
  template <class T> NativePaidRow<T> rowOf(const json::value& value) {
    return NativePaidRow<T>{.value = recordOrNull<T>(value.at("value")), .versionstamp = textOrNull(value.at("versionstamp"))};
  }
  NativePaidRow<json::value> rawRowOf(const json::value& value) {
    return NativePaidRow<json::value>{.value = value.at("value"), .versionstamp = textOrNull(value.at("versionstamp"))};
  }
  // The typed row when the stored value converts to the record; nullopt when the TS value has no
  // typed C++ form (a string, an array, a member of the wrong type).
  template <class T> std::optional<NativePaidRow<T>> typedRowOf(const json::value& value) {
    try {
      return rowOf<T>(value);
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }

  NativePaidAdmissionInput admissionInputOf(const json::value& value) {
    NativePaidAdmissionInput input{
      .keyId = text(value.at("keyId")),
      .requestId = text(value.at("requestId")),
      .createdAtMs = number(value.at("createdAtMs")),
      .policyVersion = text(value.at("policyVersion")),
      .limitMicrocredits = number(value.at("limitMicrocredits")),
      .maximumExposureMicrocredits = numberOrNull(value.at("maximumExposureMicrocredits")),
      .initialSettledMicrocredits = number(value.at("initialSettledMicrocredits")),
      .quotaPerCredit = number(value.at("quotaPerCredit")),
      .windowResetAtMs = number(value.at("windowResetAtMs")),
      .model = text(value.at("model")),
      .route = text(value.at("route")),
      .path = text(value.at("path")),
      .stream = value.at("stream").as_bool(),
      .reasoning = textOrNull(value.at("reasoning")),
    };
    if (const json::value* dispatchIntent = rt::get(value, "dispatchIntent")) input.dispatchIntent = dispatchIntent->as_bool();
    return input;
  }

  NativePaidAdmissionSnapshot admissionSnapshotOf(const json::value& value) {
    NativePaidAdmissionSnapshot snapshot{
      .request = rowOf<PaidFallbackRequestV3>(value.at("request")),
      .window = rowOf<PaidFallbackWindowV3>(value.at("window")),
      .deletionGuard = rowOf<PaidFallbackDeletionGuardV3>(value.at("deletionGuard")),
    };
    if (const json::value* fence = rt::get(value, "policyFence")) snapshot.policyFence = NativePaidAdmissionSnapshot::PolicyFence{.versionstamp = textOrNull(fence->at("versionstamp"))};
    return snapshot;
  }

  NativePaidAdmissionPolicy admissionPolicyOf(const json::value& value) {
    return {.unlimited = value.at("unlimited").as_bool(), .maximumExposure = numberOrNull(value.at("maximumExposure"))};
  }

  NativePaidReleaseSnapshot releaseSnapshotOf(const json::value& value) {
    return {
      .request = rowOf<PaidFallbackRequestV3>(value.at("request")),
      .window = rowOf<PaidFallbackWindowV3>(value.at("window")),
      .gate = rowOf<PaidFallbackReconciliationGateV3>(value.at("gate")),
    };
  }

  NativePaidSettlementSnapshot settlementSnapshotOf(const json::value& value) {
    return {
      .request = rowOf<PaidFallbackRequestV3>(value.at("request")),
      .pending = rowOf<PaidFallbackPendingV3>(value.at("pending")),
      .gate = rowOf<PaidFallbackReconciliationGateV3>(value.at("gate")),
      .window = rowOf<PaidFallbackWindowV3>(value.at("window")),
      .rollup = rowOf<PaidFallbackUsageRollup>(value.at("rollup")),
    };
  }

  NativePaidSettlementMetadata settlementMetadataOf(const json::value& value) {
    NativePaidSettlementMetadata metadata{.correlation = text(value.at("correlation")), .microcreditsPerCredit = number(value.at("microcreditsPerCredit"))};
    if (const json::value* cost = rt::get(value, "surplusBuyerCostMicro")) metadata.surplusBuyerCostMicro = number(*cost);
    return metadata;
  }

  NativePaidTerminalUsage terminalUsageOf(const json::value& value) {
    return {.model = text(value.at("model")), .provider = text(value.at("provider")), .inputTokens = number(value.at("inputTokens")), .outputTokens = number(value.at("outputTokens"))};
  }

  // ---- C++ results -> JSON with the TS member names ------------------------------------------
  json::value numberJson(double value) { return rt::makeNumber(value); }
  json::value numberOrNullJson(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  json::value stringsJson(const std::vector<std::string>& values) {
    json::array out;
    for (const std::string& value : values) out.emplace_back(value);
    return out;
  }
  template <class T> json::value recordJson(const T& value) { return json::value_from(value); }
  template <class T> json::value recordOrNullJson(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }

  json::value keyJson(const NativePaidKey& key) {
    json::array out;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) out.emplace_back(part.string());
      else if (part.isNumber()) out.push_back(rt::makeNumber(part.number()));
      else throw std::logic_error("unexpected key part");
    }
    return out;
  }

  json::value admissionJson(const NativePaidAdmissionPlanBlocked& plan) { return json::object{{"kind", plan.kind}, {"reason", plan.reason}}; }
  json::value admissionJson(const NativePaidAdmissionPlanReserved& plan) { return json::object{{"kind", plan.kind}, {"reservation", recordJson(plan.reservation)}}; }
  json::value admissionJson(const NativePaidAdmissionPlanExhaustedPolicy& plan) {
    return json::object{{"kind", plan.kind}, {"window", recordJson(plan.window)}, {"windowExpireIn", numberJson(plan.windowExpireIn)}, {"checks", stringsJson(plan.checks)}};
  }
  json::value admissionJson(const NativePaidAdmissionPlanWrite& plan) {
    return json::object{
      {"kind", plan.kind},
      {"request", recordJson(plan.request)},
      {"pending", recordJson(plan.pending)},
      {"window", recordOrNullJson(plan.window)},
      {"reservation", recordJson(plan.reservation)},
      {"checkGate", plan.checkGate},
      {"checkPolicy", plan.checkPolicy},
      {"requestExpireIn", numberJson(plan.requestExpireIn)},
      {"windowExpireIn", numberOrNullJson(plan.windowExpireIn)},
      {"checks", stringsJson(plan.checks)},
    };
  }
  json::value planJson(const NativePaidAdmissionPlan& plan) {
    return std::visit([](const auto& alternative) { return admissionJson(alternative); }, plan);
  }

  json::value planJson(const NativePaidLifecyclePlan& plan) {
    return json::object{
      {"request", recordJson(plan.request)},
      {"dispatchBoundary", plan.dispatchBoundary},
      {"requestExpireIn", numberJson(plan.requestExpireIn)},
      {"checks", stringsJson(plan.checks)},
      {"readGate", plan.readGate},
    };
  }

  json::value planJson(const NativePaidReleasePlan& plan) {
    return json::object{
      {"request", recordJson(plan.request)},
      {"window", recordOrNullJson(plan.window)},
      {"gate", recordJson(plan.gate)},
      {"requestExpireIn", numberJson(plan.requestExpireIn)},
      {"windowExpireIn", numberOrNullJson(plan.windowExpireIn)},
      {"checks", stringsJson(plan.checks)},
    };
  }

  json::value planJson(const NativePaidSettlementPlan& plan) {
    return json::object{
      {"request", recordJson(plan.request)},
      {"window", recordOrNullJson(plan.window)},
      {"gate", recordJson(plan.gate)},
      {"requestExpireIn", numberJson(plan.requestExpireIn)},
      {"windowExpireIn", numberOrNullJson(plan.windowExpireIn)},
      {"checks", stringsJson(plan.checks)},
    };
  }

  template <class Plan> json::value planOrNullJson(const std::optional<Plan>& plan) { return plan ? planJson(*plan) : json::value(nullptr); }

  json::value deferralJson(const NativePaidDeferralPlanNoop& plan) { return json::object{{"kind", plan.kind}, {"checks", stringsJson(plan.checks)}}; }
  json::value deferralJson(const NativePaidDeferralPlanCleanup& plan) { return json::object{{"kind", plan.kind}, {"checks", stringsJson(plan.checks)}}; }
  json::value deferralJson(const NativePaidDeferralPlanWrite& plan) {
    return json::object{
      {"kind", plan.kind},
      {"delay", numberJson(plan.delay)},
      {"requestExpireIn", numberJson(plan.requestExpireIn)},
      {"checks", stringsJson(plan.checks)},
      {"request", recordJson(plan.request)},
      {"pending", recordJson(plan.pending)},
    };
  }
  json::value planJson(const NativePaidDeferralPlan& plan) {
    return std::visit([](const auto& alternative) { return deferralJson(alternative); }, plan);
  }

  json::value evaluationJson(const NativePaidSettlementEvaluationSettle& evaluation) { return json::object{{"kind", evaluation.kind}, {"spend", numberJson(evaluation.spend)}}; }
  template <class Evaluation> json::value evaluationJson(const Evaluation& evaluation) { return json::object{{"kind", evaluation.kind}}; }
  json::value planJson(const NativePaidSettlementEvaluation& evaluation) {
    return std::visit([](const auto& alternative) { return evaluationJson(alternative); }, evaluation);
  }

  json::value identityJson(const NativePaidRollupIdentity& identity) {
    return json::object{{"model", identity.model}, {"provider", identity.provider}, {"bucketStartAtMs", numberJson(identity.bucketStartAtMs)}, {"shard", numberJson(identity.shard)}};
  }

  json::value usageJson(const NativePaidTerminalUsage& usage) {
    return json::object{{"model", usage.model}, {"provider", usage.provider}, {"inputTokens", numberJson(usage.inputTokens)}, {"outputTokens", numberJson(usage.outputTokens)}};
  }
  json::value usageOrNullJson(const std::optional<NativePaidTerminalUsage>& usage) { return usage ? usageJson(*usage) : json::value(nullptr); }

  json::value planJson(const paid::PaidFallbackSettlementRollupV3Result& result) {
    return json::object{{"foldIntoRollup", result.foldIntoRollup}, {"rollup", recordJson(result.rollup)}};
  }

  json::value planJson(const paid::PaidFallbackBackfillFoldPlanV3& plan) {
    return json::object{
      {"request", recordJson(plan.request)},
      {"requestExpireIn", numberJson(plan.requestExpireIn)},
      {"checks", stringsJson(plan.checks)},
      {"rollup", recordOrNullJson(plan.rollup)},
    };
  }

  json::value deletionJson(const paid::PaidFallbackDeletionPlanV3Blocked& plan) { return json::object{{"kind", plan.kind}, {"outstanding", recordJson(plan.outstanding)}}; }
  template <class Plan> json::value deletionJson(const Plan& plan) { return json::object{{"kind", plan.kind}}; }
  json::value planJson(const paid::PaidFallbackDeletionPlanV3& plan) {
    return std::visit([](const auto& alternative) { return deletionJson(alternative); }, plan);
  }

  json::value planJson(const paid::PaidFallbackGateAfterScanPlanV3& plan) {
    return json::object{
      {"gate", recordJson(plan.gate)},
      {"checks", stringsJson(plan.checks)},
      {"versionstamp", plan.versionstamp ? json::value(*plan.versionstamp) : json::value(nullptr)},
    };
  }

  // JSON.stringify(result), read back: NaN and Infinity become null, -0 becomes 0.
  bool sameJson(const json::value& actual, std::string_view expected) { return rt::deepEqual(rt::jsonParse(rt::jsonStringify(actual)), rt::jsonParse(expected)); }

  template <class T> T same(const T& first, const T& second, const char* what) {
    if (!(first == second)) throw std::logic_error(std::string("overloads disagree: ") + what);
    return first;
  }

  // ---- one TS call ------------------------------------------------------------------------------
  json::value run(std::string_view fn, const json::array& args) {
    if (fn == "isPaidFallbackWindowV3") return same(paid::isPaidFallbackWindowV3(args.at(0)), paid::isPaidFallbackWindowV3(&args.at(0)), "isPaidFallbackWindowV3");
    if (fn == "isPaidFallbackUsageRollup") return same(paid::isPaidFallbackUsageRollup(args.at(0)), paid::isPaidFallbackUsageRollup(&args.at(0)), "isPaidFallbackUsageRollup");
    if (fn == "isBackfillableSettledPaidFallbackRequest")
      return same(paid::isBackfillableSettledPaidFallbackRequest(args.at(0)), paid::isBackfillableSettledPaidFallbackRequest(&args.at(0)), "isBackfillableSettledPaidFallbackRequest");
    if (fn == "isPaidFallbackReconciliationGate") {
      const bool result = same(paid::isPaidFallbackReconciliationGate(args.at(0)), paid::isPaidFallbackReconciliationGate(&args.at(0)), "isPaidFallbackReconciliationGate");
      const json::value entry = json::object{{"value", args.at(0)}, {"versionstamp", nullptr}};
      if (const std::optional<NativePaidRow<PaidFallbackReconciliationGateV3>> typed = typedRowOf<PaidFallbackReconciliationGateV3>(entry))
        same(paid::isPaidFallbackReconciliationGate(typed->value), result, "isPaidFallbackReconciliationGate typed");
      return result;
    }
    if (fn == "paidFallbackReconciliationGateDueNow" || fn == "paidFallbackReconciliationGateNeedsArm") {
      const bool dueNow = fn == "paidFallbackReconciliationGateDueNow";
      NativePaidRow<json::value> raw = rawRowOf(args.at(0));
      const double at = number(args.at(1));
      const auto call = [&](const auto& entry) {
        return dueNow ? recordJson(paid::paidFallbackReconciliationGateDueNow(entry, at)) : json::value(paid::paidFallbackReconciliationGateNeedsArm(entry, at));
      };
      const json::value result = call(raw);
      if (raw.value && raw.value->is_null()) {
        // null stored either as JSON null or as an absent optional
        raw.value = std::nullopt;
        if (!rt::deepEqual(call(raw), result)) throw std::logic_error("raw row null forms disagree");
      }
      if (const std::optional<NativePaidRow<PaidFallbackReconciliationGateV3>> typed = typedRowOf<PaidFallbackReconciliationGateV3>(args.at(0)))
        if (!rt::deepEqual(call(*typed), result)) throw std::logic_error("typed and raw gate rows disagree");
      return result;
    }
    if (fn == "isPositiveSafeInteger") {
      const bool result = same(paid::isPositiveSafeInteger(args.at(0)), paid::isPositiveSafeInteger(&args.at(0)), "isPositiveSafeInteger");
      if (const std::optional<double> value = rt::getNumber(args.at(0))) {
        same(paid::isPositiveSafeInteger(*value), result, "isPositiveSafeInteger(number)");
        same(paid::isPositiveSafeInteger(std::optional<double>(*value)), result, "isPositiveSafeInteger(number | null)");
      }
      if (args.at(0).is_null()) same(paid::isPositiveSafeInteger(std::optional<double>()), result, "isPositiveSafeInteger(null)");
      return result;
    }
    if (fn == "paidFallbackBackfillNeedsSweepV3")
      return same(paid::paidFallbackBackfillNeedsSweepV3(args.at(0), args.at(1), args.at(2)), paid::paidFallbackBackfillNeedsSweepV3(&args.at(0), &args.at(1), &args.at(2)),
                  "paidFallbackBackfillNeedsSweepV3");
    if (fn == "keys") {
      const std::string keyId = text(args.at(0));
      const std::string requestId = text(args.at(1));
      const double resetAtMs = number(args.at(2));
      return json::object{
        {"window", keyJson(paid::paidFallbackWindowV3Key(keyId, resetAtMs))},
        {"request", keyJson(paid::paidFallbackRequestV3Key(keyId, requestId))},
        {"pending", keyJson(paid::paidFallbackPendingV3Key(keyId, requestId))},
        {"windowPrefix", keyJson(paid::paidFallbackWindowV3Prefix(keyId))},
        {"requestPrefix", keyJson(paid::paidFallbackRequestV3Prefix(keyId))},
        {"requestGlobalPrefix", keyJson(paid::paidFallbackRequestV3GlobalPrefix)},
        {"windowGlobalPrefix", keyJson(paid::paidFallbackWindowV3GlobalPrefix)},
        {"pendingPrefix", keyJson(paid::paidFallbackPendingV3Prefix(keyId))},
        {"pendingGlobalPrefix", keyJson(paid::paidFallbackPendingV3GlobalPrefix)},
        {"gate", keyJson(paid::paidFallbackReconciliationGateV3Key())},
        {"lease", keyJson(paid::paidFallbackReconciliationLeaseV3Key(keyId))},
        {"backfillCursor", keyJson(paid::paidFallbackBackfillCursorV3Key())},
        {"backfillWindowCursor", keyJson(paid::paidFallbackBackfillWindowCursorV3Key())},
        {"backfillLease", keyJson(paid::paidFallbackBackfillLeaseV3Key())},
        {"backfillState", keyJson(paid::paidFallbackBackfillStateV3Key())},
        {"deletionGuard", keyJson(paid::paidFallbackDeletionGuardV3Key(keyId))},
        {"rollupPrefix", keyJson(paid::PAID_FALLBACK_USAGE_ROLLUP_PREFIX)},
        {"rollup", keyJson(paid::paidFallbackUsageRollupKey(resetAtMs, keyId, requestId, paid::paidFallbackUsageRollupShard(requestId)))},
      };
    }
    if (fn == "paidFallbackUsageRollupShard") return numberJson(paid::paidFallbackUsageRollupShard(text(args.at(0))));
    if (fn == "paidFallbackUsageRollupKey") return keyJson(paid::paidFallbackUsageRollupKey(number(args.at(0)), text(args.at(1)), text(args.at(2)), number(args.at(3))));
    if (fn == "requestRowExpireIn") return numberJson(paid::requestRowExpireIn(json::value_to<PaidFallbackRequestV3>(args.at(0)), number(args.at(1))));
    if (fn == "windowExpireIn") return numberJson(paid::windowExpireIn(json::value_to<PaidFallbackWindowV3>(args.at(0)), number(args.at(1))));
    if (fn == "planPaidFallbackAdmissionV3")
      return planJson(paid::planPaidFallbackAdmissionV3(admissionInputOf(args.at(0)), admissionSnapshotOf(args.at(1)), admissionPolicyOf(args.at(2)), number(args.at(3))));
    if (fn == "isBillableRequestV3") return paid::isBillableRequestV3(recordOrNull<PaidFallbackRequestV3>(args.at(0)));
    if (fn == "isInvalidAdmissionPolicy") return paid::isInvalidAdmissionPolicy(admissionInputOf(args.at(0)), args.at(1).as_bool(), numberOrNull(args.at(2)));
    if (fn == "planPaidFallbackLifecycleV3")
      return planOrNullJson(paid::planPaidFallbackLifecycleV3(json::value_to<PaidFallbackRequestV3>(args.at(0)), json::value_to<PaidFallbackRequestLifecyclePatchV3>(args.at(1)), number(args.at(2))));
    if (fn == "planPaidFallbackReleaseV3") return planOrNullJson(paid::planPaidFallbackReleaseV3(releaseSnapshotOf(args.at(0)), args.at(1).as_bool(), number(args.at(2))));
    if (fn == "planPaidFallbackDeferralV3")
      return planJson(paid::planPaidFallbackDeferralV3(recordOrNull<PaidFallbackRequestV3>(args.at(0)), recordOrNull<PaidFallbackPendingV3>(args.at(1)), number(args.at(2))));
    if (fn == "evaluatePaidFallbackSettlementV3")
      return planJson(paid::evaluatePaidFallbackSettlementV3(recordOrNull<PaidFallbackRequestV3>(args.at(0)), recordOrNull<PaidFallbackPendingV3>(args.at(1)),
                                                             json::value_to<NativePaidProviderLog>(args.at(2)), settlementMetadataOf(args.at(3))));
    if (fn == "paidFallbackRequestRollupIdentity") {
      const PaidFallbackRequestV3 request = json::value_to<PaidFallbackRequestV3>(args.at(0));
      return identityJson(args.size() > 1 ? paid::paidFallbackRequestRollupIdentity(request, text(args.at(1))) : paid::paidFallbackRequestRollupIdentity(request));
    }
    if (fn == "paidFallbackSettlementRollupV3")
      return planJson(paid::paidFallbackSettlementRollupV3(json::value_to<PaidFallbackRequestV3>(args.at(0)), json::value_to<NativePaidProviderLog>(args.at(1)), number(args.at(2)), text(args.at(3)),
                                                           recordOrNull<PaidFallbackUsageRollup>(args.at(4)), number(args.at(5))));
    if (fn == "mergePaidFallbackUsageRollup")
      return recordJson(paid::mergePaidFallbackUsageRollup(recordOrNull<PaidFallbackUsageRollup>(args.at(0)), json::value_to<PaidFallbackUsageRollupInput>(args.at(1))));
    if (fn == "planPaidFallbackSettlementRowsV3")
      return planJson(paid::planPaidFallbackSettlementRowsV3(settlementSnapshotOf(args.at(0)), json::value_to<NativePaidProviderLog>(args.at(1)), number(args.at(2)), args.at(3).as_bool(),
                                                             number(args.at(4))));
    if (fn == "planPaidFallbackBackfillFoldV3")
      return planJson(paid::planPaidFallbackBackfillFoldV3(json::value_to<PaidFallbackRequestV3>(args.at(0)), recordOrNull<PaidFallbackUsageRollup>(args.at(1)), number(args.at(2))));
    if (fn == "paidFallbackBackfillRollupEligibleV3") return paid::paidFallbackBackfillRollupEligibleV3(json::value_to<PaidFallbackRequestV3>(args.at(0)));
    if (fn == "paidFallbackRequestBackfillStateV3") return json::value(paid::paidFallbackRequestBackfillStateV3(recordOrNull<PaidFallbackRequestV3>(args.at(0))));
    if (fn == "planPaidFallbackDeletionGuardV3") return recordOrNullJson(paid::planPaidFallbackDeletionGuardV3(recordOrNull<PaidFallbackDeletionGuardV3>(args.at(0)), number(args.at(1))));
    if (fn == "planPaidFallbackDeletionV3") return planJson(paid::planPaidFallbackDeletionV3(recordOrNull<PaidFallbackOutstandingV3>(args.at(0))));
    if (fn == "paidFallbackLeaseActiveV3")
      return same(paid::paidFallbackLeaseActiveV3(recordOrNull<PaidFallbackReconciliationLeaseV3>(args.at(0)), number(args.at(1))),
                  paid::paidFallbackLeaseActiveV3(recordOrNull<PaidFallbackBackfillLeaseV3>(args.at(0)), number(args.at(1))), "paidFallbackLeaseActiveV3");
    if (fn == "paidFallbackLeaseOwnedV3")
      return same(paid::paidFallbackLeaseOwnedV3(recordOrNull<PaidFallbackReconciliationLeaseV3>(args.at(0)), text(args.at(1))),
                  paid::paidFallbackLeaseOwnedV3(recordOrNull<PaidFallbackBackfillLeaseV3>(args.at(0)), text(args.at(1))), "paidFallbackLeaseOwnedV3");
    if (fn == "paidFallbackReconciliationLeaseV3") return recordJson(paid::paidFallbackReconciliationLeaseV3(text(args.at(0)), number(args.at(1))));
    if (fn == "paidFallbackBackfillLeaseV3") return recordJson(paid::paidFallbackBackfillLeaseV3(text(args.at(0)), number(args.at(1))));
    if (fn == "paidFallbackGateAfterScanV3") return recordJson(paid::paidFallbackGateAfterScanV3(numberOrNull(args.at(0))));
    if (fn == "planPaidFallbackGateAfterScanV3") {
      const json::value result = planJson(paid::planPaidFallbackGateAfterScanV3(rowOf<PaidFallbackReconciliationGateV3>(args.at(0)), numberOrNull(args.at(1))));
      if (!rt::deepEqual(planJson(paid::planPaidFallbackGateAfterScanV3(rawRowOf(args.at(0)), numberOrNull(args.at(1)))), result)) throw std::logic_error("gate scan rows disagree");
      return result;
    }
    if (fn == "paidFallbackTerminalUsageV3") return usageOrNullJson(paid::paidFallbackTerminalUsageV3(json::value_to<TerminalUsageRollupInput>(args.at(0))));
    if (fn == "paidFallbackTerminalRollupV3")
      return recordJson(paid::paidFallbackTerminalRollupV3(json::value_to<TerminalUsageRollupInput>(args.at(0)), terminalUsageOf(args.at(1)), text(args.at(2)), number(args.at(3))));
    throw std::logic_error("no C++ call for " + std::string(fn));
  }

  json::value conformanceJson(const shared::PaidConformanceVectors& v) {
    json::array leases;
    leases.push_back(recordJson(std::get<0>(v.leases)));
    leases.push_back(recordJson(std::get<1>(v.leases)));
    leases.emplace_back(std::get<2>(v.leases));
    leases.emplace_back(std::get<3>(v.leases));
    json::array originalExpiry;
    for (const double value : v.originalExpiry) originalExpiry.push_back(numberJson(value));
    json::array gateDue;
    for (const PaidFallbackReconciliationGateV3& gate : v.gateDue) gateDue.push_back(recordJson(gate));
    json::array terminalSkipped;
    for (const std::optional<NativePaidTerminalUsage>& usage : v.terminalSkipped) terminalSkipped.push_back(usageOrNullJson(usage));
    return json::object{
      {"admitted", planJson(v.admitted)},
      {"partial", planJson(v.partial)},
      {"exhausted", planJson(v.exhausted)},
      {"invalidPolicy", v.invalidPolicy},
      {"unlimited", planJson(v.unlimited)},
      {"deletionGuard", planJson(v.deletionGuard)},
      {"reuse", planJson(v.reuse)},
      {"consumed", planJson(v.consumed)},
      {"dispatched", planJson(v.dispatched)},
      {"lifecycleReplay", planOrNullJson(v.lifecycleReplay)},
      {"cancelled", planOrNullJson(v.cancelled)},
      {"releaseIntentDefault", planOrNullJson(v.releaseIntentDefault)},
      {"releaseIntentKnownNoFetch", planOrNullJson(v.releaseIntentKnownNoFetch)},
      {"releaseCorrelated", planOrNullJson(v.releaseCorrelated)},
      {"releaseAfterSettlement", planOrNullJson(v.releaseAfterSettlement)},
      {"meteredEvaluation", planJson(v.meteredEvaluation)},
      {"rollup", planJson(v.rollup)},
      {"settled", planJson(v.settled)},
      {"settlementReplay", planJson(v.settlementReplay)},
      {"mismatchedMetered", planJson(v.mismatchedMetered)},
      {"missingPending", planJson(v.missingPending)},
      {"surplusExact", planJson(v.surplusExact)},
      {"surplusZero", planJson(v.surplusZero)},
      {"surplusCannotSettleMetered", planJson(v.surplusCannotSettleMetered)},
      {"invalidSpend", planJson(v.invalidSpend)},
      {"deferred", planJson(v.deferred)},
      {"deferralCleanup", planJson(v.deferralCleanup)},
      {"deferralNoop", planJson(v.deferralNoop)},
      {"originalExpiry", originalExpiry},
      {"gateDue", gateDue},
      {"guard", recordOrNullJson(v.guard)},
      {"guardReplay", recordOrNullJson(v.guardReplay)},
      {"deletion", planJson(v.deletion)},
      {"leases", leases},
      {"backfill", planJson(v.backfill)},
      {"backfillPending", planJson(v.backfillPending)},
      {"legacyMetered", identityJson(v.legacyMetered)},
      {"terminalRollup", recordJson(v.terminalRollup)},
      {"terminalSkipped", terminalSkipped},
      {"key", keyJson(v.key)},
      {"rollupKey", keyJson(v.rollupKey)},
      {"backfillStates", stringsJson(v.backfillStates)},
      {"gateScan", planJson(v.gateScan)},
      {"immutable", v.immutable},
    };
  }

  PaidFallbackRequestV3 pendingRequest(double reconciliationAttempts) {
    return {
      .key_id = "key",
      .request_id = "request",
      .policy_version = "policy-v3",
      .route = "responses",
      .path = "/v1/responses",
      .model = " model ",
      .stream = false,
      .reasoning = std::nullopt,
      .window_reset_at_ms = now + 60'000,
      .reserved_microcredits = 80,
      .quota_per_credit = 500'000,
      .provider_request_id = std::nullopt,
      .provider_quota = std::nullopt,
      .input_tokens = std::nullopt,
      .cached_input_tokens = nullptr,
      .output_tokens = std::nullopt,
      .dispatch_state = "reserved",
      .terminal_state = "pending",
      .spend_microcredits = std::nullopt,
      .billing_state = "pending",
      .reconciliation_attempts = reconciliationAttempts,
      .last_reconciliation_at_ms = std::nullopt,
      .dispatched_at_ms = std::nullopt,
      .terminal_at_ms = std::nullopt,
      .settled_at_ms = std::nullopt,
      .created_at_ms = now,
      .updated_at_ms = now,
    };
  }

  const PaidFallbackPendingV3 pending{.created_at_ms = now, .next_reconciliation_at_ms = now};
}

TEST("native-paid differential cases recorded from Bun") {
  int compared = 0;
  for (const NativePaidCase& testCase : kNativePaidCases) {
    try {
      const json::value args = rt::jsonParse(testCase.args);
      const json::value actual = run(testCase.fn, args.as_array());
      if (!sameJson(actual, testCase.out))
        rt::test::report(__FILE__, __LINE__, std::string(testCase.name) + "\n      actual:   " + rt::jsonStringify(actual) + "\n      expected: " + testCase.out, false);
    } catch (const std::exception& error) {
      rt::test::report(__FILE__, __LINE__, std::string(testCase.name) + ": threw " + error.what(), false);
    }
    ++compared;
  }
  std::printf("    %d cases compared\n", compared);
  EXPECT_EQ(compared, static_cast<int>(std::size(kNativePaidCases)));
}

TEST("the comparison rejects a changed result") {
  EXPECT(sameJson(rt::jsonParse(R"({"a":[1,2],"b":null})"), R"({"b":null,"a":[1,2]})"));
  EXPECT(!sameJson(json::value(true), "false"));
  EXPECT(!sameJson(rt::jsonParse(R"({"a":[1,2]})"), R"({"a":[1,3]})"));
  EXPECT(!sameJson(rt::jsonParse(R"({"a":1})"), R"({"a":1,"b":null})"));
  const NativePaidCase& first = kNativePaidCases[0];
  const json::value args = rt::jsonParse(first.args);
  EXPECT(sameJson(run(first.fn, args.as_array()), first.out));
  EXPECT(!sameJson(run(first.fn, args.as_array()), first.out == std::string_view("true") ? "false" : "true"));
}

TEST("conformance vectors equal JSON.stringify(runPaidConformanceVectors()) under Bun") {
  const json::value expected = rt::jsonParse(kConformanceVectorsJson);
  const json::value actual = rt::jsonParse(rt::jsonStringify(conformanceJson(shared::runPaidConformanceVectors())));
  ASSERT_EQ(actual.as_object().size(), expected.as_object().size());
  for (const json::key_value_pair& member : expected.as_object()) {
    const json::value* mine = rt::get(actual, member.key());
    if (mine == nullptr || !rt::deepEqual(*mine, member.value()))
      rt::test::report(__FILE__, __LINE__,
                       std::string(member.key()) + "\n      actual:   " + (mine ? rt::jsonStringify(*mine) : std::string("<missing>")) + "\n      expected: " + rt::jsonStringify(member.value()),
                       false);
  }
}

TEST("exported constants") {
  EXPECT_EQ(paid::MAX_CAS_ATTEMPTS, 128.0);
  EXPECT_EQ((std::vector<double>(paid::RETRY_DELAYS_MS.begin(), paid::RETRY_DELAYS_MS.end())), (std::vector<double>{5000, 30000, 120000, 600000, 3600000, 21600000}));
  EXPECT_EQ(paid::UNRESOLVED_AFTER_MS, 86'400'000.0);
  EXPECT_EQ(paid::RECONCILIATION_LEASE_MS, 60'000.0);
  EXPECT_EQ(paid::PAID_FALLBACK_BACKFILL_LEASE_MS, 300'000.0);
  EXPECT_EQ(paid::PAID_FALLBACK_AUTOMATIC_BACKFILL_LIMIT, 5'000.0);
  EXPECT_EQ(paid::PAID_FALLBACK_REQUEST_LOG_RETENTION_MS, 31'536'000'000.0);
  EXPECT_EQ(paid::PAID_FALLBACK_USAGE_ROLLUP_PREFIX, (NativePaidKey{"uos_ai", "paid_fallback", "v3", "usage_rollup"}));
  EXPECT_EQ(paid::PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS, 3'600'000.0);
  EXPECT_EQ(paid::PAID_FALLBACK_USAGE_ROLLUP_SHARD_COUNT, 16.0);
  EXPECT_EQ(paid::PAID_FALLBACK_SETTLED_PROVIDERS.size(), std::size_t{2});
  EXPECT(paid::PAID_FALLBACK_SETTLED_PROVIDERS.contains("metered"));
  EXPECT(paid::PAID_FALLBACK_SETTLED_PROVIDERS.contains("surplus"));
}

TEST("rollup collision pair of the native authority collision test") {
  // tests/native-authority.test.ts "native authority paid rollup collisions retain both requests
  // and cannot backfill twice" searches for the first two collision-<n> request ids whose
  // `${requestId}:deepseek_waterfall:openlux` shares a shard; Bun finds collision-3 and collision-10.
  std::unordered_map<double, std::string> shards;
  std::vector<std::string> pair;
  for (int index = 0; index < 100; index++) {
    const std::string requestId = "collision-" + rt::numberToString(index);
    const double shard = paid::paidFallbackUsageRollupShard(requestId + ":deepseek_waterfall:openlux");
    const auto previous = shards.find(shard);
    if (previous != shards.end() && !previous->second.empty()) {
      pair = {previous->second, requestId};
      break;
    }
    shards.insert_or_assign(shard, requestId);
  }
  EXPECT_EQ(pair, (std::vector<std::string>{"collision-3", "collision-10"}));
  EXPECT_EQ(paid::paidFallbackUsageRollupShard(""), 5.0);
}

TEST("undefined, NaN, Infinity and -0 inputs") {
  const json::value backfillState = rt::jsonParse(R"({"v":1,"completed_at_ms":1700000000000})");
  const json::value null = nullptr;
  // paidFallbackBackfillNeedsSweepV3(state, undefined, null) and (undefined, null, null): true.
  EXPECT(paid::paidFallbackBackfillNeedsSweepV3(&backfillState, nullptr, &null));
  EXPECT(paid::paidFallbackBackfillNeedsSweepV3(nullptr, &null, &null));
  EXPECT(!paid::paidFallbackBackfillNeedsSweepV3(&backfillState, &null, &null));

  // paidFallbackReconciliationGateDueNow({ value: undefined }, now): { next_due_at_ms: now }.
  EXPECT_EQ(paid::paidFallbackReconciliationGateDueNow(NativePaidRow<json::value>{}, now).next_due_at_ms, std::optional<double>(now));
  // Due -0 at now 0 stays -0 (Object.is(result, -0) in Bun).
  const NativePaidRow<PaidFallbackReconciliationGateV3> negativeZero{.value = PaidFallbackReconciliationGateV3{.next_due_at_ms = -0.0}, .versionstamp = std::nullopt};
  const std::optional<double> typedDue = paid::paidFallbackReconciliationGateDueNow(negativeZero, 0).next_due_at_ms;
  ASSERT(typedDue);
  EXPECT(*typedDue == 0 && std::signbit(*typedDue));
  const NativePaidRow<json::value> rawNegativeZero{.value = rt::jsonParse(R"({"next_due_at_ms":-0})"), .versionstamp = std::nullopt};
  const std::optional<double> rawDue = paid::paidFallbackReconciliationGateDueNow(rawNegativeZero, 0).next_due_at_ms;
  ASSERT(rawDue);
  EXPECT(*rawDue == 0 && std::signbit(*rawDue));
  // Math.min(5, NaN): NaN.
  const NativePaidRow<PaidFallbackReconciliationGateV3> five{.value = PaidFallbackReconciliationGateV3{.next_due_at_ms = 5}, .versionstamp = std::nullopt};
  const std::optional<double> nanDue = paid::paidFallbackReconciliationGateDueNow(five, rt::Number::NaN).next_due_at_ms;
  ASSERT(nanDue);
  EXPECT(std::isnan(*nanDue));

  // reconciliation_attempts -1 and 2.5: RETRY_DELAYS_MS[-1] / [2.5] are undefined in Bun (delay
  // undefined, next_reconciliation_at_ms NaN); the port's delay is NaN.
  for (const auto& [attempts, recorded] : {std::pair<double, double>{-1, 0}, std::pair<double, double>{2.5, 3.5}}) {
    const NativePaidDeferralPlan plan = paid::planPaidFallbackDeferralV3(pendingRequest(attempts), pending, now);
    const NativePaidDeferralPlanWrite* write = std::get_if<NativePaidDeferralPlanWrite>(&plan);
    ASSERT(write != nullptr);
    EXPECT(std::isnan(write->delay));
    EXPECT(std::isnan(write->pending.next_reconciliation_at_ms));
    EXPECT_EQ(write->request.reconciliation_attempts, recorded);
  }
  // Infinity attempts: Math.min(Infinity, 5) is the last delay.
  const NativePaidDeferralPlan infinite = paid::planPaidFallbackDeferralV3(pendingRequest(rt::Number::POSITIVE_INFINITY), pending, now);
  ASSERT(std::holds_alternative<NativePaidDeferralPlanWrite>(infinite));
  EXPECT_EQ(std::get<NativePaidDeferralPlanWrite>(infinite).delay, 21'600'000.0);
  EXPECT_EQ(std::get<NativePaidDeferralPlanWrite>(infinite).pending.next_reconciliation_at_ms, 1'700'021'600'000.0);

  // Math.round(-0.5) is -0, which is a safe integer and not < 0: settles -0.
  PaidFallbackRequestV3 halfCredit = pendingRequest(0);
  halfCredit.provider_request_id = "provider-id";
  halfCredit.quota_per_credit = 2;
  const NativePaidProviderLog minusOne{.request_id = "provider-id", .quota = -1, .prompt_tokens = 12, .cached_prompt_tokens = 3, .completion_tokens = 4, .model = "model", .created_at = now / 1000};
  const NativePaidSettlementEvaluation minusHalf = paid::evaluatePaidFallbackSettlementV3(halfCredit, pending, minusOne, NativePaidSettlementMetadata{.correlation = "provider_request_id", .microcreditsPerCredit = 1});
  ASSERT(std::holds_alternative<NativePaidSettlementEvaluationSettle>(minusHalf));
  EXPECT(std::signbit(std::get<NativePaidSettlementEvaluationSettle>(minusHalf).spend));
  PaidFallbackRequestV3 correlated = pendingRequest(0);
  correlated.provider_request_id = "provider-id";
  NativePaidProviderLog infiniteQuota = minusOne;
  infiniteQuota.quota = rt::Number::POSITIVE_INFINITY;
  EXPECT(paid::evaluatePaidFallbackSettlementV3(correlated, pending, infiniteQuota, NativePaidSettlementMetadata{.correlation = "provider_request_id", .microcreditsPerCredit = 1'000'000}) ==
         NativePaidSettlementEvaluation(NativePaidSettlementEvaluationDefer{}));

  // request_created_at_ms NaN / Infinity fall back to now; -0.5 truncates to -0 and Math.max(0, -0) is +0.
  const NativePaidTerminalUsage usage{.model = "model", .provider = "chatgpt_codex", .inputTokens = 12, .outputTokens = 4};
  TerminalUsageRollupInput terminal{
    .model = " model ",
    .provider = " chatgpt_codex ",
    .request_id = "terminal",
    .request_created_at_ms = rt::Number::NaN,
    .input_tokens = 12,
    .cached_input_tokens = 3,
    .output_tokens = 4,
  };
  EXPECT_EQ(paid::paidFallbackTerminalRollupV3(terminal, usage, "t", now).request_created_at_ms, now);
  terminal.request_created_at_ms = rt::Number::POSITIVE_INFINITY;
  EXPECT_EQ(paid::paidFallbackTerminalRollupV3(terminal, usage, "t", now).request_created_at_ms, now);
  terminal.request_created_at_ms = -0.5;
  const double created = paid::paidFallbackTerminalRollupV3(terminal, usage, "t", now).request_created_at_ms;
  EXPECT(created == 0 && !std::signbit(created));
  // An input_tokens of -0 is an observed count of -0.
  terminal.input_tokens = -0.0;
  const std::optional<NativePaidTerminalUsage> negativeZeroTokens = paid::paidFallbackTerminalUsageV3(terminal);
  ASSERT(negativeZeroTokens);
  EXPECT(std::signbit(negativeZeroTokens->inputTokens));

  // A window whose settled_microcredits is -0 is valid; NaN and Infinity are not positive safe
  // integers; a rollup with an infinite quota_sum is not a rollup.
  EXPECT(paid::isPaidFallbackWindowV3(rt::jsonParse(
    R"({"v":3,"key_id":"key","policy_version":"policy-v3","window_reset_at_ms":1700000060000,"limit_microcredits":100,"settled_microcredits":-0,"reserved_microcredits":0,"pending_count":0,"updated_at_ms":1700000000000})")));
  EXPECT(!paid::isPositiveSafeInteger(rt::Number::NaN));
  EXPECT(!paid::isPositiveSafeInteger(rt::makeNumber(rt::Number::NaN)));
  EXPECT(!paid::isPositiveSafeInteger(rt::Number::POSITIVE_INFINITY));
  EXPECT(!paid::isPositiveSafeInteger(rt::makeNumber(rt::Number::POSITIVE_INFINITY)));
  json::value infiniteRollup = rt::jsonParse(
    R"({"v":1,"bucket_start_at_ms":1699999200000,"model":"model","provider":"metered","request_count":2,"quota_sum":10,"input_tokens":20,"cached_input_tokens":5,"output_tokens":7,"spend_microcredits":30,"first_request_at_ms":1699999999000,"last_request_at_ms":1699999999500,"updated_at_ms":1699999999500})");
  EXPECT(paid::isPaidFallbackUsageRollup(infiniteRollup));
  rt::objectSet(infiniteRollup.as_object(), "quota_sum", rt::makeNumber(rt::Number::POSITIVE_INFINITY));
  EXPECT(!paid::isPaidFallbackUsageRollup(infiniteRollup));

  // requestRowExpireIn(row, NaN): Math.max(1, NaN) is NaN.
  EXPECT(std::isnan(paid::requestRowExpireIn(pendingRequest(0), rt::Number::NaN)));

  // { provider_request_id: undefined } changes nothing; a NaN attempt count never equals itself.
  EXPECT(!paid::planPaidFallbackLifecycleV3(pendingRequest(0), PaidFallbackRequestLifecyclePatchV3{.provider_request_id = rt::undefined}, now));
  const std::optional<NativePaidLifecyclePlan> nanAttempts = paid::planPaidFallbackLifecycleV3(pendingRequest(rt::Number::NaN), PaidFallbackRequestLifecyclePatchV3{}, now);
  ASSERT(nanAttempts);
  EXPECT(std::isnan(nanAttempts->request.reconciliation_attempts));
}

TEST("settlement rows require the request row") {
  const NativePaidSettlementSnapshot missing{
    .request = NativePaidRow<PaidFallbackRequestV3>{.value = std::nullopt, .versionstamp = std::nullopt},
    .pending = NativePaidRow<PaidFallbackPendingV3>{.value = pending, .versionstamp = "v"},
    .gate = NativePaidRow<PaidFallbackReconciliationGateV3>{.value = std::nullopt, .versionstamp = std::nullopt},
    .window = NativePaidRow<PaidFallbackWindowV3>{.value = std::nullopt, .versionstamp = std::nullopt},
    .rollup = NativePaidRow<PaidFallbackUsageRollup>{.value = std::nullopt, .versionstamp = std::nullopt},
  };
  const NativePaidProviderLog log{.request_id = "provider-id", .quota = 1, .prompt_tokens = 0, .completion_tokens = 0, .model = "model", .created_at = 0};
  EXPECT_THROWS_MESSAGE(paid::planPaidFallbackSettlementRowsV3(missing, log, 1, false, now), rt::JsError, "Paid fallback settlement request is missing");
  try {
    (void)paid::planPaidFallbackSettlementRowsV3(missing, log, 1, false, now);
  } catch (const rt::JsError& error) {
    EXPECT_EQ(error.name, "Error");
  }
}
