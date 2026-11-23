// Port of src/auth/index.ts.
#include "uos/auth/index.hpp"
#include "uos/rt/memory.hpp"

#include "uos/api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/auth/local_admin.hpp"
#include "uos/auth/local_development_key.hpp"
#include "uos/auth/passkeys.hpp"
#include "uos/auth/passport.hpp"
#include "uos/config.hpp"
#include "uos/http.hpp"
#include "uos/kernel/attestation.hpp"
#include "uos/kernel/policy_queue.hpp"
#include "uos/kernel/usage.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"
#include "uos/utils.hpp"

#include <exception>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

namespace uos::auth::index {
  namespace {
    using config::config;

    constexpr std::string_view GITHUB_API_BASE_URL = "https://api.github.com";
    constexpr double GITHUB_TOKEN_CACHE_TTL_MS = 5 * 60'000;
    // `new Map<string, number>()`: only get/set, iteration order is never observed.
    std::unordered_map<std::string, double>& githubTokenCache() {
      static std::unordered_map<std::string, double> cache;
      return cache;
    }

    bool looksLikeGitHubToken(const std::string& token) {
      const std::string trimmed = rt::trim(token);
      return rt::startsWith(trimmed, "gh") || rt::startsWith(trimmed, "github_pat_");
    }

    // `{ ok: true } | { ok: false; reason: "rejected" | "upstream_unavailable" }`: std::nullopt is ok.
    using GitHubTokenRepoAccessResult = std::optional<std::string>;

    // Cancels a body the caller does not read (`await res.body?.cancel()`).
    rt::Task<void> cancelBody(rt::Response res) {
      if (std::optional<rt::ByteStream> body = res.body()) co_await body->cancel();
    }

    rt::Task<GitHubTokenRepoAccessResult> verifyGitHubTokenRepoAccess(std::string token, std::string owner, std::string repo) {
      rt::RequestInit init;
      init.method = "GET";
      init.headers = rt::Headers{{"Authorization", "Bearer " + token},
                                 {"Accept", "application/vnd.github+json"},
                                 {"X-GitHub-Api-Version", "2022-11-28"},
                                 {"User-Agent", "ai.ubq.fi"}};
      init.redirect = "manual";
      const std::string url = std::string(GITHUB_API_BASE_URL) + "/repos/" + owner + "/" + repo;
      auto call = rt::fetch(url, std::move(init));
      rt::Response res = co_await std::move(call);

      const int status = res.status();

      try {
        co_await cancelBody(res);
      } catch (...) {
        // ignore
      }

      if (res.ok()) co_return std::nullopt;
      if (status == 401 || status == 404) {
        co_return std::string("rejected");
      }
      co_return std::string("upstream_unavailable");
    }

    struct GitHubBearerVerificationVerified {
      static constexpr std::string_view kind = "verified";
      std::string owner;
      std::string repo;
      std::string stateId;
      std::string cacheKey;
      bool cacheHit;
    };
    struct GitHubBearerVerificationRefused {
      std::string kind;  // "rejected" | "unavailable"
      rt::Response response;
    };
    // `... | null`: std::nullopt is null.
    using GitHubBearerVerification = std::optional<std::variant<GitHubBearerVerificationVerified, GitHubBearerVerificationRefused>>;

    std::string_view verificationKind(const GitHubBearerVerification& verification) {
      if (!verification) return {};
      if (std::holds_alternative<GitHubBearerVerificationVerified>(*verification)) return GitHubBearerVerificationVerified::kind;
      return std::get<GitHubBearerVerificationRefused>(*verification).kind;
    }

    GitHubBearerVerification refused(std::string kind, rt::Response response) {
      return GitHubBearerVerificationRefused{.kind = std::move(kind), .response = std::move(response)};
    }

    rt::Task<GitHubBearerVerification> verifyGitHubBearer(rt::Request req, std::string token) {
      if (!looksLikeGitHubToken(token)) co_return std::nullopt;
      // An explicitly configured admin token remains an allowlisted credential,
      // even if its value happens to use a GitHub token prefix. Let the normal
      // admin-token check handle it without attempting GitHub attestation.
      if (config.adminTokens.contains(token)) co_return std::nullopt;

      const std::optional<kernel::attestation::GitHubRepo> repoHeaders = kernel::attestation::getGitHubRepoHeaders(req);
      const std::string kernelToken = rt::trim(req.headers().get("X-Ubiquity-Kernel-Token").value_or(""));
      if (!repoHeaders && kernelToken.empty()) {
        co_return refused("rejected", http::openaiError(401, "Unauthorized", "invalid_api_key"));
      }

      kernel::attestation::VerifyKernelAttestationOptions options{.token = token};
      if (repoHeaders) {
        options.owner = repoHeaders->owner;
        options.repo = repoHeaders->repo;
      }
      auto attestationTask = kernel::attestation::verifyKernelAttestation(req, std::move(options));
      const kernel::attestation::KernelAttestationResult attestation = co_await std::move(attestationTask);
      if (const auto* failure = std::get_if<kernel::attestation::KernelAttestationFailure>(&attestation)) {
        co_return refused(failure->response.status() == 401 ? "rejected" : "unavailable", failure->response);
      }
      const kernel::attestation::KernelAttestationPayload& payload = std::get<kernel::attestation::KernelAttestationSuccess>(attestation).payload;
      const std::string owner = payload.owner;
      const std::string repo = payload.repo;
      const std::string stateId = payload.state_id;

      std::string cacheInput = token + ":" + owner + "/" + repo;
      const std::string cacheKey = co_await utils::sha256Base64Url(std::move(cacheInput));
      const auto cached = githubTokenCache().find(cacheKey);
      const double cachedUntil = cached == githubTokenCache().end() ? 0 : cached->second;
      if (cachedUntil > rt::dateNow()) {
        co_return GitHubBearerVerificationVerified{.owner = owner, .repo = repo, .stateId = stateId, .cacheKey = cacheKey, .cacheHit = true};
      }

      bool failed = false;
      rt::Reason failure;
      GitHubTokenRepoAccessResult access;
      try {
        access = co_await verifyGitHubTokenRepoAccess(token, owner, repo);
      } catch (...) {
        failed = true;
        failure = std::current_exception();
      }
      if (failed) {
        rt::console::error("[ai.ubq.fi] GitHub token verification failed:", rt::console::Value(failure));
        co_return refused("unavailable", http::openaiError(502, "Failed to verify GitHub token", "bad_gateway"));
      }
      if (access && *access == "rejected") {
        co_return refused("rejected", http::openaiError(401, "Invalid GitHub token for repo", "invalid_auth_for_repo"));
      }
      if (access) {
        co_return refused("unavailable", http::openaiError(502, "Failed to verify GitHub token", "bad_gateway"));
      }
      co_return GitHubBearerVerificationVerified{.owner = owner, .repo = repo, .stateId = stateId, .cacheKey = cacheKey, .cacheHit = false};
    }

