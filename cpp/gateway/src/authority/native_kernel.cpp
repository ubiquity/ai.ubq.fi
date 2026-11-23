#include "uos/authority/native_kernel.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <utility>

namespace uos::authority::native_kernel {
  namespace {
    // `typeof value === "number" ? value : (typeof value === "string" ? Number(value) : <not a number>)`
    // for the normalizers that read a string as a number; std::nullopt when the value is neither.
    std::optional<double> numericOrString(const json::value* value) {
      if (value == nullptr) return std::nullopt;
      if (value->is_string()) return rt::stringToNumber(value->get_string());
      return rt::getNumber(value);
    }

    double usageLimitOf(std::optional<double> value, double fallback, double noUsageLimit) {
      if (!value || !rt::Number::isFinite(*value)) return fallback;
      const double parsed = rt::Math::trunc(*value);
      if (parsed == noUsageLimit) return parsed;
      return parsed >= 0 ? parsed : fallback;
    }

    double windowOf(std::optional<double> value, double fallback) {
      if (!value || !rt::Number::isFinite(*value)) return fallback;
      const double parsed = rt::Math::trunc(*value);
      return parsed > 0 ? parsed : fallback;
    }

    double expirationOf(std::optional<double> value, double noExpirationMs) {
      if (!value || !rt::Number::isFinite(*value)) return noExpirationMs;
      const double parsed = rt::Math::trunc(*value);
      return parsed == noExpirationMs || parsed >= 0 ? parsed : noExpirationMs;
    }

    const json::value* member(const json::object& object, std::string_view key) { return rt::get(object, key); }
    bool isNullMember(const json::object& object, std::string_view key) {
      const json::value* value = member(object, key);
      return value != nullptr && value->is_null();
    }
    // `typeof value === "string" && value` (a non-empty string)
    std::optional<std::string> nonEmptyText(const json::object& object, std::string_view key) {
      const std::optional<std::string_view> text = rt::getString(member(object, key));
      if (!text || text->empty()) return std::nullopt;
      return std::string(*text);
    }
    bool isTextMember(const json::object& object, std::string_view key, std::string_view expected) {
      const std::optional<std::string_view> text = rt::getString(member(object, key));
      return text && *text == expected;
    }
  }

  rt::KvKey kernelRepoPolicyKey(const std::string& owner, const std::string& repo) { return rt::KvKey{KERNEL_REPO_POLICY_V2_PREFIX, owner, repo}; }
  rt::KvKey kernelOrgPolicyKey(const std::string& owner) { return rt::KvKey{KERNEL_ORG_POLICY_V2_PREFIX, owner}; }
  rt::KvKey kernelRepoWindowKey(const std::string& owner, const std::string& repo) { return rt::KvKey{KERNEL_REPO_WINDOW_V2_PREFIX, owner, repo}; }
  rt::KvKey kernelOrgWindowKey(const std::string& owner) { return rt::KvKey{KERNEL_ORG_WINDOW_V2_PREFIX, owner}; }
  rt::KvKey kernelRepoReservationKey(const std::string& owner, const std::string& repo, double windowCreatedAtMs, const std::string& requestId) {
    return rt::KvKey{KERNEL_REPO_RESERVATION_V2_PREFIX, owner, repo, windowCreatedAtMs, requestId};
  }
  rt::KvKey kernelOrgReservationKey(const std::string& owner, double windowCreatedAtMs, const std::string& requestId) {
    return rt::KvKey{KERNEL_ORG_RESERVATION_V2_PREFIX, owner, windowCreatedAtMs, requestId};
  }

  std::string kernelRepoPart(const std::optional<std::string>& repo) {
    if (!repo) throw rt::JsError("Kernel quota repo scope requires a repo");
    return *repo;
  }

  rt::KvKey kernelScopedPolicyKey(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo) {
    return scope == "repo" ? kernelRepoPolicyKey(owner, kernelRepoPart(repo)) : kernelOrgPolicyKey(owner);
  }

  rt::KvKey kernelScopedWindowKey(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo) {
    return scope == "repo" ? kernelRepoWindowKey(owner, kernelRepoPart(repo)) : kernelOrgWindowKey(owner);
  }

  KernelPolicyKeys kernelPolicyKeys(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo) {
    rt::KvKey policyKey = kernelScopedPolicyKey(scope, owner, repo);
    rt::KvKey windowKey = kernelScopedWindowKey(scope, owner, repo);
    return KernelPolicyKeys{std::move(policyKey), std::move(windowKey)};
  }

  bool hasLiveKernelReservations(const std::optional<KernelQuotaWindowV2>& window, double nowMs) {
    return window.has_value() && window->usage_reset_at_ms > nowMs && window->reserved_requests > 0;
  }

