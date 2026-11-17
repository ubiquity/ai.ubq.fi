/**
 * The owner-only Claude subscription hop. Only current exact-model full-pool
 * proof can authorize paid continuation. Transient and configuration errors
 * remain the original subscription failure, not invented capacity exhaustion.
 */
#include "uos/paid_fallback/claude_code_route.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/health.hpp"
#include "uos/provider/claude_code.hpp"
#include "uos/provider/claude_code_access.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/surplus_claude_request.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <vector>

namespace uos::paid_fallback::claude_code_route {
  namespace {
    namespace telemetry = openai_telemetry;
    namespace claude = provider::claude_code;
    namespace accounts = provider::claude_code_accounts;
    namespace models = provider::claude_code_models;
    namespace quota = provider::claude_code_quota;
    namespace access = provider::claude_code_access;
    std::string healthEventByClassification(const std::string& kind) {
      if (kind == "ok") return "success";
      if (kind == "durable_exhaustion") return "quota_exhausted";
      if (kind == "auth_invalid") return "auth_invalid";
      // The transport worked; a rejected request leaves the route reachable.
      if (kind == "request_rejected") return "reachable";
      return "upstream_error";
    }
    telemetry::RoutedResponsesUpstream routedClaudeResponse(rt::Response response, bool gatewayResponse = false) {
      return {.response = response, .provider = "claude_code", .paidFallback = std::nullopt, .gatewayResponse = gatewayResponse, .fallbackReason = std::nullopt};
    }
    const json::value* requestedEffort(const json::object& body) {
      const auto* reasoning = rt::get(body, "reasoning");
      return utils::isRecord(reasoning) ? rt::get(*reasoning, "effort") : nullptr;
    }
    struct NativeCapabilities { bool supportsNativeDisabledThinking = false; std::vector<std::string> nativeReasoningEfforts; };
    rt::Task<NativeCapabilities> nativeCapabilities(std::shared_ptr<rt::Kv> store, accounts::ClaudeCodeAccountBinding account,
      json::object body, SubscriptionOptions options) {
      if (!requestedEffort(body)) co_return NativeCapabilities{};
      models::CatalogOptions catalogOptions;
      catalogOptions.kv = store;
      catalogOptions.env = options.env;
      catalogOptions.accountBinding = account;
      catalogOptions.expectedBinding = models::CatalogOptions::ExpectedBinding{
        {account.credential.kind, account.credential.secret}, account.workspace ? std::optional<std::string>(account.workspace->id) : std::nullopt};
      rt::Promise<std::optional<models::ClaudeCodeModelsSnapshot>> loading(models::fetchClaudeCodeModels(catalogOptions));
      const auto snapshot = co_await loading.wait();
      if (snapshot && snapshot->stale != true) for (const auto& model : snapshot->models) if (model.id == options.model)
        co_return NativeCapabilities{model.supports_disabled_thinking == true, model.supported_reasoning_levels.value_or(std::vector<std::string>{})};
      co_return NativeCapabilities{};
    }
    rt::Task<bool> reasoningProofCurrent(std::shared_ptr<rt::Kv> store, accounts::ClaudeCodeAccountBinding account,
      json::object body, SubscriptionOptions options) {
      rt::Promise<bool> checking(accounts::isClaudeCodeAccountCurrent(store, account));
      const bool current = co_await checking.wait();
      if (!current) co_return false;
      const auto* effort = requestedEffort(body);
      if (!effort) co_return true;
      rt::Promise<NativeCapabilities> loading(nativeCapabilities(store, account, body, options));
      const auto capability = co_await loading.wait();
      if (effort->is_string() && effort->as_string() == "none") co_return capability.supportsNativeDisabledThinking;
      co_return effort->is_string() && std::find(capability.nativeReasoningEfforts.begin(), capability.nativeReasoningEfforts.end(), std::string(effort->as_string())) != capability.nativeReasoningEfforts.end();
    }
    rt::Task<void> recordQuotaOutcome(std::shared_ptr<rt::Kv> store, quota::QuotaOutcome outcome) {
      rt::Promise<std::optional<quota::ClaudeCodeQuotaState>> pending(quota::recordClaudeCodeQuotaOutcome(store, outcome));
      const auto result = co_await pending.wait();
      (void)result;
    }
    rt::Task<void> recordAccountOutcome(std::shared_ptr<rt::Kv> store, accounts::ClaudeCodeAccountBinding account,
      claude::ClaudeCodeResponsesResult result, std::string model) {
      const auto classification = result.classification;
      quota::QuotaOutcome outcome;
      outcome.binding = account; outcome.model_id = model; outcome.response_status = result.response.status();
      outcome.response_observed_at_ms = result.observed_at_ms; outcome.observation = result.quota; outcome.observation_source = "response_headers";
      outcome.exhaustion = classification.exhaustion; outcome.auth_invalid = classification.kind == "auth_invalid"; outcome.served = classification.kind == "ok";
      if (classification.kind == "transient_rate_limited") outcome.transient = quota::QuotaOutcome::Transient{classification.retry_after_ms};
      if (claude::isClaudeCodeConfigurationKind(classification.kind)) outcome.config_error = quota::QuotaOutcome::ConfigError{
        classification.kind, result.response.status(), classification.detail, classification.retry_after_ms};
      const std::function<rt::Task<void>()> recordQuota = [store, outcome] { return recordQuotaOutcome(store, outcome); };
      rt::Promise<void> recordingQuota(health::bestEffortPaidFallbackBookkeeping("claude_code quota recording", recordQuota));
      co_await recordingQuota.wait();
      const std::function<rt::Task<void>()> recordHealth = [classification, result, account] {
        return provider::health::recordClaudeCodeProviderHealth(healthEventByClassification(classification.kind), result.response.status(), rt::dateNow, result.request_id, account).wait();
      };
      rt::Promise<void> recordingHealth(health::bestEffortPaidFallbackBookkeeping("claude_code health recording", recordHealth));
      co_await recordingHealth.wait();
    }
    struct PoolState { accounts::ClaudeCodeAccountBinding account; std::optional<quota::ClaudeCodeQuotaState> quota; };
    struct PoolView { accounts::ClaudeCodeAccountStore roster; std::vector<PoolState> states; bool fresh; };
    rt::Task<PoolState> loadPoolState(std::shared_ptr<rt::Kv> store, accounts::ClaudeCodeAccountBinding account) {
      rt::Promise<std::optional<quota::ClaudeCodeQuotaState>> loading(quota::loadClaudeCodeQuotaState(store, account));
      const auto state = co_await loading.wait();
      co_return PoolState{account, state};
    }
    std::string identity(const accounts::ClaudeCodeAccountStore& store) {
      json::array rows;
      for (const auto& account : store.accounts) rows.push_back(json::array{account.account_id, account.account_cohort_id, account.evidence_generation});
      return rt::jsonStringify(json::array{store.active_account_id ? json::value(*store.active_account_id) : json::value(nullptr), rows});
    }
    rt::Task<PoolView> poolView(std::shared_ptr<rt::Kv> store, std::string model, claude::ClaudeCodeEnvLookup env) {
      (void)model;
      rt::Promise<accounts::ClaudeCodeAccountStore> loading(accounts::loadClaudeCodeAccountStore(store, env));
      const auto roster = co_await loading.wait();
      std::vector<rt::Promise<PoolState>> pending;
      for (const auto& account : roster.accounts) pending.emplace_back(loadPoolState(store, account));
      const auto all = rt::all(std::move(pending));
      const auto states = co_await all.wait();
      rt::Promise<accounts::ClaudeCodeAccountStore> reloading(accounts::loadClaudeCodeAccountStore(store, env));
      const auto latest = co_await reloading.wait();
      co_return PoolView{roster, states, identity(roster) == identity(latest)};
    }
    bool hasProof(const PoolState& state, const std::string& model) { return quota::claudeCodePoolProofForModel(state.quota, state.account, model).has_value(); }
    struct RequestLifetime { bool dispatched = false; std::unordered_set<std::string> attempted; std::string keyId; };
    struct PoolStep { std::string kind; std::optional<telemetry::RoutedResponsesUpstream> routed{}; };
    telemetry::RoutedResponsesUpstream claudeLocalError(const claude::ClaudeCodeError& error) {
      const http::OpenaiErrorOptions options{.type = error.status >= 500 ? "server_error" : "invalid_request_error"};
      return routedClaudeResponse(http::openaiError(error.status, error.message, error.code, options), true);
    }
    rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>> beforeAccountDispatch(std::shared_ptr<rt::Kv> store,
      accounts::ClaudeCodeAccountBinding account, json::object body, SubscriptionOptions options, std::shared_ptr<RequestLifetime> lifetime) {
      std::optional<api_key_policy::ApiKeyProviderDispatch> dispatch;
      if (!lifetime->dispatched) {
        if (options.usageContext && options.usageContext->beforeProviderDispatch) {
          rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> pending(options.usageContext->beforeProviderDispatch("claude_code"));
          dispatch = co_await pending.wait();
        } else co_await rt::microtask();
      }
      rt::Promise<access::ClaudeCodeAccessPolicy> loading(access::loadClaudeCodeAccessPolicy(store, options.env));
      const auto policy = co_await loading.wait();
      if (std::find(policy.allowed_key_ids.begin(), policy.allowed_key_ids.end(), lifetime->keyId) == policy.allowed_key_ids.end()) {
        if (dispatch) { rt::Promise<void> cancelling(dispatch->cancelBeforeTransport()); co_await cancelling.wait(); }
        else co_await rt::microtask();
        throw claude::ClaudeCodeError("The principal is no longer allowed to use the Claude subscription.", "claude_code_request_invalid", 403);
      }
      rt::Promise<bool> checking(reasoningProofCurrent(store, account, body, options));
      const bool proofCurrent = co_await checking.wait();
      if (!proofCurrent) {
        if (dispatch) { rt::Promise<void> cancelling(dispatch->cancelBeforeTransport()); co_await cancelling.wait(); }
        else co_await rt::microtask();
        throw claude::ClaudeCodeError("Claude credential/workspace capability proof changed before dispatch.", "claude_code_request_invalid", 400);
      }
      co_return dispatch;
    }
    struct DispatchResult { std::optional<claude::ClaudeCodeResponsesResult> result{}; std::optional<telemetry::RoutedResponsesUpstream> routed{}; };
    rt::Task<DispatchResult> dispatchAccount(std::shared_ptr<rt::Kv> store, accounts::ClaudeCodeAccountBinding account,
      json::object body, SubscriptionOptions options, std::shared_ptr<RequestLifetime> lifetime) {
      rt::Reason failure;
      try {
        rt::Promise<NativeCapabilities> loading(nativeCapabilities(store, account, body, options));
        const auto capabilities = co_await loading.wait();
        claude::ClaudeCodeAuthenticatedFetchOptions fetchOptions;
        fetchOptions.credential = account.credential;
        if (account.workspace) fetchOptions.workspace = provider::claude_code_credentials::ClaudeCodeResolvedWorkspace{
          account.workspace->id, account.workspace->name, account.workspace->source};
        else fetchOptions.workspace = nullptr;
        fetchOptions.accountBinding = account; fetchOptions.kv = store; fetchOptions.env = options.env;
        fetchOptions.fetcher = options.fetcher; fetchOptions.signal = options.signal;
        fetchOptions.reasoningReplayScope = health::codexCacheScopeForUsageContext(options.usageContext).value_or("undefined") + ":" + account.account_id + ":" + account.account_cohort_id;
        fetchOptions.supportsNativeDisabledThinking = capabilities.supportsNativeDisabledThinking;
        fetchOptions.nativeReasoningEfforts = capabilities.nativeReasoningEfforts;
        fetchOptions.beforeDispatch = [store, account, body, options, lifetime] { return beforeAccountDispatch(store, account, body, options, lifetime); };
        fetchOptions.onDispatch = [options, lifetime] {
          if (!lifetime->dispatched) telemetry::recordFirstProviderDispatch(options.usageContext);
          lifetime->dispatched = true;
        };
        if (options.usageContext) fetchOptions.sentinelUpstreamRecorder = options.usageContext->sentinelUpstreamRecorder;
        rt::Promise<claude::ClaudeCodeResponsesResult> fetching(claude::fetchClaudeCodeResponses(body, fetchOptions));
        const auto result = co_await fetching.wait();
        co_return DispatchResult{result, std::nullopt};
      } catch (const api_key_policy::ApiKeyQuotaDispatchError&) { throw; }
      catch (...) { failure = std::current_exception(); }
      if (options.signal && options.signal->aborted()) {
        const auto reason = options.signal->reason();
        const auto value = rt::thrownValue(reason);
        std::rethrow_exception(reason && (!value || !value->is_null()) ? reason : failure);
      }
      std::unique_ptr<claude::ClaudeCodeError> local;
      try { std::rethrow_exception(failure); } catch (const claude::ClaudeCodeError& error) { local = std::make_unique<claude::ClaudeCodeError>(error); } catch (...) {}
      if (!local) std::rethrow_exception(failure);
      if (local->code == "claude_code_upstream_unreachable") {
        const std::function<rt::Task<void>()> record = [account] {
          return provider::health::recordClaudeCodeProviderHealth("upstream_error", std::nullopt, rt::dateNow, std::nullopt, account).wait();
        };
        rt::Promise<void> recording(health::bestEffortPaidFallbackBookkeeping("claude_code failure recording", record));
        co_await recording.wait();
      }
      co_return DispatchResult{std::nullopt, claudeLocalError(*local)};
    }
    struct PoolChoice { std::string kind; std::optional<PoolState> state{}; };
    rt::Task<PoolChoice> choosePoolAccount(std::shared_ptr<rt::Kv> store, SubscriptionOptions options) {
      rt::Promise<PoolView> loading(poolView(store, options.model, options.env));
      const auto view = co_await loading.wait();
      if (!view.fresh) co_return PoolChoice{"retry"};
      const auto active = std::find_if(view.states.begin(), view.states.end(), [&](const auto& state) { return state.account.account_id == view.roster.active_account_id; });
      if (active == view.states.end()) co_return PoolChoice{"paid"};
      if (!hasProof(*active, options.model)) co_return PoolChoice{"account", *active};
      const auto next = std::find_if(view.states.begin(), view.states.end(), [&](const auto& state) { return !hasProof(state, options.model); });
      if (next == view.states.end()) co_return PoolChoice{"paid"};
      rt::Promise<bool> selecting(accounts::selectClaudeCodeAccount(store, next->account.account_id, "authoritative_unavailability", active->account));
      const bool selected = co_await selecting.wait(); (void)selected;
      co_return PoolChoice{"retry"};
    }
    rt::Task<std::string> advanceAfterOutcome(std::shared_ptr<rt::Kv> store, accounts::ClaudeCodeAccountBinding account, SubscriptionOptions options) {
      rt::Promise<std::optional<quota::ClaudeCodeQuotaState>> loading(quota::loadClaudeCodeQuotaState(store, account));
      const auto state = co_await loading.wait();
      if (!quota::claudeCodePoolProofForModel(state, account, options.model)) co_return std::string("preserve");
      rt::Promise<PoolView> reloading(poolView(store, options.model, options.env));
      const auto latest = co_await reloading.wait();
      if (!latest.fresh) co_return std::string("preserve");
      if (std::all_of(latest.states.begin(), latest.states.end(), [&](const auto& candidate) { return hasProof(candidate, options.model); })) co_return std::string("paid");
      const auto next = std::find_if(latest.states.begin(), latest.states.end(), [&](const auto& candidate) { return !hasProof(candidate, options.model); });
      if (next == latest.states.end()) co_return std::string("preserve");
      rt::Promise<bool> selecting(accounts::selectClaudeCodeAccount(store, next->account.account_id, "authoritative_unavailability", account));
      const bool selected = co_await selecting.wait();
      co_return std::string(selected ? "retry" : "preserve");
    }
    rt::Task<PoolStep> servePoolAccount(std::shared_ptr<rt::Kv> store, PoolState state, json::object body,
      SubscriptionOptions options, std::shared_ptr<RequestLifetime> lifetime) {
      const auto account = state.account;
      if (lifetime->attempted.contains(account.account_id)) co_return PoolStep{"response", claudeLocalError(claude::ClaudeCodeError(
        "Claude account selection changed during the request.", "claude_code_upstream_unreachable", 409))};
      lifetime->attempted.insert(account.account_id);
      if (quota::claudeCodeQuotaCoolingDown(state.quota)) {
        const auto lastError = state.quota ? state.quota->last_error : std::nullopt;
        const auto status = lastError ? lastError->status.value_or(429) : 429;
        const auto message = lastError && lastError->message ? *lastError->message : "The Claude subscription is cooling down after an upstream rejection.";
        co_return PoolStep{"response", routedClaudeResponse(http::openaiError(status, message, "claude_code_cooling_down"), true)};
      }
      rt::Promise<DispatchResult> dispatching(dispatchAccount(store, account, body, options, lifetime));
      const auto attempted = co_await dispatching.wait();
      if (attempted.routed) co_return PoolStep{"response", attempted.routed};
      const auto result = *attempted.result;
      const auto outcomeAccount = result.account_binding.value_or(account);
      rt::Promise<void> recording(recordAccountOutcome(store, outcomeAccount, result, options.model));
      co_await recording.wait();
      if (result.classification.kind == "ok") {
        if (options.usageContext && options.usageContext->responseTelemetry) options.usageContext->responseTelemetry->provider = "claude_code";
        co_return PoolStep{"response", routedClaudeResponse(result.response)};
      }
      rt::Promise<std::string> advancing(advanceAfterOutcome(store, outcomeAccount, options));
      const auto advance = co_await advancing.wait();
      if (advance == "preserve") co_return PoolStep{"response", routedClaudeResponse(result.response)};
      upstream_wire::cancelResponseBody(result.response);
      co_return PoolStep{advance};
    }
    rt::Task<std::optional<telemetry::RoutedResponsesUpstream>> runClaudePool(std::shared_ptr<rt::Kv> store, json::object body,
      SubscriptionOptions options, std::string keyId) {
      const auto lifetime = std::make_shared<RequestLifetime>(); lifetime->keyId = keyId;
      for (int iteration = 0; iteration < 34; ++iteration) {
        rt::Promise<PoolChoice> choosing(choosePoolAccount(store, options));
        const auto choice = co_await choosing.wait();
        if (choice.kind == "retry") continue;
        if (choice.kind == "paid") co_return std::nullopt;
        rt::Promise<PoolStep> serving(servePoolAccount(store, *choice.state, body, options, lifetime));
        const auto result = co_await serving.wait();
        if (result.kind == "response") co_return result.routed;
        if (result.kind == "paid") co_return std::nullopt;
      }
      co_return claudeLocalError(claude::ClaudeCodeError("Claude account selection did not settle.", "claude_code_upstream_unreachable", 409));
    }
  }
  rt::Task<std::optional<telemetry::RoutedResponsesUpstream>> attemptClaudeCodeSubscription(json::object body, SubscriptionOptions options) {
    const std::optional<telemetry::UsageContext> context = options.usageContext ? std::optional<telemetry::UsageContext>(*options.usageContext) : std::nullopt;
    options.usageContext = context ? &*context : nullptr;
    rt::Promise<std::optional<provider::selection::ProviderSelection>> selecting(provider::selection::loadProviderSelectionCached());
    const auto selection = co_await selecting.wait();
    if (!provider::surplus_claude_request::isSurplusClaudeModel(options.model) || !provider::selection::isProviderEnabled("claude_code", selection)) co_return std::nullopt;
    const auto getting = kv::getKv();
    const auto store = co_await getting.wait();
    if (!store) co_return claudeLocalError(claude::ClaudeCodeError("Claude account storage is unavailable.", "claude_code_upstream_unreachable", 503));
    rt::Promise<access::ClaudeCodeAccessPolicy> loading(access::loadClaudeCodeAccessPolicy(store, options.env));
    const auto policy = co_await loading.wait();
    const auto keyId = options.usageContext ? options.usageContext->keyId : std::nullopt;
    if (!keyId || keyId->empty() || std::find(policy.allowed_key_ids.begin(), policy.allowed_key_ids.end(), *keyId) == policy.allowed_key_ids.end()) co_return std::nullopt;
    rt::Promise<std::optional<telemetry::RoutedResponsesUpstream>> serving(runClaudePool(store, body, options, *keyId));
    const auto result = co_await serving.wait();
    co_return result;
  }
}