    std::string getRequestPath(const rt::Request& req);

    // `{ ok: true; method: ClientAuthMethod } | { ok: false; response: Response } | null`
    struct GitHubTokenAuthentication {
      bool ok;
      std::optional<ClientAuthMethod> method;
      std::optional<rt::Response> response;
    };

    rt::Task<std::optional<GitHubTokenAuthentication>> authenticateGitHubToken(rt::Request req, std::string token) {
      const GitHubBearerVerification verification = co_await verifyGitHubBearer(req, token);
      if (!verification) co_return std::nullopt;
      if (const auto* refusal = std::get_if<GitHubBearerVerificationRefused>(&*verification)) {
        co_return GitHubTokenAuthentication{.ok = false, .method = std::nullopt, .response = refusal->response};
      }
      const GitHubBearerVerificationVerified& verified = std::get<GitHubBearerVerificationVerified>(*verification);

      const std::string owner = verified.owner;
      const std::string repo = verified.repo;
      const std::string stateId = verified.stateId;
      if (!verified.cacheHit) {
        auto& cache = githubTokenCache();
        cache[verified.cacheKey] = rt::dateNow() + GITHUB_TOKEN_CACHE_TTL_MS;
        rt::memory::setCache(rt::memory::Cache::github_auth, cache.size(),
                             cache.size() * (sizeof(std::pair<const std::string, double>) + verified.cacheKey.size() + 1));
      }
      const kernel::quota_v2::KernelQuotaPolicyStateDecision policyState = co_await kernel::usage::resolveKernelQuotaPolicyState(owner, repo);
      if (const auto* notOk = std::get_if<kernel::quota_v2::KernelQuotaPolicyStateDecisionNotOk>(&policyState)) {
        co_return GitHubTokenAuthentication{.ok = false, .method = std::nullopt, .response = notOk->response};
      }
      const auto& state = std::get<kernel::quota_v2::KernelQuotaPolicyStateDecisionOk>(policyState);
      if (!state.has_policy) {
        const std::string path = getRequestPath(req);
        co_await kernel::policy_queue::recordKernelPolicyQueue(owner, repo, path);
      }
      ClientAuthMethod method = ClientAuthMethodGithubToken{.owner = owner, .repo = repo, .state_id = stateId, .limit_scope = state.limit_scope};
      co_return GitHubTokenAuthentication{.ok = true, .method = std::move(method), .response = std::nullopt};
    }

    bool looksLikeUbqAiClientToken(const std::string& token) { return authority::native_api_key_policy::looksLikeUosApiKey(token); }

    std::string classifyToken(const std::string& token) {
      static const rt::Regex HEX64("^[0-9a-fA-F]{64}$", "");
      const std::string trimmed = rt::trim(token);
      if (trimmed.empty()) return "unset";
      if (rt::startsWith(trimmed, "ddw_")) return "deno_deploy_like(ddw_)";
      if (looksLikeGitHubToken(trimmed)) return "github_prefix";
      if (rt::startsWith(trimmed, "uos_ai_session_")) return "uos_ai_session_prefix";
      if (authority::native_api_key_policy::looksLikeUosApiKey(trimmed)) return "uos_api_key";
      if (HEX64.test(trimmed)) return "hex64";
      if (rt::includes(trimmed, "_")) return "has_underscore";
      return "other";
    }

    std::string getRequestPath(const rt::Request& req) {
      try {
        return rt::URL(req.url()).pathname();
      } catch (const rt::JsError&) {
        return "unknown";
      }
    }

    /**
     * How a local development request is admitted. `"admin"` is the fully checked
     * listener bypass (a bound loopback peer plus origin checks); `"legacy"` is the
     * hostname-only fallback for a development checkout whose Git revision is
     * unknown, which stays policy-free. `null` means neither applies.
     *
     * The production timeline takes neither: the VPS serves Caddy-proxied public
     * traffic on its loopback listener, so a loopback peer or URL there proves
     * nothing about the caller (#262). It is checked before the listener bypass,
     * which also refuses it on its own.
     */
    enum class LocalClientAuthMode { admin, legacy };

    // std::nullopt is null.
    std::optional<LocalClientAuthMode> resolveLocalClientAuthMode(const rt::Request& req) {
      if (config.isProductionTimeline) return std::nullopt;
      if (local_admin::isAdminAuthDisabledForRequest(req)) return LocalClientAuthMode::admin;
      if (config.isDeploy || config::runtimeGitSha() != "unknown") return std::nullopt;
      try {
        const std::string hostname = rt::toLowerCase(rt::URL(req.url()).hostname());
        return hostname == "localhost" || hostname == "127.0.0.1" || hostname == "::1" || hostname == "[::1]" ? std::optional(LocalClientAuthMode::legacy)
                                                                                                             : std::nullopt;
      } catch (const rt::JsError&) {
        return std::nullopt;
      }
    }

    // `Omit<AuthLogEntry, "scope" | "token_present" | "token_shape">`, the entry a logger receives.
    struct ClientAuthLogEntry {
      bool ok;
      std::string method;
      std::optional<double> status{};
      std::optional<std::string> reason{};
    };

    // `AuthLogEntry`: `{ scope, ok, method, status?, reason?, token_present, token_shape }`, built as
    // `{ scope, token_present, token_shape, ...entry }` by both loggers.
    struct AuthLogEntry {
      std::string scope;  // "client" | "admin"
      bool token_present;
      std::optional<std::string> token_shape;
      ClientAuthLogEntry entry;
    };

