// Port of src/paid-fallback/routing.ts.
#include "uos/paid_fallback/routing.hpp"
#include "uos/paid_fallback/health.hpp"
#include "uos/paid_fallback/claude_code_route.hpp"
#include "uos/codex/index.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/debug_routing.hpp"
#include "uos/http.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/openai.hpp"
#include "uos/request_policy.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/rt/json.hpp"
namespace uos::paid_fallback::routing {
namespace {
  namespace tel = openai_telemetry;
  namespace metered = provider::metered;
  namespace surplus = provider::surplus;
  using Options = FetchResponsesWithPaidFallbackOptions;
  using Reservation = index::PaidFallbackReservation;
  using Routed = tel::RoutedResponsesUpstream;
  using Telemetry = std::shared_ptr<tel::ResponseTelemetryState>;
  struct Responding { std::string provider; std::optional<std::string> requestId; };
  struct DispatchState { bool transportStarted = false; };
  struct Candidate {
    rt::Response response;
    std::optional<std::string> request_id;
    std::optional<tel::PromptCacheMode> prompt_cache_mode;
    std::optional<double> explicit_breakpoint_count;
  };
  struct PaidProviderAttemptResult { Candidate candidate; std::optional<tel::PaidProviderHealthClassification> classification; };
  struct AttemptsDelivered { Candidate result; std::string selectedProvider; std::optional<tel::PaidProviderHealthClassification> errorHealth; };
  struct AttemptsFailed { rt::Reason providerError; std::string selectedProvider; };
  using Attempts = std::variant<AttemptsDelivered, AttemptsFailed>;
  bool isDispatchError(const rt::Reason& error) { try { std::rethrow_exception(error); } catch (const api_key_policy::ApiKeyQuotaDispatchError&) { return true; } catch (...) { return false; } }
  rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>> beforeDispatch(const tel::UsageContext* context, std::string provider) {
    if (!context || !context->beforeProviderDispatch) co_return std::nullopt;
    auto task = context->beforeProviderDispatch(provider); rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> pending(std::move(task)); auto awaitedResult1 = co_await pending.wait(); co_return awaitedResult1;
  }
  rt::Task<rt::Response> dispatchCodexPrimaryResponse(json::object body, Options options, std::optional<rt::Response> directPaidPrimary) {
    auto config = co_await debug_routing::loadDebugRoutingConfig(); co_await rt::microtask();
    const auto forcedStatus = health::forcedDebugCodexStatus(config.scenario);
    if (directPaidPrimary) co_return *directPaidPrimary;
    if (forcedStatus) co_return http::openaiError(*forcedStatus, "Debug routing scenario forced Codex " + rt::numberToString(*forcedStatus) + ".", *forcedStatus == 429 ? "rate_limit_error" : "debug_forced_codex", {.headers = rt::Headers{{"x-uos-upstream", "chatgpt_codex"}, {"x-uos-debug-scenario", config.scenario}}});
    rt::Reason error; std::optional<rt::Response> response;
    try {
      codex::dispatch::FetchCodexResponsesOptions fetchOptions;
      fetchOptions.clientVersion = options.clientVersion.toOptional(); fetchOptions.cacheScope = health::codexCacheScopeForUsageContext(options.usageContext);
      fetchOptions.signal = options.signal; if (options.usageContext) fetchOptions.requestId = options.usageContext->requestId;
      fetchOptions.timing = codex::dispatch::CodexResponseTimingHooks{[context = options.usageContext] { tel::recordFirstCodexDispatch(context); }, [context = options.usageContext] { tel::recordFirstCodexHeaders(context); }};
      if (options.usageContext && options.usageContext->responseTelemetry) {
        fetchOptions.onFollowerStale = [telemetry = options.usageContext->responseTelemetry] { telemetry->upstreamAuthFollowerStale = true; };
      }
      fetchOptions.beforeDispatch = std::function<rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>>()>([context = options.usageContext] { return beforeDispatch(context, "chatgpt_codex"); });
      fetchOptions.bankedReset = openai::codexBankedResetOptionsForTest; if (options.usageContext) fetchOptions.sentinelUpstreamRecorder = options.usageContext->sentinelUpstreamRecorder;
      const json::value raw = body; auto task = codex::index::fetchCodexResponses(raw, fetchOptions); rt::Promise<rt::Response> pending(std::move(task)); response = co_await pending.wait();
    } catch (...) { error = std::current_exception(); }
    if (error) { const auto* codexError = codex::index::codexErrorOf(error); if (!codexError || codexError->status != 401) std::rethrow_exception(error); co_return http::openaiError(codexError->status, codexError->message, codexError->code); }
    co_return *response;
  }
  rt::Task<void> recordCodexPrimaryTelemetry(rt::Response primary, Telemetry telemetry) {
    if (!telemetry) co_return;
    telemetry->accountSlot = codex::index::getCodexResponseSlot(primary);
    auto task = codex::index::getCodexResponseAccountCohortId(primary); rt::Promise<std::optional<std::string>> pending(std::move(task)); telemetry->accountCohortId = co_await pending.wait();
    const auto active = codex::index::getCodexResponseActiveTelemetry(primary); telemetry->activeGeneration = active.activeGeneration;
    telemetry->activeTransitionReason = active.activeTransitionReason; telemetry->providerRequestId = upstream_wire::providerRequestIdFromResponse(primary);
  }
  std::optional<std::string> directPaidProviderFor(const health::PaidRoutingState& routing) {
    return (routing.meteredOnly || !routing.codexEnabled) && !routing.paidProviders.empty() ? std::optional(routing.paidProviders[0]) : std::nullopt;
  }
  std::optional<std::string> paidFallbackReasonFor(const std::optional<std::string>& direct, double status, const std::optional<std::string>& error) {
    if (direct) return "dynamic_paid_model";
    if (status == 429 && error == codex::index::CODEX_QUOTA_BLOCKED_ERROR_CODE) return "primary_quota_blocked";
    if ((status == 401 || status == 503) && error == codex::index::CODEX_SUBSCRIPTIONS_UNAVAILABLE_ERROR_CODE) return "primary_subscriptions_unavailable";
    return std::nullopt;
  }
  rt::Task<health::PaidResponsesCatalogs> replenishPaidFallbackCatalogs(health::PaidResponsesCatalogs catalogs, Options options, std::optional<rt::AbortSignal> signal) {
    if (catalogs.meteredCatalog && catalogs.surplusCatalog && (!catalogs.routing.paidProviders.empty() ||
      (!health::paidCatalogNeedsRefresh(catalogs.meteredCatalog, metered::METERED_MODELS_CACHE_TTL_MS) && !health::paidCatalogNeedsRefresh(catalogs.surplusCatalog, surplus::SURPLUS_MODELS_CACHE_TTL_MS)))) {
      if (!catalogs.routing.paidProviders.empty()) health::refreshStalePaidCatalogsInBackground(catalogs.meteredCatalog, catalogs.surplusCatalog);
      co_return catalogs;
    }
    auto task = health::refreshPaidCatalogs(catalogs.meteredCatalog, catalogs.surplusCatalog, signal); rt::Promise<health::PaidCatalogs> pending(std::move(task)); const auto refreshed = co_await pending.wait();
    catalogs.meteredCatalog = refreshed.first; catalogs.surplusCatalog = refreshed.second;
    health::PaidRoutingStateInput input{.meteredCatalog = catalogs.meteredCatalog, .surplusCatalog = catalogs.surplusCatalog, .codexModelKnown = catalogs.codexModelKnown, .endpointType = catalogs.endpointType, .requestUsesTools = catalogs.requestUsesTools, .model = options.model, .selection = catalogs.selection, .allowedPaidProviders = options.allowedPaidProviders, .deepSeekWaterfallPaidHop = options.deepSeekWaterfallPaidHop};
    catalogs.routing = health::resolvePaidRoutingState(input);
    if (!catalogs.routing.paidProviders.empty()) health::refreshStalePaidCatalogsInBackground(catalogs.meteredCatalog, catalogs.surplusCatalog);
    co_return catalogs;
  }
  rt::Task<void> bookkeeping(std::string operation, std::function<rt::Task<void>()> run) { auto task = health::bestEffortPaidFallbackBookkeeping(operation, run); rt::Promise<void> pending(std::move(task)); co_await pending.wait(); }
  rt::Task<void> cancelPaidFallbackPrefetchBeforeTransport(std::optional<rt::AbortSignal> signal, rt::Response primary, Reservation reservation) {
    if (signal && signal->aborted()) {
      upstream_wire::cancelResponseBody(primary);
      auto run = [reservation] { return index::recordMeteredPrefetchCancellation(reservation); };
      auto task = bookkeeping("prefetch cancellation recording", run); co_await std::move(task); std::rethrow_exception(health::paidFallbackAbortReason(signal));
    }
    upstream_wire::cancelResponseBody(primary);
    if (signal && signal->aborted()) {
      auto run = [reservation] { return index::recordMeteredPrefetchCancellation(reservation); };
      auto task = bookkeeping("prefetch cancellation recording", run); co_await std::move(task); std::rethrow_exception(health::paidFallbackAbortReason(signal));
    }
  }
  rt::Task<void> recordPaidFallbackProviderTransition(Reservation reservation, std::optional<Responding> responding,
    std::function<rt::Task<void>(Reservation)> recordUndispatched, std::string undispatched, std::string ambiguous) {
    if (!responding) {
      auto run = [reservation, recordUndispatched] { return recordUndispatched(reservation); };
      auto task = bookkeeping(undispatched, run); co_await std::move(task); co_return;
    }
    auto run = [reservation, responding] { return index::recordMeteredAmbiguousFailure(reservation, responding->provider, responding->requestId); };
    auto task = bookkeeping(ambiguous, run); co_await std::move(task);
  }
  void retainRespondingProviderTelemetry(const Telemetry& telemetry, const std::optional<Responding>& responding) {
    if (!telemetry || !responding) return;
    telemetry->provider = responding->provider; telemetry->providerRequestId = responding->requestId;
  }
  rt::Task<void> transition(Reservation reservation, std::optional<Responding> responding, bool prefetch, std::string undispatched, std::string ambiguous) {
    std::function<rt::Task<void>(Reservation)> recorder = prefetch ? index::recordMeteredPrefetchCancellation : index::recordMeteredUndispatchedCancellation;
    auto task = recordPaidFallbackProviderTransition(reservation, responding, recorder, undispatched, ambiguous); rt::Promise<void> pending(std::move(task)); co_await pending.wait();
  }
  struct AttemptFailure { bool retry; rt::Reason providerError; std::optional<Responding> responding; };
  rt::Task<AttemptFailure> resolvePaidProviderAttemptFailure(rt::Reason error, std::string provider, double providerIndex,
    std::vector<std::string> providers, DispatchState dispatchState, std::optional<Responding> responding, Reservation reservation,
    std::optional<rt::AbortSignal> signal, Telemetry telemetry) {
    if (isDispatchError(error)) {
      retainRespondingProviderTelemetry(telemetry, responding);
      const std::string undispatched = "pre-dispatch quota cancellation recording", ambiguous = "prior-provider quota rejection ambiguity recording";
      auto task = transition(reservation, responding, false, undispatched, ambiguous); co_await std::move(task); std::rethrow_exception(error);
    }
    if (signal && signal->aborted()) {
      const auto ambiguousProvider = dispatchState.transportStarted ? std::optional(Responding{provider, std::nullopt}) : responding;
      retainRespondingProviderTelemetry(telemetry, ambiguousProvider);
      const std::string undispatched = "pre-transport cancellation recording", ambiguous = "aborted transport ambiguity recording";
      auto task = transition(reservation, ambiguousProvider, false, undispatched, ambiguous); co_await std::move(task); std::rethrow_exception(health::paidFallbackAbortReason(signal));
    }
    const auto status = health::paidProviderErrorStatus(error);
    auto healthTask = health::recordPaidProviderResponseHealth(provider, status); rt::Promise<void> healthPending(std::move(healthTask)); co_await healthPending.wait();
    const auto attempted = dispatchState.transportStarted ? std::optional(Responding{provider, std::nullopt}) : std::nullopt;
    co_return AttemptFailure{!attempted && health::isIntermediatePaidProviderAttempt(providerIndex, static_cast<double>(providers.size()), status), error, attempted ? attempted : responding};
  }
  rt::Task<Candidate> fetchPaidProviderResponses(json::object body, Options options, std::optional<rt::AbortSignal> signal,
    std::string provider, std::optional<surplus::SurplusModelsSnapshot> surplusCatalog, std::shared_ptr<DispatchState> dispatchState) {
    const json::value raw = body;
    if (provider == "surplus") {
      surplus::SurplusAuthenticatedFetchOptions fetchOptions;
      fetchOptions.signal = signal; fetchOptions.reasoningReplayScope = health::codexCacheScopeForUsageContext(options.usageContext);
      fetchOptions.supportsParallelToolCalls = false;
      if (surplusCatalog) for (const auto& entry : surplusCatalog->models) if (entry.id == options.model) { fetchOptions.supportsParallelToolCalls = entry.supports_parallel_tool_calls == true; break; }
      fetchOptions.beforeDispatch = [context = options.usageContext] { return beforeDispatch(context, "surplus"); };
      fetchOptions.onDispatch = [dispatchState, context = options.usageContext] { dispatchState->transportStarted = true; tel::recordFirstProviderDispatch(context); };
      if (options.usageContext) fetchOptions.sentinelUpstreamRecorder = options.usageContext->sentinelUpstreamRecorder;
      auto task = surplus::fetchSurplusResponses(raw, fetchOptions); rt::Promise<surplus::SurplusResponsesResult> pending(std::move(task)); const auto result = co_await pending.wait();
      co_return Candidate{result.response, result.request_id, result.prompt_cache_mode, result.explicit_breakpoint_count};
    }
    metered::MeteredAuthenticatedFetchOptions fetchOptions; fetchOptions.signal = signal; fetchOptions.reasoningReplayScope = health::codexCacheScopeForUsageContext(options.usageContext);
    fetchOptions.beforeDispatch = [context = options.usageContext] { return beforeDispatch(context, "metered"); };
    fetchOptions.onDispatch = [dispatchState, context = options.usageContext] { dispatchState->transportStarted = true; tel::recordFirstProviderDispatch(context); };
    if (options.usageContext) fetchOptions.sentinelUpstreamRecorder = options.usageContext->sentinelUpstreamRecorder;
    auto task = metered::fetchMeteredResponses(raw, fetchOptions); rt::Promise<metered::MeteredResponsesResult> pending(std::move(task)); const auto result = co_await pending.wait();
    co_return Candidate{result.response, result.request_id, result.prompt_cache_mode, result.explicit_breakpoint_count};
  }
  rt::Task<PaidProviderAttemptResult> runSinglePaidProviderAttempt(json::object body, Options options, std::optional<rt::AbortSignal> signal,
    std::string provider, std::optional<surplus::SurplusModelsSnapshot> catalog, std::shared_ptr<DispatchState> state) {
    const auto deadline = inference_deadline::createPaidProviderAttemptDeadline(signal); auto clear = rt::scopeExit([deadline] { deadline.clear(); });
    const std::optional<rt::AbortSignal> attemptSignal = deadline.signal;
    auto fetchTask = fetchPaidProviderResponses(body, options, attemptSignal, provider, catalog, state); rt::Promise<Candidate> fetchPending(std::move(fetchTask)); const auto candidate = co_await fetchPending.wait();
    deadline.clear(); tel::recordFirstProviderHeaders(options.usageContext);
    bool meteredQuotaExhausted = false, surplusQuotaExhausted = false;
    if (provider == "metered") { auto task = health::meteredForbiddenIndicatesQuotaExhaustion(candidate.response, signal); rt::Promise<bool> pending(std::move(task)); meteredQuotaExhausted = co_await pending.wait(); }
    if (provider == "surplus") { auto task = health::surplus402IndicatesQuotaExhaustion(candidate.response, signal); rt::Promise<bool> pending(std::move(task)); surplusQuotaExhausted = co_await pending.wait(); }
    const auto classification = health::paidProviderHealthClassification(provider, candidate.response.status(), meteredQuotaExhausted, surplusQuotaExhausted);
    auto task = health::recordPaidProviderResponseHealth(provider, candidate.response.status(), candidate.request_id, meteredQuotaExhausted, surplusQuotaExhausted); rt::Promise<void> pending(std::move(task)); co_await pending.wait();
    co_return PaidProviderAttemptResult{candidate, classification};
  }
  rt::Task<Attempts> runPaidProviderAttempts(json::object body, Options options, std::optional<rt::AbortSignal> signal,
    Telemetry telemetry, Reservation reservation, std::vector<std::string> providers, std::optional<surplus::SurplusModelsSnapshot> catalog) {
    std::string selectedProvider = providers[0]; rt::Reason providerError; std::optional<Responding> responding;
    for (std::size_t i = 0; i < providers.size(); ++i) {
      if (signal && signal->aborted()) {
        retainRespondingProviderTelemetry(telemetry, responding);
        const std::string undispatched = "prefetch cancellation recording", ambiguous = "inter-provider cancellation ambiguity recording";
        auto task = transition(reservation, responding, true, undispatched, ambiguous); co_await std::move(task); std::rethrow_exception(health::paidFallbackAbortReason(signal));
      }
      const auto& provider = providers[i]; selectedProvider = provider;
      if (telemetry) { telemetry->provider = provider; telemetry->providerRequestId = std::nullopt; } tel::recordAttemptedProvider(options.usageContext, provider);
      const auto dispatchState = std::make_shared<DispatchState>(); std::optional<PaidProviderAttemptResult> attempt; rt::Reason error;
      try { auto task = runSinglePaidProviderAttempt(body, options, signal, provider, catalog, dispatchState); rt::Promise<PaidProviderAttemptResult> pending(std::move(task)); attempt = co_await pending.wait(); }
      catch (...) { error = std::current_exception(); }
      if (error) {
        auto task = resolvePaidProviderAttemptFailure(error, provider, static_cast<double>(i), providers, *dispatchState, responding, reservation, signal, telemetry); rt::Promise<AttemptFailure> pending(std::move(task)); const auto failure = co_await pending.wait();
        providerError = failure.providerError; responding = failure.responding; if (!failure.retry) break; continue;
      }
      if (health::isIntermediatePaidProviderAttempt(static_cast<double>(i), static_cast<double>(providers.size()), attempt->candidate.response.status())) {
        responding = Responding{provider, attempt->candidate.request_id ? upstream_wire::normalizeProviderRequestId(std::string_view(*attempt->candidate.request_id)) : std::nullopt}; upstream_wire::cancelResponseBody(attempt->candidate.response); continue;
      }
      const auto errorHealth = !attempt->candidate.response.ok() && attempt->classification && attempt->classification->event != "reachable" ? attempt->classification : std::nullopt;
      co_return AttemptsDelivered{attempt->candidate, selectedProvider, errorHealth};
    }
    if (providerError && isDispatchError(providerError)) std::rethrow_exception(providerError);
    const std::string undispatched = "undispatched failure recording", ambiguous = "ambiguous failure recording";
    auto task = transition(reservation, responding, false, undispatched, ambiguous); co_await std::move(task); co_await rt::microtask();
    co_return AttemptsFailed{providerError, selectedProvider};
  }
  Routed paidProviderFailureResponse(const AttemptsFailed& attempts, const Reservation& reservation, const std::optional<rt::AbortSignal>& signal, const std::string& reason) {
    const auto label = attempts.selectedProvider == "surplus" ? "Surplus" : "Metered";
    double status = 502; std::string message = std::string(label) + " upstream request failed before response headers were received.", code = "upstream_error";
    bool timeout = false;
    if (signal && signal->aborted() && signal->reason()) { try { std::rethrow_exception(signal->reason()); } catch (const rt::JsError& e) { timeout = e.name == "TimeoutError"; } catch (...) {} }
    if (attempts.providerError) try { std::rethrow_exception(attempts.providerError); }
    catch (const metered::MeteredError& e) { status = e.status; message = e.message; code = e.code; timeout = timeout || e.name == "TimeoutError"; }
    catch (const surplus::SurplusError& e) { status = e.status; message = e.message; code = e.code; timeout = timeout || e.name == "TimeoutError"; }
    catch (const rt::JsError& e) { timeout = timeout || e.name == "TimeoutError"; } catch (...) {}
    if (timeout) { status = 504; message = std::string(label) + " upstream exceeded the gateway deadline before response headers were received."; code = "gateway_timeout"; }
    return {.response = http::openaiError(status, message, code, {.type = "server_error", .headers = rt::Headers{{"x-uos-upstream", attempts.selectedProvider}}}), .provider = attempts.selectedProvider, .paidFallback = reservation, .gatewayResponse = true, .fallbackReason = reason};
  }
  rt::Task<Routed> deliverPaidProviderResponse(AttemptsDelivered attempts, Reservation reservation, Telemetry telemetry, std::optional<index::SurplusBillingPricing> billing, std::string reason) {
    const auto requestId = attempts.result.request_id ? upstream_wire::normalizeProviderRequestId(std::string_view(*attempts.result.request_id)) : std::nullopt;
    if (telemetry) { telemetry->providerRequestId = requestId; if (attempts.result.prompt_cache_mode) telemetry->promptCacheMode = *attempts.result.prompt_cache_mode; if (attempts.result.explicit_breakpoint_count) telemetry->explicitBreakpointCount = *attempts.result.explicit_breakpoint_count; }
    auto run = [reservation, attempts, requestId] { return index::recordMeteredUpstreamResponse(reservation, attempts.result.response, requestId, attempts.selectedProvider); };
    auto task = bookkeeping("upstream response recording", run); co_await std::move(task);
    if (attempts.selectedProvider != "surplus") billing = std::nullopt;
    if (billing && attempts.result.prompt_cache_mode) billing->require_provider_cost = true;
    co_return Routed{.response = attempts.result.response, .provider = attempts.selectedProvider, .paidFallback = reservation, .paidFallbackBilling = rt::orNull(billing), .paidFallbackProviderRequestId = rt::orNull(requestId), .paidFallbackErrorHealth = attempts.errorHealth, .gatewayResponse = false, .fallbackReason = reason};
  }
}
rt::Task<Routed> fetchResponsesWithPaidFallback(json::object body, Options options) {
  const auto signal = options.fallbackSignal ? options.fallbackSignal : options.signal;
  const auto telemetry = options.usageContext ? options.usageContext->responseTelemetry : nullptr;
  const auto selection = co_await provider::selection::loadProviderSelectionCached(); co_await rt::microtask();
  if (request_policy::isTemporaryFreeSurplusModel(options.model) && provider::selection::isProviderEnabled("surplus", selection)) {
    health::TemporaryFreeSurplusRoutedResponsesOptions freeOptions{.model = options.model, .usageContext = options.usageContext, .signal = options.signal};
    auto task = health::fetchTemporaryFreeSurplusRoutedResponses(body, freeOptions); auto awaitedResult2 = co_await std::move(task); co_return awaitedResult2;
  }
  claude_code_route::SubscriptionOptions subscriptionOptions{.model = options.model, .signal = options.signal, .usageContext = options.usageContext};
  auto subscriptionTask = claude_code_route::attemptClaudeCodeSubscription(body, subscriptionOptions); rt::Promise<std::optional<Routed>> subscriptionPending(std::move(subscriptionTask)); const auto subscription = co_await subscriptionPending.wait();
  if (subscription) co_return *subscription;
  health::LoadPaidResponsesCatalogsOptions catalogOptions{.model = options.model, .route = options.route, .signal = options.signal, .allowedPaidProviders = options.allowedPaidProviders, .deepSeekWaterfallPaidHop = options.deepSeekWaterfallPaidHop};
  auto catalogTask = health::loadPaidResponsesCatalogs(body, catalogOptions, selection); rt::Promise<health::PaidResponsesCatalogs> catalogPending(std::move(catalogTask)); auto catalogs = co_await catalogPending.wait();
  const auto unroutable = health::rejectUnroutablePaidModel(catalogs.codexModelKnown, catalogs.routing, catalogs.surplusCatalog, catalogs.endpointType, catalogs.requestUsesTools, options.usageContext, options.model, telemetry);
  if (unroutable) co_return *unroutable;
  const auto direct = directPaidProviderFor(catalogs.routing);
  const auto directPrimary = direct ? std::optional(http::openaiError(503, "Paid-provider routing did not reach the selected upstream.", "paid_provider_not_dispatched", {.type = "server_error"})) : std::nullopt;
  if (telemetry) { telemetry->provider = direct.value_or("chatgpt_codex"); } if (!direct) tel::recordAttemptedProvider(options.usageContext, "chatgpt_codex");
  if (!catalogs.routing.codexEnabled && !direct) co_return health::rejectDisabledCodexProvider(options.model, telemetry, options.usageContext);
  auto primaryTask = dispatchCodexPrimaryResponse(body, options, directPrimary); rt::Promise<rt::Response> primaryPending(std::move(primaryTask)); auto primary = co_await primaryPending.wait();
  const auto primaryStatus = primary.status(); const auto warnings = request_policy::responseWarnings(primary);
  const bool authReauthenticationPrimary = primaryStatus == 401 && rt::includes(warnings, std::string(codex::index::CODEX_AUTH_REAUTH_WARNING));
  auto telemetryTask = recordCodexPrimaryTelemetry(primary, telemetry); rt::Promise<void> telemetryPending(std::move(telemetryTask)); co_await telemetryPending.wait();
  const auto routingError = codex::index::getCodexRoutingError(primary);
  const bool gatewayResponse = direct.has_value() || routingError == codex::index::CODEX_QUOTA_BLOCKED_ERROR_CODE || (routingError == codex::index::CODEX_SUBSCRIPTIONS_UNAVAILABLE_ERROR_CODE && primaryStatus != 401) || routingError == codex::index::CODEX_UPSTREAM_DEGRADED_ERROR_CODE;
  if (authReauthenticationPrimary) primary = rt::Response(rt::bodyInit(primary.body()), {.status = 503, .statusText = primary.statusText(), .headers = primary.headers()});
  const auto reason = paidFallbackReasonFor(direct, primaryStatus, routingError); if (telemetry) telemetry->fallbackReason = reason;
  auto rejectAdmission = [direct, primary, gatewayResponse, reason, telemetry, context = options.usageContext, model = options.model](const std::string& errorReason, const std::string& logReason) -> Routed {
    if (direct) return health::rejectPaidAdmission(*direct, errorReason, telemetry, context, model, logReason);
    return {.response = primary, .provider = "chatgpt_codex", .paidFallback = std::nullopt, .gatewayResponse = gatewayResponse, .fallbackReason = reason};
  };
  const auto* context = options.usageContext;
  if (direct && context && context->paidFallbackEnabled == false) co_return rejectAdmission("disabled", "disabled");
  if (!reason || (context && context->paidFallbackEnabled == false) || !context || !context->keyId || context->keyId->empty() || !context->requestId || context->requestId->empty() || !context->startedAtMs) {
    if (direct) co_return rejectAdmission("invalid_policy", "invalid_context");
    co_return rejectAdmission("invalid_policy", "invalid_policy");
  }
  if (signal && signal->aborted()) { upstream_wire::cancelResponseBody(primary); std::rethrow_exception(health::paidFallbackAbortReason(signal)); }
  auto replenishTask = replenishPaidFallbackCatalogs(catalogs, options, signal); rt::Promise<health::PaidResponsesCatalogs> replenishPending(std::move(replenishTask)); catalogs = co_await replenishPending.wait();
  if (catalogs.routing.paidProviders.empty()) co_return Routed{.response = primary, .provider = "chatgpt_codex", .paidFallback = std::nullopt, .gatewayResponse = gatewayResponse, .fallbackReason = reason};
  index::ReservePaidFallbackInput input{.keyId = *context->keyId, .requestId = context->paidFallbackRequestId.value_or(*context->requestId), .createdAtMs = *context->startedAtMs, .model = options.model, .route = options.route, .path = options.route == "responses" ? "/v1/responses" : "/v1/chat/completions", .stream = options.stream, .reasoning = options.reasoning, .allowUnrosteredModel = catalogs.routing.meteredOnly, .reason = *reason};
  std::optional<index::PaidFallbackReservationDecision> decision; rt::Reason admissionError;
  try { auto task = index::reservePaidFallback(input); rt::Promise<index::PaidFallbackReservationDecision> pending(std::move(task)); decision = co_await pending.wait(); }
  catch (...) { admissionError = std::current_exception(); }
  if (admissionError) { health::warnPaidFallbackBookkeepingFailure("admission", admissionError); co_return rejectAdmission("invalid_policy", "admission_error"); }
  if (const auto* skipped = std::get_if<index::PaidFallbackReservationDecisionSkip>(&*decision)) co_return rejectAdmission(skipped->reason, skipped->reason);
  if (const auto* blocked = std::get_if<index::PaidFallbackReservationDecisionBlocked>(&*decision)) co_return rejectAdmission(blocked->reason, blocked->reason);
  const auto reservation = std::get<index::PaidFallbackReservationDecisionReserved>(*decision).reservation; co_await rt::microtask();
  auto cancelTask = cancelPaidFallbackPrefetchBeforeTransport(signal, primary, reservation); rt::Promise<void> cancelPending(std::move(cancelTask)); co_await cancelPending.wait();
  if (telemetry) { telemetry->provider = catalogs.routing.paidProviders[0]; telemetry->accountSlot = std::nullopt; telemetry->accountCohortId = std::nullopt; telemetry->activeGeneration = std::nullopt; telemetry->activeTransitionReason = std::nullopt; telemetry->providerRequestId = std::nullopt; telemetry->quotaUsedPercent = rt::orNull(reservation.quota_used_percent); }
  health::logPaidProviderSelected(*context->requestId, *reason, catalogs.routing.paidProviders[0]);
  auto attemptsTask = runPaidProviderAttempts(body, options, signal, telemetry, reservation, catalogs.routing.paidProviders, catalogs.surplusCatalog); rt::Promise<Attempts> attemptsPending(std::move(attemptsTask)); const auto attempts = co_await attemptsPending.wait();
  if (const auto* failed = std::get_if<AttemptsFailed>(&attempts)) co_return paidProviderFailureResponse(*failed, reservation, signal, *reason);
  auto deliveryTask = deliverPaidProviderResponse(std::get<AttemptsDelivered>(attempts), reservation, telemetry, catalogs.routing.surplusBilling, *reason); auto awaitedResult3 = co_await std::move(deliveryTask); co_return awaitedResult3;
}
}
