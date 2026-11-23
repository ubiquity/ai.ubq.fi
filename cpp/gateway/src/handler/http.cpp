// Port of src/handler/http.ts.
#include "uos/handler/http.hpp"

#include "uos/api_key_policy.hpp"
#include "uos/codex/quota.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <type_traits>
#include <utility>

namespace uos::handler::http {
  namespace {
    SentinelBackgroundTaskRegistrar sentinelBackgroundTaskRegistrar() {
      const std::optional<SentinelBackgroundRuntime>& edgeRuntime = globalEdgeRuntime();
      if (edgeRuntime && edgeRuntime->waitUntil) {
        return edgeRuntime->waitUntil;
      }
      return {};
    }
  }

  std::optional<SentinelBackgroundRuntime>& globalEdgeRuntime() {
    static std::optional<SentinelBackgroundRuntime> edgeRuntime;
    return edgeRuntime;
  }

  bool scheduleSentinelBackgroundTask(rt::Promise<void> task, SentinelBackgroundTaskRegistrar registrar) {
    const SentinelBackgroundTaskRegistrar waitUntil = registrar ? registrar : sentinelBackgroundTaskRegistrar();
    if (!waitUntil) return false;
    try {
      waitUntil(task);
      return true;
    } catch (...) {
      return false;
    }
  }

  bool shouldSignalSentinelProviderDegradation(const SentinelProviderDegradationInput& input) {
    return input.status >= 200 && input.status < 400 && input.completed && input.removedProviderTriggerClass.has_value();
  }

  // `assertNeverAuthMethod` (the TS exhaustiveness guard, unreachable at run time) has no C++ twin:
  // the static_assert below pins the number of method kinds these branches cover.
  rt::Task<std::string> resolveIdempotencyPrincipal(PrincipalAuthResult authResult) {
    if (const auto* apiKey = std::get_if<PrincipalAuthMethodKvApiKey>(&authResult.method)) co_return "api-key:" + apiKey->key_id;
    if (const auto* github = std::get_if<auth::index::ClientAuthMethodGithubToken>(&authResult.method)) {
      co_return "github-repo:" + rt::toLowerCase(github->owner) + "/" + rt::toLowerCase(github->repo);
    }
    if (const auto* passkey = std::get_if<auth::index::ClientAuthMethodPasskeySession>(&authResult.method)) co_return "passkey-user:" + passkey->user_id;
    if (const auto* passport = std::get_if<auth::index::PassportAuthMethod>(&authResult.method)) co_return passport->principal;
    if (std::holds_alternative<auth::index::ClientAuthMethodAuthTokensAllowlist>(authResult.method)) {
      co_return "auth-method:" + std::string(auth::index::ClientAuthMethodAuthTokensAllowlist::kind);
    }
    if (std::holds_alternative<auth::index::ClientAuthMethodAdminAllowlist>(authResult.method)) {
      co_return "auth-method:" + std::string(auth::index::ClientAuthMethodAdminAllowlist::kind);
    }
    if (std::holds_alternative<auth::index::ClientAuthMethodDenoDeployToken>(authResult.method)) {
      co_return "auth-method:" + std::string(auth::index::ClientAuthMethodDenoDeployToken::kind);
    }
    static_assert(std::variant_size_v<PrincipalAuthMethod> == 8, "every method kind is handled above or below");
    // "disabled"
    if (rt::truthy(authResult.token)) {
      const std::string digest = co_await utils::sha256Hex(*authResult.token);
      co_return "bearer-sha256:" + digest;
    }
    co_return "local-auth-disabled";
  }

  rt::Task<std::string> resolveIdempotencyPrincipal(AuthenticatedClientResult authResult) {
    PrincipalAuthMethod method = std::visit(
      [](const auto& alternative) -> PrincipalAuthMethod {
        if constexpr (std::is_same_v<std::decay_t<decltype(alternative)>, auth::index::ClientAuthMethodKvApiKey>) {
          return PrincipalAuthMethodKvApiKey{.key_id = alternative.key_id};
        } else {
          return alternative;
        }
      },
      authResult.method);
    PrincipalAuthResult principal{.token = authResult.token, .method = std::move(method)};
    auto task = resolveIdempotencyPrincipal(std::move(principal));
    std::string resolved = co_await std::move(task);
    co_return resolved;
  }

  std::string normalizePath(std::string_view path) {
    if (path == "/") return std::string(path);
    // Equivalent to `path.replace(/\/+$/, "")`, without the quadratic
    // backtracking that pattern needs when a request path ends in many slashes.
    std::size_t end = path.size();
    while (end > 0 && path[end - 1] == '/') end -= 1;
    return std::string(path.substr(0, end));
  }