  bool positiveSafeInteger(const json::value* value) {
    const std::optional<double> number = rt::getNumber(value);
    return number.has_value() && positiveSafeInteger(*number);
  }
  bool positiveSafeInteger(const json::value& value) { return positiveSafeInteger(&value); }
  bool positiveSafeInteger(double value) { return rt::Number::isSafeInteger(value) && value > 0; }
  bool nonNegativeSafeInteger(const json::value* value) {
    const std::optional<double> number = rt::getNumber(value);
    return number.has_value() && nonNegativeSafeInteger(*number);
  }
  bool nonNegativeSafeInteger(const json::value& value) { return nonNegativeSafeInteger(&value); }
  bool nonNegativeSafeInteger(double value) { return rt::Number::isSafeInteger(value) && value >= 0; }

  double normalizeUsageLimit(const json::value* value, double fallback, double noUsageLimit) { return usageLimitOf(numericOrString(value), fallback, noUsageLimit); }
  double normalizeUsageLimit(double value, double fallback, double noUsageLimit) { return usageLimitOf(value, fallback, noUsageLimit); }

  double normalizeWindow(const json::value* value, double fallback) { return windowOf(numericOrString(value), fallback); }
  double normalizeWindow(double value, double fallback) { return windowOf(value, fallback); }

  // No string reading here: only a number is an expiration.
  double normalizeExpiration(const json::value* value, double noExpirationMs) { return expirationOf(rt::getNumber(value), noExpirationMs); }
  double normalizeExpiration(double value, double noExpirationMs) { return expirationOf(value, noExpirationMs); }

  bool validIdentity(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo, const json::object& value) {
    if (rt::getNumber(member(value, "v")) != 2 || !isTextMember(value, "scope", scope) || !isTextMember(value, "owner", owner)) return false;
    const json::value* storedRepo = member(value, "repo");
    if (scope == "org") return storedRepo == nullptr || storedRepo->is_null();
    // `value.repo === repo` with `repo: string | undefined`
    if (!repo) return storedRepo == nullptr;
    return storedRepo != nullptr && storedRepo->is_string() && storedRepo->get_string() == *repo;
  }

  std::optional<KernelQuotaPolicyV2> normalizeKernelQuotaPolicyV2(const json::value& value, const KernelQuotaScope& scope, const std::string& owner,
                                                                  const std::optional<std::string>& repo, const KernelQuotaConstants& constants) {
    const json::object* object = value.if_object();
    if (!utils::isRecord(value) || object == nullptr || !validIdentity(scope, owner, repo, *object)) return std::nullopt;
    if (!nonNegativeSafeInteger(member(*object, "created_at_ms")) || !nonNegativeSafeInteger(member(*object, "updated_at_ms"))) return std::nullopt;
    const double windowMs = normalizeWindow(member(*object, "window_ms"), 0);
    const double limit = normalizeUsageLimit(member(*object, "usage_limit_requests"), rt::Number::NaN, constants.noUsageLimit);
    if (!positiveSafeInteger(windowMs) || !(limit == constants.noUsageLimit || nonNegativeSafeInteger(limit))) {
      return std::nullopt;
    }
    const double expiresAtMs = normalizeExpiration(member(*object, "expires_at_ms"), constants.noExpirationMs);
    return KernelQuotaPolicyV2{
      .v = 2,
      .scope = scope,
      .owner = owner,
      .repo = scope == "repo" ? repo : std::nullopt,
      .usage_limit_requests = limit,
      .window_ms = windowMs,
      .expires_at_ms = expiresAtMs,
      .created_at_ms = *rt::getNumber(member(*object, "created_at_ms")),
      .updated_at_ms = *rt::getNumber(member(*object, "updated_at_ms")),
    };
  }

  std::optional<KernelQuotaWindowV2> normalizeKernelQuotaWindowV2(const json::value& value, const KernelQuotaScope& scope, const std::string& owner,
                                                                  const std::optional<std::string>& repo) {
    const json::object* object = value.if_object();
    if (!utils::isRecord(value) || object == nullptr || !validIdentity(scope, owner, repo, *object)) return std::nullopt;
    // `value.reserved_requests === undefined ? 0 : value.reserved_requests`
    const json::value zero = 0;
    const json::value* reservedRequests = member(*object, "reserved_requests");
    if (reservedRequests == nullptr) reservedRequests = &zero;
    if (!nonNegativeSafeInteger(member(*object, "usage_requests")) || !nonNegativeSafeInteger(reservedRequests) ||
        !positiveSafeInteger(member(*object, "usage_reset_at_ms")) || !positiveSafeInteger(member(*object, "applied_window_ms")) ||
        !nonNegativeSafeInteger(member(*object, "created_at_ms")) || !nonNegativeSafeInteger(member(*object, "updated_at_ms")))
      return std::nullopt;
    return KernelQuotaWindowV2{
      .v = 2,
      .scope = scope,
      .owner = owner,
      .repo = scope == "repo" ? repo : std::nullopt,
      .usage_requests = *rt::getNumber(member(*object, "usage_requests")),
      .reserved_requests = *rt::getNumber(reservedRequests),
      .usage_reset_at_ms = *rt::getNumber(member(*object, "usage_reset_at_ms")),
      .applied_window_ms = *rt::getNumber(member(*object, "applied_window_ms")),
      .created_at_ms = *rt::getNumber(member(*object, "created_at_ms")),
      .updated_at_ms = *rt::getNumber(member(*object, "updated_at_ms")),
    };
  }

