#include "uos/authority/native_paid.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::authority::native_paid {
  using namespace native_paid_contracts;

  namespace {
    const NativePaidKey PREFIX{"uos_ai", "paid_fallback", "v3"};
  }

  double requestRowExpireIn(const PaidFallbackRequestV3& row, double nowMs) {
    return rt::Math::max(1, row.created_at_ms + PAID_FALLBACK_REQUEST_LOG_RETENTION_MS - nowMs);
  }

  namespace {
    bool isSafeUsageCount(const json::value* value) { return value != nullptr && rt::Number::isSafeInteger(*value) && *rt::getNumber(*value) >= 0; }
  }

  bool isPositiveSafeInteger(const json::value& value) { return rt::Number::isSafeInteger(value) && *rt::getNumber(value) > 0; }

  bool isPositiveSafeInteger(const json::value* value) { return value != nullptr && isPositiveSafeInteger(*value); }

  bool isPositiveSafeInteger(double value) { return rt::Number::isSafeInteger(value) && value > 0; }

  bool isPositiveSafeInteger(const std::optional<double>& value) { return value && isPositiveSafeInteger(*value); }

  namespace {
    bool isApiKeyId(const json::value* value) {
      const std::optional<std::string_view> text = rt::getString(value);
      return text && rt::length(*text) > 0 && rt::length(*text) <= 200;
    }
  }

  double windowExpireIn(const PaidFallbackWindowV3& window, double nowMs) {
    return rt::Math::max(1, window.window_reset_at_ms + PAID_FALLBACK_REQUEST_LOG_RETENTION_MS - nowMs);
  }

  bool isPaidFallbackWindowV3(const json::value& value) {
    if (!utils::isRecord(value)) return false;
    const std::optional<std::string_view> policyVersion = rt::getString(rt::get(value, "policy_version"));
    const json::value* limitMicrocredits = rt::get(value, "limit_microcredits");
    return rt::getNumber(rt::get(value, "v")) == 3.0 && isApiKeyId(rt::get(value, "key_id")) && policyVersion && rt::length(*policyVersion) > 0 &&
           isPositiveSafeInteger(rt::get(value, "window_reset_at_ms")) && (rt::getNumber(limitMicrocredits) == -1.0 || isPositiveSafeInteger(limitMicrocredits)) &&
           isSafeUsageCount(rt::get(value, "settled_microcredits")) && isSafeUsageCount(rt::get(value, "reserved_microcredits")) && isSafeUsageCount(rt::get(value, "pending_count")) &&
           isPositiveSafeInteger(rt::get(value, "updated_at_ms"));
  }

  bool isPaidFallbackWindowV3(const json::value* value) { return value != nullptr && isPaidFallbackWindowV3(*value); }

  NativePaidKey paidFallbackWindowV3Key(std::string_view keyId, double resetAtMs) { return NativePaidKey{PREFIX, "window", keyId, resetAtMs}; }
  NativePaidKey paidFallbackRequestV3Key(std::string_view keyId, std::string_view requestId) { return NativePaidKey{PREFIX, "request", keyId, requestId}; }
  NativePaidKey paidFallbackPendingV3Key(std::string_view keyId, std::string_view requestId) { return NativePaidKey{PREFIX, "pending", keyId, requestId}; }
  NativePaidKey paidFallbackWindowV3Prefix(std::string_view keyId) { return NativePaidKey{PREFIX, "window", keyId}; }
  NativePaidKey paidFallbackRequestV3Prefix(std::string_view keyId) { return NativePaidKey{PREFIX, "request", keyId}; }
  NativePaidKey paidFallbackPendingV3Prefix(std::string_view keyId) { return NativePaidKey{PREFIX, "pending", keyId}; }
  NativePaidKey paidFallbackReconciliationGateV3Key() { return NativePaidKey{PREFIX, "reconciliation_gate"}; }
  NativePaidKey paidFallbackReconciliationLeaseV3Key(std::string_view keyId) { return NativePaidKey{PREFIX, "reconciliation_lease", keyId}; }
  NativePaidKey paidFallbackBackfillCursorV3Key() { return NativePaidKey{PREFIX, "rollup_backfill_cursor"}; }
  NativePaidKey paidFallbackBackfillWindowCursorV3Key() { return NativePaidKey{PREFIX, "rollup_backfill_window_cursor"}; }
  NativePaidKey paidFallbackBackfillLeaseV3Key() { return NativePaidKey{PREFIX, "rollup_backfill_lease"}; }
  NativePaidKey paidFallbackBackfillStateV3Key() { return NativePaidKey{PREFIX, "rollup_backfill_state"}; }
  NativePaidKey paidFallbackDeletionGuardV3Key(std::string_view keyId) { return NativePaidKey{PREFIX, "deletion_guard", keyId}; }

  bool isPaidFallbackReconciliationGate(const json::value& value) {
    const json::object* object = value.if_object();
    if (object == nullptr) return false;
    const json::value* nextDueAtMs = rt::get(*object, "next_due_at_ms");
    if (nextDueAtMs == nullptr) return false;
    return nextDueAtMs->is_null() || (rt::Number::isSafeInteger(*nextDueAtMs) && *rt::getNumber(*nextDueAtMs) >= 0);
  }

  bool isPaidFallbackReconciliationGate(const json::value* value) { return value != nullptr && isPaidFallbackReconciliationGate(*value); }

  bool isPaidFallbackReconciliationGate(const std::optional<PaidFallbackReconciliationGateV3>& value) {
    if (!value) return false;
    return !value->next_due_at_ms || (rt::Number::isSafeInteger(*value->next_due_at_ms) && *value->next_due_at_ms >= 0);
  }

  PaidFallbackReconciliationGateV3 paidFallbackReconciliationGateDueNow(const NativePaidRow<PaidFallbackReconciliationGateV3>& entry, double now) {
    const std::optional<double> current = isPaidFallbackReconciliationGate(entry.value) ? entry.value->next_due_at_ms : std::nullopt;
    return {.next_due_at_ms = !current ? now : rt::Math::min(*current, now)};
  }

  PaidFallbackReconciliationGateV3 paidFallbackReconciliationGateDueNow(const NativePaidRow<json::value>& entry, double now) {
    const json::value* value = entry.value ? &*entry.value : nullptr;
    const std::optional<double> current = isPaidFallbackReconciliationGate(value) ? rt::getNumber(rt::get(value, "next_due_at_ms")) : std::nullopt;
    return {.next_due_at_ms = !current ? now : rt::Math::min(*current, now)};
  }

  bool paidFallbackReconciliationGateNeedsArm(const NativePaidRow<PaidFallbackReconciliationGateV3>& entry, double now) {
    if (!isPaidFallbackReconciliationGate(entry.value)) return true;
    return !entry.value->next_due_at_ms || *entry.value->next_due_at_ms > now;
  }

  bool paidFallbackReconciliationGateNeedsArm(const NativePaidRow<json::value>& entry, double now) {
    const json::value* value = entry.value ? &*entry.value : nullptr;
    if (!isPaidFallbackReconciliationGate(value)) return true;
    const std::optional<double> nextDueAtMs = rt::getNumber(rt::get(value, "next_due_at_ms"));
    return !nextDueAtMs || *nextDueAtMs > now;
  }

  namespace {
    PaidFallbackWindowV3 defaultPaidFallbackWindowV3(const NativePaidAdmissionInput& input) {
      return {
        .v = 3,
        .key_id = input.keyId,
        .policy_version = input.policyVersion,
        .window_reset_at_ms = input.windowResetAtMs,
        .limit_microcredits = input.limitMicrocredits,
        .settled_microcredits = input.initialSettledMicrocredits,
        .reserved_microcredits = 0,
        .pending_count = 0,
        .updated_at_ms = input.createdAtMs,
      };
    }

    /** The row written for one admitted request. */
    PaidFallbackRequestV3 buildPaidFallbackRequestV3(const NativePaidAdmissionInput& input, double reservation, double now) {
      const bool dispatchIntent = rt::truthy(input.dispatchIntent);
      return {
        .v = 3,
        .key_id = input.keyId,
        .request_id = input.requestId,
        .policy_version = input.policyVersion,
        .route = input.route,
        .path = input.path,
        .model = input.model,
        .stream = input.stream,
        .reasoning = input.reasoning,
        .window_reset_at_ms = input.windowResetAtMs,
        .reserved_microcredits = reservation,
        .quota_per_credit = input.quotaPerCredit,
        .provider_request_id = std::nullopt,
        .provider_quota = std::nullopt,
        .input_tokens = std::nullopt,
        .cached_input_tokens = nullptr,
        .output_tokens = std::nullopt,
        .dispatch_state = dispatchIntent ? "dispatched" : "reserved",
        .terminal_state = "pending",
        .spend_microcredits = std::nullopt,
        .billing_state = "pending",
        .reconciliation_attempts = 0,
        .last_reconciliation_at_ms = std::nullopt,
        .dispatched_at_ms = dispatchIntent ? std::optional<double>(now) : std::nullopt,
        .terminal_at_ms = std::nullopt,
        .settled_at_ms = std::nullopt,
        .created_at_ms = input.createdAtMs,
        .updated_at_ms = now,
      };
    }

    PaidFallbackAdmissionV3 existingReservationV3(const NativePaidAdmissionInput& input, const PaidFallbackRequestV3& existing) {
      return {
        .key_id = input.keyId,
        .request_id = input.requestId,
        .created_at_ms = existing.created_at_ms,
        .reserved_microcredits = existing.reserved_microcredits,
        .quota_per_credit = input.quotaPerCredit,
        .window_reset_at_ms = existing.window_reset_at_ms,
        .quota_used_percent = std::nullopt,
      };
    }
  }

  bool isInvalidAdmissionPolicy(const NativePaidAdmissionInput& input, bool unlimited, const std::optional<double>& maximumExposure) {
    return (!unlimited && !isPositiveSafeInteger(input.limitMicrocredits)) || !rt::Number::isSafeInteger(input.initialSettledMicrocredits) || input.initialSettledMicrocredits < 0 ||
           !isPositiveSafeInteger(input.quotaPerCredit) || (!unlimited && !isPositiveSafeInteger(maximumExposure));
  }

  namespace {
    ResolvedRequestLifecycleV3 resolveRequestLifecyclePatchV3(const PaidFallbackRequestV3& current, const PaidFallbackRequestLifecyclePatchV3& patch, double now) {
      const std::optional<PaidFallbackProvider> provider = current.provider ? current.provider : patch.provider;
      const std::optional<std::string> providerRequestId = current.provider_request_id ? current.provider_request_id : patch.provider_request_id.toOptional();
      const std::string dispatchState = current.dispatch_state == "reserved" && patch.dispatch_state ? *patch.dispatch_state : current.dispatch_state;
      const std::string terminalState = current.terminal_state == "pending" && patch.terminal_state ? *patch.terminal_state : current.terminal_state;
      const bool shouldIncrementReconciliationAttempts = rt::truthy(patch.increment_reconciliation_attempts) && (!patch.terminal_state || current.terminal_state == "pending");
      const double reconciliationAttempts = shouldIncrementReconciliationAttempts ? current.reconciliation_attempts + 1 : patch.reconciliation_attempts.value_or(current.reconciliation_attempts);
      const std::optional<double> lastReconciliationAtMs =
        shouldIncrementReconciliationAttempts ? std::optional<double>(now) : (patch.last_reconciliation_at_ms.hasValue() ? std::optional<double>(*patch.last_reconciliation_at_ms) : current.last_reconciliation_at_ms);
      return {
        .provider = provider,
        .providerRequestId = providerRequestId,
        .dispatchState = dispatchState,
        .terminalState = terminalState,
        .reconciliationAttempts = reconciliationAttempts,
        .lastReconciliationAtMs = lastReconciliationAtMs,
      };
    }

    /** True when a resolved patch differs from the stored row. */
    bool requestLifecycleChangedV3(const PaidFallbackRequestV3& current, const ResolvedRequestLifecycleV3& resolved) {
      return resolved.providerRequestId != current.provider_request_id || resolved.provider != current.provider || resolved.dispatchState != current.dispatch_state ||
             resolved.terminalState != current.terminal_state || resolved.reconciliationAttempts != current.reconciliation_attempts ||
             resolved.lastReconciliationAtMs != current.last_reconciliation_at_ms;
    }

    /** Builds the row a resolved lifecycle patch writes. */
    PaidFallbackRequestV3 buildRequestLifecycleRowV3(const PaidFallbackRequestV3& current, const ResolvedRequestLifecycleV3& resolved, double now) {
      const std::optional<PaidFallbackProvider>& provider = resolved.provider;
      PaidFallbackRequestV3 row = current;
      if (provider) row.provider = provider;
      row.provider_request_id = resolved.providerRequestId;
      row.dispatch_state = resolved.dispatchState;
      row.terminal_state = resolved.terminalState;
      row.reconciliation_attempts = resolved.reconciliationAttempts;
      row.last_reconciliation_at_ms = resolved.lastReconciliationAtMs;
      row.dispatched_at_ms = resolved.dispatchState == "dispatched" && !current.dispatched_at_ms ? std::optional<double>(now) : current.dispatched_at_ms;
      row.terminal_at_ms = resolved.terminalState != "pending" && !current.terminal_at_ms ? std::optional<double>(now) : current.terminal_at_ms;
      row.updated_at_ms = now;
      return row;
    }

    // `correlation: "provider_request_id" | "surplus_reservation"`
    bool correlationMatchesV3(const PaidFallbackRequestV3& request, const NativePaidProviderLog& providerLog, std::string_view correlation) {
      return correlation == "surplus_reservation" ? request.provider == "surplus" : request.provider_request_id == providerLog.request_id;
    }
  }

  bool isBillableRequestV3(const std::optional<PaidFallbackRequestV3>& request) {
    return request && request->billing_state != "settled" && request->billing_state != "not_billed";
  }

  namespace {
    PaidFallbackRequestV3 buildSettledRequestRowV3(const PaidFallbackRequestV3& request, const NativePaidProviderLog& providerLog, double spend, double dispatchedAtMs, bool foldIntoRollup,
                                                   double now) {
      PaidFallbackRequestV3 row = request;
      row.provider_quota = providerLog.quota;
      row.input_tokens = providerLog.prompt_tokens;
      row.cached_input_tokens = providerLog.cached_prompt_tokens ? rt::Nullable<double>(*providerLog.cached_prompt_tokens) : rt::Nullable<double>(nullptr);
      row.output_tokens = providerLog.completion_tokens;
      row.dispatch_state = request.dispatch_state == "reserved" ? "dispatched" : request.dispatch_state;
      row.dispatched_at_ms = dispatchedAtMs;
      row.spend_microcredits = spend;
      row.billing_state = "settled";
      // The terminal lifecycle update records the attempt that triggered this
      // lookup. Settlement itself must be replay-idempotent and not inflate
      // the attempt count when a queue delivery is duplicated.
      row.reconciliation_attempts = request.reconciliation_attempts;
      row.last_reconciliation_at_ms = now;
      row.settled_at_ms = request.settled_at_ms.value_or(now);
      row.updated_at_ms = now;
      // The settlement write just folded this usage into the hourly rollup;
      // mark it so a later backfill run cannot double-count it.
      if (foldIntoRollup) row.usage_rollup_at_ms = now;
      return row;
    }

    constexpr double FNV_PRIME = 0x01000193;
  }

  double paidFallbackUsageRollupShard(std::string_view requestId) {
    double hash = 0x811c9dc5;
    for (const char16_t unit : rt::toUtf16(requestId)) {
      hash = rt::bitXor(hash, unit);
      hash = rt::toUint32(rt::Math::imul(hash, FNV_PRIME));
    }
    return rt::mod(hash, PAID_FALLBACK_USAGE_ROLLUP_SHARD_COUNT);
  }

  namespace {
    bool safeInteger(const json::value* value, double min = 0) { return value != nullptr && rt::Number::isSafeInteger(*value) && *rt::getNumber(*value) >= min; }

    bool nonNegativeFinite(const json::value* value) { return value != nullptr && rt::Number::isFinite(*value) && *rt::getNumber(*value) >= 0; }
  }

  bool isPaidFallbackUsageRollup(const json::value& value) {
    if (!utils::isRecord(value)) return false;
    const std::optional<std::string_view> model = rt::getString(rt::get(value, "model"));
    const std::optional<std::string_view> provider = rt::getString(rt::get(value, "provider"));
    return rt::getNumber(rt::get(value, "v")) == 1.0 && safeInteger(rt::get(value, "bucket_start_at_ms")) && model && rt::length(*model) > 0 && provider && rt::length(*provider) > 0 &&
           safeInteger(rt::get(value, "request_count")) && nonNegativeFinite(rt::get(value, "quota_sum")) && safeInteger(rt::get(value, "input_tokens")) &&
           safeInteger(rt::get(value, "cached_input_tokens")) && safeInteger(rt::get(value, "output_tokens")) && nonNegativeFinite(rt::get(value, "spend_microcredits")) &&
           safeInteger(rt::get(value, "first_request_at_ms")) && safeInteger(rt::get(value, "last_request_at_ms")) && safeInteger(rt::get(value, "updated_at_ms"));
  }

  bool isPaidFallbackUsageRollup(const json::value* value) { return value != nullptr && isPaidFallbackUsageRollup(*value); }

  NativePaidKey paidFallbackUsageRollupKey(double bucketStartAtMs, std::string_view model, std::string_view provider, double shard) {
    return NativePaidKey{PAID_FALLBACK_USAGE_ROLLUP_PREFIX, bucketStartAtMs, model, provider, shard};
  }

  PaidFallbackUsageRollup mergePaidFallbackUsageRollup(const std::optional<PaidFallbackUsageRollup>& existing, const PaidFallbackUsageRollupInput& input) {
    const double firstRequestAtMs = existing ? rt::Math::min(existing->first_request_at_ms, input.request_created_at_ms) : input.request_created_at_ms;
    const double lastRequestAtMs = existing ? rt::Math::max(existing->last_request_at_ms, input.request_created_at_ms) : input.request_created_at_ms;
    return {
      .v = 1,
      .bucket_start_at_ms = existing ? existing->bucket_start_at_ms : input.bucket_start_at_ms,
      .model = existing ? existing->model : input.model,
      .provider = existing ? existing->provider : input.provider,
      .request_count = (existing ? existing->request_count : 0) + 1,
      .quota_sum = (existing ? existing->quota_sum : 0) + input.quota,
      .input_tokens = (existing ? existing->input_tokens : 0) + input.input_tokens,
      .cached_input_tokens = (existing ? existing->cached_input_tokens : 0) + input.cached_input_tokens.value_or(0),
      .output_tokens = (existing ? existing->output_tokens : 0) + input.output_tokens,
      .spend_microcredits = (existing ? existing->spend_microcredits : 0) + input.spend_microcredits,
      .first_request_at_ms = firstRequestAtMs,
      .last_request_at_ms = lastRequestAtMs,
      .updated_at_ms = input.updated_at_ms,
    };
  }

  namespace {
    // `value: unknown`; every caller passes a `number | null` field.
    std::optional<double> observedTokenCount(const std::optional<double>& value) {
      if (!value || !rt::Number::isSafeInteger(*value) || *value < 0) return std::nullopt;
      return value;
    }

    bool isPaidFallbackBackfillState(const json::value* value) {
      if (!utils::isRecord(value)) return false;
      const json::value* completedAtMs = rt::get(value, "completed_at_ms");
      return rt::getNumber(rt::get(value, "v")) == 1.0 && completedAtMs != nullptr && rt::Number::isSafeInteger(*completedAtMs) && *rt::getNumber(*completedAtMs) >= 0;
    }
  }

  bool isBackfillableSettledPaidFallbackRequest(const json::value& value) {
    if (!utils::isRecord(value)) return false;
    const std::optional<std::string_view> model = rt::getString(rt::get(value, "model"));
    const std::optional<double> providerQuota = rt::getNumber(rt::get(value, "provider_quota"));
    const std::optional<double> spendMicrocredits = rt::getNumber(rt::get(value, "spend_microcredits"));
    return rt::getString(rt::get(value, "billing_state")) == "settled" && model && rt::length(rt::trim(*model)) > 0 && providerQuota && rt::Number::isFinite(*providerQuota) &&
           *providerQuota >= 0 && spendMicrocredits && rt::Number::isFinite(*spendMicrocredits) && *spendMicrocredits >= 0;
  }

  bool isBackfillableSettledPaidFallbackRequest(const json::value* value) { return value != nullptr && isBackfillableSettledPaidFallbackRequest(*value); }

  namespace {
    NativePaidAdmissionPlanWrite buildPaidFallbackAdmissionPlanV3(const NativePaidAdmissionInput& input, const NativePaidAdmissionSnapshot& snapshot, const NativePaidAdmissionPolicy& policy,
                                                                  const PaidFallbackWindowV3& transitioned, double reservation, double now) {
      PaidFallbackRequestV3 request = buildPaidFallbackRequestV3(input, reservation, now);
      const double requestExpireIn = requestRowExpireIn(request, now);
      std::optional<PaidFallbackWindowV3> window;
      if (!policy.unlimited) {
        PaidFallbackWindowV3 reserved = transitioned;
        reserved.reserved_microcredits = transitioned.reserved_microcredits + reservation;
        reserved.pending_count = transitioned.pending_count + 1;
        reserved.updated_at_ms = now;
        window = std::move(reserved);
      }
      std::vector<std::string> checks{"request", "deletionGuard"};
      if (snapshot.policyFence) checks.emplace_back("policy");
      if (rt::truthy(input.dispatchIntent)) checks.emplace_back("gate");
      if (!policy.unlimited) checks.emplace_back("window");
      return {
        .request = std::move(request),
        .pending = {.created_at_ms = now, .next_reconciliation_at_ms = now},
        .window = std::move(window),
        .reservation =
          {
            .key_id = input.keyId,
            .request_id = input.requestId,
            .created_at_ms = input.createdAtMs,
            .reserved_microcredits = reservation,
            .quota_per_credit = input.quotaPerCredit,
            .window_reset_at_ms = input.windowResetAtMs,
            .quota_used_percent = policy.unlimited ? std::nullopt : std::optional<double>((100 * transitioned.settled_microcredits) / transitioned.limit_microcredits),
          },
        .checkGate = rt::truthy(input.dispatchIntent),
        .checkPolicy = snapshot.policyFence.has_value(),
        .requestExpireIn = requestExpireIn,
        .windowExpireIn = policy.unlimited ? std::nullopt : std::optional<double>(windowExpireIn(transitioned, now)),
        .checks = std::move(checks),
      };
    }
  }

  NativePaidAdmissionPlan planPaidFallbackAdmissionV3(const NativePaidAdmissionInput& input, const NativePaidAdmissionSnapshot& snapshot, const NativePaidAdmissionPolicy& policy, double now) {
    if (snapshot.deletionGuard.value) return NativePaidAdmissionPlanBlocked{.reason = "invalid_policy"};
    const std::optional<PaidFallbackRequestV3>& existing = snapshot.request.value;
    if (existing) {
      if (existing->billing_state == "settled" || existing->billing_state == "not_billed") return NativePaidAdmissionPlanBlocked{.reason = "invalid_policy"};
      return NativePaidAdmissionPlanReserved{.reservation = existingReservationV3(input, *existing)};
    }
    const PaidFallbackWindowV3 current = snapshot.window.value ? *snapshot.window.value : defaultPaidFallbackWindowV3(input);
    const bool policyChanged = !policy.unlimited && (current.policy_version != input.policyVersion || current.limit_microcredits != input.limitMicrocredits);
    PaidFallbackWindowV3 transitioned = current;
    if (policyChanged) {
      transitioned.policy_version = input.policyVersion;
      transitioned.limit_microcredits = input.limitMicrocredits;
      transitioned.updated_at_ms = now;
    }
    const double remaining = policy.unlimited ? 0 : transitioned.limit_microcredits - transitioned.settled_microcredits - transitioned.reserved_microcredits;
    if (!policy.unlimited && remaining <= 0) {
      if (!policyChanged) return NativePaidAdmissionPlanBlocked{.reason = "limit_exceeded"};
      std::vector<std::string> checks{"window", "deletionGuard"};
      if (snapshot.policyFence) checks.emplace_back("policy");
      const double transitionedExpireIn = windowExpireIn(transitioned, now);
      return NativePaidAdmissionPlanExhaustedPolicy{.window = transitioned, .windowExpireIn = transitionedExpireIn, .checks = std::move(checks)};
    }
    const double reservation = policy.unlimited ? 0 : rt::Math::min(remaining, policy.maximumExposure.value_or(0));
    return buildPaidFallbackAdmissionPlanV3(input, snapshot, policy, transitioned, reservation, now);
  }

  std::optional<NativePaidLifecyclePlan> planPaidFallbackLifecycleV3(const PaidFallbackRequestV3& current, const PaidFallbackRequestLifecyclePatchV3& patch, double now) {
    const ResolvedRequestLifecycleV3 resolved = resolveRequestLifecyclePatchV3(current, patch, now);
    if (!requestLifecycleChangedV3(current, resolved)) return std::nullopt;
    const bool dispatchBoundary = current.dispatch_state != "dispatched" && (resolved.dispatchState == "dispatched" || resolved.providerRequestId.has_value());
    const bool readGate = dispatchBoundary || (!current.provider_request_id && resolved.providerRequestId.has_value());
    std::vector<std::string> checks{"request"};
    if (readGate) checks.emplace_back("gate");
    return NativePaidLifecyclePlan{
      .request = buildRequestLifecycleRowV3(current, resolved, now),
      .readGate = dispatchBoundary || (!current.provider_request_id && resolved.providerRequestId.has_value()),
      .dispatchBoundary = dispatchBoundary,
      .requestExpireIn = requestRowExpireIn(current, now),
      .checks = std::move(checks),
    };
  }

  std::optional<NativePaidReleasePlan> planPaidFallbackReleaseV3(const NativePaidReleaseSnapshot& snapshot, bool allowDispatchIntent, double now) {
    const std::optional<PaidFallbackRequestV3>& request = snapshot.request.value;
    const std::optional<PaidFallbackWindowV3>& window = snapshot.window.value;
    if (!request || request->billing_state != "pending" || request->provider_request_id ||
        (request->dispatch_state != "reserved" && !(allowDispatchIntent && request->dispatch_state == "dispatched")))
      return std::nullopt;
    PaidFallbackRequestV3 released = *request;
    released.dispatch_state = "not_dispatched";
    released.terminal_state = "cancelled";
    released.spend_microcredits = 0;
    released.billing_state = "not_billed";
    released.terminal_at_ms = request->terminal_at_ms.value_or(now);
    released.updated_at_ms = now;
    std::optional<PaidFallbackWindowV3> refunded;
    if (window) {
      PaidFallbackWindowV3 next = *window;
      next.reserved_microcredits = rt::Math::max(0, window->reserved_microcredits - request->reserved_microcredits);
      next.pending_count = rt::Math::max(0, window->pending_count - 1);
      next.updated_at_ms = now;
      refunded = std::move(next);
    }
    std::vector<std::string> checks{"request", "gate"};
    if (window) checks.emplace_back("window");
    return NativePaidReleasePlan{
      .request = std::move(released),
      .window = std::move(refunded),
      .gate = paidFallbackReconciliationGateDueNow(snapshot.gate, now),
      .requestExpireIn = requestRowExpireIn(*request, now),
      .windowExpireIn = window ? std::optional<double>(windowExpireIn(*window, now)) : std::nullopt,
      .checks = std::move(checks),
    };
  }

  NativePaidDeferralPlan planPaidFallbackDeferralV3(const std::optional<PaidFallbackRequestV3>& request, const std::optional<PaidFallbackPendingV3>& pending, double now) {
    if (!pending) return NativePaidDeferralPlanNoop{};
    if (!request || request->billing_state == "settled" || request->billing_state == "not_billed") return NativePaidDeferralPlanCleanup{};
    const double attempts = request->reconciliation_attempts + 1;
    const bool unresolved = now - request->created_at_ms >= UNRESOLVED_AFTER_MS;
    const double index = rt::Math::min(attempts - 1, static_cast<double>(RETRY_DELAYS_MS.size() - 1));
    // RETRY_DELAYS_MS[index]: undefined (NaN here) unless index is an array index.
    const double delay = rt::Number::isInteger(index) && index >= 0 ? RETRY_DELAYS_MS[static_cast<std::size_t>(index)] : rt::Number::NaN;
    PaidFallbackRequestV3 deferred = *request;
    deferred.reconciliation_attempts = attempts;
    deferred.last_reconciliation_at_ms = now;
    deferred.billing_state = unresolved ? "unresolved" : request->billing_state;
    deferred.updated_at_ms = now;
    PaidFallbackPendingV3 rescheduled = *pending;
    rescheduled.next_reconciliation_at_ms = now + delay;
    return NativePaidDeferralPlanWrite{
      .request = std::move(deferred),
      .pending = rescheduled,
      .delay = delay,
      .requestExpireIn = requestRowExpireIn(*request, now),
    };
  }

  NativePaidSettlementEvaluation evaluatePaidFallbackSettlementV3(const std::optional<PaidFallbackRequestV3>& request, const std::optional<PaidFallbackPendingV3>& pending,
                                                                  const NativePaidProviderLog& providerLog, const NativePaidSettlementMetadata& metadata) {
    if (!isBillableRequestV3(request)) return NativePaidSettlementEvaluationCleanup{};
    if (!pending || !correlationMatchesV3(*request, providerLog, metadata.correlation)) return NativePaidSettlementEvaluationNoop{};
    const double spend = metadata.correlation == "surplus_reservation" && metadata.surplusBuyerCostMicro
                           ? *metadata.surplusBuyerCostMicro
                           : rt::Math::round((providerLog.quota * metadata.microcreditsPerCredit) / request->quota_per_credit);
    if (!rt::Number::isSafeInteger(spend) || spend < 0) return NativePaidSettlementEvaluationDefer{};
    return NativePaidSettlementEvaluationSettle{.spend = spend};
  }

  NativePaidRollupIdentity paidFallbackRequestRollupIdentity(const PaidFallbackRequestV3& request, std::string_view correlation) {
    return {
      .bucketStartAtMs = rt::Math::floor(request.created_at_ms / PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS) * PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS,
      .model = rt::trim(request.model),
      .provider = request.provider ? *request.provider : std::string(correlation == "surplus_reservation" ? "surplus" : "metered"),
      .shard = paidFallbackUsageRollupShard(request.request_id),
    };
  }

  PaidFallbackSettlementRollupV3Result paidFallbackSettlementRollupV3(const PaidFallbackRequestV3& request, const NativePaidProviderLog& providerLog, double spend, std::string_view correlation,
                                                                      const std::optional<PaidFallbackUsageRollup>& existing, double now) {
    const NativePaidRollupIdentity identity = paidFallbackRequestRollupIdentity(request, correlation);
    const std::string& model = identity.model;
    const std::string& provider = identity.provider;
    const double bucketStartAtMs = identity.bucketStartAtMs;
    const PaidFallbackUsageRollupInput input{
      .bucket_start_at_ms = bucketStartAtMs,
      .model = model,
      .provider = provider,
      .request_id = request.request_id,
      .quota = providerLog.quota,
      .input_tokens = providerLog.prompt_tokens,
      .cached_input_tokens = providerLog.cached_prompt_tokens,
      .output_tokens = providerLog.completion_tokens,
      .spend_microcredits = spend,
      .request_created_at_ms = request.created_at_ms,
      .updated_at_ms = now,
    };
    return {
      .rollup = mergePaidFallbackUsageRollup(existing, input),
      .foldIntoRollup = !model.empty() && !provider.empty(),
    };
  }

  NativePaidSettlementPlan planPaidFallbackSettlementRowsV3(const NativePaidSettlementSnapshot& snapshot, const NativePaidProviderLog& providerLog, double spend, bool foldIntoRollup, double now) {
    const std::optional<PaidFallbackRequestV3>& request = snapshot.request.value;
    if (!request) throw rt::JsError("Paid fallback settlement request is missing");
    const std::optional<PaidFallbackWindowV3>& window = snapshot.window.value;
    const double dispatchedAtMs = request->dispatched_at_ms ? *request->dispatched_at_ms : rt::Math::max(request->created_at_ms, providerLog.created_at * 1000);
    std::optional<PaidFallbackWindowV3> settledWindow;
    if (window) {
      PaidFallbackWindowV3 next = *window;
      next.settled_microcredits = window->settled_microcredits + spend;
      next.reserved_microcredits = rt::Math::max(0, window->reserved_microcredits - request->reserved_microcredits);
      next.pending_count = rt::Math::max(0, window->pending_count - 1);
      next.updated_at_ms = now;
      settledWindow = std::move(next);
    }
    std::vector<std::string> checks{"request", "pending"};
    if (foldIntoRollup) checks.emplace_back("rollup");
    checks.emplace_back("gate");
    if (window) checks.emplace_back("window");
    return {
      .request = buildSettledRequestRowV3(*request, providerLog, spend, dispatchedAtMs, foldIntoRollup, now),
      .window = std::move(settledWindow),
      .gate = paidFallbackReconciliationGateDueNow(snapshot.gate, now),
      .requestExpireIn = requestRowExpireIn(*request, now),
      .windowExpireIn = window ? std::optional<double>(windowExpireIn(*window, now)) : std::nullopt,
      .checks = std::move(checks),
    };
  }

  PaidFallbackBackfillFoldPlanV3 planPaidFallbackBackfillFoldV3(const PaidFallbackRequestV3& request, const std::optional<PaidFallbackUsageRollup>& existing, double now) {
    const bool settled = request.billing_state == "settled" && request.provider_quota.has_value() && request.spend_microcredits.has_value();
    const NativePaidRollupIdentity identity = paidFallbackRequestRollupIdentity(request);
    const std::string& model = identity.model;
    const std::string& provider = identity.provider;
    const double bucketStartAtMs = identity.bucketStartAtMs;
    PaidFallbackRequestV3 marked = request;
    marked.updated_at_ms = now;
    marked.usage_rollup_at_ms = now;
    std::vector<std::string> checks{"request"};
    if (settled && !model.empty()) checks.emplace_back("rollup");
    std::optional<PaidFallbackUsageRollup> rollup;
    if (settled && !model.empty()) {
      const PaidFallbackUsageRollupInput input{
        .bucket_start_at_ms = bucketStartAtMs,
        .model = model,
        .provider = provider,
        .request_id = request.request_id,
        .quota = *request.provider_quota,
        .input_tokens = request.input_tokens.value_or(0),
        .cached_input_tokens = request.cached_input_tokens.toOptional(),
        .output_tokens = request.output_tokens.value_or(0),
        .spend_microcredits = *request.spend_microcredits,
        .request_created_at_ms = request.created_at_ms,
        .updated_at_ms = now,
      };
      rollup = mergePaidFallbackUsageRollup(existing, input);
    }
    return {
      .request = std::move(marked),
      .requestExpireIn = requestRowExpireIn(request, now),
      .checks = std::move(checks),
      .rollup = std::move(rollup),
    };
  }

  bool paidFallbackBackfillRollupEligibleV3(const PaidFallbackRequestV3& request) {
    return request.billing_state == "settled" && request.provider_quota.has_value() && request.spend_microcredits.has_value() && !rt::trim(request.model).empty();
  }

  bool paidFallbackBackfillNeedsSweepV3(const json::value& state, const json::value& requestCursor, const json::value& windowCursor) {
    return paidFallbackBackfillNeedsSweepV3(&state, &requestCursor, &windowCursor);
  }

  bool paidFallbackBackfillNeedsSweepV3(const json::value* state, const json::value* requestCursor, const json::value* windowCursor) {
    const bool requestCursorIsNull = requestCursor != nullptr && requestCursor->is_null();
    const bool windowCursorIsNull = windowCursor != nullptr && windowCursor->is_null();
    return !isPaidFallbackBackfillState(state) || !requestCursorIsNull || !windowCursorIsNull;
  }

  std::optional<PaidFallbackDeletionGuardV3> planPaidFallbackDeletionGuardV3(const std::optional<PaidFallbackDeletionGuardV3>& guard, double now) {
    if (guard) return std::nullopt;
    return PaidFallbackDeletionGuardV3{.created_at_ms = now};
  }

  PaidFallbackDeletionPlanV3 planPaidFallbackDeletionV3(const std::optional<PaidFallbackOutstandingV3>& outstanding) {
    if (!outstanding) return PaidFallbackDeletionPlanV3Unavailable{};
    if (outstanding->has_outstanding) return PaidFallbackDeletionPlanV3Blocked{.outstanding = *outstanding};
    return PaidFallbackDeletionPlanV3Delete{};
  }

  bool paidFallbackLeaseActiveV3(const std::optional<PaidFallbackReconciliationLeaseV3>& lease, double now) { return lease && lease->expires_at_ms > now; }

  bool paidFallbackLeaseActiveV3(const std::optional<PaidFallbackBackfillLeaseV3>& lease, double now) { return lease && lease->expires_at_ms > now; }

  PaidFallbackReconciliationLeaseV3 paidFallbackReconciliationLeaseV3(std::string token, double now) {
    return {.token = std::move(token), .expires_at_ms = now + RECONCILIATION_LEASE_MS};
  }

  PaidFallbackBackfillLeaseV3 paidFallbackBackfillLeaseV3(std::string token, double now) {
    return {.token = std::move(token), .expires_at_ms = now + PAID_FALLBACK_BACKFILL_LEASE_MS};
  }

  bool paidFallbackLeaseOwnedV3(const std::optional<PaidFallbackReconciliationLeaseV3>& lease, std::string_view token) { return lease && lease->token == token; }

  bool paidFallbackLeaseOwnedV3(const std::optional<PaidFallbackBackfillLeaseV3>& lease, std::string_view token) { return lease && lease->token == token; }

  PaidFallbackReconciliationGateV3 paidFallbackGateAfterScanV3(std::optional<double> earliestDueAtMs) { return {.next_due_at_ms = earliestDueAtMs}; }

  std::optional<NativePaidTerminalUsage> paidFallbackTerminalUsageV3(const TerminalUsageRollupInput& input) {
    const std::string model = input.model ? rt::trim(*input.model) : "";
    const std::string provider = input.provider ? rt::trim(*input.provider) : "";
    if (model.empty() || provider.empty() || provider == "gateway" || provider == "mixed" || PAID_FALLBACK_SETTLED_PROVIDERS.contains(provider)) return std::nullopt;
    const std::optional<double> inputTokens = observedTokenCount(input.input_tokens);
    const std::optional<double> outputTokens = observedTokenCount(input.output_tokens);
    if (!inputTokens || !outputTokens) return std::nullopt;
    return NativePaidTerminalUsage{.model = model, .provider = provider, .inputTokens = *inputTokens, .outputTokens = *outputTokens};
  }

  PaidFallbackUsageRollupInput paidFallbackTerminalRollupV3(const TerminalUsageRollupInput& input, const NativePaidTerminalUsage& usage, std::string_view requestId, double now) {
    const std::optional<double>& requested = input.request_created_at_ms;
    const double created = requested && rt::Number::isFinite(*requested) ? rt::Math::max(0, rt::Math::trunc(*requested)) : now;
    return {
      .bucket_start_at_ms = rt::Math::floor(created / PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS) * PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS,
      .model = usage.model,
      .provider = usage.provider,
      .request_id = std::string(requestId),
      .quota = 0,
      .input_tokens = usage.inputTokens,
      .cached_input_tokens = observedTokenCount(input.cached_input_tokens),
      .output_tokens = usage.outputTokens,
      .spend_microcredits = 0,
      .request_created_at_ms = created,
      .updated_at_ms = now,
    };
  }

  std::string paidFallbackRequestBackfillStateV3(const std::optional<PaidFallbackRequestV3>& request) {
    if (!request) return "missing";
    return request->usage_rollup_at_ms.hasValue() ? "retention" : "fold";
  }

  PaidFallbackGateAfterScanPlanV3 planPaidFallbackGateAfterScanV3(const NativePaidRow<PaidFallbackReconciliationGateV3>& gate, std::optional<double> earliestDueAtMs) {
    return {.gate = paidFallbackGateAfterScanV3(earliestDueAtMs), .checks = {"gate"}, .versionstamp = gate.versionstamp};
  }

  PaidFallbackGateAfterScanPlanV3 planPaidFallbackGateAfterScanV3(const NativePaidRow<json::value>& gate, std::optional<double> earliestDueAtMs) {
    return {.gate = paidFallbackGateAfterScanV3(earliestDueAtMs), .checks = {"gate"}, .versionstamp = gate.versionstamp};
  }
}
