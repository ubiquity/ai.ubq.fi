#include "uos/handler/index.hpp"
#include "uos/handler/terminal_route.hpp"
#include "uos/admin/index.hpp"
#include "uos/admin/api_key_audit.hpp"
#include "uos/admin/codex_reset.hpp"
#include "uos/admin/error_log.hpp"
#include "uos/admin/gemini_quota.hpp"
#include "uos/upstream_auth.hpp"
#include "uos/admin/chart_source_proxy.hpp"
#include "uos/admin/capacity_observation_sync.hpp"
#include "uos/admin/sentinel_incident.hpp"
#include "uos/agent_messages.hpp"
#include "uos/auth/passkeys.hpp"
#include "uos/auth/passport.hpp"
#include "uos/codex/supervisor.hpp"
#include "uos/codex/supervisor_brief.hpp"
#include "uos/health.hpp"
#include "uos/http.hpp"
#include "uos/live/handler.hpp"
#include "uos/models/catalog.hpp"
#include "uos/provider/capacity.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/url.hpp"
#include "uos/sentinel/replay_admin.hpp"
#include "uos/static.hpp"

#include <algorithm>
#include <utility>

namespace uos::handler::index {
  namespace {
    struct AdminRouteContext {
      auth::index::AdminAuthResultOk auth;
      std::string requestId;
    };
    struct AdminRouteEntry {
      std::vector<std::string> methods;
      std::string path;
      std::function<rt::Task<rt::Response>(rt::Request req, AdminRouteContext context)> run;
    };
    template<class TRoute>
    const TRoute* matchExactRoute(const std::vector<TRoute>& routes, const rt::Request& req, const std::string& path) {
      const auto found = std::find_if(routes.begin(), routes.end(), [&](const TRoute& entry) {
        return std::find(entry.methods.begin(), entry.methods.end(), req.method()) != entry.methods.end() && entry.path == path;
      });
      return found == routes.end() ? nullptr : &*found;
    }
    authority::native_api_key_audit::ApiKeyAuditContext apiKeyAuditContext(const AdminRouteContext& context) {
      return {.auth = auth::index::AdminAuthResult(context.auth), .request_id = context.requestId};
    }
    rt::Task<std::optional<http::ClientAuthResult>> optionalClientAuth(rt::Request req) {
      if (!req.headers().has("authorization") || !req.headers().has("cookie")) co_return std::nullopt;
      rt::Promise<http::ClientAuthResult> pending(auth::index::authenticateClient(req));
      auto result = co_await pending.wait();
      co_return result;
    }
    std::optional<std::string> passkeyTokenFrom(const std::optional<http::ClientAuthResult>& auth) {
      if (!auth) return std::nullopt;
      const auto* accepted = std::get_if<auth::index::AuthenticateClientResultOk>(&*auth);
      if (!accepted) return std::nullopt;
      return std::holds_alternative<auth::index::ClientAuthMethodPasskeySession>(accepted->method) ? accepted->token : std::nullopt;
    }
    const std::vector<ExactRouteEntry>& authRoutes() {
      static const std::vector<ExactRouteEntry> AUTH_ROUTES = {
        {{"POST"}, "/api/auth/register/finish", [](rt::Request req) { return ::uos::auth::passkeys::handlePasskeyRegisterFinish(req); }},
        {{"POST"}, "/api/auth/login/start", [](rt::Request req) { return ::uos::auth::passkeys::handlePasskeyLoginStart(req); }},
        {{"POST"}, "/api/auth/login/finish", [](rt::Request req) { return ::uos::auth::passkeys::handlePasskeyLoginFinish(req); }}
      };
      return AUTH_ROUTES;
    }
    const std::vector<AdminRouteEntry>& adminRoutes() {
      static const std::vector<AdminRouteEntry> ADMIN_ROUTES = {
        {{"GET"}, "/admin/passkey-users", [](rt::Request, AdminRouteContext) { return ::uos::auth::passkeys::handlePasskeyUsersList(); }},
        {{"PATCH"}, "/admin/passkey-users", [](rt::Request req, AdminRouteContext) { return ::uos::auth::passkeys::handlePasskeyUsersUpdate(req); }},
        {{"POST"}, "/admin/codex/auth", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminCodexAuth(req); }},
        {{"GET", "PATCH"}, "/admin/providers/codex/banked-resets", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminCodexResetSettings(req); }},
        {{"GET", "PATCH"}, "/admin/providers/codex/overage-usage", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminCodexOverageUsage(req); }},
        {{"GET"}, "/admin/providers/codex/banked-resets/shadow-decisions", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminCodexBankedResetShadowDecisions(); }},
        {{"GET"}, "/admin/providers/codex/cache-scope-experiment", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminCodexCacheScopeExperimentTelemetryBaseline(); }},
        {{"POST"}, "/admin/providers/codex/cache-scope-experiment", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminCodexCacheScopeExperiment(req); }},
        {{"GET"}, "/admin/codex/models", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminCodexModelsGet(); }},
        {{"POST"}, "/admin/codex/models", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminCodexModelsSet(req); }},
        {{"GET"}, "/admin/models/whitelist", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminCodexModelsWhitelistGet(); }},
        {{"GET"}, "/admin/models/catalog", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminModelsCatalogGet(); }},
        {{"POST"}, "/admin/models/refresh", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminModelsRefresh(); }},
        {{"POST"}, "/admin/models/whitelist", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminCodexModelsWhitelistSet(req); }},
        {{"POST"}, "/admin/codex/prompts/purge", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminCodexPromptsPurge(); }},
        {{"POST"}, "/admin/kv-migration/import", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminKvMigrationImport(req); }},
        {{"GET"}, "/admin/kv-migration/validate", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminKvMigrationValidate(); }},
        {{"GET"}, "/admin/sentinel/replay-captures", [](rt::Request req, AdminRouteContext) { return ::uos::sentinel::replay_admin::handleAdminSentinelReplayCaptures(req); }},
        {{"GET"}, "/admin/sentinel/incidents", [](rt::Request req, AdminRouteContext) { return ::uos::admin::sentinel_incident::handleAdminSentinelIncidents(req); }},
        {{"GET"}, "/admin/codex/supervisor/sessions", [](rt::Request, AdminRouteContext) { return ::uos::codex::supervisor::handleAdminCodexSupervisorSessions(); }},
        {{"GET"}, "/admin/codex/supervisor/output", [](rt::Request req, AdminRouteContext) { return ::uos::codex::supervisor::handleAdminCodexSupervisorOutput(req); }},
        {{"POST"}, "/admin/codex/supervisor/brief", [](rt::Request req, AdminRouteContext) { return ::uos::codex::supervisor_brief::handleAdminCodexSupervisorBrief(req); }},
        {{"GET"}, "/admin/errors", [](rt::Request req, AdminRouteContext) { return ::uos::admin::chart_source_proxy::proxyAdminChartSource(req, rt::Promise<rt::Response>(::uos::admin::error_log::handleAdminErrors(req))); }},
        {{"GET", "POST"}, "/admin/defaults", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminDefaults(req); }},
        {{"GET", "POST", "DELETE"}, "/admin/debug/routing", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminDebugRouting(req); }},
        {{"GET"}, "/admin/providers", [](rt::Request, AdminRouteContext) { health::PassiveProviderHealthOptions options; options.includeQuota = true; return health::handleHealthProviders(options); }},
        {{"POST"}, "/admin/providers/codex/banked-resets/consume", [](rt::Request req, AdminRouteContext) { return ::uos::admin::codex_reset::handleAdminCodexResetConsume(req); }},
        {{"GET"}, "/admin/providers/gemini/quota", [](rt::Request, AdminRouteContext) { return ::uos::admin::gemini_quota::handleAdminGeminiQuota(); }},
        {{"POST"}, "/admin/providers/gemini/quota/refresh", [](rt::Request req, AdminRouteContext) { return ::uos::admin::gemini_quota::handleAdminGeminiQuotaRefresh(req); }},
        {{"GET"}, "/admin/providers/selection", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminProviderSelectionGet(); }},
        {{"POST"}, "/admin/providers/selection", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminProviderSelectionSet(req); }},
        {{"GET"}, "/admin/providers/capacity", [](rt::Request req, AdminRouteContext) { return ::uos::admin::chart_source_proxy::proxyAdminChartSource(req, rt::Promise<rt::Response>(::uos::provider::capacity::handleProviderCapacity(req))); }},
        {{"POST"}, "/admin/providers/capacity/observations", [](rt::Request req, AdminRouteContext) { return ::uos::admin::capacity_observation_sync::handleCapacityObservationIngest(req); }},
        {{"GET"}, "/admin/providers/capacity/rollups", [](rt::Request req, AdminRouteContext) { return ::uos::provider::capacity::handleProviderCapacityRollups(req); }},
        {{"GET", "POST"}, "/admin/providers/claude-code/access", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminClaudeCodeAccess(req); }},
        {{"POST"}, "/admin/providers/claude-code/accounts/active", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminClaudeCodeAccountActive(req); }},
        {{"GET"}, "/admin/providers/claude-code/auth", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminClaudeCodeAuthGet(); }},
        {{"POST"}, "/admin/providers/claude-code/auth", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminClaudeCodeAuthSet(req); }},
        {{"DELETE"}, "/admin/providers/claude-code/auth", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminClaudeCodeAuthDelete(req); }},
        {{"POST"}, "/admin/providers/claude-code/quota/clear", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminClaudeCodeQuotaClear(req); }},
        {{"POST"}, "/admin/providers/claude-code/quota/refresh", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminClaudeCodeQuotaRefresh(req); }},
        {{"GET"}, "/admin/providers/quota-projection", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminProvidersQuotaProjection(req); }},
        {{"POST"}, "/admin/providers/quota-projection/backfill", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminProvidersQuotaProjectionBackfill(req); }},
        {{"GET"}, "/admin/prompt-cache-analytics", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminPromptCacheAnalytics(req); }},
        {{"POST"}, "/admin/api-keys", [](rt::Request req, AdminRouteContext context) { return ::uos::admin::index::handleAdminApiKeysCreate(req, apiKeyAuditContext(context)); }},
        {{"GET"}, "/admin/api-keys", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminApiKeysList(req); }},
        {{"GET"}, "/admin/api-keys/changes", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminApiKeyChanges(req); }},
        {{"PATCH"}, "/admin/api-keys", [](rt::Request req, AdminRouteContext context) { return ::uos::admin::index::handleAdminApiKeysUpdate(req, apiKeyAuditContext(context)); }},
        {{"POST"}, "/admin/api-keys/revoke", [](rt::Request req, AdminRouteContext context) { return ::uos::admin::index::handleAdminApiKeysRevoke(req, apiKeyAuditContext(context)); }},
        {{"POST"}, "/admin/api-keys/unrevoke", [](rt::Request req, AdminRouteContext context) { return ::uos::admin::index::handleAdminApiKeysUnrevoke(req, apiKeyAuditContext(context)); }},
        {{"DELETE"}, "/admin/api-keys", [](rt::Request req, AdminRouteContext context) { return ::uos::admin::index::handleAdminApiKeysDelete(req, apiKeyAuditContext(context)); }},
        {{"GET"}, "/admin/kernel-usage", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminKernelUsageGet(req); }},
        {{"GET"}, "/admin/kernel-policy-queue", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminKernelPolicyQueueList(); }},
        {{"POST"}, "/admin/kernel-usage", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminKernelUsageSet(req); }},
        {{"DELETE"}, "/admin/kernel-usage", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminKernelUsageDelete(req); }},
        {{"GET"}, "/admin/kernel-pubkeys", [](rt::Request, AdminRouteContext) { return ::uos::admin::index::handleAdminKernelPubKeysList(); }},
        {{"POST"}, "/admin/kernel-pubkeys", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminKernelPubKeysCreate(req); }},
        {{"DELETE"}, "/admin/kernel-pubkeys", [](rt::Request req, AdminRouteContext) { return ::uos::admin::index::handleAdminKernelPubKeysDelete(req); }}
      };
      return ADMIN_ROUTES;
    }
    rt::Task<std::optional<rt::Response>> handleStaticRoute(rt::Request req, std::string path) {
      if ((req.method() == "GET" || req.method() == "HEAD") && (path == "/" || path == "/index.html")) {
        rt::Promise<rt::Response> pending(static_::handleRoot(req));
        auto rootResponse = co_await pending.wait();
        co_return req.method() == "HEAD" ? ::uos::http::withoutBody(rootResponse) : rootResponse;
      }
      if (req.method() == "GET" || req.method() == "HEAD") {
        rt::Promise<std::optional<rt::Response>> pending(static_::handleStaticAsset(path));
        auto staticResponse = co_await pending.wait();
        if (staticResponse) co_return req.method() == "HEAD" ? ::uos::http::withoutBody(*staticResponse) : *staticResponse;
      }
      co_return std::nullopt;
    }
    rt::Task<std::optional<rt::Response>> handleHealthRoute(rt::Request req, std::string path) {
      if ((req.method() == "GET" || req.method() == "HEAD") && path == "/health") {
        if (upstream_auth::followerMode()) {
          auto reading = health::handleFollowerHealth();
          auto response = co_await std::move(reading);
          co_return req.method() == "HEAD" ? ::uos::http::withoutBody(response) : response;
        }
        auto response = health::handleHealth();
        co_return req.method() == "HEAD" ? ::uos::http::withoutBody(response) : response;
      }
      if (req.method() != "GET") co_return std::nullopt;
      if (path == "/health/providers" || path == "/health/upstream") {
        rt::Promise<std::optional<rt::Response>> authPending(auth::index::requireAdminAuth(req));
        auto authError = co_await authPending.wait();
        if (authError) co_return *authError;
        rt::Promise<rt::Response> pending(path == "/health/providers" ? health::handleHealthProviders() : health::handleHealthUpstream());
        auto response = co_await pending.wait();
        co_return response;
      }
      co_return std::nullopt;
    }
    rt::Task<std::optional<rt::Response>> handleAuthRoute(rt::Request req, std::string path) {
      rt::Promise<std::optional<rt::Response>> passportPending(auth::passport::handlePassportRoute(req, path));
      auto passport = co_await passportPending.wait();
      if (passport) co_return *passport;
      if (req.method() == "POST" && path == "/api/auth/register/start") {
        rt::Promise<auth::index::AdminAuthResult> authPending(auth::index::authenticateAdmin(req));
        auto auth = co_await authPending.wait();
        if (const auto* rejected = std::get_if<auth::index::AdminAuthResultNotOk>(&auth)) co_return rejected->response;
        const auto& accepted = std::get<auth::index::AdminAuthResultOk>(auth);
        auth::passkeys::PasskeyRegisterStartOptions options;
        options.defaultIsAdmin = true;
        if (std::holds_alternative<auth::index::ClientAuthMethodPasskeySession>(accepted.method)) options.authenticatedPasskeyToken = accepted.token;
        rt::Promise<rt::Response> pending(auth::passkeys::handlePasskeyRegisterStart(req, options));
        auto response = co_await pending.wait();
        co_return response;
      }
      if (const auto* route = matchExactRoute(authRoutes(), req, path)) {
        rt::Promise<rt::Response> pending(route->run(req));
        auto response = co_await pending.wait();
        co_return response;
      }
      if ((req.method() == "GET" && path == "/api/auth/session") || (req.method() == "POST" && path == "/api/auth/logout")) {
        rt::Promise<std::optional<http::ClientAuthResult>> authPending(optionalClientAuth(req));
        auto auth = co_await authPending.wait();
        auth::passkeys::PasskeySessionOptions options;
        options.authenticatedPasskeyToken = passkeyTokenFrom(auth);
        rt::Promise<rt::Response> pending(path == "/api/auth/session" ? auth::passkeys::handlePasskeySession(req, options) : auth::passkeys::handlePasskeyLogout(req, options));
        auto response = co_await pending.wait();
        co_return response;
      }
      co_return std::nullopt;
    }
    rt::Task<std::optional<rt::Response>> handleApiKeyPaidFallbacksRoute(rt::Request req, std::string path) {
      static const rt::Regex pattern(R"(^/admin/api-keys/([^/]+)/paid-fallbacks$)");
      const auto match = pattern.exec(path);
      if (!match) co_return std::nullopt;
      if (req.method() != "GET") co_return ::uos::http::openaiError(405, "Method not allowed", "method_not_allowed");
      rt::Promise<std::optional<rt::Response>> authPending(auth::index::requireAdminAuth(req));
      auto authError = co_await authPending.wait();
      if (authError) co_return *authError;
      std::string keyId;
      bool invalid = false;
      try { keyId = rt::decodeURIComponent(*(*match)[1]); }
      catch (...) { invalid = true; }
      if (invalid) co_return ::uos::http::openaiError(400, "Invalid API key id", "invalid_request_error");
      rt::Promise<rt::Response> pending(admin::index::handleAdminApiKeysPaidFallbacks(req, keyId));
      auto response = co_await pending.wait();
      co_return response;
    }
    rt::Task<std::optional<rt::Response>> handleAdminRoute(rt::Request req, std::string path, std::string requestId) {
      if (const auto* route = matchExactRoute(adminRoutes(), req, path)) {
        rt::Promise<auth::index::AdminAuthResult> authPending(auth::index::authenticateAdmin(req));
        auto auth = co_await authPending.wait();
        if (const auto* rejected = std::get_if<auth::index::AdminAuthResultNotOk>(&auth)) co_return rejected->response;
        AdminRouteContext context{std::get<auth::index::AdminAuthResultOk>(auth), requestId};
        rt::Promise<rt::Response> pending(route->run(req, context));
        auto response = co_await pending.wait();
        co_return response;
      }
      static const rt::Regex pattern(R"(^/admin/providers/codex/(\d+)/recheck$)");
      const auto recheckMatch = pattern.exec(path);
      if (req.method() == "POST" && recheckMatch) {
        rt::Promise<std::optional<rt::Response>> authPending(auth::index::requireAdminAuth(req));
        auto authError = co_await authPending.wait();
        if (authError) co_return *authError;
        rt::Promise<rt::Response> pending(admin::index::handleAdminCodexRecheck(rt::toNumber(json::value(*(*recheckMatch)[1]))));
        auto response = co_await pending.wait();
        co_return response;
      }
      rt::Promise<std::optional<rt::Response>> pending(handleApiKeyPaidFallbacksRoute(req, path));
      auto response = co_await pending.wait();
      co_return response;
    }
    rt::Task<std::optional<rt::Response>> handleUosRoute(rt::Request req, std::string path) {
      if (const auto* route = matchExactRoute(UOS_ROUTES, req, path)) {
        rt::Promise<rt::Response> pending(route->run(req));
        auto response = co_await pending.wait();
        co_return response;
      }
      if (path == "/uos/agent-messages") co_return ::uos::http::openaiError(405, "Method not allowed", "method_not_allowed");
      co_return std::nullopt;
    }
    bool isTerminalInferencePath(const std::string& path) {
      return path.starts_with("/v1/") || path == "/uos/embeddings" || path == "/uos/embedding-jobs" || path.starts_with("/uos/embedding-jobs/");
    }
  }

  rt::Task<rt::Response> handler(rt::Request req, std::optional<http::RequestDeliveryInfo> delivery) {
    const auto requestStartedAtMs = rt::dateNow();
    const auto requestStartedAtMonotonicMs = rt::performanceNow();
    const auto requestId = rt::crypto::randomUUID();
    if (req.method() == "OPTIONS") {
      rt::ResponseInit init{.status = 204, .headers = ::uos::http::corsHeaders(req)};
      co_return ::uos::http::withCors(rt::Response(nullptr, init), req);
    }
    const auto url = rt::URL(req.url());
    const auto path = http::normalizePath(url.pathname());
    rt::Promise<std::optional<rt::Response>> staticPending(handleStaticRoute(req, path));
    auto staticResponse = co_await staticPending.wait();
    if (staticResponse) co_return ::uos::http::withCors(*staticResponse, req);
    rt::Promise<std::optional<rt::Response>> healthPending(handleHealthRoute(req, path));
    auto healthResponse = co_await healthPending.wait();
    if (healthResponse) co_return ::uos::http::withCors(*healthResponse, req);
    rt::Promise<std::optional<rt::Response>> authPending(handleAuthRoute(req, path));
    auto authResponse = co_await authPending.wait();
    if (authResponse) co_return ::uos::http::withCors(*authResponse, req);
    rt::Promise<std::optional<rt::Response>> adminPending(handleAdminRoute(req, path, requestId));
    auto adminResponse = co_await adminPending.wait();
    if (adminResponse) co_return ::uos::http::withCors(*adminResponse, req);
    rt::Promise<std::optional<rt::Response>> uosPending(handleUosRoute(req, path));
    auto uosResponse = co_await uosPending.wait();
    if (uosResponse) co_return ::uos::http::withCors(*uosResponse, req);
    rt::Promise<std::optional<rt::Response>> livePending(live::handler::handleLiveRoute(req, requestId));
    auto liveResponse = co_await livePending.wait();
    if (liveResponse) co_return *liveResponse;
    if (!isTerminalInferencePath(path)) {
      const auto response = ::uos::http::notFound();
      co_return ::uos::http::withCors(req.method() == "HEAD" ? ::uos::http::withoutBody(response) : response, req);
    }
    rt::Promise<rt::Response> pending(terminal_route::handleTerminalRoute(req, path, delivery, requestId, requestStartedAtMs, requestStartedAtMonotonicMs));
    auto response = co_await pending.wait();
    co_return response;
  }

  rt::Task<runtime::http_upgrade::UpgradeResult> upgradeHandler(rt::Request req, runtime::http_upgrade::UpgradeInfo info) {
    return live::handler::handleLiveUpgrade(req, info);
  }
}