  KernelQuotaWindowV2 newWindow(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo, double windowMs, double nowMs) {
    return KernelQuotaWindowV2{
      .v = 2,
      .scope = scope,
      .owner = owner,
      .repo = scope == "repo" ? repo : std::nullopt,
      .usage_requests = 0,
      .reserved_requests = 0,
      .usage_reset_at_ms = nowMs + windowMs,
      .applied_window_ms = windowMs,
      .created_at_ms = nowMs,
      .updated_at_ms = nowMs,
    };
  }

  KernelQuotaEffectiveWindow windowForEffectivePolicy(const std::optional<KernelQuotaWindowV2>& existing, const KernelQuotaScope& scope, const std::string& owner,
                                                      const std::optional<std::string>& repo, double effectiveWindowMs, double nowMs) {
    if (!existing || existing->applied_window_ms != effectiveWindowMs || existing->usage_reset_at_ms <= nowMs) {
      return KernelQuotaEffectiveWindow{.window = newWindow(scope, owner, repo, effectiveWindowMs, nowMs), .needsWrite = true};
    }
    return KernelQuotaEffectiveWindow{.window = *existing, .needsWrite = false};
  }

  KernelQuotaEffectivePolicy policyFor(const KernelQuotaRead<>& entry, const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo,
                                       const KernelQuotaDefaults& defaults, const KernelQuotaConstants& constants) {
    if (entry.value.is_null()) {
      return KernelQuotaEffectivePolicy{
        .policy = std::nullopt,
        .limit = defaults.limit,
        .windowMs = defaults.windowMs,
        .expiresAtMs = constants.noExpirationMs,
        .source = "default",
      };
    }
    std::optional<KernelQuotaPolicyV2> policy = normalizeKernelQuotaPolicyV2(entry.value, scope, owner, repo, constants);
    if (!policy) {
      // A corrupt explicit policy must never silently turn into a permissive
      // default; callers surface it as an unavailable quota record.
      throw rt::JsError("kernel quota policy is malformed");
    }
    const double limit = policy->usage_limit_requests;
    const double windowMs = policy->window_ms;
    const double expiresAtMs = policy->expires_at_ms;
    return KernelQuotaEffectivePolicy{
      .policy = std::move(policy),
      .limit = limit,
      .windowMs = windowMs,
      .expiresAtMs = expiresAtMs,
      .source = "kv",
    };
  }

  bool isExpired(double expiresAtMs, double nowMs, double noExpirationMs) { return expiresAtMs != noExpirationMs && expiresAtMs <= nowMs; }

  double kernelReservationRetentionMs(double windowResetAtMs, double nowMs) {
    return rt::Math::max(1.0, windowResetAtMs + KERNEL_QUOTA_RESERVATION_RETENTION_MS - nowMs);
  }

  rt::KvKey kernelReservationKey(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo, double windowCreatedAtMs,
                                 const std::string& requestId) {
    return scope == "repo" ? kernelRepoReservationKey(owner, kernelRepoPart(repo), windowCreatedAtMs, requestId)
                           : kernelOrgReservationKey(owner, windowCreatedAtMs, requestId);
  }

  rt::KvKey kernelReservationWindowPrefix(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo, double windowCreatedAtMs) {
    return scope == "repo" ? rt::KvKey{KERNEL_REPO_RESERVATION_V2_PREFIX, owner, kernelRepoPart(repo), windowCreatedAtMs}
                           : rt::KvKey{KERNEL_ORG_RESERVATION_V2_PREFIX, owner, windowCreatedAtMs};
  }