    json::object getAuthHeaderSnapshot(const rt::Request& req) {
      const std::string owner = rt::trim(req.headers().get("X-GitHub-Owner").value_or(""));
      const std::string repo = rt::trim(req.headers().get("X-GitHub-Repo").value_or(""));
      const std::string installationId = rt::trim(req.headers().get("X-GitHub-Installation-Id").value_or(""));
      const bool kernelTokenPresent = !rt::trim(req.headers().get("X-Ubiquity-Kernel-Token").value_or("")).empty();
      json::object snapshot;
      snapshot["x-github-owner"] = owner.empty() ? json::value(nullptr) : json::value(owner);
      snapshot["x-github-repo"] = repo.empty() ? json::value(nullptr) : json::value(repo);
      snapshot["x-github-installation-id"] = installationId.empty() ? json::value(nullptr) : json::value(installationId);
      snapshot["x-ubiquity-kernel-token"] = kernelTokenPresent ? "present" : "missing";
      return snapshot;
    }

    void logAuthDecision(const rt::Request& req, const AuthLogEntry& entry) {
      if (entry.entry.ok) return;
      json::object payload;
      payload["scope"] = entry.scope;
      payload["token_present"] = entry.token_present;
      payload["token_shape"] = entry.token_shape ? json::value(*entry.token_shape) : json::value(nullptr);
      payload["ok"] = entry.entry.ok;
      payload["method"] = entry.entry.method;
      rt::objectSet(payload, "status", entry.entry.status);
      rt::objectSet(payload, "reason", entry.entry.reason);
      payload["path"] = getRequestPath(req);
      payload["headers"] = getAuthHeaderSnapshot(req);
      const std::string line = rt::jsonStringify(json::value(std::move(payload)));
      rt::console::warn("[ai.ubq.fi] auth", line);
    }

    rt::Task<std::optional<passkeys::PasskeySession>> getPasskeyCookieSessionForRequest(const rt::Request& req) {
      rt::Headers passkeyHeaders(req.headers());
      passkeyHeaders.delete_("authorization");
      rt::RequestInit init;
      init.headers = std::move(passkeyHeaders);
      return passkeys::getPasskeySessionForRequest(rt::Request(req.url(), std::move(init)));
    }

    using ClientAuthLogger = std::function<void(ClientAuthLogEntry entry)>;

    AuthenticateClientResult passkeyClientResult(std::optional<std::string> token, const passkeys::PasskeySession& passkeySession) {
      return AuthenticateClientResultOk{
        .token = std::move(token),
        .method =
          ClientAuthMethodPasskeySession{
            .user_id = passkeySession.user.id,
            .handle = passkeySession.user.handle,
            .is_admin = passkeys::isPasskeyUserAdmin(passkeySession.user),
            .credential_count = static_cast<double>(passkeySession.user.credential_ids.size()),
          },
      };
    }

    AuthenticateClientResult clientRefusal(rt::Response response) { return AuthenticateClientResultNotOk{.response = std::move(response)}; }

    rt::Task<AuthenticateClientResult> authenticateClientWithoutToken(rt::Request req, ClientAuthLogger logClientAuth) {
      const std::optional<passkeys::PasskeySession> passkeySession = co_await passkeys::getPasskeySessionForRequest(req);
      if (passkeySession) {
        logClientAuth({.ok = true, .method = "passkey_session"});
        co_return passkeyClientResult(passkeySession->token, *passkeySession);
      }
      logClientAuth({.ok = false, .method = "missing", .status = 401, .reason = "missing_token"});
      co_return clientRefusal(http::openaiError(401, "Unauthorized", "invalid_api_key"));
    }

    rt::Task<AuthenticateClientResult> authenticateClientSessionToken(rt::Request req, std::string token, ClientAuthLogger logClientAuth) {
      const std::optional<passkeys::PasskeySession> passkeySession = co_await passkeys::getPasskeySessionForRequest(req);
      if (!passkeySession) {
        logClientAuth({.ok = false, .method = "passkey_session", .status = 401, .reason = "invalid_api_key"});
        co_return clientRefusal(http::openaiError(401, "Unauthorized", "invalid_api_key"));
      }
      logClientAuth({.ok = true, .method = "passkey_session"});
      co_return passkeyClientResult(token, *passkeySession);
    }

    rt::Task<std::optional<AuthenticateClientResult>> authenticateClientGitHubToken(rt::Request req, std::string token, ClientAuthLogger logClientAuth) {
      const std::optional<GitHubTokenAuthentication> githubResult = co_await authenticateGitHubToken(req, token);
      if (githubResult && githubResult->ok) {
        logClientAuth({.ok = true, .method = "github_token"});
        co_return AuthenticateClientResultOk{.token = token, .method = *githubResult->method};
      }
      if (!githubResult) co_return std::nullopt;
      // A rejected GitHub credential may still be accompanied by a browser session
      // cookie, which is then accepted as the passkey session.
      if (githubResult->response->status() == 401 && req.headers().has("cookie")) {
        const std::optional<passkeys::PasskeySession> passkeySession = co_await getPasskeyCookieSessionForRequest(req);
        if (passkeySession) {
          logClientAuth({.ok = true, .method = "passkey_session"});
          co_return passkeyClientResult(passkeySession->token, *passkeySession);
        }
      }
      logClientAuth({.ok = false, .method = "github_token", .status = static_cast<double>(githubResult->response->status()), .reason = "github_token_rejected"});
      co_return clientRefusal(*githubResult->response);
    }

