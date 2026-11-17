#pragma once
// Port of src/handler/http.ts: shared HTTP primitives for the request handler, split out of src/handler.ts.
#include "uos/auth/index.hpp"
#include "uos/authority/native_contracts.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::handler::http {
  using ClientAuthResult = auth::index::AuthenticateClientResult;
  using AuthenticatedClientResult = auth::index::AuthenticateClientResultOk;
  struct RequestDeliveryInfo {
    rt::Promise<void> completed;
    rt::AbortSignal downstreamSignal;
  };
  using DeliveryOutcome = std::string;  // "delivered" | "interrupted" | "unobserved"
  using BodyOutcome = std::string;      // "drained" | "interrupted" | "failed"
  // `(task: Promise<unknown>) => void`; an empty function is undefined.
  using SentinelBackgroundTaskRegistrar = std::function<void(rt::Promise<void> task)>;
  // `Readonly<{ waitUntil?: SentinelBackgroundTaskRegistrar }>`
  struct SentinelBackgroundRuntime {
    SentinelBackgroundTaskRegistrar waitUntil{};
  };

  // `globalThis.EdgeRuntime` (defined by edge hosts, never by Bun): the process-wide slot that
  // sentinelBackgroundTaskRegistrar reads. std::nullopt is undefined.
  std::optional<SentinelBackgroundRuntime>& globalEdgeRuntime();

  // `registrar: SentinelBackgroundTaskRegistrar | undefined`: an empty function is undefined.
  bool scheduleSentinelBackgroundTask(rt::Promise<void> task, SentinelBackgroundTaskRegistrar registrar);

  // The `input` of shouldSignalSentinelProviderDegradation:
  // `Readonly<{ status: number; completed: boolean; removedProviderTriggerClass: string | null }>`.
  struct SentinelProviderDegradationInput {
    double status;
    bool completed;
    std::optional<std::string> removedProviderTriggerClass;
  };
  bool shouldSignalSentinelProviderDegradation(const SentinelProviderDegradationInput& input);

  // `PrincipalAuthResult`: `Readonly<{ token: string | null; method: Readonly<{ kind: "kv_api_key"; key_id: string }> |
  // Exclude<AuthenticatedClientResult["method"], { kind: "kv_api_key" }> }>`.
  struct PrincipalAuthMethodKvApiKey {
    static constexpr std::string_view kind = "kv_api_key";
    std::string key_id;
  };
  using PrincipalAuthMethod =
    std::variant<PrincipalAuthMethodKvApiKey, auth::index::PassportAuthMethod, auth::index::ClientAuthMethodDisabled, auth::index::ClientAuthMethodGithubToken,
                 auth::index::ClientAuthMethodAuthTokensAllowlist, auth::index::ClientAuthMethodAdminAllowlist, auth::index::ClientAuthMethodDenoDeployToken,
                 auth::index::ClientAuthMethodPasskeySession>;
  struct PrincipalAuthResult {
    std::optional<std::string> token;
    PrincipalAuthMethod method;
  };

  rt::Task<std::string> resolveIdempotencyPrincipal(PrincipalAuthResult authResult);
  // An AuthenticatedClientResult is a PrincipalAuthResult (its kv_api_key method is read only for key_id).
  rt::Task<std::string> resolveIdempotencyPrincipal(AuthenticatedClientResult authResult);

  /**
   * Provider-native correlation headers. The gateway never reflects them: it
   * exposes one bounded `x-uos-provider-request-id` whose value has already
   * passed the gateway sanitizer.
   *
   * Every header a provider reader accepts must be listed here, or that
   * provider's own spelling can survive to the client.
   * `getCerebrasProviderRequestId` reads the first four spellings and
   * `getDeepSeekProviderRequestId` reads `x-request-id`, `x-ds-request-id` and
   * `x-deepseek-request-id`.
   */
  inline constexpr std::array<std::string_view, 7> PROVIDER_NATIVE_CORRELATION_HEADERS = {
    "x-request-id",           "x-api-request-id",      "x-oneapi-request-id",       "x-cerebras-request-id",
    "x-ds-request-id",        "x-deepseek-request-id", "x-uos-provider-request-id",
  };

  void scrubProviderNativeCorrelationHeaders(rt::Headers& headers);

  // `policy: ApiKeyPolicy | null` and `telemetry: ResponseTelemetry | null`: nullptr is null.
  rt::Response decorateInferenceQuota(const rt::Response& response, const authority::native_contracts::ApiKeyPolicy* policy,
                                      const openai_telemetry::ResponseTelemetry* telemetry);

  // A path that names an inherited Object.prototype member ("constructor", "toString", ...) reads that
  // member in the TS; request paths always start with "/", so the C++ reads only the listed routes.
  // std::nullopt is null.
  std::optional<std::string> kernelQuotaRouteForRequest(std::string_view method, std::string_view path);

  std::string normalizePath(std::string_view path);

  // std::nullopt is null.
  std::optional<std::string> terminalRouteForRequest(std::string_view method, std::string_view path);

  // `providerRequestId: string | null`: std::nullopt is null.
  rt::Response withProviderRequestId(const rt::Response& response, const std::optional<std::string>& providerRequestId);

  rt::Response withRequestId(const rt::Response& response, const std::string& requestId);
}
