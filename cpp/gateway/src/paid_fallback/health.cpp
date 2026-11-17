// Paid-fallback health, catalogs and routing state, extracted from src/openai.ts.
#include "uos/paid_fallback/health.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/bounded_response_body.hpp"
#include "uos/deepseek/flash_aliases.hpp"
#include "uos/http.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/provider/health.hpp"
#include "uos/request_policy.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <memory>

namespace uos::paid_fallback::health {
  namespace {
    namespace telemetry = openai_telemetry;
    namespace metered = provider::metered;
    namespace surplus = provider::surplus;
    namespace selection = provider::selection;
    void logPaidProviderAdmissionRejected(const std::string& requestId, const std::string& model, const std::string& reason) {
      try {
        rt::console::warn("[ai.ubq.fi] paid_provider_admission_rejected", rt::jsonStringify(json::object{{"request_id", requestId}, {"model", model}, {"reason", reason}}));
      } catch (...) {}
    }
    rt::Response paidProviderAdmissionError(const std::string& reason) {
      if (reason == "disabled") return http::openaiError(403, "Paid-provider routing is disabled for this API key.", "paid_fallback_disabled");
      if (reason == "limit_exceeded") return http::openaiError(429, "The API key's paid-provider limit is exhausted.", "paid_fallback_limit_exceeded");
      if (reason == "model_not_priced") return http::openaiError(403, "This model is not admitted by the API key's paid-provider policy.", "paid_model_not_admitted");
      const http::OpenaiErrorOptions options{.type = "server_error"};
      if (reason == "provider_unconfigured") return http::openaiError(503, "No paid provider is configured for this model.", "paid_provider_unconfigured", options);
      if (reason == "reconciliation_pending") return http::openaiError(503, "Paid-provider billing reconciliation is pending.", "paid_fallback_reconciliation_pending", options);
      if (reason == "concurrent_update") return http::openaiError(503, "Paid-provider admission changed concurrently; retry the request.", "paid_fallback_concurrent_update", options);
      return http::openaiError(503, "Paid-provider admission is unavailable.", "paid_fallback_invalid_policy", options);
    }
    std::optional<std::string> paidProviderHealthEvent(const std::string& providerName, std::optional<double> status,
      bool meteredQuotaExhausted, bool surplusQuotaExhausted) {
      if (status == 401 || status == 403) return providerName == "metered" && meteredQuotaExhausted ? "quota_exhausted" : "auth_invalid";
      if (status == 402 || status == 429) return providerName == "surplus" && status == 402 && !surplusQuotaExhausted ? "upstream_error" : "quota_exhausted";
      if (!status || *status >= 500) return "upstream_error";
      if (*status >= 100) return "reachable";
      return std::nullopt;
    }
    std::optional<std::string> meteredTransportTerminalState(const std::string& eventType) {
      if (eventType == "response.completed") return "completed";
      if (eventType == "response.failed" || eventType == "error") return "failed";
      if (eventType == "response.incomplete") return "incomplete";
      return std::nullopt;
    }
    std::string paidLedgerProviderLabel(const std::string& providerName) { return providerName == "surplus" ? "surplus" : "metered"; }
    rt::Promise<void> recordProviderHealthForProvider(const std::string& providerName, const std::string& event,
      std::optional<double> status, const std::optional<std::string>& providerRequestId) {
      return providerName == "surplus" ? provider::health::recordSurplusProviderHealth(event, status, rt::dateNow, providerRequestId)
        : provider::health::recordMeteredProviderHealth(event, status, rt::dateNow, providerRequestId);
    }
    rt::Task<void> settleSurplusAfterTerminal(rt::Promise<void> terminal, index::PaidFallbackReservation reservation,
      std::optional<std::string> providerRequestId, std::string model, telemetry::UsageTokens usage, index::SurplusBillingPricing pricing) {
      co_await terminal.wait();
      index::SurplusUsage tokens{usage.inputTokens, usage.cachedInputTokens, usage.cacheWriteInputTokens, usage.outputTokens, usage.buyerCostMicro};
      const auto requestId = providerRequestId.value_or("surplus:" + reservation.request_id);
      rt::Promise<void> settling(index::recordSurplusUsage(reservation, requestId, model, tokens, pricing));
      co_await settling.wait();
    }
    rt::Task<void> reconcileMeteredTransportTerminal(index::PaidFallbackReservation reservation, std::string terminalState,
      std::string providerName, std::optional<std::string> providerRequestId, std::optional<index::SurplusBillingPricing> surplusBilling,
      std::optional<std::string> model, std::optional<telemetry::UsageTokens> usage,
      std::optional<telemetry::PaidProviderHealthClassification> deliveredHealth) {
      rt::Promise<void> terminal(index::recordMeteredTerminal(reservation, terminalState, paidLedgerProviderLabel(providerName)));
      rt::Promise<void> settlement = terminal;
      if (providerName == "surplus" && surplusBilling && model && !model->empty() && usage)
        settlement = rt::Promise<void>(settleSurplusAfterTerminal(terminal, reservation, providerRequestId, *model, *usage, *surplusBilling));
      // A delivered non-2xx paid response already carries its provider-specific classification.
      const auto healthEvent = terminalState == "completed" ? "success" : deliveredHealth ? deliveredHealth->event : "upstream_error";
      const std::optional<double> healthStatus = terminalState == "completed" ? std::optional<double>(200) : deliveredHealth ? deliveredHealth->status : std::nullopt;
      auto health = recordProviderHealthForProvider(providerName, healthEvent, healthStatus, providerRequestId);
      const auto complete = rt::all(settlement, health);
      co_await complete.wait();
    }
    rt::Task<void> recordMeteredTransportAmbiguity(index::PaidFallbackReservation reservation, std::string providerName,
      std::optional<std::string> providerRequestId, std::optional<telemetry::PaidProviderHealthClassification> deliveredHealth) {
      rt::Promise<void> ambiguous(index::recordMeteredAmbiguousFailure(reservation, paidLedgerProviderLabel(providerName), providerRequestId));
      auto health = recordProviderHealthForProvider(providerName, deliveredHealth ? deliveredHealth->event : "upstream_error",
        deliveredHealth ? deliveredHealth->status : std::nullopt, providerRequestId);
      const auto complete = rt::all(ambiguous, health);
      co_await complete.wait();
    }
    struct LifecycleState {
      std::optional<index::PaidFallbackReservation> reservation;
      std::string providerName;
      std::optional<std::string> providerRequestId;
      std::optional<index::SurplusBillingPricing> surplusBilling;
      std::optional<std::string> model;
      bool providerHealthOnly;
      std::optional<telemetry::PaidProviderHealthClassification> deliveredHealth;
      bool recorded = false;
    };
    void schedule(std::shared_ptr<LifecycleState> state, const std::string& operation, std::function<rt::Task<void>()> run) {
      if (!state->reservation || state->recorded) return;
      state->recorded = true;
      rt::spawn(bestEffortPaidFallbackBookkeeping(operation, std::move(run)));
    }
    void scheduleProviderHealthOnly(std::shared_ptr<LifecycleState> state, const std::string& event, std::optional<double> status) {
      if (state->reservation || !state->providerHealthOnly || state->recorded) return;
      if (state->providerName != "metered" && state->providerName != "surplus") return;
      state->recorded = true;
      const std::function<rt::Task<void>()> run = [state, event, status] {
        return recordProviderHealthForProvider(state->providerName, event, status, state->providerRequestId).wait();
      };
      rt::spawn(bestEffortPaidFallbackBookkeeping("unreserved provider health recording", run));
    }
    bool supports(const std::vector<std::string>& endpoints, const std::string& endpoint) {
      return std::find(endpoints.begin(), endpoints.end(), endpoint) != endpoints.end();
    }
    bool meteredSupports(const std::optional<metered::MeteredModelsSnapshot>& catalog, const std::string& model, const std::string& endpoint) {
      return catalog && std::any_of(catalog->models.begin(), catalog->models.end(), [&](const auto& entry) { return entry.id == model && supports(entry.supported_endpoint_types, endpoint); });
    }
    bool surplusSupports(const std::optional<surplus::SurplusModelsSnapshot>& catalog, const std::string& model, const std::string& endpoint) {
      return catalog && std::any_of(catalog->models.begin(), catalog->models.end(), [&](const auto& entry) { return entry.id == model && supports(entry.supported_endpoint_types, endpoint); });
    }
    rt::Response surplusPreHeaderFailureResponse(const rt::Reason& error, bool timedOut) {
      const http::OpenaiErrorOptions options{.type = "server_error", .headers = rt::Headers{{"x-uos-upstream", "surplus"}}};
      if (timedOut) return http::openaiError(504, "Surplus upstream exceeded the gateway deadline before response headers were received.", "gateway_timeout", options);
      try { if (error) std::rethrow_exception(error); } catch (const surplus::SurplusError& value) {
        return http::openaiError(value.status, value.message, value.code, options);
      } catch (...) {}
      return http::openaiError(502, "Surplus upstream request failed before response headers were received.", "upstream_error", options);
    }
    void recordTemporarySurplusMetadata(const std::shared_ptr<telemetry::ResponseTelemetryState>& state,
      const surplus::SurplusResponsesResult& result, const std::optional<std::string>& providerRequestId) {
      if (!state) return;
      state->providerRequestId = providerRequestId;
      if (result.prompt_cache_mode) state->promptCacheMode = *result.prompt_cache_mode;
      if (result.explicit_breakpoint_count) state->explicitBreakpointCount = *result.explicit_breakpoint_count;
    }
    rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>> temporaryBeforeDispatch(std::optional<telemetry::UsageContext> context) {
      if (!context || !context->beforeProviderDispatch) {
        co_await rt::microtask();
        co_return std::nullopt;
      }
      rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> dispatch(context->beforeProviderDispatch("surplus"));
      auto result = co_await dispatch.wait();
      co_return result;
    }
    std::optional<telemetry::RoutedResponsesUpstream> rejectToolUnverifiedSurplusModel(
      const std::optional<surplus::SurplusModelsSnapshot>& surplusCatalog, const std::optional<index::SurplusBillingPricing>& surplusBilling,
      bool requestUsesTools, const std::string& endpointType, const std::string& model,
      const telemetry::UsageContext* usageContext, const std::shared_ptr<telemetry::ResponseTelemetryState>& state) {
      const bool modelSupportsRoute = surplusSupports(surplusCatalog, model, endpointType);
      const bool lacksProof = surplusCatalog && std::any_of(surplusCatalog->models.begin(), surplusCatalog->models.end(), [&](const auto& entry) {
        return entry.id == model && entry.supports_tools != true;
      });
      if (!(requestUsesTools && modelSupportsRoute && surplus::readSurplusApiKey() && surplusBilling && lacksProof)) return std::nullopt;
      if (state) { state->provider = "gateway"; state->fallbackReason = "dynamic_paid_model"; }
      logPaidProviderAdmissionRejected(usageContext && usageContext->requestId ? *usageContext->requestId : "unknown", model, "tool_capability_unverified");
      const http::OpenaiErrorOptions options{.param = std::optional<std::string>("tools")};
      return telemetry::RoutedResponsesUpstream{.response = http::openaiError(400, "Model '" + model + "' does not support tool calling through the configured providers.", "model_tool_calling_unsupported", options),
        .provider = "surplus", .paidFallback = std::nullopt, .gatewayResponse = true, .fallbackReason = "dynamic_paid_model", .allowRemovedProviderRecovery = false};
    }
  }