    rt::Task<std::optional<AuthenticateClientResult>> authenticateClientKvApiKey(std::string token, std::shared_ptr<rt::Kv> kv, ClientAuthLogger logClientAuth) {
      if (!authority::native_api_key_policy::looksLikeUosApiKey(token)) co_return std::nullopt;
      api_key_policy::AuthenticateApiKeyTokenOptions options;
      options.kv = kv;
      auto authentication = api_key_policy::authenticateApiKeyToken(token, std::move(options));
      const api_key_policy::ApiKeyPolicyDecision result = co_await std::move(authentication);
      if (const auto* notOk = std::get_if<api_key_policy::ApiKeyPolicyDecisionNotOk>(&result)) {
        logClientAuth({.ok = false, .method = "kv_api_key", .status = static_cast<double>(notOk->response.status()), .reason = "invalid_or_limited"});
        co_return clientRefusal(notOk->response);
      }
      const api_key_policy::ApiKeyPolicy& policy = std::get<api_key_policy::ApiKeyPolicyDecisionOk>(result).policy;
      logClientAuth({.ok = true, .method = "kv_api_key"});
      co_return AuthenticateClientResultOk{
        .token = token,
        .method = ClientAuthMethodKvApiKey{.key_id = policy.key_id, .policy = policy},
      };
    }

    // `{ ok: true; kind: "admin_allowlist" | "deno_deploy_token" } | { ok: false; response: Response | null }`
    struct CheckAdminTokenResult {
      bool ok;
      std::string kind{};
      std::optional<rt::Response> response{};
    };

    rt::Task<CheckAdminTokenResult> checkAdminToken(std::string token);

    rt::Task<std::optional<AuthenticateClientResult>> authenticateClientAdminToken(std::string token, ClientAuthLogger logClientAuth) {
      const CheckAdminTokenResult adminResult = co_await checkAdminToken(token);
      if (adminResult.ok) {
        logClientAuth({.ok = true, .method = adminResult.kind});
        ClientAuthMethod method = ClientAuthMethodAdminAllowlist{};
        if (adminResult.kind == ClientAuthMethodDenoDeployToken::kind) method = ClientAuthMethodDenoDeployToken{};
        co_return AuthenticateClientResultOk{.token = token, .method = std::move(method)};
      }
      if (!adminResult.response) co_return std::nullopt;
      logClientAuth({
        .ok = false,
        .method = "deno_deploy_token",
        .status = static_cast<double>(adminResult.response->status()),
        .reason = "admin_token_verification_failed",
      });
      co_return clientRefusal(*adminResult.response);
    }

    void logUnknownClientAuth(const ClientAuthLogger& logClientAuth, bool githubCandidate, const std::optional<kernel::attestation::GitHubRepo>& githubHeaders) {
      const std::string method = githubCandidate && !githubHeaders ? "github_token" : "unknown";
      logClientAuth({
        .ok = false,
        .method = method,
        .status = 401,
        .reason = method == "github_token" ? "missing_repo_headers" : "invalid_api_key",
      });
    }

    rt::Task<AuthenticateClientResult> authenticateLocalClient(LocalClientAuthMode mode, std::shared_ptr<rt::Kv> kv, std::optional<std::string> token,
                                                               ClientAuthLogger logClientAuth) {
      // A loopback development server provisions one unlimited local key so the
      // fully checked listener bypass is a super admin with a real paid-provider
      // policy. The key is optional: without it (or once it is revoked) the
      // principal stays policy-free, exactly as before.
      //
      // The hostname-only legacy fallback never resolves that key: it applies only
      // after the peer/origin checks failed, so a mismatching-Origin request stays
      // policy-free instead of receiving the paid super-admin principal.
      std::optional<api_key_policy::ApiKeyPolicy> localDevelopmentPolicy;
      if (mode == LocalClientAuthMode::admin) localDevelopmentPolicy = co_await local_development_key::resolveLocalDevelopmentApiKeyPolicy(kv);
      if (localDevelopmentPolicy) {
        logClientAuth({.ok = true, .method = "local_development_key"});
        co_return AuthenticateClientResultOk{
          .token = token,
          .method = ClientAuthMethodKvApiKey{.key_id = localDevelopmentPolicy->key_id, .policy = *localDevelopmentPolicy},
        };
      }
      logClientAuth({.ok = true, .method = "disabled"});
      co_return AuthenticateClientResultOk{.token = token, .method = ClientAuthMethodDisabled{}};
    }

    AuthenticateClientResult fromPassportResult(const passport::PassportResult& passport) {
      if (const auto* ok = std::get_if<passport::PassportResultOk>(&passport)) {
        return AuthenticateClientResultOk{.token = std::nullopt, .method = ok->method};
      }
      return clientRefusal(std::get<passport::PassportResultNotOk>(passport).response);
    }
  }

  std::string_view clientAuthMethodKind(const ClientAuthMethod& method) {
    return std::visit([](const auto& alternative) -> std::string_view { return std::decay_t<decltype(alternative)>::kind; }, method);
  }

  std::string_view adminAuthMethodKind(const AdminAuthMethod& method) {
    return std::visit([](const auto& alternative) -> std::string_view { return std::decay_t<decltype(alternative)>::kind; }, method);
  }

