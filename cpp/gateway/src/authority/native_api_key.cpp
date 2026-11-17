#include "uos/authority/native_api_key.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace uos::authority::native_api_key {
  using native_contracts::ApiKeyPolicy;
  using native_contracts::ApiKeyUsageDispatchPlan;
  using native_contracts::ApiKeyUsageDispatchPlanAlreadyDispatched;
  using native_contracts::ApiKeyUsageDispatchPlanInvalid;
  using native_contracts::ApiKeyUsageProvider;
  using native_contracts::ApiKeyUsageReleasePlan;
  using native_contracts::ApiKeyUsageReleasePlanMissingInvalidSettled;
  using native_contracts::ApiKeyUsageReservationPlan;
  using native_contracts::ApiKeyUsageReservationPlanFull;
  using native_contracts::ApiKeyUsageReservationPlanReuse;
  using native_contracts::ApiKeyUsageReservationPlanUnavailable;
  using native_contracts::ApiKeyUsageSnapshot;
  using native_contracts::ApiKeyUsageWritePlan;
  using ::uos::types::ApiKeyUsageRequestV3;
  using ::uos::types::ApiKeyUsageWindowV3;

  namespace {
    using utils::isRecord;

    // `value.key` on an unknown value (arrays and primitives have no such member).
    const json::value* member(const json::value* value, std::string_view key) { return value != nullptr ? rt::get(*value, key) : nullptr; }
    bool isNull(const json::value* value) { return value != nullptr && value->is_null(); }
    bool numberIs(const json::value* value, double number) {
      const std::optional<double> actual = rt::getNumber(value);
      return actual && *actual == number;
    }
    bool stringIs(const json::value* value, std::string_view text) { return value != nullptr && value->is_string() && value->get_string() == text; }
    // `typeof value === "string" && value` (a non-empty string)
    bool isNonEmptyString(const json::value* value) { return value != nullptr && value->is_string() && !value->get_string().empty(); }
    std::string stringOf(const json::value* value) { return std::string(value->get_string()); }
    std::optional<std::string> stringOrNull(const json::value* value) {
      if (value == nullptr || !value->is_string()) return std::nullopt;
      return std::string(value->get_string());
    }
    std::optional<double> numberOrNull(const json::value* value) { return rt::getNumber(value); }

    bool isApiKeyUsageProvider(const json::value* value) {
      return value != nullptr && value->is_string() &&
             std::find(API_KEY_USAGE_PROVIDERS.begin(), API_KEY_USAGE_PROVIDERS.end(), std::string_view(value->get_string())) != API_KEY_USAGE_PROVIDERS.end();
    }

    ApiKeyUsageWritePlan writePlan(std::vector<std::string> checks, ApiKeyUsageRequestV3 request, ApiKeyUsageWindowV3 window, double nowMs) {
      const double expireIn = apiKeyUsageV3RetentionMs(window.window_reset_at_ms, nowMs);
      return ApiKeyUsageWritePlan{std::move(checks), std::move(request), std::move(window), expireIn};
    }

    ApiKeyUsageReleasePlan releaseKind(std::string kind) { return ApiKeyUsageReleasePlanMissingInvalidSettled{std::move(kind)}; }
  }

  ApiKeyUsageWindowBounds currentApiKeyUsageWindow(double usageResetAtMs, double windowMs, double nowMs) {
    const double initialStart = usageResetAtMs - windowMs;
    if (nowMs < usageResetAtMs) return ApiKeyUsageWindowBounds{initialStart, usageResetAtMs};
    const double elapsedWindows = rt::Math::floor((nowMs - initialStart) / windowMs);
    const double start = initialStart + elapsedWindows * windowMs;
    return ApiKeyUsageWindowBounds{start, start + windowMs};
  }

  ApiKeyUsageWindowV3 makeApiKeyUsageWindowV3(const ApiKeyPolicy& policy, double nowMs) {
    return ApiKeyUsageWindowV3{
      .v = 3,
      .key_id = policy.key_id,
      .policy_version = policy.policy_version,
      .window_start_ms = policy.window_start_ms,
      .window_reset_at_ms = policy.usage_reset_at_ms,
      .committed_requests = 0,
      .reserved_requests = 0,
      .updated_at_ms = nowMs,
    };
  }

  bool isSafeNonNegativeInteger(const json::value* value) {
    const std::optional<double> number = rt::getNumber(value);
    return number && isSafeNonNegativeInteger(*number);
  }

  bool isSafeNonNegativeInteger(const json::value& value) { return isSafeNonNegativeInteger(&value); }

  bool isSafeNonNegativeInteger(double value) { return rt::Number::isSafeInteger(value) && value >= 0; }

  std::optional<ApiKeyUsageWindowV3> normalizeApiKeyUsageWindowV3(const json::value* value) {
    if (!isRecord(value)) return std::nullopt;
    if (!numberIs(member(value, "v"), 3) || !isNonEmptyString(member(value, "key_id")) || !isNonEmptyString(member(value, "policy_version")) ||
        !isSafeNonNegativeInteger(member(value, "window_start_ms")) || !isSafeNonNegativeInteger(member(value, "window_reset_at_ms")) ||
        *rt::getNumber(member(value, "window_reset_at_ms")) <= *rt::getNumber(member(value, "window_start_ms")) ||
        !isSafeNonNegativeInteger(member(value, "committed_requests")) || !isSafeNonNegativeInteger(member(value, "reserved_requests")) ||
        !isSafeNonNegativeInteger(member(value, "updated_at_ms")))
      return std::nullopt;
    return ApiKeyUsageWindowV3{
      .v = 3,
      .key_id = stringOf(member(value, "key_id")),
      .policy_version = stringOf(member(value, "policy_version")),
      .window_start_ms = *rt::getNumber(member(value, "window_start_ms")),
      .window_reset_at_ms = *rt::getNumber(member(value, "window_reset_at_ms")),
      .committed_requests = *rt::getNumber(member(value, "committed_requests")),
      .reserved_requests = *rt::getNumber(member(value, "reserved_requests")),
      .updated_at_ms = *rt::getNumber(member(value, "updated_at_ms")),
    };
  }

  std::optional<ApiKeyUsageWindowV3> normalizeApiKeyUsageWindowV3(const json::value& value) { return normalizeApiKeyUsageWindowV3(&value); }

  std::optional<ApiKeyUsageRequestV3> normalizeApiKeyUsageRequestV3(const json::value* value) {
    if (!isRecord(value)) return std::nullopt;
    const json::value* state = member(value, "state");
    const json::value* provider = member(value, "provider");
    const json::value* dispatchedAtMs = member(value, "dispatched_at_ms");
    const json::value* releasedAtMs = member(value, "released_at_ms");
    const json::value* releaseReason = member(value, "release_reason");
    if (!numberIs(member(value, "v"), 3) || !isNonEmptyString(member(value, "key_id")) || !isNonEmptyString(member(value, "request_id")) ||
        !isNonEmptyString(member(value, "route")) || (!stringIs(state, "reserved") && !stringIs(state, "dispatched") && !stringIs(state, "released")) ||
        !isSafeNonNegativeInteger(member(value, "reserved_at_ms")) || !isSafeNonNegativeInteger(member(value, "lease_expires_at_ms")) ||
        !(isNull(provider) || isApiKeyUsageProvider(provider)) || !(isNull(dispatchedAtMs) || isSafeNonNegativeInteger(dispatchedAtMs)) ||
        !(isNull(releasedAtMs) || isSafeNonNegativeInteger(releasedAtMs)) ||
        !(isNull(releaseReason) || (releaseReason != nullptr && releaseReason->is_string())))
      return std::nullopt;
    if ((stringIs(state, "reserved") && (!isNull(provider) || !isNull(dispatchedAtMs) || !isNull(releasedAtMs) || !isNull(releaseReason))) ||
        (stringIs(state, "dispatched") && (isNull(provider) || isNull(dispatchedAtMs) || !isNull(releasedAtMs) || !isNull(releaseReason))) ||
        (stringIs(state, "released") && (!isNull(provider) || !isNull(dispatchedAtMs) || isNull(releasedAtMs) || isNull(releaseReason))))
      return std::nullopt;
    return ApiKeyUsageRequestV3{
      .v = 3,
      .key_id = stringOf(member(value, "key_id")),
      .request_id = stringOf(member(value, "request_id")),
      .route = stringOf(member(value, "route")),
      .state = stringOf(state),
      .reserved_at_ms = *rt::getNumber(member(value, "reserved_at_ms")),
      .lease_expires_at_ms = *rt::getNumber(member(value, "lease_expires_at_ms")),
      .provider = stringOrNull(provider),
      .dispatched_at_ms = numberOrNull(dispatchedAtMs),
      .released_at_ms = numberOrNull(releasedAtMs),
      .release_reason = stringOrNull(releaseReason),
    };
  }

  std::optional<ApiKeyUsageRequestV3> normalizeApiKeyUsageRequestV3(const json::value& value) { return normalizeApiKeyUsageRequestV3(&value); }

  std::optional<ApiKeyUsageWindowV3> matchingApiKeyUsageWindowV3(const json::value* value, const ApiKeyPolicy& policy) {
    std::optional<ApiKeyUsageWindowV3> window = normalizeApiKeyUsageWindowV3(value);
    if (!window) return std::nullopt;
    return window->key_id == policy.key_id && window->policy_version == policy.policy_version && window->window_start_ms == policy.window_start_ms &&
               window->window_reset_at_ms == policy.usage_reset_at_ms
             ? std::move(window)
             : std::nullopt;
  }

  std::optional<ApiKeyUsageWindowV3> matchingApiKeyUsageWindowV3(const json::value& value, const ApiKeyPolicy& policy) {
    return matchingApiKeyUsageWindowV3(&value, policy);
  }

  std::string apiKeyUsageV3PolicyVersion(double windowMs) { return "v3:" + rt::numberToString(windowMs); }

  double apiKeyUsageV3RetentionMs(double windowResetAtMs, double nowMs) { return rt::Math::max(1, windowResetAtMs + API_KEY_USAGE_V3_RETENTION_MS - nowMs); }

  bool isApiKeyUsageReservationExpired(const ApiKeyUsageRequestV3& request, double nowMs) {
    return request.state == "reserved" && request.lease_expires_at_ms <= nowMs;
  }

  ApiKeyUsageReservationPlan planApiKeyUsageReservation(const ApiKeyPolicy& policy, const std::string& requestId, const std::string& route,
                                                        const ApiKeyUsageSnapshot& snapshot, double nowMs) {
    const std::optional<ApiKeyUsageRequestV3> request = normalizeApiKeyUsageRequestV3(snapshot.request);
    if (!snapshot.request.is_null() && !request) return ApiKeyUsageReservationPlanUnavailable{"request_malformed"};
    if (request) {
      if (request->key_id != policy.key_id || request->request_id != requestId || request->route != route)
        return ApiKeyUsageReservationPlanUnavailable{"identity_conflict"};
      if (request->state != "reserved") return ApiKeyUsageReservationPlanUnavailable{"identity_consumed"};
      return ApiKeyUsageReservationPlanReuse{};
    }
    const std::optional<ApiKeyUsageWindowV3> window =
      snapshot.window.is_null() ? std::optional<ApiKeyUsageWindowV3>(makeApiKeyUsageWindowV3(policy, nowMs)) : matchingApiKeyUsageWindowV3(snapshot.window, policy);
    if (!window) return ApiKeyUsageReservationPlanUnavailable{"window_malformed"};
    if (policy.usage_limit_requests != -1 && window->committed_requests + window->reserved_requests >= policy.usage_limit_requests)
      return ApiKeyUsageReservationPlanFull{*window};
    ApiKeyUsageWindowV3 reserved = *window;
    reserved.reserved_requests = window->reserved_requests + 1;
    reserved.updated_at_ms = nowMs;
    return writePlan({"policy", "window", "request"},
                     ApiKeyUsageRequestV3{
                       .v = 3,
                       .key_id = policy.key_id,
                       .request_id = requestId,
                       .route = route,
                       .state = "reserved",
                       .reserved_at_ms = nowMs,
                       .lease_expires_at_ms = nowMs + API_KEY_USAGE_V3_RESERVATION_LEASE_MS,
                       .provider = std::nullopt,
                       .dispatched_at_ms = std::nullopt,
                       .released_at_ms = std::nullopt,
                       .release_reason = std::nullopt,
                     },
                     std::move(reserved), nowMs);
  }

  ApiKeyUsageDispatchPlan planApiKeyUsageDispatch(const ApiKeyPolicy& policy, const std::string& requestId, const std::string& route,
                                                  const ApiKeyUsageProvider& provider, const ApiKeyUsageSnapshot& snapshot, double nowMs) {
    const std::optional<ApiKeyUsageRequestV3> request = normalizeApiKeyUsageRequestV3(snapshot.request);
    if (!request || request->key_id != policy.key_id || request->request_id != requestId || request->route != route || request->state == "released")
      return ApiKeyUsageDispatchPlanInvalid{};
    if (request->state == "dispatched") return ApiKeyUsageDispatchPlanAlreadyDispatched{};
    const std::optional<ApiKeyUsageWindowV3> window = matchingApiKeyUsageWindowV3(snapshot.window, policy);
    if (!window || window->reserved_requests < 1) return ApiKeyUsageDispatchPlanInvalid{};
    ApiKeyUsageRequestV3 dispatched = *request;
    dispatched.state = "dispatched";
    dispatched.provider = provider;
    dispatched.dispatched_at_ms = nowMs;
    ApiKeyUsageWindowV3 committed = *window;
    committed.committed_requests = window->committed_requests + 1;
    committed.reserved_requests = window->reserved_requests - 1;
    committed.updated_at_ms = nowMs;
    return writePlan({"request", "window"}, std::move(dispatched), std::move(committed), nowMs);
  }

  ApiKeyUsageReleasePlan planApiKeyUsageReservedRelease(const ApiKeyPolicy& policy, const std::string& requestId, const std::string& reason,
                                                        const ApiKeyUsageSnapshot& snapshot, double nowMs, const std::optional<std::string>& expectedRoute) {
    const std::optional<ApiKeyUsageRequestV3> request = normalizeApiKeyUsageRequestV3(snapshot.request);
    if (!request) return releaseKind(snapshot.request.is_null() ? "missing" : "invalid");
    if (request->key_id != policy.key_id || request->request_id != requestId || (expectedRoute && request->route != *expectedRoute)) return releaseKind("invalid");
    if (request->state != "reserved") return releaseKind("settled");
    const std::optional<ApiKeyUsageWindowV3> window = matchingApiKeyUsageWindowV3(snapshot.window, policy);
    if (!window || window->reserved_requests < 1) return releaseKind("invalid");
    ApiKeyUsageRequestV3 released = *request;
    released.state = "released";
    released.released_at_ms = nowMs;
    released.release_reason = reason;
    ApiKeyUsageWindowV3 unreserved = *window;
    unreserved.reserved_requests = window->reserved_requests - 1;
    unreserved.updated_at_ms = nowMs;
    return writePlan({"request", "window"}, std::move(released), std::move(unreserved), nowMs);
  }

  ApiKeyUsageReleasePlan planApiKeyUsageDispatchCancellation(const ApiKeyPolicy& policy, const std::string& requestId, const std::string& route,
                                                             const ApiKeyUsageProvider& provider, const ApiKeyUsageSnapshot& snapshot, double nowMs) {
    const std::optional<ApiKeyUsageRequestV3> request = normalizeApiKeyUsageRequestV3(snapshot.request);
    if (!request) return releaseKind(snapshot.request.is_null() ? "missing" : "invalid");
    if (request->key_id != policy.key_id || request->request_id != requestId || request->route != route) return releaseKind("invalid");
    if (request->state != "dispatched") return releaseKind("settled");
    if (request->provider != provider) return releaseKind("invalid");
    const std::optional<ApiKeyUsageWindowV3> window = matchingApiKeyUsageWindowV3(snapshot.window, policy);
    if (!window || window->committed_requests < 1) return releaseKind("invalid");
    ApiKeyUsageRequestV3 cancelled = *request;
    cancelled.state = "released";
    cancelled.provider = std::nullopt;
    cancelled.dispatched_at_ms = std::nullopt;
    cancelled.released_at_ms = nowMs;
    cancelled.release_reason = "transport_cancelled_before_fetch";
    ApiKeyUsageWindowV3 uncommitted = *window;
    uncommitted.committed_requests = window->committed_requests - 1;
    uncommitted.updated_at_ms = nowMs;
    return writePlan({"request", "window"}, std::move(cancelled), std::move(uncommitted), nowMs);
  }
}
