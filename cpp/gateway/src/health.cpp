/**
     * SHA-256 of the slot's refresh token. A digest only: it lets a local watchdog compare
     * the pool against credential files without exposing token material.
     */

/**
     * The bound native-owner home, when the slot delegates its refresh. A digest matching a
     * credential file is expected for a slot bound to that home; without a binding it means
     * two independent refreshers share one lineage.
     */

/**
     * The Claude subscription credential probe. It reports independently so a
     * billing or workspace rejection on that tier never masquerades as a Codex
     * upstream outage in the aggregate status.
     */

/**
 * The gateway-owned waterfall identity: "at least one hop is configured" is the
 * whole state, because this route owns no upstream observations of its own.
 */

/**
 * Workspace readback for the Claude subscription tier. The resolved selection
 * (KV pin, environment pin, or discovery) is reported together with the last
 * verification and the last workspace-resolution failure, so an operator sees
 * which workspace the gateway will name and whether it was ever accepted.
 */

/** Quota and configuration-state readback for the Claude subscription tier. */

/**
 * The admin/health readback for the Claude subscription tier: credential
 * provenance, workspace selection, gates, and quota state, never secret
 * material. Shared by the passive provider snapshot and the admin status
 * route so both always agree.
 */

/**
 * The Claude subscription probe lists models with the exact credential and
 * workspace the tier would use for inference. A listing is free, so a green
 * probe proves credential, workspace, and transport without spending quota on
 * a completion; the tier's own billing or workspace rejections stay visible in
 * the passive snapshot instead of failing this probe.
 */