  rt::Task<AuthenticateClientResult> authenticateClient(rt::Request req) {
    rt::Promise<std::shared_ptr<rt::Kv>> kvPromise = kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await kvPromise.wait();
    const std::optional<LocalClientAuthMode> localAuthMode = resolveLocalClientAuthMode(req);
    const std::optional<std::string> token = http::getBearerToken(req);
    const bool tokenPresent = rt::truthy(token);
    const std::optional<std::string> tokenShape = rt::truthy(token) ? std::optional(classifyToken(*token)) : std::nullopt;
    const std::optional<kernel::attestation::GitHubRepo> githubHeaders =
      rt::truthy(token) ? kernel::attestation::getGitHubRepoHeaders(req) : std::nullopt;
    const bool githubCandidate = rt::truthy(token) ? looksLikeGitHubToken(*token) : false;
    const ClientAuthLogger logClientAuth = [req, tokenPresent, tokenShape](ClientAuthLogEntry entry) {
      logAuthDecision(req, AuthLogEntry{.scope = "client", .token_present = tokenPresent, .token_shape = tokenShape, .entry = std::move(entry)});
    };
    if (localAuthMode) {
      AuthenticateClientResult local = co_await authenticateLocalClient(*localAuthMode, kv, token, logClientAuth);
      co_return local;
    }

    const std::optional<passport::PassportResult> passport = co_await passport::authenticatePassportSession(req);
    if (passport) co_return fromPassportResult(*passport);

    if (!rt::truthy(token)) {
      AuthenticateClientResult anonymous = co_await authenticateClientWithoutToken(req, logClientAuth);
      co_return anonymous;
    }

    if (config::config.authTokens.contains(*token)) {
      logClientAuth({.ok = true, .method = "auth_tokens_allowlist"});
      co_return AuthenticateClientResultOk{.token = token, .method = ClientAuthMethodAuthTokensAllowlist{}};
    }

    if (rt::startsWith(*token, "uos_ai_session_")) {
      AuthenticateClientResult session = co_await authenticateClientSessionToken(req, *token, logClientAuth);
      co_return session;
    }

    std::optional<AuthenticateClientResult> githubClient = co_await authenticateClientGitHubToken(req, *token, logClientAuth);
    if (githubClient) co_return std::move(*githubClient);

    std::optional<AuthenticateClientResult> kvApiKeyClient = co_await authenticateClientKvApiKey(*token, kv, logClientAuth);
    if (kvApiKeyClient) co_return std::move(*kvApiKeyClient);

    std::optional<AuthenticateClientResult> adminTokenClient = co_await authenticateClientAdminToken(*token, logClientAuth);
    if (adminTokenClient) co_return std::move(*adminTokenClient);

    if (kv) {
      logUnknownClientAuth(logClientAuth, githubCandidate, githubHeaders);
      co_return clientRefusal(http::openaiError(401, "Unauthorized", "invalid_api_key"));
    }

    if (config::config.isDeploy && config::config.authTokens.empty()) {
      logClientAuth({.ok = false, .method = "server", .status = 500, .reason = "misconfigured"});
      co_return clientRefusal(http::openaiError(500, "Server misconfigured: set UOS_AI_TOKEN or enable Deno KV", "server_error"));
    }

    logClientAuth({.ok = false, .method = "unknown", .status = 401, .reason = "invalid_api_key"});
    co_return clientRefusal(http::openaiError(401, "Unauthorized", "invalid_api_key"));
  }

  namespace {
    constexpr std::string_view DENO_API_V1_BASE_URL = "https://api.deno.com/v1";
    constexpr std::string_view DENO_API_V2_BASE_URL = "https://api.deno.com/v2";
    constexpr std::string_view DENO_CONSOLE_BASE_URL = "https://console.deno.com";
    constexpr double DEPLOY_TOKEN_ADMIN_CACHE_TTL_MS = 10 * 60'000;
    // `new Map<string, number>()`: only get/set, iteration order is never observed.
    std::unordered_map<std::string, double>& deployTokenAdminCache() {
      static std::unordered_map<std::string, double> cache;
      return cache;
    }

    bool looksLikeDenoDeployToken(const std::string& token) {
      static const rt::Regex WHITESPACE("\\s", "");
      const std::string trimmed = rt::trim(token);
      if (rt::length(trimmed) < 20) return false;
      if (rt::length(trimmed) > 500) return false;
      if (WHITESPACE.test(trimmed)) return false;
      if (looksLikeUbqAiClientToken(trimmed)) return false;
      if (rt::startsWith(trimmed, "uos_ai_session_")) return false;
      if (!rt::includes(trimmed, "_")) return false;
      return true;
    }

    rt::Task<bool> fetchDenoApiOk(std::string url, std::string token) {
      rt::RequestInit init;
      init.method = "GET";
      init.headers = rt::Headers{{"Authorization", "Bearer " + token}, {"Accept", "application/json"}};
      init.redirect = "manual";
      auto call = rt::fetch(url, std::move(init));
      rt::Response res = co_await std::move(call);

      try {
        co_await cancelBody(res);
      } catch (...) {
        // ignore
      }

      co_return res.ok();
    }

    rt::Task<bool> fetchDenoConsoleAppOk(std::string orgSlug, std::string appSlug, std::string token) {
      static const rt::Regex TITLE("<title[^>]*>([^<]*)<\\/title>", "i");
      const std::string url = std::string(DENO_CONSOLE_BASE_URL) + "/" + rt::encodeURIComponent(orgSlug) + "/" + rt::encodeURIComponent(appSlug);
      rt::RequestInit init;
      init.method = "GET";
      init.headers = rt::Headers{{"Accept", "text/html"}, {"Cookie", "token=" + token + "; deno_auth_ghid=force"}};
      init.redirect = "manual";
      auto call = rt::fetch(url, std::move(init));
      rt::Response res = co_await std::move(call);

      if (!res.ok()) {
        try {
          co_await cancelBody(res);
        } catch (...) {
          // ignore
        }
        co_return false;
      }

      const std::string body = co_await res.text();
      std::string title;
      if (std::optional<rt::RegexMatch> match = TITLE.exec(body)) title = (*match)[1].value_or("");
      co_return rt::includes(title, "| " + appSlug + " | Deploy");
    }

    rt::Task<bool> verifyDenoDeployTokenForThisDeployment(std::string token) {
      const std::string appSlug = rt::trim(kernel::attestation::getEnv("DENO_DEPLOY_APP_SLUG").value_or(""));
      if (!appSlug.empty()) {
        const std::string appUrl = std::string(DENO_API_V2_BASE_URL) + "/apps/" + rt::encodeURIComponent(appSlug);
        const bool appOk = co_await fetchDenoApiOk(appUrl, token);
        if (appOk) co_return true;
        const std::string orgSlug = rt::trim(kernel::attestation::getEnv("DENO_DEPLOY_ORG_SLUG").value_or(""));
        if (!orgSlug.empty()) {
          const bool consoleOk = co_await fetchDenoConsoleAppOk(orgSlug, appSlug, token);
          if (consoleOk) co_return true;
        }
      }

      const std::string deploymentId = rt::trim(kernel::attestation::getEnv("DENO_DEPLOYMENT_ID").value_or(""));
      if (deploymentId.empty()) co_return false;

      const std::string url = std::string(DENO_API_V1_BASE_URL) + "/deployments/" + deploymentId;
      const bool ok = co_await fetchDenoApiOk(url, token);
      co_return ok;
    }

    // `{ ok: true } | { ok: false; response: Response | null }`
    struct DeployTokenVerification {
      bool ok;
      std::optional<rt::Response> response{};
    };