  rt::Response withRequestId(const rt::Response& response, const std::string& requestId) {
    rt::Headers headers(response.headers());
    headers.set("x-uos-request-id", requestId);
    rt::ResponseInit init;
    init.status = response.status();
    init.statusText = response.statusText();
    init.headers = std::move(headers);
    return rt::Response(rt::bodyInit(response.body()), std::move(init));
  }

  rt::Response decorateInferenceQuota(const rt::Response& response, const authority::native_contracts::ApiKeyPolicy* policy,
                                      const openai_telemetry::ResponseTelemetry* telemetry) {
    std::optional<double> usedPercent;
    if (telemetry && telemetry->quotaUsedPercent != rt::undefined) {
      if (telemetry->quotaUsedPercent != nullptr) usedPercent = *telemetry->quotaUsedPercent;
    } else {
      usedPercent = api_key_policy::apiKeyQuotaUsedPercent(policy);
    }
    std::optional<codex::quota::ClientQuotaSnapshot> snapshot;
    if (usedPercent) snapshot = codex::quota::ClientQuotaSnapshot{.used_percent = *usedPercent};
    const rt::Response codexDecorated = codex::quota::withCodexQuotaHeaders(response, snapshot);
    rt::Headers headers(codexDecorated.headers());
    for (const auto& [name, value] : api_key_policy::apiKeyRateLimitPolicyHeaders(policy)) headers.set(name, value);
    rt::ResponseInit init;
    init.status = codexDecorated.status();
    init.statusText = codexDecorated.statusText();
    init.headers = std::move(headers);
    return rt::Response(rt::bodyInit(codexDecorated.body()), std::move(init));
  }

  namespace {
    // std::nullopt is null.
    std::optional<std::string> providerRequestIdHeaderValue(const std::optional<std::string>& value) {
      if (!value) return std::nullopt;
      const std::string requestId = rt::trim(*value);
      if (requestId.empty() || rt::length(requestId) > 256) return std::nullopt;
      for (const std::uint32_t code : rt::codePoints(requestId)) {
        if (code < 0x20 || code == 0x7f) return std::nullopt;
      }
      return requestId;
    }
  }

  void scrubProviderNativeCorrelationHeaders(rt::Headers& headers) {
    for (const std::string_view header : PROVIDER_NATIVE_CORRELATION_HEADERS) headers.delete_(header);
  }

  rt::Response withProviderRequestId(const rt::Response& response, const std::optional<std::string>& providerRequestId) {
    const std::optional<std::string> requestId = providerRequestIdHeaderValue(providerRequestId);
    rt::Headers headers(response.headers());
    scrubProviderNativeCorrelationHeaders(headers);
    if (requestId) headers.set("x-uos-provider-request-id", *requestId);
    rt::ResponseInit init;
    init.status = response.status();
    init.statusText = response.statusText();
    init.headers = std::move(headers);
    return rt::Response(rt::bodyInit(response.body()), std::move(init));
  }

  namespace {
    /** Exact-path POST routes; the two readers below add their GET-only cases. */
    const rt::OrderedMap<std::string, std::string>& POST_TERMINAL_ROUTES() {
      static const rt::OrderedMap<std::string, std::string> routes{
        {"/uos/embeddings", "embeddings"},
        {"/uos/embedding-jobs", "embeddings.jobs.create"},
        {"/v1/chat/completions", "chat.completions"},
        {"/v1/responses", "responses"},
        {"/v1/decisions", "decisions"},
        {"/v1/systemone", "systemone"},
        {"/v1/images/generations", "images.generations"},
        {"/v1/images/edits", "images.edits"},
      };
      return routes;
    }

    std::optional<std::string> postTerminalRoute(std::string_view path) {
      if (const std::string* route = POST_TERMINAL_ROUTES().get(path)) return *route;
      return std::nullopt;
    }
  }

  std::optional<std::string> terminalRouteForRequest(std::string_view method, std::string_view path) {
    if (method == "POST") return postTerminalRoute(path);
    if (method == "GET" && rt::startsWith(path, "/uos/embedding-jobs/")) return "embeddings.jobs.get";
    return std::nullopt;
  }

  std::optional<std::string> kernelQuotaRouteForRequest(std::string_view method, std::string_view path) {
    return method == "POST" ? postTerminalRoute(path) : std::nullopt;
  }
}