  std::optional<KernelQuotaReservationRowV2> normalizeKernelQuotaReservationRowV2(const json::value& value, const KernelQuotaScope& scope, const std::string& owner,
                                                                                  const std::optional<std::string>& repo) {
    const json::object* object = value.if_object();
    if (!utils::isRecord(value) || object == nullptr || !validIdentity(scope, owner, repo, *object)) return std::nullopt;
    // `value.terminal_intent === undefined ? null : value.terminal_intent`
    const json::value* storedIntent = member(*object, "terminal_intent");
    const json::value terminalIntent = storedIntent == nullptr ? json::value(nullptr) : *storedIntent;
    const std::optional<std::string> requestId = nonEmptyText(*object, "request_id");
    const std::optional<std::string> route = nonEmptyText(*object, "route");
    const bool reserved = isTextMember(*object, "state", "reserved");
    const bool committed = isTextMember(*object, "state", "committed");
    const bool released = isTextMember(*object, "state", "released");
    const bool intentCommitted = terminalIntent.is_string() && terminalIntent.get_string() == "committed";
    const bool intentReleased = terminalIntent.is_string() && terminalIntent.get_string() == "released";
    const json::value* releaseReason = member(*object, "release_reason");
    const bool releaseReasonNull = releaseReason != nullptr && releaseReason->is_null();
    const bool releaseReasonText = releaseReason != nullptr && releaseReason->is_string();
    if (!requestId || !route || !nonNegativeSafeInteger(member(*object, "window_created_at_ms")) || !positiveSafeInteger(member(*object, "window_reset_at_ms")) ||
        (!reserved && !committed && !released) || (!terminalIntent.is_null() && !intentCommitted && !intentReleased) ||
        !nonNegativeSafeInteger(member(*object, "reserved_at_ms")) || !positiveSafeInteger(member(*object, "lease_expires_at_ms")) ||
        !(isNullMember(*object, "committed_at_ms") || nonNegativeSafeInteger(member(*object, "committed_at_ms"))) ||
        !(isNullMember(*object, "released_at_ms") || nonNegativeSafeInteger(member(*object, "released_at_ms"))) || !(releaseReasonNull || releaseReasonText))
      return std::nullopt;
    const double windowCreatedAtMs = *rt::getNumber(member(*object, "window_created_at_ms"));
    const double windowResetAtMs = *rt::getNumber(member(*object, "window_reset_at_ms"));
    const double reservedAtMs = *rt::getNumber(member(*object, "reserved_at_ms"));
    const double leaseExpiresAtMs = *rt::getNumber(member(*object, "lease_expires_at_ms"));
    if (windowResetAtMs <= windowCreatedAtMs || leaseExpiresAtMs <= reservedAtMs) return std::nullopt;
    const bool committedAtNull = isNullMember(*object, "committed_at_ms");
    const bool releasedAtNull = isNullMember(*object, "released_at_ms");
    if ((reserved && (!committedAtNull || !releasedAtNull || !releaseReasonNull)) ||
        (committed && (committedAtNull || !releasedAtNull || !releaseReasonNull || intentReleased)) ||
        (released && (!committedAtNull || releasedAtNull || !releaseReasonText || (releaseReasonText && rt::trim(releaseReason->get_string()).empty()) || intentCommitted)))
      return std::nullopt;
    const json::value* storedRepo = member(*object, "repo");
    return KernelQuotaReservationRowV2{
      .v = 2,
      .scope = scope,
      .owner = owner,
      .repo = storedRepo != nullptr && storedRepo->is_string() ? std::optional<std::string>(std::string(storedRepo->get_string())) : std::nullopt,
      .request_id = *requestId,
      .route = *route,
      .window_created_at_ms = windowCreatedAtMs,
      .window_reset_at_ms = windowResetAtMs,
      .state = std::string(*rt::getString(member(*object, "state"))),
      .terminal_intent = terminalIntent.is_string() ? std::optional<std::string>(std::string(terminalIntent.get_string())) : std::nullopt,
      .reserved_at_ms = reservedAtMs,
      .lease_expires_at_ms = leaseExpiresAtMs,
      .committed_at_ms = rt::getNumber(member(*object, "committed_at_ms")),
      .released_at_ms = rt::getNumber(member(*object, "released_at_ms")),
      .release_reason = releaseReasonText ? std::optional<std::string>(std::string(releaseReason->get_string())) : std::nullopt,
    };
  }

  bool kernelReservationMatchesRequest(const std::optional<KernelQuotaReservationRowV2>& reservation, const std::string& requestId, const std::string& route,
                                       double windowCreatedAtMs) {
    return reservation.has_value() && reservation->request_id == requestId && reservation->route == route && reservation->window_created_at_ms == windowCreatedAtMs;
  }