    rt::Task<DeployTokenVerification> verifyDenoDeployTokenCached(std::string token) {
      std::optional<std::string> keyHash;
      try {
        keyHash = co_await utils::sha256Base64Url(token);
        const auto cached = deployTokenAdminCache().find(*keyHash);
        const double cachedUntil = cached == deployTokenAdminCache().end() ? 0 : cached->second;
        if (cachedUntil > rt::dateNow()) co_return DeployTokenVerification{.ok = true};
      } catch (...) {
        // ignore and try network verification
      }

      bool failed = false;
      rt::Reason failure;
      bool ok = false;
      try {
        ok = co_await verifyDenoDeployTokenForThisDeployment(token);
      } catch (...) {
        failed = true;
        failure = std::current_exception();
      }
      if (failed) {
        rt::console::error("[ai.ubq.fi] Failed to verify Deno Deploy token:", rt::console::Value(failure));
        co_return DeployTokenVerification{.ok = false, .response = http::openaiError(502, "Failed to verify admin token", "bad_gateway")};
      }
      if (!ok) co_return DeployTokenVerification{.ok = false, .response = std::nullopt};
      if (keyHash && !keyHash->empty()) {
        auto& cache = deployTokenAdminCache();
        cache[*keyHash] = rt::dateNow() + DEPLOY_TOKEN_ADMIN_CACHE_TTL_MS;
        rt::memory::setCache(rt::memory::Cache::deploy_admin_auth, cache.size(),
                             cache.size() * (sizeof(std::pair<const std::string, double>) + keyHash->size() + 1));
      }
      co_return DeployTokenVerification{.ok = true};
    }

    rt::Task<CheckAdminTokenResult> checkAdminToken(std::string token) {
      if (config.adminTokens.contains(token)) {
        co_return CheckAdminTokenResult{.ok = true, .kind = std::string(ClientAuthMethodAdminAllowlist::kind)};
      }

      // GitHub credentials must be handled by the attested GitHub path. Never
      // treat an unattested GitHub credential as a candidate Deno Deploy token.
      if (looksLikeGitHubToken(token)) co_return CheckAdminTokenResult{.ok = false, .response = std::nullopt};

      if (!looksLikeDenoDeployToken(token)) co_return CheckAdminTokenResult{.ok = false, .response = std::nullopt};

      const DeployTokenVerification verified = co_await verifyDenoDeployTokenCached(token);
      if (verified.ok) co_return CheckAdminTokenResult{.ok = true, .kind = std::string(ClientAuthMethodDenoDeployToken::kind)};
      co_return CheckAdminTokenResult{.ok = false, .response = verified.response};
    }

    using AdminAuthLogger = ClientAuthLogger;

    AdminAuthResult adminRefusal(rt::Response response) { return AdminAuthResultNotOk{.response = std::move(response)}; }

    // `AdminAuthResult | null`: std::nullopt is null.
    std::optional<AdminAuthResult> adminGitHubVerificationRejection(const GitHubBearerVerification& githubVerification, const AdminAuthLogger& logAdminAuth) {
      if (verificationKind(githubVerification) == "verified") {
        logAdminAuth({.ok = false, .method = "github_token", .status = 401, .reason = "github_token_not_admin"});
        return adminRefusal(http::openaiError(401, "Unauthorized", "invalid_api_key"));
      }
      if (verificationKind(githubVerification) == "unavailable") {
        const rt::Response& response = std::get<GitHubBearerVerificationRefused>(*githubVerification).response;
        logAdminAuth({
          .ok = false,
          .method = "github_token",
          .status = static_cast<double>(response.status()),
          .reason = "github_token_verification_failed",
        });
        return adminRefusal(response);
      }
      return std::nullopt;
    }

    AdminAuthResult adminPasskeyResult(const passkeys::PasskeySession& passkeySession, const AdminAuthLogger& logAdminAuth) {
      if (!passkeys::isPasskeyUserAdmin(passkeySession.user)) {
        logAdminAuth({.ok = false, .method = "passkey_session", .status = 403, .reason = "passkey_user_not_admin"});
        return adminRefusal(http::openaiError(403, "Forbidden", "forbidden"));
      }
      logAdminAuth({.ok = true, .method = "passkey_session"});
      return AdminAuthResultOk{
        .token = passkeySession.token,
        .method =
          AdminAuthMethodPasskeySession{
            .user_id = passkeySession.user.id,
            .handle = passkeySession.user.handle,
            .is_admin = true,
            .credential_count = static_cast<double>(passkeySession.user.credential_ids.size()),
          },
        .is_super_admin = false,
      };
    }

    rt::Task<AdminAuthResult> authenticateAdminTokenResult(std::string token, AdminAuthLogger logAdminAuth) {
      if (looksLikeGitHubToken(token) && !config.adminTokens.contains(token)) {
        logAdminAuth({.ok = false, .method = "github_token", .status = 401, .reason = "missing_github_attestation"});
        co_return adminRefusal(http::openaiError(401, "Unauthorized", "invalid_api_key"));
      }

      if (config.adminTokens.empty() && !looksLikeDenoDeployToken(token)) {
        logAdminAuth({.ok = false, .method = "admin_allowlist", .status = 401, .reason = "admin_tokens_unconfigured"});
        co_return adminRefusal(http::openaiError(401, "Unauthorized", "invalid_api_key"));
      }

      const CheckAdminTokenResult result = co_await checkAdminToken(token);
      if (result.ok) {
        logAdminAuth({.ok = true, .method = result.kind});
        AdminAuthMethod method = AdminAuthMethodAdminAllowlist{};
        if (result.kind == AdminAuthMethodDenoDeployToken::kind) method = AdminAuthMethodDenoDeployToken{};
        co_return AdminAuthResultOk{.token = token, .method = std::move(method), .is_super_admin = true};
      }
      if (result.response) {
        logAdminAuth({
          .ok = false,
          .method = "deno_deploy_token",
          .status = static_cast<double>(result.response->status()),
          .reason = "admin_token_verification_failed",
        });
        co_return adminRefusal(*result.response);
      }
      logAdminAuth({.ok = false, .method = "admin_allowlist", .status = 401, .reason = "invalid_admin_token"});
      co_return adminRefusal(http::openaiError(401, "Unauthorized", "invalid_api_key"));
    }

