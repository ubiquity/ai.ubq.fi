#pragma once
// Port of src/paid-fallback/index.ts.
//
// Defaulted TS parameters (`provider = "metered"`, `providerRequestId = null`) are overloads, so a
// caller never builds a std::string temporary inside a co_await expression.
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/authority/native_paid_policy.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::paid_fallback::index {
  // `export { hasStrictPaidFallbackPolicy, hasStrictPaidFallbackKeyPolicy, paidFallbackHashFields } from "../authority/native-paid-policy.ts"`
  using authority::native_paid_policy::hasStrictPaidFallbackPolicy;
  using authority::native_paid_policy::hasStrictPaidFallbackKeyPolicy;
  using authority::native_paid_policy::paidFallbackHashFields;

  // Pick<ApiKeyRecord, "paid_fallback_enabled" | "paid_fallback_limit_microcredits" |
  //   "paid_fallback_spent_microcredits" | "paid_fallback_reserved_microcredits" |
  //   "paid_fallback_reservation_request_id" | "paid_fallback_model_ids" | "paid_fallback_quota_per_credit" |
  //   "paid_fallback_max_exposure_microcredits" | "paid_fallback_pricing_checked_at_ms">
  // Member types are ApiKeyRecord's (uos/types.hpp).
  struct PaidFallbackPolicyFields {
    bool paid_fallback_enabled;
    double paid_fallback_limit_microcredits;
    double paid_fallback_spent_microcredits;
    double paid_fallback_reserved_microcredits;
    std::optional<std::string> paid_fallback_reservation_request_id;
    std::vector<std::string> paid_fallback_model_ids;
    double paid_fallback_quota_per_credit;
    std::optional<json::object> paid_fallback_max_exposure_microcredits{};  // Record<string, number>
    std::optional<double> paid_fallback_pricing_checked_at_ms;
    friend bool operator==(const PaidFallbackPolicyFields&, const PaidFallbackPolicyFields&) = default;
  };

  // `Readonly<{ key_id; request_id; created_at_ms; reserved_microcredits; quota_per_credit;
  // window_reset_at_ms; quota_used_percent: number | null }>`. The TS spells out the same shape
  // as PaidFallbackAdmissionV3 and passes one for the other, so the C++ type is that record.
  using PaidFallbackReservation = authority::native_paid_contracts::PaidFallbackAdmissionV3;

  struct PaidFallbackReservationDecisionReserved {
    static constexpr std::string_view kind = "reserved";
    PaidFallbackReservation reservation;
    friend bool operator==(const PaidFallbackReservationDecisionReserved&, const PaidFallbackReservationDecisionReserved&) = default;
  };
  struct PaidFallbackReservationDecisionSkip {
    static constexpr std::string_view kind = "skip";
    std::string reason;  // "disabled" | "provider_unconfigured" | "model_not_priced"
    friend bool operator==(const PaidFallbackReservationDecisionSkip&, const PaidFallbackReservationDecisionSkip&) = default;
  };
  struct PaidFallbackReservationDecisionBlocked {
    static constexpr std::string_view kind = "blocked";
    std::string reason;  // "limit_exceeded" | "reconciliation_pending" | "invalid_policy" | "concurrent_update"
    std::optional<double> reset_at_ms;
    friend bool operator==(const PaidFallbackReservationDecisionBlocked&, const PaidFallbackReservationDecisionBlocked&) = default;
  };
  using PaidFallbackReservationDecision =
    std::variant<PaidFallbackReservationDecisionReserved, PaidFallbackReservationDecisionSkip, PaidFallbackReservationDecisionBlocked>;

  PaidFallbackPolicyFields defaultPaidFallbackPolicy();

  // The resolved value of initializePaidFallbackPolicy: Pick<PaidFallbackPolicyFields,
  //   "paid_fallback_model_ids" | "paid_fallback_quota_per_credit" | "paid_fallback_pricing_checked_at_ms" |
  //   "paid_fallback_max_exposure_microcredits">, members in the order the TS object literal writes them.
  struct InitializePaidFallbackPolicyResult {
    std::vector<std::string> paid_fallback_model_ids;
    double paid_fallback_quota_per_credit;
    std::optional<json::object> paid_fallback_max_exposure_microcredits{};  // Record<string, number>
    std::optional<double> paid_fallback_pricing_checked_at_ms;
    friend bool operator==(const InitializePaidFallbackPolicyResult&, const InitializePaidFallbackPolicyResult&) = default;
  };

  // `initializePaidFallbackPolicy(signal?: AbortSignal)`. Throws provider::metered::MeteredError
  // ("metered_api_key_missing" / "metered_pricing_invalid", status 503) as the TS does.
  rt::Task<InitializePaidFallbackPolicyResult> initializePaidFallbackPolicy();
  rt::Task<InitializePaidFallbackPolicyResult> initializePaidFallbackPolicy(std::optional<rt::AbortSignal> signal);

  // The `input` of reservePaidFallback (`Parameters<typeof reservePaidFallback>[0]`).
  struct ReservePaidFallbackInput {
    std::string keyId;
    std::string requestId;
    double createdAtMs;
    std::string model;
    std::string route;
    std::string path;
    bool stream;
    std::optional<std::string> reasoning;
    std::optional<bool> allowUnrosteredModel{};
    std::string reason;  // "primary_429" | "primary_quota_blocked" | "primary_subscriptions_unavailable" | "dynamic_paid_model"
    friend bool operator==(const ReservePaidFallbackInput&, const ReservePaidFallbackInput&) = default;
  };

  rt::Task<PaidFallbackReservationDecision> reservePaidFallback(ReservePaidFallbackInput input);

  // `provider: PaidFallbackProvider = "metered"`: the three-argument overload is "metered".
  rt::Task<void> recordMeteredUpstreamResponse(PaidFallbackReservation reservation, rt::Response _response, std::optional<std::string> providerRequestId);
  rt::Task<void> recordMeteredUpstreamResponse(PaidFallbackReservation reservation, rt::Response _response, std::optional<std::string> providerRequestId,
                                               ::uos::types::PaidFallbackProvider provider);

  // `sweep: (() => void) | null`: an empty function is null (restores the real sweep).
  void setPaidFallbackTerminalSweepForTest(std::function<void()> sweep);

  // `provider = "metered"`, `providerRequestId: string | null = null`.
  rt::Task<void> recordMeteredAmbiguousFailure(PaidFallbackReservation reservation);
  rt::Task<void> recordMeteredAmbiguousFailure(PaidFallbackReservation reservation, ::uos::types::PaidFallbackProvider provider);
  rt::Task<void> recordMeteredAmbiguousFailure(PaidFallbackReservation reservation, ::uos::types::PaidFallbackProvider provider,
                                               std::optional<std::string> providerRequestId);

  rt::Task<void> recordMeteredUndispatchedCancellation(PaidFallbackReservation reservation);

  rt::Task<void> recordMeteredPrefetchCancellation(PaidFallbackReservation reservation);

  // `terminalState: "completed" | "failed" | "incomplete" | "cancelled" | "ambiguous"`, `provider = "metered"`.
  rt::Task<void> recordMeteredTerminal(PaidFallbackReservation reservation, std::string terminalState);
  rt::Task<void> recordMeteredTerminal(PaidFallbackReservation reservation, std::string terminalState, ::uos::types::PaidFallbackProvider provider);

  struct SurplusBillingPricing {
    double input_price_per_token;
    double output_price_per_token;
    std::optional<double> cache_read_price_per_token{};
    std::optional<double> cache_write_price_per_token{};
    /** Native Messages billing requires the original exact provider charge. */
    std::optional<bool> require_provider_cost{};  // `?: true`
    friend bool operator==(const SurplusBillingPricing&, const SurplusBillingPricing&) = default;
  };

  struct SurplusUsage {
    std::optional<double> input_tokens;
    std::optional<double> cached_input_tokens;
    std::optional<double> cache_write_input_tokens;
    std::optional<double> output_tokens;
    /** Provider-reported exact charge, when present in the original usage. */
    // The TS tests `hasOwnProperty("buyer_cost_micro")`: undefined is an absent member, null a
    // present one (which then fails the safe-integer check).
    rt::Nullable<double> buyer_cost_micro{};
    friend bool operator==(const SurplusUsage&, const SurplusUsage&) = default;
  };

  /**
   * Surplus returns usage in the terminal Responses event instead of exposing a
   * provider token-log query. Its catalog prices are per token; convert that
   * charge into the existing paid-fallback quota unit before the shared ledger
   * settles the exact gateway reservation. The settlement key is internal and
   * is not recorded as an upstream provider request ID.
   */
  rt::Task<void> recordSurplusUsage(PaidFallbackReservation reservation, std::string settlementRequestId, std::string model, SurplusUsage usage,
                                    SurplusBillingPricing pricing);

  // ---- JSON conversions of the snake_case records (members in declaration order) ----------
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackPolicyFields& value);
  PaidFallbackPolicyFields tag_invoke(const json::value_to_tag<PaidFallbackPolicyFields>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const InitializePaidFallbackPolicyResult& value);
  InitializePaidFallbackPolicyResult tag_invoke(const json::value_to_tag<InitializePaidFallbackPolicyResult>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SurplusBillingPricing& value);
  SurplusBillingPricing tag_invoke(const json::value_to_tag<SurplusBillingPricing>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SurplusUsage& value);
  SurplusUsage tag_invoke(const json::value_to_tag<SurplusUsage>&, const json::value& value);
}