  KernelQuotaReservationRowV2 requireKernelReservationRow(const rt::KvKey& key, const json::value& value, const KernelQuotaScope& scope, const std::string& owner,
                                                          const std::optional<std::string>& repo, double windowCreatedAtMs) {
    const std::optional<KernelQuotaReservationRowV2> reservation = normalizeKernelQuotaReservationRowV2(value, scope, owner, repo);
    // `key.at(-1)`
    const rt::KvKeyPart* requestId = key.empty() ? nullptr : &key.back();
    if (!reservation || requestId == nullptr || !requestId->isString() || requestId->string() != reservation->request_id ||
        reservation->window_created_at_ms != windowCreatedAtMs) {
      throw rt::JsError("Kernel quota reservation is malformed");
    }
    return *reservation;
  }

  std::optional<KernelReservationSettlement> kernelReservationPrecondition(const KernelQuotaReservationRowV2& reservation, const std::string& requestId,
                                                                           const std::string& route, double windowCreatedAtMs, const std::string& terminalState) {
    if (!kernelReservationMatchesRequest(reservation, requestId, route, windowCreatedAtMs)) return "invalid";
    if (reservation.state != "reserved") return reservation.state == terminalState ? "settled" : "terminal_mismatch";
    if (reservation.terminal_intent && *reservation.terminal_intent != terminalState) return "terminal_mismatch";
    return std::nullopt;
  }

  KernelQuotaReservationRowV2 settledKernelReservationRow(const KernelQuotaReservationRowV2& reservation, const std::string& terminalState, const std::string& reason,
                                                          double nowMs) {
    KernelQuotaReservationRowV2 settled = reservation;
    if (terminalState == "committed") {
      settled.state = "committed";
      settled.terminal_intent = "committed";
      settled.committed_at_ms = nowMs;
      return settled;
    }
    settled.state = "released";
    settled.terminal_intent = "released";
    settled.released_at_ms = nowMs;
    const std::string truncated = rt::slice(reason, 0, 120);
    settled.release_reason = truncated.empty() ? std::string("request_incomplete") : truncated;
    return settled;
  }

  std::optional<KernelDefaultWindowCutoverV2> normalizeKernelDefaultWindowCutoverV2(const json::value& value) {
    const json::object* object = value.if_object();
    if (!utils::isRecord(value) || object == nullptr || rt::getNumber(member(*object, "v")) != 2) return std::nullopt;
    const std::optional<std::string> id = nonEmptyText(*object, "id");
    if (!id || !nonNegativeSafeInteger(member(*object, "created_at_ms")) || !positiveSafeInteger(member(*object, "expires_at_ms")) ||
        *rt::getNumber(member(*object, "expires_at_ms")) <= *rt::getNumber(member(*object, "created_at_ms")))
      return std::nullopt;
    return KernelDefaultWindowCutoverV2{
      .v = 2,
      .id = *id,
      .created_at_ms = *rt::getNumber(member(*object, "created_at_ms")),
      .expires_at_ms = *rt::getNumber(member(*object, "expires_at_ms")),
    };
  }

  KernelWindowScopeTarget kernelWindowScopeTarget(const KernelQuotaScope& scope, const rt::KvKey& key, const rt::KvKey& prefix) {
    const rt::KvKeyPart* ownerPart = prefix.size() < key.size() ? &key[prefix.size()] : nullptr;
    if (ownerPart == nullptr || !ownerPart->isString() || ownerPart->string().empty()) throw rt::JsError("Kernel quota window key is malformed");
    const rt::KvKeyPart* repoPart = prefix.size() + 1 < key.size() ? &key[prefix.size() + 1] : nullptr;
    const std::optional<std::string> repo = scope == "repo" && repoPart != nullptr && repoPart->isString() && !repoPart->string().empty()
                                              ? std::optional<std::string>(repoPart->string())
                                              : std::nullopt;
    if (scope == "repo" && !repo) throw rt::JsError("Kernel repo quota window key is malformed");
    return KernelWindowScopeTarget{.owner = ownerPart->string(), .repo = repo};
  }

  KernelQuotaScopeDecision decideKernelQuotaScope(const KernelQuotaScopeSnapshot& snapshot, const std::string& owner, const std::string& repo,
                                                  const KernelQuotaConstants& constants) {
    if (!snapshot.repoPolicy.value.is_null() && !normalizeKernelQuotaPolicyV2(snapshot.repoPolicy.value, "repo", owner, repo, constants)) {
      throw rt::JsError("Kernel repo quota policy is malformed");
    }
    if (!snapshot.orgPolicy.value.is_null() && !normalizeKernelQuotaPolicyV2(snapshot.orgPolicy.value, "org", owner, std::nullopt, constants)) {
      throw rt::JsError("Kernel org quota policy is malformed");
    }
    return !snapshot.repoPolicy.value.is_null() ? KernelQuotaScopeDecision{.limit_scope = "repo", .has_policy = true}
                                                : KernelQuotaScopeDecision{.limit_scope = "org", .has_policy = !snapshot.orgPolicy.value.is_null()};
  }

