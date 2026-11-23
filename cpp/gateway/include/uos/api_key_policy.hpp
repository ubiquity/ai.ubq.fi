#pragma once
// Port of src/api-key-policy.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>; `kv?: Kv | null` (an option or `kvOverride`) is
// std::optional<std::shared_ptr<rt::Kv>> (std::nullopt is undefined, which reads getKv(); a null
// pointer is null). `nowMs = Date.now()` defaults and `options = {}` are overloads.
// `policy: Pick<ApiKeyPolicy, ...>` parameters take the whole policy (every TS caller passes one);
// members the function does not pick are unused. `Record<string, string>` header bags are
// rt::OrderedMap<std::string, std::string> in insertion order.
#include "uos/authority/native_api_key.hpp"
#include "uos/authority/native_api_key_policy.hpp"
#include "uos/authority/native_contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace uos::api_key_policy {
  // `export type { ApiKeyPolicy, ApiKeyUsageProvider } from "./authority/native-contracts.ts"`
  using authority::native_contracts::ApiKeyPolicy;
  using authority::native_contracts::ApiKeyUsageProvider;
  // `export { ... } from "./authority/native-api-key.ts"`
  using authority::native_api_key::API_KEY_USAGE_PROVIDERS;
  using authority::native_api_key::API_KEY_USAGE_V3_RESERVATION_LEASE_MS;
  using authority::native_api_key::API_KEY_USAGE_V3_RETENTION_MS;
  using authority::native_api_key::normalizeApiKeyUsageRequestV3;
  using authority::native_api_key::normalizeApiKeyUsageWindowV3;

  double apiKeyUsageV3RetentionMs(double windowResetAtMs);
  double apiKeyUsageV3RetentionMs(double windowResetAtMs, double nowMs);
  // `policy: Pick<ApiKeyPolicy, ApiKeyUsagePolicyScope | "usage_reset_at_ms">`
  ::uos::types::ApiKeyUsageWindowV3 makeApiKeyUsageWindowV3(const ApiKeyPolicy& policy);
  ::uos::types::ApiKeyUsageWindowV3 makeApiKeyUsageWindowV3(const ApiKeyPolicy& policy, double nowMs);

  inline constexpr double API_KEY_POLICY_CACHE_TTL_MS = 30'000;
  /** Retained only so the incident migration can consume old-isolate counters. */
  inline const rt::KvKey API_KEY_USAGE_V2_PREFIX{"uos_ai", "api_key_usage", "v2"};
  inline const rt::KvKey API_KEY_USAGE_V3_PREFIX{"uos_ai", "api_key_usage", "v3"};
  // `[...API_KEY_USAGE_V3_PREFIX, "window"]`
  inline const rt::KvKey API_KEY_USAGE_V3_WINDOW_PREFIX{"uos_ai", "api_key_usage", "v3", "window"};
  // `[...API_KEY_USAGE_V3_PREFIX, "request"]`
  inline const rt::KvKey API_KEY_USAGE_V3_REQUEST_PREFIX{"uos_ai", "api_key_usage", "v3", "request"};

  struct ApiKeyProviderDispatch {
    std::function<void()> markTransportStarted;
    std::function<rt::Task<void>()> cancelBeforeTransport;
  };

  // `Readonly<{ ok: true; policy: ApiKeyPolicy }> | Readonly<{ ok: false; response: Response }>`
  struct ApiKeyPolicyDecisionOk {
    static constexpr bool ok = true;
    ApiKeyPolicy policy;
  };
  struct ApiKeyPolicyDecisionNotOk {
    static constexpr bool ok = false;
    rt::Response response;
  };
  using ApiKeyPolicyDecision = std::variant<ApiKeyPolicyDecisionOk, ApiKeyPolicyDecisionNotOk>;

  struct ApiKeyUsageReservation {
    // `ApiKeyUsageReservation["beforeProviderDispatch"]`: std::nullopt is undefined (no dispatch
    // context: this reservation was already dispatched).
    using BeforeProviderDispatch = std::function<rt::Task<std::optional<ApiKeyProviderDispatch>>(ApiKeyUsageProvider provider)>;
    // `(reason?: string) => Promise<void>`: std::nullopt is an omitted reason.
    using Release = std::function<rt::Task<void>(std::optional<std::string> reason)>;

    ApiKeyPolicy policy;
    std::string request_id;
    std::string route;
    BeforeProviderDispatch beforeProviderDispatch;
    Release release;
  };

  // `Readonly<{ ok: true; reservation: ApiKeyUsageReservation }> | Readonly<{ ok: false; response: Response }>`
  struct ApiKeyUsageReservationDecisionOk {
    static constexpr bool ok = true;
    ApiKeyUsageReservation reservation;
  };
  struct ApiKeyUsageReservationDecisionNotOk {
    static constexpr bool ok = false;
    rt::Response response;
  };
  using ApiKeyUsageReservationDecision = std::variant<ApiKeyUsageReservationDecisionOk, ApiKeyUsageReservationDecisionNotOk>;

  // The `options` of the ApiKeyQuotaDispatchError constructor:
  // `Readonly<{ status?; code?; errorType?; retryAfter?: string | null; headers?: Readonly<Record<string, string>> }>`.
  struct ApiKeyQuotaDispatchErrorOptions {
    std::optional<double> status{};
    std::optional<std::string> code{};
    std::optional<std::string> errorType{};
    rt::Nullable<std::string> retryAfter{};
    std::optional<rt::OrderedMap<std::string, std::string>> headers{};
  };

  // `code` is also stored in rt::JsError::code, so code that reads `error.code` off any caught
  // error sees it as JS does.
  class ApiKeyQuotaDispatchError : public rt::JsError {
  public:
    const double status;
    const std::string code;
    const std::string errorType;
    const std::optional<std::string> retryAfter;  // string | null
    const rt::OrderedMap<std::string, std::string> headers;

    explicit ApiKeyQuotaDispatchError(std::string message_ = "API key quota reservation is no longer available")
      : ApiKeyQuotaDispatchError(std::move(message_), ApiKeyQuotaDispatchErrorOptions{}) {}

    ApiKeyQuotaDispatchError(std::string message_, ApiKeyQuotaDispatchErrorOptions options)
      : rt::JsError(std::move(message_), "ApiKeyQuotaDispatchError"),
        status(options.status.value_or(503)),
        code(options.code.value_or("api_key_quota_reservation_unavailable")),
        errorType(options.errorType.value_or("server_error")),
        retryAfter(resolveRetryAfter(options)),
        headers(resolveHeaders(std::move(options.headers), retryAfter)) {
      rt::JsError::code = code;
    }

  private:
    // The index signature says `string`, but an absent header key is `undefined` at
    // runtime, so the lookup is read as possibly absent before defaulting to null:
    // `options.retryAfter ?? (Object.hasOwn(headers, "Retry-After") ? headers["Retry-After"] : null)`.
    static std::optional<std::string> resolveRetryAfter(const ApiKeyQuotaDispatchErrorOptions& options) {
      if (options.retryAfter.hasValue()) return *options.retryAfter;
      if (options.headers) {
        if (const std::string* header = options.headers->get("Retry-After")) return *header;
      }
      return std::nullopt;
    }

    // `{ ...(options.headers ?? {}) }`, then
    // `if (this.retryAfter && !headers["Retry-After"]) headers["Retry-After"] = this.retryAfter`.
    static rt::OrderedMap<std::string, std::string> resolveHeaders(std::optional<rt::OrderedMap<std::string, std::string>> given,
                                                                   const std::optional<std::string>& resolvedRetryAfter) {
      rt::OrderedMap<std::string, std::string> result = given ? std::move(*given) : rt::OrderedMap<std::string, std::string>();
      const std::string* current = result.get("Retry-After");
      if (resolvedRetryAfter && !resolvedRetryAfter->empty() && (current == nullptr || current->empty())) result.set("Retry-After", *resolvedRetryAfter);
      return result;
    }
  };

  // `export { looksLikeUosApiKey, apiKeyPolicyFromHashRecord } from "./authority/native-api-key-policy.ts"`
  using authority::native_api_key_policy::looksLikeUosApiKey;
  using authority::native_api_key_policy::apiKeyPolicyFromHashRecord;

  /** Legacy key for one-way V2-to-V3 incident reconciliation only. */
  // `[...API_KEY_USAGE_V2_PREFIX, key_id, policy_version without its "v3:" prefix, window_start_ms]`
  rt::KvKey apiKeyUsageV2Key(const ApiKeyPolicy& policy);

  // `[...API_KEY_USAGE_V3_WINDOW_PREFIX, key_id, policy_version, window_start_ms]`
  rt::KvKey apiKeyUsageV3WindowKey(const ApiKeyPolicy& policy);

  // `[...API_KEY_USAGE_V3_REQUEST_PREFIX, key_id, policy_version, window_start_ms, requestId]`
  rt::KvKey apiKeyUsageV3RequestKey(const ApiKeyPolicy& policy, const std::string& requestId);

  // `policy: ApiKeyPolicy | null`: nullptr is null.
  rt::OrderedMap<std::string, std::string> apiKeyRateLimitPolicyHeaders(const ApiKeyPolicy* policy);

  rt::Task<void> reclaimExpiredApiKeyUsageReservationsV3(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy);
  rt::Task<void> reclaimExpiredApiKeyUsageReservationsV3(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, double nowMs);

  // The `options` of reserveApiKeyUsageV3: `Readonly<{ kv?: Kv | null; nowMs?: number; deferWhenFull?: boolean }>`.
  struct ReserveApiKeyUsageV3Options {
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::optional<double> nowMs{};
    std::optional<bool> deferWhenFull{};
  };

  /**
   * Auth only proves that a key is valid. Route admission is a separate V3
   * reservation so non-inference endpoints never consume or block on quota.
   */
  rt::Task<ApiKeyUsageReservationDecision> reserveApiKeyUsageV3(ApiKeyPolicy policy, std::string requestId, std::string route);
  rt::Task<ApiKeyUsageReservationDecision> reserveApiKeyUsageV3(ApiKeyPolicy policy, std::string requestId, std::string route, ReserveApiKeyUsageV3Options options);

  rt::Task<double> getApiKeyUsageV3(ApiKeyPolicy policy);
  rt::Task<double> getApiKeyUsageV3(ApiKeyPolicy policy, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  rt::Task<bool> hasLiveApiKeyUsageReservationsV3(std::shared_ptr<rt::Kv> kv, std::string keyId);
  rt::Task<bool> hasLiveApiKeyUsageReservationsV3(std::shared_ptr<rt::Kv> kv, std::string keyId, double nowMs);

  rt::Task<void> reclaimApiKeyUsageReservationsForKeyV3(std::shared_ptr<rt::Kv> kv, std::string keyId);
  rt::Task<void> reclaimApiKeyUsageReservationsForKeyV3(std::shared_ptr<rt::Kv> kv, std::string keyId, double nowMs);

  rt::Task<void> deleteApiKeyUsageV3(std::shared_ptr<rt::Kv> kv, std::string keyId);

  // The `options` of authenticateApiKeyToken: `Readonly<{ kv?: Kv | null; nowMs?: number }>`.
  struct AuthenticateApiKeyTokenOptions {
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::optional<double> nowMs{};
  };

  rt::Task<ApiKeyPolicyDecision> authenticateApiKeyToken(std::string token);
  rt::Task<ApiKeyPolicyDecision> authenticateApiKeyToken(std::string token, AuthenticateApiKeyTokenOptions options);

  // `keyId?: string`: the overload without it (and an empty keyId) clears every cached policy.
  void invalidateApiKeyPolicy();
  void invalidateApiKeyPolicy(const std::string& keyId);

  void resetApiKeyPolicyCacheForTest();

  // `policy: ApiKeyPolicy | null`: nullptr is null. std::nullopt is null.
  std::optional<double> apiKeyQuotaUsedPercent(const ApiKeyPolicy* policy);
}