  void warnPaidFallbackBookkeepingFailure(const std::string& operation, const rt::Reason& error) {
    rt::console::warn("[ai.ubq.fi] Paid fallback " + operation + " failed; leaving the reservation pending:", rt::isError(error) ? rt::errorMessage(error) : rt::reasonToString(error));
  }
  void logPaidProviderSelected(const std::string& requestId, const std::string& reason, const std::string& providerName) {
    try {
      if (providerName == "metered") rt::console::info("[ai.ubq.fi] metered_selected", rt::jsonStringify(json::object{{"request_id", requestId}, {"reason", reason}}));
      else rt::console::info("[ai.ubq.fi] paid_provider_selected", rt::jsonStringify(json::object{{"request_id", requestId}, {"reason", reason}, {"provider", providerName}}));
    } catch (...) {}
  }
  bool canAttemptPaidFallback(const telemetry::UsageContext* context, const std::optional<selection::ProviderSelection>& selected) {
    return context && context->paidFallbackEnabled == true && context->keyId && !context->keyId->empty() && context->requestId && !context->requestId->empty()
      && context->startedAtMs.has_value() && ((selection::isProviderEnabled("surplus", selected) && surplus::readSurplusApiKey())
        || (selection::isProviderEnabled("openlux", selected) && metered::readMeteredApiKey()));
  }
  rt::Task<void> bestEffortPaidFallbackBookkeeping(std::string operation, std::function<rt::Task<void>()> run) {
    try { rt::Promise<void> pending(run()); co_await pending.wait(); }
    catch (...) { warnPaidFallbackBookkeepingFailure(operation, std::current_exception()); }
  }
  std::optional<double> paidProviderErrorStatus(const rt::Reason& error) {
    try { if (error) std::rethrow_exception(error); }
    catch (const metered::MeteredError& value) { return value.status; }
    catch (const surplus::SurplusError& value) { return value.status; }
    catch (...) {}
    return std::nullopt;
  }
  std::optional<telemetry::PaidProviderHealthClassification> paidProviderHealthClassification(const std::string& providerName,
    std::optional<double> status, bool meteredQuotaExhausted, bool surplusQuotaExhausted) {
    const auto event = paidProviderHealthEvent(providerName, status, meteredQuotaExhausted, surplusQuotaExhausted);
    if (!event) return std::nullopt;
    return telemetry::PaidProviderHealthClassification{*event, status};
  }
  rt::Task<bool> meteredForbiddenIndicatesQuotaExhaustion(rt::Response response, std::optional<rt::AbortSignal> signal) {
    if (response.status() != 403) co_return false;
    bounded_response_body::ReadBoundedResponseBodyOptions options{signal, std::nullopt, std::nullopt, "Metered 403 classification read"};
    rt::Promise<bounded_response_body::BoundedResponseBody> read(bounded_response_body::readBoundedResponseBody(response.clone(), options));
    const auto result = co_await read.wait();
    if (!result.complete) co_return false;
    try {
      const auto payload = rt::jsonParse(rt::decodeUtf8(result.bytes));
      if (!utils::isRecord(payload)) co_return false;
      const auto* error = rt::get(payload, "error");
      if (!utils::isRecord(error)) co_return false;
      const auto errorCode = rt::getString(*error, "code");
      const auto message = rt::getString(*error, "message");
      co_return (errorCode && rt::toLowerCase(rt::trim(*errorCode)) == "local:insufficient_quota")
        || (message && rt::includes(rt::toLowerCase(*message), "quota is not enough"));
    } catch (...) { }
    co_return false;
  }
  rt::Task<bool> surplus402IndicatesQuotaExhaustion(rt::Response response, std::optional<rt::AbortSignal> signal) {
    if (response.status() != 402) co_return false;
    bounded_response_body::ReadBoundedResponseBodyOptions options{signal, std::nullopt, std::nullopt, "Surplus 402 classification read"};
    rt::Promise<bounded_response_body::BoundedResponseBody> read(bounded_response_body::readBoundedResponseBody(response.clone(), options));
    const auto result = co_await read.wait();
    if (!result.complete) co_return false;
    try {
      const auto payload = rt::jsonParse(rt::decodeUtf8(result.bytes));
      if (!utils::isRecord(payload)) co_return false;
      const auto* nested = rt::get(payload, "error");
      const auto& error = utils::isRecord(nested) ? *nested : payload;
      const auto errorCode = rt::getString(error, "code");
      const auto type = rt::getString(error, "type");
      co_return (errorCode && rt::toLowerCase(rt::trim(*errorCode)) == "insufficient_credit")
        || (type && rt::toLowerCase(rt::trim(*type)) == "insufficient_credit");
    } catch (...) { }
    co_return false;
  }
  rt::Task<void> recordPaidProviderResponseHealth(std::string providerName, std::optional<double> status,
    std::optional<std::string> providerRequestId, bool meteredQuotaExhausted, bool surplusQuotaExhausted) {
    try {
      const auto classification = paidProviderHealthClassification(providerName, status, meteredQuotaExhausted, surplusQuotaExhausted);
      if (!classification) co_return;
      auto recording = recordProviderHealthForProvider(providerName, classification->event, status, providerRequestId);
      co_await recording.wait();
    } catch (...) {}
  }
  telemetry::MeteredTransportLifecycle createMeteredTransportLifecycle(std::optional<index::PaidFallbackReservation> reservation,
    std::string providerName, std::optional<std::string> providerRequestId, std::optional<index::SurplusBillingPricing> surplusBilling,
    std::optional<std::string> model, bool providerHealthOnly, std::optional<telemetry::PaidProviderHealthClassification> deliveredHealth) {
    const auto state = std::make_shared<LifecycleState>(LifecycleState{reservation, providerName, providerRequestId, surplusBilling, model, providerHealthOnly, deliveredHealth});
    return {
      .terminal = [state](std::string eventType, std::optional<telemetry::UsageTokens> usage) {
        const auto terminal = meteredTransportTerminalState(eventType);
        if (!terminal) return;
        if (!state->reservation) { const bool completed = *terminal == "completed"; scheduleProviderHealthOnly(state, completed ? "success" : "upstream_error", completed ? std::optional<double>(200) : std::nullopt); return; }
        const std::function<rt::Task<void>()> run = [state, terminal, usage] { return reconcileMeteredTransportTerminal(*state->reservation, *terminal, state->providerName, state->providerRequestId, state->surplusBilling, state->model, usage, state->deliveredHealth); };
        schedule(state, "terminal reconciliation", run);
      },
      .ambiguous = [state] {
        if (!state->reservation) { scheduleProviderHealthOnly(state, "upstream_error", std::nullopt); return; }
        const std::function<rt::Task<void>()> run = [state] { return recordMeteredTransportAmbiguity(*state->reservation, state->providerName, state->providerRequestId, state->deliveredHealth); };
        schedule(state, "ambiguous failure recording", run);
      },
      .cancelled = [state] {
        if (!state->reservation && state->providerHealthOnly) { state->recorded = true; return; }
        const std::function<rt::Task<void>()> run = [state] { return index::recordMeteredTerminal(*state->reservation, "cancelled", paidLedgerProviderLabel(state->providerName)); };
        schedule(state, "dispatched cancellation recording", run);
      }
    };
  }
  bool paidCatalogNeedsRefresh(const std::optional<metered::MeteredModelsSnapshot>& catalog, double ttlMs) { return !catalog || rt::dateNow() - catalog->updated_at_ms >= ttlMs; }
  bool paidCatalogNeedsRefresh(const std::optional<surplus::SurplusModelsSnapshot>& catalog, double ttlMs) { return !catalog || rt::dateNow() - catalog->updated_at_ms >= ttlMs; }
  rt::Task<PaidCatalogs> refreshPaidCatalogs(std::optional<metered::MeteredModelsSnapshot> meteredCatalog,
    std::optional<surplus::SurplusModelsSnapshot> surplusCatalog, std::optional<rt::AbortSignal> signal) {
    metered::FetchMeteredModelsOptions mOptions; mOptions.signal = signal;
    surplus::FetchSurplusModelsOptions sOptions; sOptions.signal = signal;
    rt::Promise<std::optional<metered::MeteredModelsSnapshot>> m = paidCatalogNeedsRefresh(meteredCatalog, metered::METERED_MODELS_CACHE_TTL_MS)
      ? rt::Promise<std::optional<metered::MeteredModelsSnapshot>>(metered::fetchMeteredModels(mOptions)) : rt::Promise<std::optional<metered::MeteredModelsSnapshot>>::resolve(meteredCatalog);
    rt::Promise<std::optional<surplus::SurplusModelsSnapshot>> s = paidCatalogNeedsRefresh(surplusCatalog, surplus::SURPLUS_MODELS_CACHE_TTL_MS)
      ? rt::Promise<std::optional<surplus::SurplusModelsSnapshot>>(surplus::fetchSurplusModels(sOptions)) : rt::Promise<std::optional<surplus::SurplusModelsSnapshot>>::resolve(surplusCatalog);
    const auto pending = rt::all(m, s);
    auto result = co_await pending.wait();
    co_return PaidCatalogs{std::get<0>(result), std::get<1>(result)};
  }
  void refreshStalePaidCatalogsInBackground(const std::optional<metered::MeteredModelsSnapshot>& meteredCatalog,
    const std::optional<surplus::SurplusModelsSnapshot>& surplusCatalog) {
    if (meteredCatalog && paidCatalogNeedsRefresh(meteredCatalog, metered::METERED_MODELS_CACHE_TTL_MS)) rt::spawn(metered::fetchMeteredModels(), rt::ignoreRejection);
    if (surplusCatalog && paidCatalogNeedsRefresh(surplusCatalog, surplus::SURPLUS_MODELS_CACHE_TTL_MS)) rt::spawn(surplus::fetchSurplusModels(), rt::ignoreRejection);
  }
  PaidRoutingState resolvePaidRoutingState(const PaidRoutingStateInput& input) {
    const auto pinned = input.deepSeekWaterfallPaidHop == "openlux" ? "metered" : "surplus";
    const auto hop = deepseek::flash_aliases::isDeepSeekFlashPaidModel(input.model) && input.endpointType == "openai-response"
      && selection::isProviderEnabled("ubiquity", input.selection) && input.allowedPaidProviders && input.allowedPaidProviders->size() == 1
      && input.allowedPaidProviders->front() == pinned ? input.deepSeekWaterfallPaidHop : std::nullopt;
    // The exact synthetic OpenLux hop retains verified Chat capability evidence for native Responses.
    const bool mSupports = (hop == "openlux" && meteredSupports(input.meteredCatalog, input.model, "openai"))
      || meteredSupports(input.meteredCatalog, input.model, input.endpointType);
    const bool sSupports = surplusSupports(input.surplusCatalog, input.model, input.endpointType);
    const surplus::SurplusModel* model = nullptr;
    if (input.surplusCatalog) for (const auto& entry : input.surplusCatalog->models) if (entry.id == input.model) { model = &entry; break; }
    std::optional<index::SurplusBillingPricing> billing;
    if (model && model->input_price_per_token && rt::Number::isFinite(*model->input_price_per_token) && *model->input_price_per_token >= 0
      && model->output_price_per_token && rt::Number::isFinite(*model->output_price_per_token) && *model->output_price_per_token >= 0)
      billing = index::SurplusBillingPricing{*model->input_price_per_token, *model->output_price_per_token, model->cache_read_price_per_token, model->cache_write_price_per_token};
    const bool codexEnabled = selection::isProviderEnabled("codex", input.selection);
    const bool mCan = (selection::isProviderEnabled("openlux", input.selection) || hop == "openlux") && metered::readMeteredApiKey()
      && (!input.meteredCatalog ? input.codexModelKnown : mSupports);
    const bool sCan = (selection::isProviderEnabled("surplus", input.selection) || hop == "surplus")
      && (!input.requestUsesTools || (model && model->supports_tools == true)) && surplus::readSurplusApiKey() && sSupports && billing;
    std::vector<std::string> providers;
    for (const auto& providerName : {std::string("surplus"), std::string("metered")})
      if ((providerName == "surplus" ? sCan : mCan) && (!input.allowedPaidProviders || supports(*input.allowedPaidProviders, providerName))) providers.push_back(providerName);
    return {billing, providers, mSupports || sSupports, !providers.empty() && !input.codexModelKnown, codexEnabled};
  }
  bool isIntermediatePaidProviderAttempt(double providerIndex, double providerCount, std::optional<double> status) {
    return providerIndex < providerCount - 1 && (status == 402 || status == 429);
  }
  rt::Reason paidFallbackAbortReason(const std::optional<rt::AbortSignal>& fallbackSignal) {
    if (fallbackSignal && rt::isError(fallbackSignal->reason())) return fallbackSignal->reason();
    return rt::makeReason(rt::DOMException("The request was aborted.", "AbortError"));
  }
  rt::Task<telemetry::RoutedResponsesUpstream> fetchTemporaryFreeSurplusRoutedResponses(json::object body, TemporaryFreeSurplusRoutedResponsesOptions options) {
    const std::optional<telemetry::UsageContext> context = options.usageContext ? std::optional<telemetry::UsageContext>(*options.usageContext) : std::nullopt;
    const auto* usageContext = context ? &*context : nullptr;
    const auto state = usageContext ? usageContext->responseTelemetry : nullptr;
    if (state) {
      state->provider = "surplus"; state->fallbackReason.reset(); state->accountSlot.reset(); state->accountCohortId.reset();
      state->activeGeneration.reset(); state->activeTransitionReason.reset(); state->providerRequestId.reset(); state->quotaUsedPercent = nullptr;
    }
    telemetry::recordAttemptedProvider(usageContext, "surplus");
    const auto transportStarted = std::make_shared<bool>(false);
    rt::Reason failure;
    try {
      surplus::SurplusAuthenticatedFetchOptions fetchOptions;
      fetchOptions.signal = options.signal;
      fetchOptions.reasoningReplayScope = codexCacheScopeForUsageContext(usageContext);
      fetchOptions.beforeDispatch = [context] { return temporaryBeforeDispatch(context); };
      fetchOptions.onDispatch = [context, transportStarted] { *transportStarted = true; telemetry::recordFirstProviderDispatch(context ? &*context : nullptr); };
      if (context) fetchOptions.sentinelUpstreamRecorder = context->sentinelUpstreamRecorder;
      rt::Promise<surplus::SurplusResponsesResult> pending(surplus::fetchSurplusResponses(body, fetchOptions));
      const auto result = co_await pending.wait();
      telemetry::recordFirstProviderHeaders(usageContext);
      const auto providerRequestId = result.request_id ? upstream_wire::normalizeProviderRequestId(std::string_view(*result.request_id)) : std::nullopt;
      recordTemporarySurplusMetadata(state, result, providerRequestId);
      rt::Promise<bool> classifying(surplus402IndicatesQuotaExhaustion(result.response, options.signal));
      const bool exhausted = co_await classifying.wait();
      rt::Promise<void> recording(recordPaidProviderResponseHealth("surplus", result.response.status(), providerRequestId, false, exhausted));
      co_await recording.wait();
      co_return telemetry::RoutedResponsesUpstream{.response = result.response, .provider = "surplus", .paidFallback = std::nullopt,
        .paidFallbackProviderRequestId = rt::orNull(providerRequestId), .gatewayResponse = false, .fallbackReason = std::nullopt, .providerHealthOnly = result.response.ok()};
    } catch (const api_key_policy::ApiKeyQuotaDispatchError&) { throw; }
    catch (...) { failure = std::current_exception(); }
    const bool timedOut = telemetry::isTimeoutFailure(failure, options.signal ? options.signal->reason() : rt::Reason{});
    if (options.signal && options.signal->aborted() && !timedOut) std::rethrow_exception(failure);
    const auto status = paidProviderErrorStatus(failure);
    if (*transportStarted) { rt::Promise<void> recording(recordPaidProviderResponseHealth("surplus", status)); co_await recording.wait(); }
    if (options.signal && options.signal->aborted()) std::rethrow_exception(failure);
    co_return telemetry::RoutedResponsesUpstream{.response = surplusPreHeaderFailureResponse(failure, timedOut), .provider = "surplus",
      .paidFallback = std::nullopt, .gatewayResponse = true, .fallbackReason = std::nullopt};
  }
  rt::Task<PaidResponsesCatalogs> loadPaidResponsesCatalogs(json::object body, LoadPaidResponsesCatalogsOptions options,
    std::optional<selection::ProviderSelection> selected) {
    rt::Promise<std::optional<models::codex_models::CodexModelsSnapshot>> loading(models::codex_models_fetch::loadCodexModelsSnapshot());
    const auto codexCatalog = co_await loading.wait();
    bool codexModelKnown = request_policy::isAdditionalTrustedCodexModel(options.model);
    if (codexCatalog) if (const auto* entries = rt::getArray(*codexCatalog, "models")) for (const auto& entry : *entries) {
      auto id = utils::getString(rt::get(entry, "slug"));
      if (!id) id = utils::getString(rt::get(entry, "id"));
      if (!id) id = utils::getString(rt::get(entry, "model"));
      if (!id) id = utils::getString(rt::get(entry, "name"));
      if (id == options.model) codexModelKnown = true;
    }
    const std::string endpointType = options.route == "responses" ? "openai-response" : "openai";
    const auto* tools = rt::getArray(body, "tools");
    const bool requestUsesTools = tools && !tools->empty();
    const bool codexEnabled = selection::isProviderEnabled("codex", selected);
    metered::FetchMeteredModelsOptions mOptions; mOptions.cachedOnly = true;
    surplus::FetchSurplusModelsOptions sOptions; sOptions.cachedOnly = true;
    rt::Promise<std::optional<metered::MeteredModelsSnapshot>> m(metered::fetchMeteredModels(mOptions));
    rt::Promise<std::optional<surplus::SurplusModelsSnapshot>> s(surplus::fetchSurplusModels(sOptions));
    auto pending = rt::all(m, s);
    auto cached = co_await pending.wait();
    auto meteredCatalog = std::get<0>(cached);
    auto surplusCatalog = std::get<1>(cached);
    if (!codexModelKnown && (paidCatalogNeedsRefresh(meteredCatalog, metered::METERED_MODELS_CACHE_TTL_MS) || paidCatalogNeedsRefresh(surplusCatalog, surplus::SURPLUS_MODELS_CACHE_TTL_MS))) {
      rt::Promise<PaidCatalogs> refreshing(refreshPaidCatalogs(meteredCatalog, surplusCatalog, options.signal));
      const auto fresh = co_await refreshing.wait();
      meteredCatalog = fresh.first; surplusCatalog = fresh.second;
    }
    auto resolveRouting = [&] { return resolvePaidRoutingState({meteredCatalog, surplusCatalog, codexModelKnown, endpointType, requestUsesTools, options.model, selected, options.allowedPaidProviders, options.deepSeekWaterfallPaidHop}); };
    auto routing = resolveRouting();
    if (codexModelKnown && !codexEnabled && routing.paidProviders.empty()) {
      mOptions.cachedOnly.reset(); mOptions.signal = options.signal;
      sOptions.cachedOnly.reset(); sOptions.signal = options.signal;
      m = paidCatalogNeedsRefresh(meteredCatalog, metered::METERED_MODELS_CACHE_TTL_MS) && selection::isProviderEnabled("openlux", selected)
        ? rt::Promise<std::optional<metered::MeteredModelsSnapshot>>(metered::fetchMeteredModels(mOptions)) : rt::Promise<std::optional<metered::MeteredModelsSnapshot>>::resolve(meteredCatalog);
      s = paidCatalogNeedsRefresh(surplusCatalog, surplus::SURPLUS_MODELS_CACHE_TTL_MS) && selection::isProviderEnabled("surplus", selected)
        ? rt::Promise<std::optional<surplus::SurplusModelsSnapshot>>(surplus::fetchSurplusModels(sOptions)) : rt::Promise<std::optional<surplus::SurplusModelsSnapshot>>::resolve(surplusCatalog);
      pending = rt::all(m, s);
      const auto fresh = co_await pending.wait();
      meteredCatalog = std::get<0>(fresh); surplusCatalog = std::get<1>(fresh); routing = resolveRouting();
    }
    if (!routing.paidProviders.empty()) refreshStalePaidCatalogsInBackground(meteredCatalog, surplusCatalog);
    co_return PaidResponsesCatalogs{codexModelKnown, endpointType, requestUsesTools, meteredCatalog, surplusCatalog, selected, routing};
  }
  telemetry::RoutedResponsesUpstream rejectPaidAdmission(const std::string& providerName, const std::string& errorReason,
    const std::shared_ptr<telemetry::ResponseTelemetryState>& state, const telemetry::UsageContext* usageContext, const std::string& model) {
    return rejectPaidAdmission(providerName, errorReason, state, usageContext, model, errorReason);
  }
  telemetry::RoutedResponsesUpstream rejectPaidAdmission(const std::string& providerName, const std::string& errorReason,
    const std::shared_ptr<telemetry::ResponseTelemetryState>& state, const telemetry::UsageContext* usageContext, const std::string& model, const std::string& logReason) {
    if (state) { state->provider = "gateway"; state->fallbackReason = "dynamic_paid_model"; }
    logPaidProviderAdmissionRejected(usageContext && usageContext->requestId ? *usageContext->requestId : "unknown", model, logReason);
    return {.response = paidProviderAdmissionError(errorReason), .provider = providerName, .paidFallback = std::nullopt,
      .gatewayResponse = true, .fallbackReason = "dynamic_paid_model", .allowRemovedProviderRecovery = false};
  }
  telemetry::RoutedResponsesUpstream rejectDisabledCodexProvider(const std::string& model,
    const std::shared_ptr<telemetry::ResponseTelemetryState>& state, const telemetry::UsageContext* usageContext) {
    if (state) { state->provider = "gateway"; state->fallbackReason.reset(); }
    logPaidProviderAdmissionRejected(usageContext && usageContext->requestId ? *usageContext->requestId : "unknown", model, "codex_provider_disabled");
    const http::OpenaiErrorOptions options{.type = "server_error"};
    return {.response = http::openaiError(503, "The Codex provider is switched off and no enabled paid provider serves this model.", "provider_disabled", options),
      .provider = "chatgpt_codex", .paidFallback = std::nullopt, .gatewayResponse = true, .fallbackReason = std::nullopt, .allowRemovedProviderRecovery = false};
  }
  std::optional<telemetry::RoutedResponsesUpstream> rejectUnroutablePaidModel(bool codexModelKnown, const PaidRoutingState& routing,
    const std::optional<surplus::SurplusModelsSnapshot>& surplusCatalog, const std::string& endpointType, bool requestUsesTools,
    const telemetry::UsageContext* usageContext, const std::string& model, const std::shared_ptr<telemetry::ResponseTelemetryState>& state) {
    if (codexModelKnown || !routing.paidModelKnown || !routing.paidProviders.empty()) return std::nullopt;
    const std::string catalogProvider = surplusSupports(surplusCatalog, model, endpointType) ? "surplus" : "metered";
    if (usageContext && usageContext->paidFallbackEnabled == false) return rejectPaidAdmission(catalogProvider, "disabled", state, usageContext, model);
    const auto toolUnverified = rejectToolUnverifiedSurplusModel(surplusCatalog, routing.surplusBilling, requestUsesTools, endpointType, model, usageContext, state);
    if (toolUnverified) return toolUnverified;
    return rejectPaidAdmission(catalogProvider, "provider_unconfigured", state, usageContext, model);
  }
  std::optional<double> forcedDebugCodexStatus(const std::string& scenario) {
    if (scenario == "metered_first" || scenario == "codex_429") return 429;
    if (scenario == "codex_403") return 403;
    if (scenario == "codex_401") return 401;
    if (scenario == "codex_503") return 503;
    return std::nullopt;
  }
  std::optional<std::string> codexCacheScopeForUsageContext(const telemetry::UsageContext* usageContext) {
    if (usageContext && usageContext->idempotencyPrincipal.hasValue() && !usageContext->idempotencyPrincipal->empty()) return *usageContext->idempotencyPrincipal;
    if (usageContext && usageContext->keyId && !usageContext->keyId->empty()) return usageContext->keyId;
    return std::nullopt;
  }
}