  KernelQuotaEligibilityPlan decideKernelQuotaEligibility(const KernelQuotaIdentity& identity, const KernelQuotaEligibilitySnapshot& snapshot,
                                                          const KernelQuotaDefaults& defaults, const KernelQuotaConstants& constants, double nowMs) {
    using native_kernel_contracts::KernelQuotaEligibilityPlanEligible;
    using native_kernel_contracts::KernelQuotaRefusal;
    if ((snapshot.expectedRepoPolicy == "present" && snapshot.policy.value.is_null()) ||
        (snapshot.expectedRepoPolicy == "absent" && snapshot.repoGuard.has_value() && !snapshot.repoGuard->value.is_null()))
      return KernelQuotaRefusal{.status = 503, .message = "Kernel quota policy scope changed; retry"};
    KernelQuotaEffectivePolicy effective = policyFor(snapshot.policy, identity.scope, identity.owner, identity.repo, defaults, constants);
    if (kernelQuotaDefaultCutoverBlocks(effective, snapshot.defaultCutover)) {
      return KernelQuotaRefusal{.status = 503, .message = "Kernel quota defaults are changing; retry"};
    }
    if (isExpired(effective.expiresAtMs, nowMs, constants.noExpirationMs)) {
      return KernelQuotaRefusal{.status = 429, .message = "Kernel auth usage limit expired; update it via /admin/kernel-usage."};
    }
    if (effective.limit == 0) return KernelQuotaRefusal{.status = 429, .message = "Kernel auth usage limit is 0; update it via /admin/kernel-usage."};
    return KernelQuotaEligibilityPlanEligible{.effective = std::move(effective)};
  }

  KernelQuotaReservePlan decideKernelQuotaReserve(const KernelQuotaIdentity& identity, const KernelQuotaEffectivePolicy& effective, const KernelQuotaWindowV2& window,
                                                  const KernelQuotaRead<>& reservationEntry, const std::string& requestId, const std::string& route, double noUsageLimit,
                                                  double nowMs) {
    using native_kernel_contracts::KernelQuotaRefusal;
    using native_kernel_contracts::KernelQuotaReservePlanFull;
    using native_kernel_contracts::KernelQuotaReservePlanReserve;
    if (!reservationEntry.value.is_null()) {
      const std::optional<KernelQuotaReservationRowV2> existing =
        normalizeKernelQuotaReservationRowV2(reservationEntry.value, identity.scope, identity.owner, identity.repo);
      if (!kernelReservationMatchesRequest(existing, requestId, route, window.created_at_ms)) {
        return KernelQuotaRefusal{.status = 503, .message = "Kernel quota request identity conflicts"};
      }
      return KernelQuotaRefusal{.status = 503, .message = "Kernel quota request already has a reservation"};
    }
    if (effective.limit != noUsageLimit && window.usage_requests + window.reserved_requests >= effective.limit) {
      return KernelQuotaReservePlanFull{.window = window, .limit = effective.limit};
    }
    KernelQuotaReservationRowV2 reservation{
      .v = 2,
      .scope = identity.scope,
      .owner = identity.owner,
      .repo = identity.scope == "repo" ? identity.repo : std::nullopt,
      .request_id = requestId,
      .route = route,
      .window_created_at_ms = window.created_at_ms,
      .window_reset_at_ms = window.usage_reset_at_ms,
      .state = "reserved",
      .terminal_intent = std::nullopt,
      .reserved_at_ms = nowMs,
      .lease_expires_at_ms = nowMs + KERNEL_QUOTA_RESERVATION_LEASE_MS,
      .committed_at_ms = std::nullopt,
      .released_at_ms = std::nullopt,
      .release_reason = std::nullopt,
    };
    KernelQuotaWindowV2 reservedWindow = window;
    reservedWindow.reserved_requests = window.reserved_requests + 1;
    reservedWindow.updated_at_ms = nowMs;
    const double expireIn = kernelReservationRetentionMs(reservedWindow.usage_reset_at_ms, nowMs);
    return KernelQuotaReservePlanReserve{.reservation = std::move(reservation), .window = std::move(reservedWindow), .expireIn = expireIn};
  }

