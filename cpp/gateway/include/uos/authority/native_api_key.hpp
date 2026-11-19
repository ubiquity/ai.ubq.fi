#pragma once
// Port of src/authority/native-api-key.ts.
// Usage rows are the typed ApiKeyUsageRequestV3 / ApiKeyUsageWindowV3 records: the normalizers
// validate the stored JSON and return the declared members (the TS returns the stored object itself).
#include "uos/authority/native_contracts.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace uos::authority::native_api_key {
  inline constexpr double API_KEY_USAGE_V3_RESERVATION_LEASE_MS = 5 * 60'000;
  inline constexpr double API_KEY_USAGE_V3_RETENTION_MS = 7.0 * 24 * 60 * 60'000;

  /**
   * Every provider that may own an API-key usage dispatch. The request-row
   * normalizer reads this list, so a provider added here is accepted by the
   * ledger instead of turning its dispatched row into an unreadable one.
   */
  inline constexpr std::array<std::string_view, 11> API_KEY_USAGE_PROVIDERS = {
    "cerebras", "chatgpt_codex", "claude_code", "deepseek", "gemini", "lithos", "openrouter", "removed_provider", "metered", "surplus", "voyage",
  };

  // The value currentApiKeyUsageWindow returns: `{ start: number; reset: number }`.
  struct ApiKeyUsageWindowBounds {
    double start;
    double reset;
    friend bool operator==(const ApiKeyUsageWindowBounds&, const ApiKeyUsageWindowBounds&) = default;
  };

  ApiKeyUsageWindowBounds currentApiKeyUsageWindow(double usageResetAtMs, double windowMs, double nowMs);

  // `policy: Pick<ApiKeyPolicy, ApiKeyUsagePolicyScope | "usage_reset_at_ms">`: members the function does not pick are unused.
  ::uos::types::ApiKeyUsageWindowV3 makeApiKeyUsageWindowV3(const native_contracts::ApiKeyPolicy& policy, double nowMs);

  // `value: unknown`: nullptr is undefined.
  bool isSafeNonNegativeInteger(const json::value* value);
  bool isSafeNonNegativeInteger(const json::value& value);
  bool isSafeNonNegativeInteger(double value);

  // `value: unknown`: nullptr is undefined. The result is null as std::nullopt.
  std::optional<::uos::types::ApiKeyUsageWindowV3> normalizeApiKeyUsageWindowV3(const json::value* value);
  std::optional<::uos::types::ApiKeyUsageWindowV3> normalizeApiKeyUsageWindowV3(const json::value& value);

  // `value: unknown`: nullptr is undefined. The result is null as std::nullopt.
  std::optional<::uos::types::ApiKeyUsageRequestV3> normalizeApiKeyUsageRequestV3(const json::value* value);
  std::optional<::uos::types::ApiKeyUsageRequestV3> normalizeApiKeyUsageRequestV3(const json::value& value);

  // `value: unknown`: nullptr is undefined. The result is null as std::nullopt.
  std::optional<::uos::types::ApiKeyUsageWindowV3> matchingApiKeyUsageWindowV3(const json::value* value, const native_contracts::ApiKeyPolicy& policy);
  std::optional<::uos::types::ApiKeyUsageWindowV3> matchingApiKeyUsageWindowV3(const json::value& value, const native_contracts::ApiKeyPolicy& policy);

  std::string apiKeyUsageV3PolicyVersion(double windowMs);

  double apiKeyUsageV3RetentionMs(double windowResetAtMs, double nowMs);

  bool isApiKeyUsageReservationExpired(const ::uos::types::ApiKeyUsageRequestV3& request, double nowMs);

  /** The adapter supplies the live policy whose hash-row revision it will check. */
  native_contracts::ApiKeyUsageReservationPlan planApiKeyUsageReservation(const native_contracts::ApiKeyPolicy& policy, const std::string& requestId,
                                                                          const std::string& route, const native_contracts::ApiKeyUsageSnapshot& snapshot,
                                                                          double nowMs);

  native_contracts::ApiKeyUsageDispatchPlan planApiKeyUsageDispatch(const native_contracts::ApiKeyPolicy& policy, const std::string& requestId,
                                                                    const std::string& route, const native_contracts::ApiKeyUsageProvider& provider,
                                                                    const native_contracts::ApiKeyUsageSnapshot& snapshot, double nowMs);

  // `expectedRoute?: string`: std::nullopt is undefined.
  native_contracts::ApiKeyUsageReleasePlan planApiKeyUsageReservedRelease(const native_contracts::ApiKeyPolicy& policy, const std::string& requestId,
                                                                          const std::string& reason, const native_contracts::ApiKeyUsageSnapshot& snapshot,
                                                                          double nowMs, const std::optional<std::string>& expectedRoute = std::nullopt);

  /** Only the request-owned transport capability may call this before transport starts. */
  native_contracts::ApiKeyUsageReleasePlan planApiKeyUsageDispatchCancellation(const native_contracts::ApiKeyPolicy& policy, const std::string& requestId,
                                                                               const std::string& route, const native_contracts::ApiKeyUsageProvider& provider,
                                                                               const native_contracts::ApiKeyUsageSnapshot& snapshot, double nowMs);
}