#include "uos/health.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/metered_quota.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/claude_code.hpp"
#include "uos/provider/claude_code_access.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/gemini.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/upstream_auth.hpp"
#include "uos/utils.hpp"
#include <algorithm>
#include <memory>
#include <tuple>
#include <vector>
namespace uos::health {
  namespace {
    namespace ca = codex::auth;
    namespace ph = provider::health;
    namespace cc = provider::claude_code;
    namespace accounts = provider::claude_code_accounts;
    namespace credentials = provider::claude_code_credentials;
    namespace cq = provider::claude_code_quota;
    inline constexpr double AUTH_REFRESH_WINDOW_MS = 2 * 60'000;
    inline constexpr double ACTIVE_UPSTREAM_HEALTH_TIMEOUT_MS = 3'000;
    double activeUpstreamHealthTimeoutMs = ACTIVE_UPSTREAM_HEALTH_TIMEOUT_MS;
    inline constexpr std::string_view AUTH_NOT_CONFIGURED = "No Codex auth configured (CODEX_AUTH_JSON_B64 or KV entry missing).";
    struct CodexAuthContext { json::object meta; std::vector<std::string> account_ids; };
    struct HealthProbeProgress {
      std::optional<json::object> auth;
      std::optional<json::object> codex;
      rt::Nullable<json::object> metered_quota;
      rt::Nullable<json::object> claude_subscription;
      rt::Nullable<json::object> claude_workspace;
    };
    std::optional<rt::Promise<json::object>> upstreamProbeInFlight;
    double nowMs() { return rt::dateNow(); }
    json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
    json::value stringOrNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }
    json::object enrichAuthMeta(const json::object& meta) {
      const auto now = nowMs();
      auto result = meta;
      const auto enrich = [now](json::object& row) {
        const auto expiry = rt::getNumber(row, "access_token_exp_ms");
        rt::objectSet(row, "access_token_expired", expiry ? json::value(*expiry <= now) : json::value(nullptr));
        rt::objectSet(row, "refresh_recommended", expiry ? json::value(*expiry - now < AUTH_REFRESH_WINDOW_MS) : json::value(nullptr));
      };
      enrich(result);
      json::array rows;
      if (const auto* source = rt::getArray(meta, "accounts")) for (const auto& account : *source) { auto row = account.as_object(); enrich(row); rows.push_back(std::move(row)); }
      rt::objectSet(result, "accounts", rows);
      return result;
    }
    rt::Task<std::optional<CodexAuthContext>> loadEnvCodexAuth() {
      if (config::config.codexAuthJsonB64.empty()) co_return std::nullopt;
      std::optional<CodexAuthContext> result;
      try {
        const auto parsed = rt::jsonParse(utils::decodeBase64ToString(config::config.codexAuthJsonB64));
        const auto auth = ca::parseCodexAuthFromAuthJson(parsed);
        if (auth) {
          const auto expiry = ca::getJwtExpMs(std::string_view(auth->access_token));
          const auto hash = co_await utils::sha256Hex(auth->refresh_token);
          json::array rows{json::object{{"slot", 1}, {"updated_at_ms", nullptr}, {"access_token_exp_ms", numberOrNull(expiry)}, {"refresh_sha256", hash}, {"native_owner_codex_home", nullptr}}};
          result = CodexAuthContext{json::object{{"source", "env"}, {"updated_at_ms", nullptr}, {"access_token_exp_ms", numberOrNull(expiry)}, {"account_count", 1}, {"accounts", rows}}, {auth->account_id}};
        }
      } catch (...) {}
      co_return result;
    }
    rt::Task<json::object> codexAccountMeta(::uos::types::CodexAuthState account, std::size_t index) {
      const auto expiry = ca::getJwtExpMs(std::string_view(account.access_token));
      const auto hash = co_await utils::sha256Hex(account.refresh_token);
      co_return json::object{{"slot", rt::makeNumber(static_cast<double>(index + 1))}, {"updated_at_ms", rt::makeNumber(account.updated_at_ms)}, {"access_token_exp_ms", numberOrNull(expiry)}, {"refresh_sha256", hash}, {"native_owner_codex_home", account.native_owner ? json::value(account.native_owner->codex_home) : json::value(nullptr)}};
    }
    rt::Task<CodexAuthContext> getCodexAuthContext() {
      auto pendingKv = kv::getKv(); const auto store = co_await pendingKv.wait();
      if (store) {
        const auto entry = co_await store->get(ca::CODEX_AUTH_POOL_KV_KEY);
        const auto pool = ca::parseCodexAuthPool(rt::storedJsonView(entry.value));
        if (pool) {
          std::vector<rt::Promise<json::object>> pending;
          for (std::size_t index = 0; index < pool->accounts.size(); ++index) { auto task = codexAccountMeta(pool->accounts[index], index); pending.emplace_back(std::move(task)); }
          const auto rows = co_await rt::all(std::move(pending)).wait();
          std::optional<double> minimum;
          json::array accountRows;
          for (const auto& row : rows) { const auto expiry = rt::getNumber(row, "access_token_exp_ms"); if (expiry && (!minimum || *expiry < *minimum)) minimum = *expiry; accountRows.push_back(row); }
          std::vector<std::string> ids;
          for (const auto& account : pool->accounts) ids.push_back(account.account_id);
          co_return CodexAuthContext{json::object{{"source", "kv"}, {"updated_at_ms", rt::makeNumber(pool->updated_at_ms)}, {"access_token_exp_ms", numberOrNull(minimum)}, {"account_count", rt::makeNumber(static_cast<double>(rows.size()))}, {"accounts", accountRows}}, std::move(ids)};
        }
      }
      if (!upstream_auth::followerMode()) {
        const auto env = co_await loadEnvCodexAuth();
        if (env) co_return *env;
      }
      co_return CodexAuthContext{json::object{{"source", "none"}, {"updated_at_ms", nullptr}, {"access_token_exp_ms", nullptr}, {"account_count", 0}, {"accounts", json::array{}}}, {}};
    }
    std::string aggregateProviderStates(const std::vector<ph::ProviderHealthView>& views) {
      const auto all = [&](std::string_view state) { return std::all_of(views.begin(), views.end(), [&](const auto& view) { return view.state == state; }); };
      if (views.empty() || all("unknown")) return "unknown";
      if (std::any_of(views.begin(), views.end(), [](const auto& view) { return view.state == "healthy"; })) return "healthy";
      if (all("invalid")) return "invalid";
      if (all("exhausted")) return "exhausted";
      return "degraded";
    }
    ph::ProviderHealthView unknownProviderHealth() { return {"unknown", std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt}; }
    rt::Task<ph::ProviderHealthView> providerHealthOrUnknown(rt::Promise<ph::ProviderHealthView> health) {
      std::optional<ph::ProviderHealthView> result;
      try { result = co_await health.wait(); } catch (...) {}
      co_return result.value_or(unknownProviderHealth());
    }
    rt::Promise<ph::ProviderHealthView> safeHealth(rt::Task<ph::ProviderHealthView> task) { rt::Promise<ph::ProviderHealthView> started(std::move(task)); return rt::Promise(providerHealthOrUnknown(std::move(started))); }
    json::object quotaView(const std::optional<metered_quota::MeteredQuotaSnapshot>& snapshot) {
      if (!snapshot) return {{"available", false}, {"cache_state", nullptr}, {"balance_credits", nullptr}, {"baseline_credits", nullptr}, {"remaining_percent", nullptr}, {"used_percent", nullptr}, {"observed_at_ms", nullptr}, {"confidence", nullptr}, {"cycle_started_at_ms", nullptr}, {"last_credit_at_ms", nullptr}, {"unlimited_quota", nullptr}, {"total_available", nullptr}, {"total_granted", nullptr}, {"total_used", nullptr}};
      const bool tokenUsage = snapshot->unlimited_quota || snapshot->total_available || snapshot->total_used;
      return {{"available", true}, {"cache_state", snapshot->cache_state}, {"balance_credits", numberOrNull(snapshot->balance_credits)}, {"baseline_credits", numberOrNull(snapshot->baseline_credits)}, {"remaining_percent", numberOrNull(snapshot->remaining_percent)}, {"used_percent", numberOrNull(snapshot->used_percent)}, {"observed_at_ms", rt::makeNumber(snapshot->state.observed_at_ms)}, {"confidence", tokenUsage ? json::value(nullptr) : json::value(snapshot->state.confidence)}, {"cycle_started_at_ms", tokenUsage ? json::value(nullptr) : rt::makeNumber(snapshot->state.cycle_started_at_ms)}, {"last_credit_at_ms", tokenUsage ? json::value(nullptr) : numberOrNull(snapshot->state.last_credit_at_ms)}, {"unlimited_quota", snapshot->unlimited_quota}, {"total_available", numberOrNull(snapshot->total_available)}, {"total_granted", numberOrNull(snapshot->total_granted)}, {"total_used", numberOrNull(snapshot->total_used)}};
    }
    json::object gatewayProviderHealthSubtree() {
      const bool configured = deepseek::waterfall::deepSeekWaterfallCatalogEnabled(std::nullopt);
      auto view = json::value_from(unknownProviderHealth()).as_object();
      rt::objectSet(view, "state", configured ? "healthy" : "unknown"); rt::objectSet(view, "stale", false);
      return {{"configured", configured}, {"health", view}};
    }
    std::optional<json::object> claudeWorkspaceView(const std::optional<credentials::ClaudeCodeWorkspaceState>& resolved, const std::optional<credentials::ClaudeCodeWorkspaceState>& stored, bool pinnedByEnv) {
      const auto id = resolved ? std::optional(resolved->id) : stored ? std::optional(stored->id) : std::nullopt;
      if (!id || id->empty()) return std::nullopt;
      const auto name = resolved && resolved->name ? resolved->name : stored ? stored->name : std::nullopt;
      const auto source = resolved ? std::optional(resolved->source) : stored ? std::optional(stored->source) : std::nullopt;
      return json::object{{"id", *id}, {"name", stringOrNull(name)}, {"source", stringOrNull(source)}, {"pinned_by_env", pinnedByEnv}, {"discovered_at_ms", stored ? numberOrNull(stored->discovered_at_ms) : json::value(nullptr)}, {"verified_at_ms", stored ? numberOrNull(stored->verified_at_ms) : json::value(nullptr)}, {"last_error", stored ? stringOrNull(stored->last_error) : json::value(nullptr)}};
    }
    std::optional<json::object> claudeCodeQuotaView(const std::optional<cq::ClaudeCodeQuotaState>& quota) {
      if (!quota) return std::nullopt;
      const auto raw = json::value_from(*quota);
      json::object result;
      for (const auto field : {"windows", "usage", "usage_snapshot", "standard", "representative_claim", "blocked_until_ms", "block_window", "cooldown_until_ms", "last_error", "updated_at_ms"}) if (const auto* value = rt::get(raw, field)) rt::objectDefine(result, field, *value);
      return result;
    }
    bool readClaudeCodeWorkspacePinnedByEnv() { try { return !rt::trim(rt::env::get(credentials::CLAUDE_CODE_WORKSPACE_ID_ENV).value_or("")).empty(); } catch (...) { return false; } }
    rt::Task<json::object> claudeAccountStatus(std::shared_ptr<rt::Kv> store, accounts::ClaudeCodeAccountBinding account, std::optional<std::string> activeId) {
      auto quotaTask = cq::loadClaudeCodeQuotaState(store, account); rt::Promise<std::optional<cq::ClaudeCodeQuotaState>> quotaPromise(std::move(quotaTask));
      auto healthTask = ph::getClaudeCodeProviderHealth({}, account); rt::Promise<ph::ProviderHealthView> healthPromise(std::move(healthTask));
      auto pending = rt::all(quotaPromise, healthPromise); const auto values = co_await pending.wait();
      auto currentTask = accounts::isClaudeCodeAccountCurrent(store, account); const bool current = co_await std::move(currentTask);
      const auto workspace = claudeWorkspaceView(account.workspace, account.workspace, account.workspace && account.workspace->source == "env");
      const auto quota = current ? claudeCodeQuotaView(std::get<0>(values)) : std::nullopt;
      json::object credential{{"kind", account.credential.kind}, {"source", account.credential.source}, {"hint", account.credential.hint}, {"label", stringOrNull(account.credential.label)}, {"updated_at_ms", numberOrNull(account.credential.updated_at_ms)}, {"expires_at_ms", nullptr}, {"refresh_material_present", false}};
      co_return json::object{{"account_id", account.account_id}, {"account_cohort_id", account.account_cohort_id}, {"label", stringOrNull(account.credential.label)}, {"credential", credential}, {"workspace", workspace ? json::value(*workspace) : json::value(nullptr)}, {"active", activeId && *activeId == account.account_id}, {"stale", !current}, {"quota", quota ? json::value(*quota) : json::value(nullptr)}, {"health", json::value_from(current ? std::get<1>(values) : unknownProviderHealth())}, {"capabilities", json::object{{"identity_verified", false}, {"can_refresh", false}, {"can_reset_upstream_quota", false}}}};
    }
    rt::Task<ph::ProviderHealthView> claudeHealth() {
      auto pendingKv = kv::getKv(); const auto store = co_await pendingKv.wait();
      auto accountTask = accounts::resolveClaudeCodeAccount(store); const auto account = co_await std::move(accountTask);
      if (!account) co_return unknownProviderHealth();
      auto task = ph::getClaudeCodeProviderHealth({}, account); const auto health = co_await std::move(task); co_return health;
    }
    rt::Task<std::vector<ph::ProviderHealthView>> codexHealth(std::vector<std::string> ids) {
      std::vector<rt::Promise<ph::ProviderHealthView>> tasks;
      for (const auto& id : ids) tasks.push_back(safeHealth(ph::getCodexProviderHealth(id)));
      const auto values = co_await rt::all(std::move(tasks)).wait(); co_return values;
    }
    rt::Task<std::optional<metered_quota::MeteredQuotaSnapshot>> cachedQuota() { std::optional<metered_quota::MeteredQuotaSnapshot> result; try { result = co_await metered_quota::getCachedConfiguredMeteredQuotaSnapshot(); } catch (...) {} co_return result; }
    json::object providerProbe(double status, std::string_view provider, const std::optional<std::string>& contentType, const std::optional<std::string>& error = std::nullopt) {
      json::object result{{"status", rt::makeNumber(status)}, {"provider", provider}, {"content_type", stringOrNull(contentType)}}; if (error) rt::objectSet(result, "error", json::value(*error)); return result;
    }
    rt::Task<json::object> codexProbe(json::object auth, rt::AbortSignal signal) {
      if (utils::getString(rt::get(auth, "source")) == "none") co_return providerProbe(503, "chatgpt_codex", std::nullopt, std::string(AUTH_NOT_CONFIGURED));
      std::optional<json::object> result;
      try {
        models::codex_models_fetch::FetchCodexModelsOptions options; options.signal = signal;
        auto task = models::codex_models_fetch::fetchCodexModels(options); auto response = co_await std::move(task);
        const auto contentType = response.headers().get("Content-Type");
        if (auto body = response.body()) try { co_await body->cancel(); } catch (...) {}
        result = providerProbe(response.ok() ? 200 : response.status() == 401 ? 401 : 503, "chatgpt_codex", contentType, response.ok() ? std::nullopt : std::optional<std::string>("Codex models probe returned an error status."));
      } catch (...) { result = providerProbe(503, "chatgpt_codex", std::nullopt, signal.aborted() ? "Codex models probe timed out." : "Codex models probe failed."); }
      co_return *result;
    }
    rt::Task<std::optional<json::object>> claudeSubscriptionProbe(std::optional<credentials::ClaudeCodeResolvedCredential> credential, std::optional<credentials::ClaudeCodeWorkspaceState> workspace, rt::AbortSignal signal) {
      if (!credential) co_return std::nullopt;
      std::optional<json::object> result;
      try {
        const cc::ClaudeCodeAuthHeaders auth{credential->kind, credential->secret};
        const cc::ClaudeCodeRequestHeadersOptions options{false, workspace ? std::optional(workspace->id) : std::nullopt};
        rt::RequestInit init{.method = "GET", .headers = cc::claudeCodeRequestHeaders(auth, options), .signal = signal, .redirect = "manual"};
        const std::string url(cc::CLAUDE_CODE_MODELS_URL); auto task = rt::fetch(url, init); auto response = co_await std::move(task);
        const auto contentType = response.headers().get("Content-Type");
        if (response.ok()) {
          if (auto body = response.body()) try { co_await body->cancel(); } catch (...) {}
          result = providerProbe(200, "claude_subscription", contentType);
        } else {
          std::string detail; try { detail = co_await response.text(); } catch (...) {} detail = rt::trim(detail);
          result = providerProbe(response.status() == 401 ? 401 : 503, "claude_subscription", contentType, detail.empty() ? "Claude models probe returned an error status." : rt::slice(detail, 0, 240));
        }
      } catch (...) { result = providerProbe(503, "claude_subscription", std::nullopt, signal.aborted() ? "Claude models probe timed out." : "Claude models probe failed."); }
      co_return result;
    }
    rt::Task<std::optional<json::object>> meteredQuotaProbe(std::optional<metered_quota::MeteredAccountCredentials> credential, rt::AbortSignal signal) {
      if (!credential) co_return std::nullopt;
      std::optional<json::object> result;
      try { metered_quota::FetchMeteredQuotaObservationOptions options; options.signal = signal; auto task = metered_quota::fetchMeteredQuotaObservation(*credential, options); const auto observation = co_await std::move(task); static_cast<void>(observation); result = providerProbe(200, "metered_quota", "application/json"); }
      catch (...) { result = providerProbe(503, "metered_quota", std::nullopt, signal.aborted() ? "Metered quota probe timed out." : "Metered quota probe failed."); }
      co_return result;
    }
    json::object aggregateUpstreamProbe(const std::optional<json::object>& auth, const json::object& codex, const std::optional<json::object>& meteredQuota, const std::optional<json::object>& claudeSubscription, const std::optional<json::object>& claudeWorkspace) {
      double status = 200;
      for (const auto* probe : {&codex, meteredQuota ? &*meteredQuota : nullptr}) if (probe) {
        const auto code = rt::getNumber(*probe, "status").value_or(0);
        if (code == 401) status = 401; else if (code >= 400 && status != 401) status = 503;
      }
      return {{"status", rt::makeNumber(status)}, {"auth", auth ? json::value(*auth) : json::value(nullptr)}, {"probes", json::object{{"codex", codex}, {"metered_quota", meteredQuota ? json::value(*meteredQuota) : json::value(nullptr)}, {"claude_subscription", claudeSubscription ? json::value(*claudeSubscription) : json::value(nullptr)}, {"claude_workspace", claudeWorkspace ? json::value(*claudeWorkspace) : json::value(nullptr)}}}};
    }
    rt::Task<json::object> trackedCodex(json::object auth, rt::AbortSignal signal, std::shared_ptr<HealthProbeProgress> progress) { auto task = codexProbe(std::move(auth), signal); auto result = co_await std::move(task); progress->codex = result; co_return result; }
    rt::Task<std::optional<json::object>> trackedMetered(std::optional<metered_quota::MeteredAccountCredentials> credential, rt::AbortSignal signal, std::shared_ptr<HealthProbeProgress> progress) { auto task = meteredQuotaProbe(std::move(credential), signal); const auto result = co_await std::move(task); progress->metered_quota = rt::orNull(result); co_return result; }
    rt::Task<std::optional<json::object>> trackedClaude(std::shared_ptr<rt::Kv> store, std::optional<accounts::ClaudeCodeAccountBinding> account, rt::AbortSignal signal, std::shared_ptr<HealthProbeProgress> progress) {
      const auto credential = account ? std::optional(account->credential) : std::nullopt;
      const auto workspace = account ? account->workspace : std::nullopt;
      auto task = claudeSubscriptionProbe(credential, workspace, signal); const auto probe = co_await std::move(task);
      bool current = true;
      if (account) { auto currentTask = accounts::isClaudeCodeAccountCurrent(store, *account); current = co_await std::move(currentTask); }
      if (!current) { progress->claude_workspace = nullptr; const auto result = providerProbe(503, "claude_subscription", std::nullopt, "Claude account changed during the models probe."); progress->claude_subscription = result; co_return result; }
      progress->claude_subscription = rt::orNull(probe); co_return probe;
    }
    rt::Task<json::object> probeUpstream(rt::AbortSignal signal, std::optional<metered_quota::MeteredAccountCredentials> meteredCredentials, std::shared_ptr<HealthProbeProgress> progress) {
      std::optional<json::object> auth;
      try { const auto context = co_await getCodexAuthContext(); auth = enrichAuthMeta(context.meta); } catch (...) {}
      if (!auth) co_return aggregateUpstreamProbe(std::nullopt, providerProbe(503, "chatgpt_codex", std::nullopt, "Codex auth metadata could not be read."), std::nullopt, std::nullopt, std::nullopt);
      progress->auth = auth;
      auto pendingKv = kv::getKv(); const auto store = co_await pendingKv.wait();
      std::optional<accounts::ClaudeCodeAccountBinding> account;
      try { account = co_await accounts::resolveClaudeCodeAccount(store); } catch (...) {}
      if (account) {
        const auto& workspace = account->workspace;
        progress->claude_workspace = json::object{{"id", workspace ? json::value(workspace->id) : json::value(nullptr)}, {"name", workspace ? stringOrNull(workspace->name) : json::value(nullptr)}, {"source", workspace ? json::value(workspace->source) : json::value(nullptr)}, {"pinned_by_env", readClaudeCodeWorkspacePinnedByEnv()}, {"account_id", account->account_id}, {"account_cohort_id", account->account_cohort_id}};
      } else progress->claude_workspace = nullptr;
      auto codexTask = trackedCodex(*auth, signal, progress); rt::Promise<json::object> codexPromise(std::move(codexTask));
      auto meteredTask = trackedMetered(meteredCredentials, signal, progress); rt::Promise<std::optional<json::object>> meteredPromise(std::move(meteredTask));
      auto claudeTask = trackedClaude(store, account, signal, progress); rt::Promise<std::optional<json::object>> claudePromise(std::move(claudeTask));
      auto all = rt::all(codexPromise, meteredPromise, claudePromise); const auto values = co_await all.wait();
      const auto workspace = progress->claude_workspace.isNullish() ? std::optional<json::object>{} : std::optional(*progress->claude_workspace);
      co_return aggregateUpstreamProbe(auth, std::get<0>(values), std::get<1>(values), std::get<2>(values), workspace);
    }
    json::object timedOutProviderProbe(const std::string& provider) { return providerProbe(503, provider, std::nullopt, provider == "chatgpt_codex" ? "Codex models probe timed out." : provider == "claude_subscription" ? "Claude models probe timed out." : "Metered quota probe timed out."); }
    json::object timeoutProbe(const HealthProbeProgress& progress) {
      const auto nullable = [](const rt::Nullable<json::object>& value, const std::string& provider) -> std::optional<json::object> { return value == rt::undefined ? std::optional(timedOutProviderProbe(provider)) : value == nullptr ? std::nullopt : std::optional(*value); };
      return aggregateUpstreamProbe(progress.auth, progress.codex.value_or(timedOutProviderProbe("chatgpt_codex")), nullable(progress.metered_quota, "metered_quota"), nullable(progress.claude_subscription, "claude_subscription"), progress.claude_workspace.isNullish() ? std::nullopt : std::optional(*progress.claude_workspace));
    }
    rt::Task<json::object> probeUpstreamCoalesced() {
      if (upstreamProbeInFlight) { auto pending = *upstreamProbeInFlight; auto result = co_await pending.wait(); co_return result; }
      const rt::AbortController controller;
      const auto meteredCredentials = metered_quota::readMeteredAccountCredentials();
      const auto progress = std::make_shared<HealthProbeProgress>();
      if (!meteredCredentials) progress->metered_quota = nullptr;
      const auto timeout = rt::withResolvers<json::object>();
      rt::Timeout timer;
      try {
        timer = rt::setTimeout([controller, progress, resolve = timeout.resolve] { controller.abort(rt::makeReason(rt::DOMException("Active upstream health probe timed out.", "TimeoutError"))); resolve(timeoutProbe(*progress)); }, activeUpstreamHealthTimeoutMs);
      } catch (...) {
        timeout.reject(std::current_exception());
      }
      auto task = probeUpstream(controller.signal(), meteredCredentials, progress);
      std::vector<rt::Promise<json::object>> racers; racers.emplace_back(std::move(task)); racers.push_back(timeout.promise);
      auto pending = rt::race(std::move(racers)).finally_([timer, reject = timeout.reject] {
        if (timer) rt::clearTimeout(timer);
        reject();
        upstreamProbeInFlight = std::nullopt;
      });
      upstreamProbeInFlight = pending;
      const auto result = co_await pending.wait(); co_return result;
    }
  }
  void setActiveUpstreamHealthTimeoutMsForTest(std::optional<double> timeoutMs) { activeUpstreamHealthTimeoutMs = timeoutMs.value_or(ACTIVE_UPSTREAM_HEALTH_TIMEOUT_MS); }
  rt::Task<json::object> getClaudeSubscriptionStatusSnapshot() {
    auto pendingKv = kv::getKv(); const auto store = co_await pendingKv.wait();
    std::optional<provider::selection::ProviderSelection> selection;
    try { selection = co_await provider::selection::loadProviderSelectionCached(); } catch (...) {}
    std::optional<accounts::ClaudeCodeAccountStore> roster;
    std::optional<provider::claude_code_access::ClaudeCodeAccessPolicy> access;
    if (store) { try { roster = co_await accounts::loadClaudeCodeAccountStore(store); } catch (...) {} try { access = co_await provider::claude_code_access::loadClaudeCodeAccessPolicy(store); } catch (...) {} }
    std::vector<rt::Promise<json::object>> pending;
    if (store && roster) for (const auto& account : roster->accounts) { auto task = claudeAccountStatus(store, account, roster->active_account_id); pending.emplace_back(std::move(task)); }
    const auto status = co_await rt::all(std::move(pending)).wait();
    const json::object* active = nullptr;
    json::array rows;
    for (const auto& row : status) { rows.push_back(row); if (roster && utils::getString(rt::get(row, "account_id")) == roster->active_account_id && rt::getBool(row, "stale") != true) active = &row; }
    const auto field = [active](std::string_view key) { const auto* value = active ? rt::get(*active, key) : nullptr; return value ? *value : json::value(nullptr); };
    json::value accessView = nullptr;
    if (access) accessView = json::object{{"source", access->source}, {"allowed_key_ids", json::value_from(access->allowed_key_ids)}, {"updated_at_ms", rt::makeNumber(access->updated_at_ms)}};
    co_return json::object{{"configured", active != nullptr}, {"credential", field("credential")}, {"workspace", field("workspace")}, {"account_id", field("account_id")}, {"account_cohort_id", field("account_cohort_id")}, {"accounts", std::move(rows)}, {"active_account_id", roster ? stringOrNull(roster->active_account_id) : json::value(nullptr)}, {"active_reason", roster ? json::value(roster->active_reason) : json::value(nullptr)}, {"account_scan", roster ? "ok" : "unavailable"}, {"access", accessView}, {"principal_allowlist_configured", access && !access->allowed_key_ids.empty()}, {"enabled", provider::selection::isProviderEnabled("claude_code", selection)}, {"quota", field("quota")}, {"health", active ? field("health") : json::value_from(unknownProviderHealth())}};
  }
  rt::Task<json::object> getPassiveProviderHealthSnapshot() { return getPassiveProviderHealthSnapshot(PassiveProviderHealthOptions{}); }
  rt::Task<json::object> getPassiveProviderHealthSnapshot(PassiveProviderHealthOptions options) {
    const auto context = co_await getCodexAuthContext(); const auto auth = enrichAuthMeta(context.meta);
    const auto claudeStatus = co_await getClaudeSubscriptionStatusSnapshot();
    auto cerebras = safeHealth(ph::getCerebrasProviderHealth()); auto claude = safeHealth(claudeHealth());
    rt::Promise<std::vector<ph::ProviderHealthView>> codex(codexHealth(context.account_ids));
    auto deepseek = safeHealth(ph::getDeepSeekProviderHealth()); auto lithos = safeHealth(ph::getLithosProviderHealth()); auto metered = safeHealth(ph::getMeteredProviderHealth()); auto openrouter = safeHealth(ph::getOpenRouterProviderHealth()); auto surplus = safeHealth(ph::getSurplusProviderHealth());
    std::optional<rt::Promise<ph::ProviderHealthView>> gemini;
    if (provider::gemini::geminiCliConfigured()) gemini.emplace(safeHealth(ph::getGeminiProviderHealth()));
    rt::Promise<std::optional<metered_quota::MeteredQuotaSnapshot>> quota(cachedQuota());
    auto pending = rt::all(cerebras, claude, codex, deepseek, lithos, metered, openrouter, surplus, quota);
    const auto values = co_await pending.wait(); const auto& codexViews = std::get<2>(values); const auto& meteredQuota = std::get<8>(values);
    json::array codexAccounts;
    const auto* rawAccounts = rt::getArray(auth, "accounts");
    if (rawAccounts) for (std::size_t index = 0; index < rawAccounts->size(); ++index) { auto row = (*rawAccounts)[index].as_object(); rt::objectSet(row, "health", json::value_from(codexViews[index])); codexAccounts.push_back(std::move(row)); }
    json::object meteredQuotaView = options.includeQuota.value_or(false) ? quotaView(meteredQuota) : json::object{{"available", meteredQuota.has_value()}, {"cache_state", meteredQuota ? json::value(meteredQuota->cache_state) : json::value(nullptr)}, {"observed_at_ms", meteredQuota ? rt::makeNumber(meteredQuota->state.observed_at_ms) : json::value(nullptr)}};
    json::object surplusQuota;
    if (options.includeQuota.value_or(false)) { surplusQuota = quotaView(std::nullopt); rt::objectSet(surplusQuota, "source", "not_reported"); }
    else surplusQuota = {{"available", false}, {"observed_at_ms", nullptr}};
    auto claudeView = claudeStatus;
    if (rt::isNullish(rt::get(claudeView, "health"))) rt::objectSet(claudeView, "health", json::value_from(std::get<1>(values)));
    json::object result{{"mode", "passive"}, {"generated_at_ms", rt::makeNumber(rt::dateNow())}, {"stale_after_ms", rt::makeNumber(ph::PROVIDER_HEALTH_STALE_AFTER_MS)}, {"codex", json::object{{"configured", utils::getString(rt::get(auth, "source")) != "none"}, {"source", *rt::get(auth, "source")}, {"account_count", *rt::get(auth, "account_count")}, {"state", aggregateProviderStates(codexViews)}, {"accounts", codexAccounts}}}, {"cerebras", json::object{{"configured", provider::cerebras::readCerebrasApiKey().has_value()}, {"health", json::value_from(std::get<0>(values))}}}, {"claude_code", claudeView}, {"deepseek", json::object{{"configured", deepseek::index::readDeepSeekApiKey().has_value()}, {"health", json::value_from(std::get<3>(values))}}}, {"lithos", json::object{{"configured", provider::lithos::readLithosApiKey().has_value()}, {"health", json::value_from(std::get<4>(values))}}}, {"metered", json::object{{"configured", provider::metered::readMeteredApiKey().has_value()}, {"quota_monitoring_configured", metered_quota::readMeteredAccountCredentials().has_value()}, {"health", json::value_from(std::get<5>(values))}, {"quota", meteredQuotaView}}}, {"openrouter", json::object{{"configured", provider::openrouter::readOpenRouterApiKey().has_value()}, {"health", json::value_from(std::get<6>(values))}}}, {"surplus", json::object{{"configured", provider::surplus::readSurplusApiKey().has_value()}, {"quota_monitoring_configured", false}, {"health", json::value_from(std::get<7>(values))}, {"quota", surplusQuota}}}, {"ubiquity", gatewayProviderHealthSubtree()}};
    if (gemini) {
      const auto view = co_await gemini->wait();
      result.emplace("gemini", json::object{{"configured", true}, {"health", json::value_from(view)}, {"control", "antigravity-cli"}});
    }
    co_return result;
  }
  rt::Task<rt::Response> handleHealthProviders() { return handleHealthProviders(PassiveProviderHealthOptions{}); }
  rt::Task<rt::Response> handleHealthProviders(PassiveProviderHealthOptions options) {
    auto task = getPassiveProviderHealthSnapshot(options); const auto payload = co_await std::move(task);
    const rt::Headers headers{{"Cache-Control", "no-store"}, {"x-uos-git-sha", config::runtimeGitSha()}, {"x-uos-deployment-id", config::runtimeDeploymentId()}};
    co_return http::json(200, payload, headers);
  }
  namespace {
    rt::Response healthResponse(std::optional<json::object> upstreamAuth = std::nullopt) {
      const auto gitSha = config::runtimeGitSha(); const auto deploymentId = config::runtimeDeploymentId();
      json::object body{{"status", "available"}, {"release", json::object{{"git_sha", gitSha}, {"deployment_id", deploymentId}}}};
      if (upstreamAuth) body.emplace("upstream_auth", std::move(*upstreamAuth));
      return http::json(200, body, {{"Cache-Control", "no-store"}, {"x-uos-git-sha", gitSha}, {"x-uos-deployment-id", deploymentId}});
    }
  }
  rt::Response handleHealth() { return healthResponse(); }
  rt::Task<rt::Response> handleFollowerHealth() {
    std::optional<double> remaining;
    try {
      const auto auth = co_await getCodexAuthContext();
      const auto expiry = rt::getNumber(auth.meta, "access_token_exp_ms");
      if (expiry && rt::Number::isFinite(*expiry)) remaining = std::max(0.0, *expiry - rt::dateNow());
    } catch (...) {}
    co_return healthResponse(json::object{{"follower", true}, {"earliest_access_token_expires_in_ms", numberOrNull(remaining)}});
  }
  rt::Task<rt::Response> handleHealthUpstream() {
    const auto probe = co_await probeUpstreamCoalesced();
    const rt::Headers headers{{"x-uos-git-sha", config::runtimeGitSha()}, {"x-uos-deployment-id", config::runtimeDeploymentId()}};
    co_return http::json(rt::getNumber(probe, "status").value(), json::object{{"status", *rt::get(probe, "status")}, {"probes", *rt::get(probe, "probes")}, {"auth", *rt::get(probe, "auth")}}, headers);
  }
}