    AdminAuthResult fromPassportAdminResult(const passport::PassportAdminResult& passport) {
      if (const auto* ok = std::get_if<passport::PassportAdminResultOk>(&passport)) {
        return AdminAuthResultOk{.token = ok->token, .method = ok->method, .is_super_admin = passport::PassportAdminResultOk::is_super_admin};
      }
      return adminRefusal(std::get<passport::PassportAdminResultNotOk>(passport).response);
    }
  }

  rt::Task<AdminAuthResult> authenticateAdmin(rt::Request req) {
    const std::optional<std::string> token = http::getBearerToken(req);
    const bool tokenPresent = rt::truthy(token);
    const std::optional<std::string> tokenShape = rt::truthy(token) ? std::optional(classifyToken(*token)) : std::nullopt;
    const AdminAuthLogger logAdminAuth = [req, tokenPresent, tokenShape](ClientAuthLogEntry entry) {
      logAuthDecision(req, AuthLogEntry{.scope = "admin", .token_present = tokenPresent, .token_shape = tokenShape, .entry = std::move(entry)});
    };
    if (local_admin::isAdminAuthDisabledForRequest(req)) {
      logAdminAuth({.ok = true, .method = "disabled"});
      co_return AdminAuthResultOk{
        .token = token.value_or("local-dev-admin"),
        .method = AdminAuthMethodDisabled{},
        .is_super_admin = true,
      };
    }
    const std::optional<passport::PassportAdminResult> passport = co_await passport::authenticatePassportAdmin(req);
    if (passport) co_return fromPassportAdminResult(*passport);
    std::optional<passkeys::PasskeySession> passkeySession = co_await passkeys::getPasskeySessionForRequest(req);
    if (!passkeySession && rt::truthy(token) && looksLikeGitHubToken(*token) && !config::config.adminTokens.contains(*token) &&
        !config::config.authTokens.contains(*token) && req.headers().has("cookie")) {
      const GitHubBearerVerification githubVerification = co_await verifyGitHubBearer(req, *token);
      std::optional<AdminAuthResult> rejection = adminGitHubVerificationRejection(githubVerification, logAdminAuth);
      if (rejection) co_return std::move(*rejection);
      if (verificationKind(githubVerification) == "rejected") {
        passkeySession = co_await getPasskeyCookieSessionForRequest(req);
      }
    }
    if (passkeySession) co_return adminPasskeyResult(*passkeySession, logAdminAuth);

    if (!rt::truthy(token)) {
      logAdminAuth({.ok = false, .method = "missing", .status = 401, .reason = "missing_token"});
      co_return adminRefusal(http::openaiError(401, "Unauthorized", "invalid_api_key"));
    }

    AdminAuthResult result = co_await authenticateAdminTokenResult(*token, logAdminAuth);
    co_return result;
  }

  rt::Task<std::optional<rt::Response>> requireAdminAuth(rt::Request req) {
    const AdminAuthResult result = co_await authenticateAdmin(req);
    if (std::holds_alternative<AdminAuthResultOk>(result)) co_return std::nullopt;
    co_return std::get<AdminAuthResultNotOk>(result).response;
  }

  namespace {
    // `AdminAuthMethod | ClientAuthMethod`
    using ReportedAuthMethod = ClientAuthMethod;

    ReportedAuthMethod reportedAuthMethod(const AdminAuthMethod& method) {
      return std::visit([](const auto& alternative) -> ReportedAuthMethod { return alternative; }, method);
    }

    // `Readonly<{ ok: true; key: Record<string, unknown> }> | Readonly<{ ok: false; response: Response }>`
    struct V1ApiKeyMethodResult {
      bool ok;
      json::object key{};
      std::optional<rt::Response> response{};
    };

    std::string resolveV1AuthMode(bool localClientAuthDisabled, const std::shared_ptr<rt::Kv>& kv) {
      if (localClientAuthDisabled) return "disabled";
      if (config.isDeploy && config.authTokens.empty() && !kv) return "misconfigured";
      if (config.isDeploy || !config.authTokens.empty() || kv) return "required";
      return "disabled";
    }

    rt::Task<json::object> v1AuthTokenInfo(std::optional<std::string> token) {
      json::object info;
      if (!rt::truthy(token)) {
        info["present"] = false;
        info["length"] = nullptr;
        info["shape"] = nullptr;
        info["sha256_12"] = nullptr;
        co_return info;
      }
      info["present"] = true;
      info["length"] = rt::makeNumber(static_cast<double>(rt::length(*token)));
      info["shape"] = classifyToken(*token);
      const std::string digest = co_await utils::sha256Hex(*token);
      info["sha256_12"] = rt::slice(digest, 0, 12);
      co_return info;
    }

    struct V1AdminFlags {
      bool isAdmin;
      bool isSuperAdmin;
    };

    V1AdminFlags v1AdminFlags(const std::optional<AdminAuthResult>& localAdminAuth, const ReportedAuthMethod& reportingMethod) {
      const auto* localAdminOk = localAdminAuth ? std::get_if<AdminAuthResultOk>(&*localAdminAuth) : nullptr;
      const auto* passportMethod = std::get_if<PassportAuthMethod>(&reportingMethod);
      const auto* passkeyMethod = std::get_if<ClientAuthMethodPasskeySession>(&reportingMethod);
      const bool isAdmin = localAdminOk != nullptr || std::holds_alternative<ClientAuthMethodAdminAllowlist>(reportingMethod) ||
                           std::holds_alternative<ClientAuthMethodDenoDeployToken>(reportingMethod) ||
                           (passportMethod && passportMethod->user.is_admin && !passportMethod->user.agent_token_id) ||
                           (passkeyMethod && passkeyMethod->is_admin);
      const bool isSuperAdmin = localAdminOk ? localAdminOk->is_super_admin
                                             : std::holds_alternative<ClientAuthMethodAdminAllowlist>(reportingMethod) ||
                                                 std::holds_alternative<ClientAuthMethodDenoDeployToken>(reportingMethod);
      return {.isAdmin = isAdmin, .isSuperAdmin = isSuperAdmin};
    }