  KernelQuotaTerminalPlan decideKernelQuotaTerminalIntent(const KernelQuotaReservationIdentity& identity, const KernelQuotaRead<>& snapshot,
                                                          const std::string& terminalState, double nowMs) {
    using native_kernel_contracts::KernelQuotaTerminalPlanIntent;
    using native_kernel_contracts::KernelQuotaTerminalPlanUnchanged;
    const std::optional<KernelQuotaReservationRowV2> reservation = normalizeKernelQuotaReservationRowV2(snapshot.value, identity.scope, identity.owner, identity.repo);
    if (!reservation) return KernelQuotaTerminalPlanUnchanged{.outcome = snapshot.value.is_null() ? "missing" : "invalid"};
    const std::optional<KernelReservationSettlement> outcome =
      kernelReservationPrecondition(*reservation, identity.requestId, identity.route, identity.windowCreatedAtMs, terminalState);
    if (outcome) return KernelQuotaTerminalPlanUnchanged{.outcome = *outcome};
    const double leaseExpiresAtMs = nowMs + KERNEL_QUOTA_RESERVATION_LEASE_MS;
    KernelQuotaReservationRowV2 intent = *reservation;
    intent.terminal_intent = terminalState;
    intent.lease_expires_at_ms = leaseExpiresAtMs;
    return KernelQuotaTerminalPlanIntent{
      .reservation = std::move(intent),
      .expireIn = kernelReservationRetentionMs(reservation->window_reset_at_ms, nowMs),
      .leaseExpiresAtMs = leaseExpiresAtMs,
    };
  }

  KernelQuotaSettlePlan decideKernelQuotaSettlement(const KernelQuotaReservationRowV2& reservation, const KernelQuotaSettlementSnapshot& snapshot,
                                                    const std::string& terminalState, const std::string& reason, double nowMs) {
    using native_kernel_contracts::KernelQuotaSettlePlanSettle;
    using native_kernel_contracts::KernelQuotaSettlePlanUnchanged;
    const std::optional<KernelQuotaWindowV2>& window = snapshot.window;
    if (snapshot.windowPresent && !window) return KernelQuotaSettlePlanUnchanged{.outcome = "invalid"};
    const double expireIn = kernelReservationRetentionMs(reservation.window_reset_at_ms, nowMs);
    if (!window || window->created_at_ms != reservation.window_created_at_ms) {
      return KernelQuotaSettlePlanSettle{.reservation = settledKernelReservationRow(reservation, terminalState, reason, nowMs), .window = std::nullopt, .expireIn = expireIn};
    }
    if (window->reserved_requests < 1) return KernelQuotaSettlePlanUnchanged{.outcome = "invalid"};
    KernelQuotaReservationRowV2 settled = settledKernelReservationRow(reservation, terminalState, reason, nowMs);
    KernelQuotaWindowV2 settledWindow = *window;
    settledWindow.usage_requests = window->usage_requests + (terminalState == "committed" ? 1 : 0);
    settledWindow.reserved_requests = window->reserved_requests - 1;
    settledWindow.updated_at_ms = nowMs;
    return KernelQuotaSettlePlanSettle{.reservation = std::move(settled), .window = std::move(settledWindow), .expireIn = expireIn};
  }

  KernelQuotaRenewal decideKernelQuotaRenewal(const KernelQuotaReservationIdentity& identity, const KernelQuotaRead<>& snapshot, double nowMs) {
    const std::optional<KernelQuotaReservationRowV2> reservation = normalizeKernelQuotaReservationRowV2(snapshot.value, identity.scope, identity.owner, identity.repo);
    if (!kernelReservationMatchesRequest(reservation, identity.requestId, identity.route, identity.windowCreatedAtMs)) {
      throw rt::JsError("Kernel quota reservation is missing or malformed");
    }
    if (reservation->state != "reserved") throw rt::JsError("Kernel quota reservation was already " + reservation->state);
    const double leaseExpiresAtMs = nowMs + KERNEL_QUOTA_RESERVATION_LEASE_MS;
    KernelQuotaReservationRowV2 renewed = *reservation;
    renewed.lease_expires_at_ms = leaseExpiresAtMs;
    return KernelQuotaRenewal{
      .reservation = std::move(renewed),
      .expireIn = kernelReservationRetentionMs(reservation->window_reset_at_ms, nowMs),
      .leaseExpiresAtMs = leaseExpiresAtMs,
    };
  }

  std::optional<std::string> decideKernelQuotaReclaim(const KernelQuotaReservationRowV2& reservation, double nowMs) {
    if (reservation.state != "reserved" || reservation.lease_expires_at_ms > nowMs) return std::nullopt;
    return reservation.terminal_intent.value_or("released");
  }

  KernelQuotaCutoverPlan decideKernelDefaultWindowCutover(const KernelQuotaRead<>& snapshot, const std::string& id, double nowMs) {
    using native_kernel_contracts::KernelQuotaCutoverPlanAcquire;
    using native_kernel_contracts::KernelQuotaCutoverPlanRefused;
    if (!snapshot.value.is_null()) {
      return KernelQuotaCutoverPlanRefused{.reason = normalizeKernelDefaultWindowCutoverV2(snapshot.value) ? "concurrent_change" : "unavailable"};
    }
    return KernelQuotaCutoverPlanAcquire{
      .marker = KernelDefaultWindowCutoverV2{.v = 2, .id = id, .created_at_ms = nowMs, .expires_at_ms = nowMs + KERNEL_DEFAULT_WINDOW_CUTOVER_LEASE_MS},
      .expireIn = KERNEL_DEFAULT_WINDOW_CUTOVER_LEASE_MS,
    };
  }

