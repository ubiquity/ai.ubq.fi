#pragma once
// Discriminated unions are std::variant over one struct per alternative, named
// <Union><Discriminant>; the discriminant is a static member (a stored string where
// one alternative covers several kinds). ApiKeyPolicy has the boost::json conversions
// described in uos/types.hpp.
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::authority::native_contracts {
  using ::uos::types::ApiKeyUsageRequestV3;
  using ::uos::types::ApiKeyUsageWindowV3;

  using ApiKeyUsageProvider = std::string;  // NonNullable<ApiKeyUsageRequestV3["provider"]>

  /** Trusted API-key policy projection; never populated from a client request body. */
  struct ApiKeyPolicy {
    std::string token_hash;
    std::string key_id;
    double expires_at_ms;
    double usage_limit_requests;
    double window_ms;
    double window_start_ms;
    double usage_reset_at_ms;
    std::string policy_version;
    bool paid_fallback_enabled;
    double paid_fallback_limit_microcredits;
    double paid_fallback_spent_microcredits;
    double paid_fallback_reserved_microcredits;
    std::optional<std::string> paid_fallback_reservation_request_id;
    friend bool operator==(const ApiKeyPolicy&, const ApiKeyPolicy&) = default;
  };

  using ApiKeyUsagePolicyScope = std::string;  // "key_id" | "policy_version" | "window_start_ms"

  /** Named rows already read by a trusted adapter. Keys and storage capabilities stay there. */
  struct ApiKeyUsageSnapshot {
    json::value request;
    json::value window;
    friend bool operator==(const ApiKeyUsageSnapshot&, const ApiKeyUsageSnapshot&) = default;
  };

  struct ApiKeyUsageWritePlan {
    static constexpr std::string_view kind = "write";
    std::vector<std::string> checks;  // readonly ["policy", "window", "request"] | readonly ["request", "window"]
    ApiKeyUsageRequestV3 request;
    ApiKeyUsageWindowV3 window;
    double expireIn;
    friend bool operator==(const ApiKeyUsageWritePlan&, const ApiKeyUsageWritePlan&) = default;
  };

  struct ApiKeyUsageReservationPlanReuse {
    static constexpr std::string_view kind = "reuse";
    friend bool operator==(const ApiKeyUsageReservationPlanReuse&, const ApiKeyUsageReservationPlanReuse&) = default;
  };
  struct ApiKeyUsageReservationPlanFull {
    static constexpr std::string_view kind = "full";
    ApiKeyUsageWindowV3 window;
    friend bool operator==(const ApiKeyUsageReservationPlanFull&, const ApiKeyUsageReservationPlanFull&) = default;
  };
  struct ApiKeyUsageReservationPlanUnavailable {
    static constexpr std::string_view kind = "unavailable";
    std::string reason;  // "request_malformed" | "identity_conflict" | "identity_consumed" | "window_malformed"
    friend bool operator==(const ApiKeyUsageReservationPlanUnavailable&, const ApiKeyUsageReservationPlanUnavailable&) = default;
  };
  using ApiKeyUsageReservationPlan =
    std::variant<ApiKeyUsageWritePlan, ApiKeyUsageReservationPlanReuse, ApiKeyUsageReservationPlanFull, ApiKeyUsageReservationPlanUnavailable>;

  struct ApiKeyUsageDispatchPlanAlreadyDispatched {
    static constexpr std::string_view kind = "already_dispatched";
    friend bool operator==(const ApiKeyUsageDispatchPlanAlreadyDispatched&, const ApiKeyUsageDispatchPlanAlreadyDispatched&) = default;
  };
  struct ApiKeyUsageDispatchPlanInvalid {
    static constexpr std::string_view kind = "invalid";
    friend bool operator==(const ApiKeyUsageDispatchPlanInvalid&, const ApiKeyUsageDispatchPlanInvalid&) = default;
  };
  using ApiKeyUsageDispatchPlan = std::variant<ApiKeyUsageWritePlan, ApiKeyUsageDispatchPlanAlreadyDispatched, ApiKeyUsageDispatchPlanInvalid>;

  struct ApiKeyUsageReleasePlanMissingInvalidSettled {
    std::string kind;  // "missing" | "invalid" | "settled"
    friend bool operator==(const ApiKeyUsageReleasePlanMissingInvalidSettled&, const ApiKeyUsageReleasePlanMissingInvalidSettled&) = default;
  };
  using ApiKeyUsageReleasePlan = std::variant<ApiKeyUsageWritePlan, ApiKeyUsageReleasePlanMissingInvalidSettled>;

  // ---- JSON conversion of the policy projection ---------------------------------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyPolicy& value);
  ApiKeyPolicy tag_invoke(const json::value_to_tag<ApiKeyPolicy>&, const json::value& value);
}