    rt::Task<V1ApiKeyMethodResult> v1ApiKeyMethodResult(ClientAuthMethodKvApiKey reportingMethod, std::shared_ptr<rt::Kv> kv) {
      const std::string id = reportingMethod.key_id;
      json::object key;
      key["id"] = id;
      if (kv) {
        const rt::KvKey idKey = api_keys::apiKeyIdKey(id);
        const rt::KvEntryMaybe<> entry = co_await kv->get(idKey);
        if (entry.value.truthy()) {
          // `entry.value.<member>` on the stored value as JavaScript reads it; an undefined member is
          // dropped by JSON.stringify.
          const json::value stored = rt::storedJsonView(entry.value);
          double usageRequests = 0;
          bool failed = false;
          rt::Reason failure;
          try {
            auto usage = api_key_policy::getApiKeyUsageV3(reportingMethod.policy, std::optional<std::shared_ptr<rt::Kv>>(kv));
            usageRequests = co_await std::move(usage);
          } catch (...) {
            failed = true;
            failure = std::current_exception();
          }
          if (failed) {
            rt::console::warn("[ai.ubq.fi] Failed to read API key quota usage for /uos/auth:", rt::console::Value(failure));
            co_return V1ApiKeyMethodResult{
              .ok = false,
              .response = http::openaiError(503, "API key quota ledger is unavailable", "server_error", http::OpenaiErrorOptions{.type = "server_error"}),
            };
          }
          json::object next;
          const auto copy = [&](std::string_view name) {
            if (const json::value* member = rt::get(stored, name)) next[name] = *member;
          };
          copy("id");
          copy("name");
          copy("prefix");
          copy("created_at_ms");
          next["expires_at_ms"] = rt::makeNumber(api_keys::coerceApiKeyExpiresAtMs(stored));
          copy("revoked_at_ms");
          next["usage_limit_requests"] = rt::makeNumber(reportingMethod.policy.usage_limit_requests);
          next["usage_requests"] = rt::makeNumber(usageRequests);
          next["usage_reset_at_ms"] = rt::makeNumber(reportingMethod.policy.usage_reset_at_ms);
          next["window_ms"] = rt::makeNumber(reportingMethod.policy.window_ms);
          key = std::move(next);
        }
      }
      co_return V1ApiKeyMethodResult{.ok = true, .key = std::move(key)};
    }
  }

  rt::Task<rt::Response> handleV1Auth(rt::Request req) {
    const AuthenticateClientResult authResult = co_await authenticateClient(req);
    if (const auto* refusal = std::get_if<AuthenticateClientResultNotOk>(&authResult)) co_return refusal->response;
    const AuthenticateClientResultOk& authenticated = std::get<AuthenticateClientResultOk>(authResult);

    rt::Promise<std::shared_ptr<rt::Kv>> kvPromise = kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await kvPromise.wait();
    const bool localClientAuthDisabled = resolveLocalClientAuthMode(req).has_value();
    const std::string mode = resolveV1AuthMode(localClientAuthDisabled, kv);

    const std::optional<std::string> token = authenticated.token;
    const json::object tokenInfo = co_await v1AuthTokenInfo(token);

    // Local client auth remains disabled for the development inference surface,
    // but admin access is resolved independently. This keeps localhost from
    // implicitly becoming an admin while still recognizing an explicit admin
    // credential when the CLI bypass is off.
    std::optional<AdminAuthResult> localAdminAuth;
    if (localClientAuthDisabled) localAdminAuth = co_await authenticateAdmin(req);
    const auto* localAdminOk = localAdminAuth ? std::get_if<AdminAuthResultOk>(&*localAdminAuth) : nullptr;
    const ReportedAuthMethod reportingMethod = localAdminOk ? reportedAuthMethod(localAdminOk->method) : authenticated.method;
    json::object method;
    method["kind"] = clientAuthMethodKind(reportingMethod);
    const V1AdminFlags flags = v1AdminFlags(localAdminAuth, reportingMethod);

    if (const auto* passport = std::get_if<PassportAuthMethod>(&reportingMethod)) {
      method["principal"] = passport->principal;
      method["user"] = json::value_from(passport->user);
    }

    if (const auto* github = std::get_if<ClientAuthMethodGithubToken>(&reportingMethod)) {
      json::object repo;
      repo["owner"] = github->owner;
      repo["repo"] = github->repo;
      method["repo"] = std::move(repo);
      method["state_id"] = github->state_id;
      method["limit_scope"] = github->limit_scope;
    }

    if (const auto* apiKey = std::get_if<ClientAuthMethodKvApiKey>(&reportingMethod)) {
      auto keyTask = v1ApiKeyMethodResult(*apiKey, kv);
      const V1ApiKeyMethodResult keyResult = co_await std::move(keyTask);
      if (!keyResult.ok) co_return *keyResult.response;
      method["key"] = keyResult.key;
    }

    if (const auto* passkey = std::get_if<ClientAuthMethodPasskeySession>(&reportingMethod)) {
      json::object user;
      user["id"] = passkey->user_id;
      user["handle"] = passkey->handle;
      user["is_admin"] = passkey->is_admin;
      user["credential_count"] = rt::makeNumber(passkey->credential_count);
      method["user"] = std::move(user);
    }

    const auto* passportMethod = std::get_if<PassportAuthMethod>(&reportingMethod);
    json::object auth;
    auth["mode"] = mode;
    auth["is_admin"] = flags.isAdmin;
    auth["is_super_admin"] = flags.isSuperAdmin;
    if (passportMethod) auth["principal"] = passportMethod->principal;
    auth["method"] = std::move(method);
    auth["token"] = tokenInfo;
    json::object body;
    body["ok"] = true;
    body["service"] = "ai.ubq.fi";
    body["auth"] = std::move(auth);
    rt::Response response = http::json(200, json::value(std::move(body)), rt::Headers{{"Cache-Control", "no-store"}});
    if (passportMethod) co_return passport::renewPassportSessionCookie(req, response);
    co_return response;
  }
}