  bool kernelDefaultWindowCutoverAcquired(const KernelQuotaRead<>& snapshot, const std::string& id) {
    const std::optional<KernelDefaultWindowCutoverV2> marker = normalizeKernelDefaultWindowCutoverV2(snapshot.value);
    return marker.has_value() && marker->id == id && snapshot.revision.has_value();
  }

  KernelPolicyTiming resolveKernelPolicyTiming(const KernelQuotaPolicySetOptions& options, const KernelQuotaEffectivePolicy& current, double noExpirationMs) {
    return KernelPolicyTiming{
      .windowMs = !options.windowMs ? current.windowMs : normalizeWindow(*options.windowMs, current.windowMs),
      .expiresAtMs = !options.expiresAtMs ? current.expiresAtMs : normalizeExpiration(*options.expiresAtMs, noExpirationMs),
    };
  }

  KernelPolicyWindowGuard decideKernelPolicyWindowGuard(const std::optional<KernelQuotaWindowV2>& window, bool windowPresent, bool reset, double nowMs) {
    if (windowPresent && !window) return KernelPolicyWindowGuardAbort{};
    if (reset && hasLiveKernelReservations(window, nowMs)) return KernelPolicyWindowGuardReclaim{.window = *window};
    return KernelPolicyWindowGuardProceed{};
  }

  KernelQuotaPolicyV2 makeKernelQuotaPolicy(const KernelQuotaIdentity& identity, const KernelQuotaEffectivePolicy& current, double usageLimitRequests,
                                            const KernelPolicyTiming& timing, double noUsageLimit, double nowMs) {
    return KernelQuotaPolicyV2{
      .v = 2,
      .scope = identity.scope,
      .owner = identity.owner,
      .repo = identity.scope == "repo" ? identity.repo : std::nullopt,
      .usage_limit_requests = normalizeUsageLimit(usageLimitRequests, current.limit, noUsageLimit),
      .window_ms = timing.windowMs,
      .expires_at_ms = timing.expiresAtMs,
      .created_at_ms = current.policy ? current.policy->created_at_ms : nowMs,
      .updated_at_ms = nowMs,
    };
  }

  KernelQuotaPolicyWritePlan decideKernelQuotaPolicyWrite(const KernelQuotaIdentity& identity, const KernelQuotaPolicyV2& policy,
                                                          const std::optional<KernelQuotaWindowV2>& currentWindow, bool reset, double nowMs) {
    KernelQuotaWindowV2 baseWindow = reset ? newWindow(identity.scope, identity.owner, identity.repo, policy.window_ms, nowMs)
                                           : windowForEffectivePolicy(currentWindow, identity.scope, identity.owner, identity.repo, policy.window_ms, nowMs).window;
    if (!reset) baseWindow.updated_at_ms = nowMs;
    return KernelQuotaPolicyWritePlan{.policy = policy, .window = std::move(baseWindow)};
  }

  KernelQuotaWindowV2 decideKernelQuotaPolicyDeleteWindow(const KernelQuotaIdentity& identity, const KernelQuotaEffectivePolicy& current, const KernelQuotaDefaults& defaults,
                                                          const std::optional<KernelQuotaWindowV2>& oldWindow, double nowMs) {
    return current.windowMs == defaults.windowMs ? windowForEffectivePolicy(oldWindow, identity.scope, identity.owner, identity.repo, defaults.windowMs, nowMs).window
                                                 : newWindow(identity.scope, identity.owner, identity.repo, defaults.windowMs, nowMs);
  }

  bool kernelQuotaDefaultCutoverBlocks(const KernelQuotaEffectivePolicy& current, const KernelQuotaRead<>& marker) {
    return current.source == "default" && !marker.value.is_null();
  }

  bool kernelQuotaPolicyResetRequested(const KernelQuotaPolicySetOptions& options, double currentWindowMs, double nextWindowMs) {
    return options.resetUsage == true || currentWindowMs != nextWindowMs;
  }

  std::string decideKernelQuotaPolicyDeletePrecondition(const KernelQuotaPolicyDeleteSnapshot& snapshot) {
    if (snapshot.policy.value.is_null()) return "absent";
    return !snapshot.defaultCutover.value.is_null() ? "conflict" : "proceed";
  }
}
