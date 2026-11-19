// Port of src/responses-handler.ts.
#include "uos/responses_handler.hpp"
#include "uos/responses_native_headers.hpp"
#include "uos/responses_buffered.hpp"
#include "uos/provider/lithos_handlers.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/gemini.hpp"
#include "uos/provider/cerebras_responses.hpp"
#include "uos/provider/openrouter_handlers.hpp"
#include "uos/provider/openrouter_request_controls.hpp"
#include "uos/deepseek/handlers.hpp"
#include "uos/deepseek/waterfall_handler.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/codex/index.hpp"
#include "uos/paid_fallback/removed_provider.hpp"
#include "uos/paid_fallback/health.hpp"
#include "uos/chat/stream_translation.hpp"
#include "uos/debug_routing.hpp"
#include "uos/openai.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/json.hpp"
namespace uos::responses_handler {
namespace {
  namespace tel = openai_telemetry;
  namespace reqs = responses_request;
  namespace attempts = responses_attempts;
  namespace circuit = provider::removed_provider_circuit;
  namespace removed = paid_fallback::removed_provider;
  namespace fs = responses_failover_stream;
  namespace rs = responses_stream;
  using State = std::shared_ptr<reqs::ResponsesHandlerState>;
  using Delivery = std::shared_ptr<reqs::ResponsesDeliveryState>;
  const tel::UsageContext* contextFor(const State& state) { return state->usageContext ? &*state->usageContext : nullptr; }
  const tel::UsageContext* contextFor(const Delivery& delivery) { return delivery->usageContext ? &*delivery->usageContext : nullptr; }
  std::shared_ptr<tel::ResponseTelemetryState> telemetryFor(const State& state) { const auto* context = contextFor(state); return context ? context->responseTelemetry : nullptr; }
  std::shared_ptr<tel::ResponseTelemetryState> telemetryFor(const Delivery& delivery) { const auto* context = contextFor(delivery); return context ? context->responseTelemetry : nullptr; }
  rt::Task<reqs::ResponsesRoutingState> prepareResponsesRouting(rt::Request req, reqs::ResponsesRequestState state, std::string model) {
    const auto* context = state.usageContext ? &*state.usageContext : nullptr;
    const auto downstream = openai::downstreamSignalFor(req, context); const auto signal = state.clientWantsStream ? downstream : openai::inferenceSignal(req, context);
    const auto deadline = inference_deadline::createStreamFirstEventDeadline(signal);
    const auto apiKey = request_policy::isTemporaryFreeSurplusModel(model) ? std::nullopt : removed::readRemovedProviderApiKey();
    const auto selection = co_await provider::selection::loadProviderSelectionCached(); co_await rt::microtask();
    const bool paidAvailable = paid_fallback::health::canAttemptPaidFallback(context, selection);
    const auto debug = co_await debug_routing::loadDebugRoutingConfig(); co_await rt::microtask();
    std::optional<circuit::RemovedProviderCircuitRoute> selected;
    if (apiKey && !apiKey->empty()) { selected = co_await circuit::selectRemovedProviderCircuitRoute().wait(); recordRemovedProviderCircuitTransition(context, selected->transition); }
    std::optional<std::string> sessionId;
    if (apiKey && !apiKey->empty()) (void)co_await removed::deriveRemovedProviderSessionId().wait();
    const auto route = debug.scenario == "removed_provider_first" && apiKey && !apiKey->empty() ? "removed_provider" : selected ? selected->route : "codex";
    co_return reqs::ResponsesRoutingState{downstream, signal, deadline, apiKey, paidAvailable, sessionId, route, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, 0};
  }
  rt::Task<reqs::ResponsesStep<State>> prepareResponsesRequest(rt::Request req, json::object rawRecord, ::uos::types::ResponsesRequest rawBody,
    const tel::UsageContext* context, ResponsesTailOptions options) {
    const auto settingsResult = reqs::resolveResponsesStreamSettings(rawRecord, rawBody);
    if (const auto* error = std::get_if<reqs::ResponsesStepNotOk>(&settingsResult)) co_return *error;
    const auto settings = std::get<reqs::ResponsesStepOk<reqs::ResponsesStreamSettings>>(settingsResult).value;
    const reqs::ResolveResponsesModelOptions modelOptions{options.allowedPaidProviders, options.deepSeekWaterfallPaidHop};
    auto modelTask = reqs::resolveResponsesModel(rawRecord, context, modelOptions); rt::Promise<reqs::ResponsesStep<reqs::ResponsesModel>> modelPending(std::move(modelTask)); const auto modelResult = co_await modelPending.wait();
    if (const auto* error = std::get_if<reqs::ResponsesStepNotOk>(&modelResult)) co_return *error;
    const auto model = std::get<reqs::ResponsesStepOk<reqs::ResponsesModel>>(modelResult).value;
    const auto inputResult = reqs::normalizeResponsesInput(rt::get(rawBody, "input"));
    if (const auto* error = std::get_if<reqs::ResponsesStepNotOk>(&inputResult)) co_return *error;
    const auto input = std::get<reqs::ResponsesStepOk<std::vector<::uos::types::ResponseInputItem>>>(inputResult).value;
    const auto cacheError = request_policy::validateKnownUnsupportedPromptCacheUse(model.modelRaw, model.modelMetadata, rawRecord, input, "input"); if (cacheError) co_return reqs::ResponsesStepNotOk{*cacheError};
    auto reasoningTask = reqs::resolveResponsesReasoning(rawRecord, rawBody, model.modelMetadata, context); rt::Promise<reqs::ResponsesStep<reqs::ResponsesReasoning>> reasoningPending(std::move(reasoningTask)); const auto reasoningResult = co_await reasoningPending.wait();
    if (const auto* error = std::get_if<reqs::ResponsesStepNotOk>(&reasoningResult)) co_return *error;
    const auto reasoning = std::get<reqs::ResponsesStepOk<reqs::ResponsesReasoning>>(reasoningResult).value;
    const auto bodies = reqs::buildResponsesUpstreamBodies(model.model, input, reasoning.instructions, reasoning.reasoning, rawRecord);
    const tel::RecordRequestUsageDetails usage{.model = model.modelRaw, .route = "responses", .stream = settings.clientWantsStream, .reasoning = reasoning.reasoningLabel,
      .promptCacheKeyPresent = tel::promptCacheKeyPresent(rawRecord), .promptCacheMode = tel::promptCacheModeFor(rawRecord), .explicitBreakpointCount = tel::countExplicitPromptCacheBreakpoints(input)};
    co_await tel::recordRequestUsage(context, usage).wait();
    reqs::ResponsesRequestState requestState{context ? std::optional(*context) : std::nullopt, rawRecord, settings.warnings, settings.clientWantsStream, model.modelRaw, model.model, model.modelMetadata, input, reasoning.reasoningLabel, bodies.codexBody, bodies.removedProviderBody};
    auto routingTask = prepareResponsesRouting(req, requestState, model.model); rt::Promise<reqs::ResponsesRoutingState> routingPending(std::move(routingTask)); const auto routingState = co_await routingPending.wait();
    auto state = std::make_shared<reqs::ResponsesHandlerState>(reqs::ResponsesHandlerState{requestState, routingState}); co_return reqs::ResponsesStepOk<State>{state};
  }
  void releaseResponsesProbeIfSet(const std::optional<circuit::RemovedProviderCircuitProbe>& probe) { if (probe) (void)circuit::releaseRemovedProviderCircuitProbe(probe).catch_([](rt::Reason) {}); }
  rt::Response releaseResponsesProbeAndReturn(const std::optional<circuit::RemovedProviderCircuitProbe>& probe, rt::Response response) { releaseResponsesProbeIfSet(probe); return response; }
  void completeCodexPrimaryAttempt(const State& state, const attempts::ResponsesRouteAttempt& attempt) {
    state->primaryResult = attempt;
    const auto terminalType = attempt.prepared.prepared.terminal ? std::optional(attempt.prepared.prepared.terminal->type) : std::nullopt;
    if (terminalType == "response.completed" || terminalType == "response.incomplete") attempts::markPrimarySemanticRecovery(attempt.routed, state->probe, contextFor(state), terminalType);
  }
  void recordCodexPrimaryFailureTelemetry(const State& state, const attempts::FailedResponsesAttempt& failed, const std::string& terminalType) {
    const auto telemetry = telemetryFor(state);
    if (telemetry) state->primaryFailureCorrelation = reqs::ResponsesFailureCorrelation{telemetry->provider, telemetry->accountSlot, telemetry->accountCohortId, telemetry->activeGeneration, telemetry->activeTransitionReason, telemetry->providerRequestId};
    tel::recordStreamTerminalType(contextFor(state), terminalType); const auto kind = attempts::failureKindForResponsesAttemptTrigger(failed.trigger);
    if (kind && telemetry) { telemetry->failureKind = kind; if (*kind == "empty_upstream_completion") telemetry->semanticOutputObserved = false; }
  }
  std::optional<tel::UsageTokens> failedTerminalUsage(const attempts::FailedResponsesAttempt& failed) {
    if (!failed.terminal.hasValue()) return std::nullopt;
    const auto* response = rt::get(failed.terminal->value, "response"); return utils::isRecord(response) ? tel::extractUsageTokens(rt::get(response, "usage")) : std::nullopt;
  }
  rt::Task<std::optional<rt::Response>> settleCodexPrimaryFailure(State state, attempts::ResponsesRouteFailure failure) {
    const auto& routed = failure.routed; const auto& failed = failure.failed; const auto& lifecycle = failure.lifecycle;
    state->primaryFailureResponse = failed.response; const auto type = attempts::responseFailureTerminalType(failed.trigger, failed.signal, state->downstreamSignal);
    recordCodexPrimaryFailureTelemetry(state, failed, type);
    if (failed.trigger == "empty_upstream_completion") {
      tel::recordTerminalUsage(contextFor(state), failedTerminalUsage(failed), false);
      const attempts::FinalizeAbandonedPrimaryAttemptOptions options{.failureTrigger = failed.trigger}; auto task = attempts::finalizeAbandonedPrimaryAttempt(routed, lifecycle, options); rt::Promise<void> pending(std::move(task)); co_await pending.wait();
      co_return releaseResponsesProbeAndReturn(state->probe, failed.response);
    }
    if (routed.allowRemovedProviderRecovery == false || (routed.gatewayResponse && !attempts::isEligibleResponsesAttemptStatus(failed.response))) co_return releaseResponsesProbeAndReturn(state->probe, failed.response);
    if (!attempts::isEligibleResponsesAttemptStatus(failed.response) && (failed.trigger == "http_4xx" || failed.trigger == "http_error" || failed.trigger == "read_error")) {
      releaseResponsesProbeIfSet(state->probe); lifecycle.terminal("response.failed", std::nullopt); co_return failed.response;
    }
    if (!routed.gatewayResponse) {
      const attempts::FinalizeAbandonedPrimaryAttemptOptions options{.cancelled = type == "cancelled", .failureTrigger = failed.trigger};
      auto task = attempts::finalizeAbandonedPrimaryAttempt(routed, lifecycle, options); rt::Promise<void> pending(std::move(task)); co_await pending.wait();
    }
    if (type == "cancelled") { releaseResponsesProbeIfSet(state->probe); co_await tel::recordErrorUsage(contextFor(state)).wait(); co_return upstream_wire::toPreHeaderErrorResponse(state->downstreamSignal.reason(), type, routed.provider); }
    if (!state->apiKey || state->apiKey->empty()) co_return failed.response;
    const auto transition = co_await circuit::recordRemovedProviderEligibleFailure(state->probe).wait(); recordRemovedProviderCircuitTransition(contextFor(state), transition);
    tel::RecordRemovedProviderFieldsFields fields; fields.triggerClass = failed.trigger; tel::recordRemovedProviderFields(contextFor(state), fields); state->route = "removed_provider"; co_return std::nullopt;
  }
  rt::Task<rt::Response> handleCodexPrimaryThrow(State state, rt::Reason error) {
    releaseResponsesProbeIfSet(state->probe); const auto type = tel::classifyPreHeaderFailure(error, state->preHeaderDeadline.signal, state->downstreamSignal); tel::recordStreamTerminalType(contextFor(state), type);
    if (type != "cancelled") upstream_wire::logRedactedUpstreamError("[ai.ubq.fi] Upstream fetch failed:", error);
    co_await tel::recordErrorUsage(contextFor(state)).wait(); const auto telemetry = telemetryFor(state);
    co_return upstream_wire::toPreHeaderErrorResponse(error, type, telemetry ? telemetry->provider : std::nullopt);
  }
  attempts::PrimaryResponsesOptions primaryOptions(const State& state, const ResponsesTailOptions& options, double budget) {
    attempts::PrimaryResponsesOptions result{.model = state->model, .reasoning = state->reasoningLabel, .clientWantsStream = state->clientWantsStream, .usageContext = contextFor(state),
      .clientVersion = state->modelMetadata.snapshot ? rt::orUndefined(utils::getString(rt::get(*state->modelMetadata.snapshot, "client_version"))) : rt::Nullable<std::string>(),
      .requestSignal = state->requestInferenceSignal, .downstreamSignal = state->downstreamSignal, .warnings = state->warnings,
      .attemptDeadline = inference_deadline::createStreamSemanticDeadline(state->preHeaderDeadline.signal, rt::Math::ceil(budget)),
      .releaseOnProgress = state->clientWantsStream, .allowedPaidProviders = options.allowedPaidProviders, .deepSeekWaterfallPaidHop = options.deepSeekWaterfallPaidHop};
    return result;
  }
  rt::Task<std::optional<rt::Response>> runCodexPrimaryAttempt(State state, ResponsesTailOptions options) {
    std::optional<rt::Response> response; rt::Reason error;
    try {
      const auto remaining = state->preHeaderDeadline.remainingMs(); const auto reserve = rt::Math::min(inference_deadline::STREAM_FAILOVER_RESERVE_MS, remaining / 2);
      const auto budget = (state->apiKey && !state->apiKey->empty()) || state->paidFallbackAvailable ? rt::Math::max(0.0, remaining - reserve) : remaining;
      auto attemptOptions = primaryOptions(state, options, budget); attemptOptions.rejectPresemanticFailureTerminal = state->apiKey.has_value();
      if (state->paidFallbackAvailable) { attemptOptions.fallbackSignal = state->preHeaderDeadline.signal; attemptOptions.createFallbackDeadline = [state] { return inference_deadline::createStreamSemanticDeadline(state->preHeaderDeadline.signal, rt::Math::ceil(state->preHeaderDeadline.remainingMs())); }; }
      auto task = attempts::fetchAndPreparePrimaryResponses(state->codexBody, attemptOptions); rt::Promise<attempts::PrimaryResponsesResult> pending(std::move(task)); const auto result = co_await pending.wait();
      if (const auto* ready = std::get_if<attempts::PrimaryResponsesReady>(&result)) { completeCodexPrimaryAttempt(state, ready->value); co_return std::nullopt; }
      auto settle = settleCodexPrimaryFailure(state, std::get<attempts::PrimaryResponsesFailed>(result).value); rt::Promise<std::optional<rt::Response>> settling(std::move(settle)); response = co_await settling.wait();
    } catch (...) { error = std::current_exception(); }
    if (error) { auto task = handleCodexPrimaryThrow(state, error); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult1 = co_await pending.wait(); co_return awaitedResult1; }
    co_return response;
  }
  using Probe = std::optional<circuit::RemovedProviderCircuitProbe>;
  rt::Task<reqs::ResponsesStep<Probe>> acquireResponsesRecoveryProbe(rt::Response fallbackResponse) {
    (void)co_await circuit::claimRemovedProviderEarlyRecoveryProbe().wait();
    const auto route = co_await circuit::selectRemovedProviderCircuitRoute().wait();
    if (route.route != "codex") co_return reqs::ResponsesStepNotOk{fallbackResponse};
    co_return reqs::ResponsesStepOk<Probe>{std::nullopt};
  }
  rt::Task<void> releaseResponsesRecoveryProbe(State state, Probe probe) {
    std::string transition = "none"; if (probe) transition = co_await circuit::releaseRemovedProviderCircuitProbe(probe).wait();
    recordRemovedProviderCircuitTransition(contextFor(state), transition);
  }
  rt::Task<rt::Response> settleResponsesRecoveryFailure(State state, attempts::ResponsesRouteFailure failure, Probe probe, rt::Response fallback) {
    const auto& routed = failure.routed; const auto& failed = failure.failed; const auto type = attempts::responseFailureTerminalType(failed.trigger, failed.signal, state->downstreamSignal);
    const attempts::FinalizeAbandonedPrimaryAttemptOptions options{.cancelled = type == "cancelled", .failureTrigger = failed.trigger};
    auto task = attempts::finalizeAbandonedPrimaryAttempt(routed, failure.lifecycle, options); rt::Promise<void> pending(std::move(task)); co_await pending.wait();
    if (failed.trigger == "empty_upstream_completion") {
      const auto telemetry = telemetryFor(state); if (telemetry) { telemetry->failureKind = "empty_upstream_completion"; telemetry->semanticOutputObserved = false; }
      tel::recordTerminalUsage(contextFor(state), failedTerminalUsage(failed), false); tel::recordStreamTerminalType(contextFor(state), "response.failed");
      auto release = releaseResponsesRecoveryProbe(state, probe); rt::Promise<void> released(std::move(release)); co_await released.wait(); co_return failed.response;
    }
    if (type == "cancelled") {
      auto release = releaseResponsesRecoveryProbe(state, probe); rt::Promise<void> released(std::move(release)); co_await released.wait();
      tel::recordStreamTerminalType(contextFor(state), type); co_await tel::recordErrorUsage(contextFor(state)).wait(); co_return upstream_wire::toPreHeaderErrorResponse(state->downstreamSignal.reason(), type, routed.provider);
    }
    if (failed.trigger == "semantic_timeout") { auto release = releaseResponsesRecoveryProbe(state, probe); rt::Promise<void> released(std::move(release)); co_await released.wait(); co_return failed.response; }
    if (attempts::isEligibleResponsesAttemptStatus(failed.response)) { const auto transition = co_await circuit::recordRemovedProviderEligibleFailure(probe).wait(); recordRemovedProviderCircuitTransition(contextFor(state), transition); }
    else { auto release = releaseResponsesRecoveryProbe(state, probe); rt::Promise<void> released(std::move(release)); co_await released.wait(); }
    tel::selectRemovedProviderTelemetry(contextFor(state)); co_return fallback;
  }
  rt::Task<std::optional<rt::Response>> runResponsesRecoveryAttempt(State state, attempts::FailedResponsesAttempt fallback, Probe probe, ResponsesTailOptions options) {
    std::optional<attempts::PrimaryResponsesResult> recovery; rt::Reason error;
    try {
      auto attemptOptions = primaryOptions(state, options, state->preHeaderDeadline.remainingMs()); attemptOptions.rejectPresemanticFailureTerminal = true;
      auto task = attempts::fetchAndPreparePrimaryResponses(state->codexBody, attemptOptions); rt::Promise<attempts::PrimaryResponsesResult> pending(std::move(task)); recovery = co_await pending.wait();
    } catch (...) { error = std::current_exception(); }
    if (error) {
      auto task = releaseResponsesRecoveryProbe(state, probe); rt::Promise<void> pending(std::move(task)); co_await pending.wait();
      if (state->requestInferenceSignal.aborted()) std::rethrow_exception(state->requestInferenceSignal.reason() ? state->requestInferenceSignal.reason() : error);
      tel::selectRemovedProviderTelemetry(contextFor(state)); co_return fallback.response;
    }
    if (const auto* failed = std::get_if<attempts::PrimaryResponsesFailed>(&*recovery)) { auto task = settleResponsesRecoveryFailure(state, failed->value, probe, fallback.response); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult2 = co_await pending.wait(); co_return awaitedResult2; }
    completeCodexPrimaryAttempt(state, std::get<attempts::PrimaryResponsesReady>(*recovery).value); co_return std::nullopt;
  }
  rt::Task<std::optional<rt::Response>> settleRemovedProviderFailure(State state, attempts::FailedResponsesAttempt failed, ResponsesTailOptions options) {
    (void)tel::persistFailedRemovedProviderAttempt(contextFor(state), state->fallbackStartedAt, failed.trigger);
    if (failed.trigger == "empty_upstream_completion") {
      const auto telemetry = telemetryFor(state); if (telemetry) { telemetry->failureKind = "empty_upstream_completion"; telemetry->semanticOutputObserved = false; }
      tel::recordTerminalUsage(contextFor(state), failedTerminalUsage(failed), false); tel::recordStreamTerminalType(contextFor(state), "response.failed"); co_return failed.response;
    }
    if (state->primaryFailureResponse || failed.trigger == "terminal_failure") {
      const auto telemetry = telemetryFor(state);
      if (state->primaryFailureResponse && telemetry) {
        const auto correlation = state->primaryFailureCorrelation;
        telemetry->provider = correlation && correlation->provider ? correlation->provider : state->primaryFailureResponse->headers().get("x-uos-upstream").value_or("chatgpt_codex");
        telemetry->accountSlot = correlation ? correlation->accountSlot : std::nullopt; telemetry->accountCohortId = correlation ? correlation->accountCohortId : std::nullopt;
        telemetry->activeGeneration = correlation ? correlation->activeGeneration : std::nullopt; telemetry->activeTransitionReason = correlation ? correlation->activeTransitionReason : std::nullopt; telemetry->providerRequestId = correlation ? correlation->providerRequestId : std::nullopt;
      } else tel::selectRemovedProviderTelemetry(contextFor(state));
      co_return state->primaryFailureResponse.value_or(failed.response);
    }
    auto task = acquireResponsesRecoveryProbe(failed.response); rt::Promise<reqs::ResponsesStep<Probe>> pending(std::move(task)); const auto recovery = co_await pending.wait();
    if (const auto* error = std::get_if<reqs::ResponsesStepNotOk>(&recovery)) co_return error->response;
    state->probe = std::get<reqs::ResponsesStepOk<Probe>>(recovery).value;
    auto recoveryTask = runResponsesRecoveryAttempt(state, failed, state->probe, options); auto awaitedResult3 = co_await std::move(recoveryTask); co_return awaitedResult3;
  }
  rt::Task<std::optional<rt::Response>> runRemovedProviderAttempt(State state, std::string apiKey, ResponsesTailOptions options) {
    state->fallbackStartedAt = rt::performanceNow();
    const attempts::RemovedProviderResponsesOptions attemptOptions{.usageContext = contextFor(state), .requestSignal = state->requestInferenceSignal, .sessionId = state->sessionId, .apiKey = apiKey, .attemptDeadline = inference_deadline::createStreamSemanticDeadline(state->preHeaderDeadline.signal, rt::Math::ceil(state->preHeaderDeadline.remainingMs()))};
    auto task = attempts::fetchAndPrepareRemovedProviderResponses(state->removedProviderBody, attemptOptions); rt::Promise<attempts::ResponsesAttemptResult> pending(std::move(task)); const auto result = co_await pending.wait();
    if (const auto* failed = std::get_if<attempts::ResponsesAttemptResultFailed>(&result)) { auto settle = settleRemovedProviderFailure(state, failed->attempt, options); auto awaitedResult4 = co_await std::move(settle); co_return awaitedResult4; }
    const auto ready = std::get<attempts::ResponsesAttemptResultReady>(result).attempt; state->removedProviderAttempt = ready; state->selectedModel = ready.selectedModel;
    tel::RecordRemovedProviderFieldsFields fields; fields.selectedModel = rt::orNull(state->selectedModel); fields.taskType = rt::orNull(ready.taskType);
    fields.semanticCommitment = rt::orNull(ready.prepared.semanticKind ? ready.prepared.semanticKind : ready.prepared.terminal && ready.prepared.terminal->type == "response.completed" ? std::optional<std::string>("terminal_completed") : std::nullopt);
    tel::recordRemovedProviderFields(contextFor(state), fields); co_return std::nullopt;
  }
  rt::Task<std::optional<rt::Response>> runResponsesFailover(State state, ResponsesTailOptions options) {
    auto clear = rt::scopeExit([state] { state->preHeaderDeadline.clear(); });
    if (state->route == "codex") { auto task = runCodexPrimaryAttempt(state, options); rt::Promise<std::optional<rt::Response>> pending(std::move(task)); const auto response = co_await pending.wait(); if (response) co_return response; }
    if (state->route == "removed_provider" && state->apiKey && !state->apiKey->empty()) { auto task = runRemovedProviderAttempt(state, *state->apiKey, options); rt::Promise<std::optional<rt::Response>> pending(std::move(task)); const auto response = co_await pending.wait(); if (response) co_return response; }
    co_return std::nullopt;
  }
  rt::Promise<std::string> removedProviderProbeTransitionForTerminal(const Probe& probe, const std::string& type) {
    if (type != "cancelled") return circuit::recordRemovedProviderEligibleFailure(probe);
    if (probe) return circuit::releaseRemovedProviderCircuitProbe(probe);
    return rt::Promise<std::string>::resolve("none");
  }
  void recordTransition(const Delivery& delivery, rt::Promise<std::string> transition) {
    (void)transition.then([delivery](const std::string& value) { recordRemovedProviderCircuitTransition(contextFor(delivery), value); }).catch_([](rt::Reason) {});
  }
  void reconcileCommittedFailure(const Delivery& delivery, const std::string& type) {
    delivery->clearProbeRenewal(); if (delivery->providerTerminalValidated) return;
    const auto telemetry = telemetryFor(delivery); if (telemetry && !telemetry->streamTerminalType) tel::recordStreamTerminalType(contextFor(delivery), type);
    const auto routed = delivery->routed;
    if (routed) rt::spawn(rt::async([delivery, type]() -> rt::Task<void> {
      const attempts::FinalizeAbandonedPrimaryAttemptOptions options{.cancelled = type == "cancelled"};
      auto task = attempts::finalizeAbandonedPrimaryAttempt(*delivery->routed, delivery->lifecycle, options); co_await std::move(task);
    }));
    if (delivery->probe && routed && (routed->provider == "metered" || routed->provider == "surplus")) recordTransition(delivery, circuit::releaseRemovedProviderCircuitProbe(delivery->probe));
    else if (routed && routed->provider == "chatgpt_codex") recordTransition(delivery, removedProviderProbeTransitionForTerminal(delivery->probe, type));
    if (delivery->removedProviderAttempt && (!telemetry || telemetry->removedProviderTerminalStatus != "response.failed")) {
      tel::RecordRemovedProviderFieldsFields fields; fields.latencyMs = rt::Math::max(0.0, rt::Math::round(rt::performanceNow() - delivery->fallbackStartedAt)); fields.terminalStatus = type;
      tel::recordRemovedProviderFields(contextFor(delivery), fields); (void)tel::persistRemovedProviderFields(contextFor(delivery));
    }
    (void)tel::recordErrorUsage(contextFor(delivery));
  }
  void validateRemovedProviderStreamEvent(const Delivery& delivery, const rs::ResponsesStreamEvent& event) {
    if (!delivery->removedProviderAttempt || !delivery->selectedModel || delivery->selectedModel->empty()) return;
    const auto candidate = removed::removedProviderModelFromEvent(event.value); if (!candidate || candidate->empty()) return;
    if (candidate != delivery->selectedModel || !removed::isEligibleRemovedProviderModel(*candidate)) throw rs::ResponsesStreamError("RemovedProvider changed the selected model after stream release.", {.kind = "malformed_event"});
  }
  rt::Task<void> codexResponseTerminalTransition(rt::Response response, std::string type) {
    if (type == "response.completed") return codex::index::markCodexResponseCompleted(response);
    if (type == "response.failed" || type == "error") return codex::index::markCodexResponseUpstreamError(response);
    return codex::index::releaseCodexResponseProbe(response);
  }
  rt::Promise<std::string> codexTerminalCircuitTransition(const std::string& provider, const std::string& type, const Probe& probe) {
    if (provider != "chatgpt_codex") return circuit::releaseRemovedProviderCircuitProbe(probe);
    if (type == "response.completed" || type == "response.incomplete") return circuit::closeRemovedProviderCircuit(probe);
    if (type == "response.failed" || type == "error") return circuit::recordRemovedProviderEligibleFailure(probe);
    return circuit::releaseRemovedProviderCircuitProbe(probe);
  }
  void applyRemovedProviderTerminalTransition(const Delivery& delivery, const rs::ResponsesStreamEvent& event, const tel::RoutedResponsesUpstream& routed) {
    if (delivery->probe) { recordTransition(delivery, codexTerminalCircuitTransition(routed.provider, event.type, delivery->probe)); return; }
    if (routed.provider == "chatgpt_codex" && (event.type == "response.failed" || event.type == "error")) recordTransition(delivery, circuit::recordRemovedProviderEligibleFailure(std::nullopt));
  }
  void applyRoutedStreamTerminal(const Delivery& delivery, const tel::RoutedResponsesUpstream& routed, const rs::ResponsesStreamEvent& event) {
    const auto* response = rt::get(event.value, "response"); const auto usage = utils::isRecord(response) ? tel::extractUsageTokens(rt::get(response, "usage")) : std::nullopt;
    delivery->lifecycle.terminal(event.type, usage);
    if (routed.provider == "chatgpt_codex") { auto task = codexResponseTerminalTransition(routed.response, event.type); (void)rt::Promise<void>(std::move(task)).catch_([](rt::Reason) {}); }
    applyRemovedProviderTerminalTransition(delivery, event, routed);
  }
  void onResponsesTerminal(const Delivery& delivery, const rs::ResponsesStreamEvent& event) {
    if (!event.terminal) return;
    delivery->clearProbeRenewal(); const bool synthetic = fs::isSyntheticResponsesFailureEvent(event);
    if (!synthetic && delivery->providerTerminalValidated) { tel::recordStreamTerminalType(contextFor(delivery), event.type); return; }
    if (!synthetic) delivery->providerTerminalValidated = true;
    if (delivery->routed && !synthetic) applyRoutedStreamTerminal(delivery, *delivery->routed, event);
    if (!synthetic || delivery->removedProviderAttempt) chat::stream_translation::recordResponsesTerminal(event, contextFor(delivery));
    if (delivery->removedProviderAttempt) {
      tel::RecordRemovedProviderFieldsFields fields; fields.latencyMs = rt::Math::max(0.0, rt::Math::round(rt::performanceNow() - delivery->fallbackStartedAt)); fields.terminalStatus = event.type;
      tel::recordRemovedProviderFields(contextFor(delivery), fields); (void)tel::persistRemovedProviderFields(contextFor(delivery));
    }
  }
  std::vector<std::string> buildResponsesClientWarnings(const State& state, const attempts::PreparedResponsesAttempt& ready) {
    const std::vector<std::string> controls{"max_output_tokens", "max_tool_calls", "metadata", "prompt_cache_options", "prompt_cache_retention", "safety_identifier", "service_tier", "temperature", "top_p", "truncation", "user"};
    auto warnings = state->warnings;
    if (state->primaryFailureResponse) { const auto extra = request_policy::responseWarnings(*state->primaryFailureResponse); warnings.insert(warnings.end(), extra.begin(), extra.end()); }
    const auto extra = request_policy::responseWarnings(ready.response); warnings.insert(warnings.end(), extra.begin(), extra.end());
    if (!state->removedProviderAttempt) return warnings;
    std::vector<std::string> result;
    for (const auto& warning : warnings) {
      if (warning == "prompt_cache_breakpoint_ignored" && tel::countExplicitPromptCacheBreakpoints(state->input) > 0) continue;
      bool forwarded = false;
      for (const auto& key : controls) { const auto* mapped = request_policy::WARNING_KEY_MAP.get(key); if (rt::hasOwn(state->rawRecord, key) && warning == (mapped ? *mapped : key + "_ignored")) { forwarded = true; break; } }
      if (!forwarded) result.push_back(warning);
    }
    return result;
  }
  reqs::ResponsesStep<Delivery> buildResponsesDelivery(const State& state) {
    const auto ready = state->removedProviderAttempt ? state->removedProviderAttempt : state->primaryResult ? std::optional(state->primaryResult->prepared) : std::nullopt;
    if (!ready) return reqs::ResponsesStepNotOk{state->primaryFailureResponse.value_or(tel::streamErrorResponse(502, "No upstream provider produced a response.", "upstream_error", "chatgpt_codex", state->warnings))};
    const auto telemetry = telemetryFor(state); if (telemetry) { telemetry->failureKind = std::nullopt; telemetry->syntheticTerminalType = std::nullopt; telemetry->streamTerminalType = std::nullopt; }
    const auto lifecycle = state->primaryResult ? state->primaryResult->lifecycle : paid_fallback::health::createMeteredTransportLifecycle(std::nullopt);
    const auto routed = state->primaryResult ? std::optional(state->primaryResult->routed) : std::nullopt;
    const auto warnings = buildResponsesClientWarnings(state, *ready);
    const auto* text = rt::get(state->rawRecord, "text"); const auto* format = rt::get(text, "format"); const auto formatType = utils::getString(rt::get(format, "type"));
    const bool structured = utils::isRecord(text) && utils::isRecord(format) && (formatType == "json_schema" || formatType == "json_object");
    const auto warningModel = state->removedProviderAttempt && !structured ? state->selectedModel : std::nullopt;
    const auto probe = state->probe;
    if (probe && ready->prepared.semantic) (void)circuit::renewRemovedProviderCircuitProbe(*probe).catch_([](rt::Reason) {});
    rt::Timeout timer;
    if (probe && ready->prepared.semantic) timer = rt::setInterval([probe] { (void)circuit::renewRemovedProviderCircuitProbe(*probe).catch_([](rt::Reason) {}); }, 60000);
    auto clear = [timer] { if (timer) rt::clearInterval(timer); };
    auto delivery = std::make_shared<reqs::ResponsesDeliveryState>(reqs::ResponsesDeliveryState{state->usageContext, *ready, lifecycle, routed, state->clientWantsStream, state->downstreamSignal, warnings, warningModel, state->selectedModel, probe, state->removedProviderAttempt, state->fallbackStartedAt, false, clear});
    return reqs::ResponsesStepOk<Delivery>{delivery};
  }
  rt::Task<rt::Response> deliverBufferedResponses(Delivery delivery) {
    responses_buffered::BufferedResponsesOptions options; options.warningModel = delivery->warningModel; options.usageContext = contextFor(delivery);
    options.onTerminal = [delivery](const rs::ResponsesStreamEvent& event) { onResponsesTerminal(delivery, event); };
    options.validateEvent = [delivery](const rs::ResponsesStreamEvent& event) { validateRemovedProviderStreamEvent(delivery, event); };
    options.onFailure = [delivery](const rt::Reason& error, const fs::OwnedResponsesStreamFailureDetails* details) -> std::optional<rt::Response> {
      const auto type = tel::classifyStreamFailure(error, delivery->ready.signal, delivery->downstreamSignal);
      if (type != "cancelled") tel::recordResponsesFailureTelemetry(contextFor(delivery), error, details);
      else if (const auto telemetry = telemetryFor(delivery)) telemetry->responseCreatedObserved = details ? details->responseCreatedObserved : telemetry->responseCreatedObserved;
      reconcileCommittedFailure(delivery, type);
      if (type == "cancelled" || type == "deadline") return upstream_wire::toPreHeaderErrorResponse(error, type, delivery->ready.provider);
      return std::nullopt;
    };
    auto task = responses_buffered::collectBufferedResponses(delivery->ready, options); rt::Promise<rt::Response> pending(std::move(task)); const auto response = co_await pending.wait();
    co_return request_policy::withUosWarning(response, delivery->clientWarnings);
  }
  rt::Response deliverStreamingResponses(const Delivery& delivery) {
    fs::OwnedResponsesStreamOptions options{.codexContextErrorCompatibility = delivery->ready.provider == "chatgpt_codex", .initial = delivery->ready.prepared.buffered,
      .iterator = delivery->ready.prepared.iterator, .responseId = delivery->ready.responseId};
    if (delivery->warningModel && !delivery->warningModel->empty()) options.warning = fs::OwnedResponsesStreamOptions::Warning{*delivery->warningModel};
    options.signal = delivery->ready.signal; options.downstreamSignal = delivery->downstreamSignal; options.abortUpstream = delivery->ready.abort;
    options.onEvent = [delivery](rs::ResponsesStreamEvent event) { tel::recordResponsesEventTelemetry(contextFor(delivery), event); onResponsesTerminal(delivery, event); };
    options.validateEvent = [delivery](const rs::ResponsesStreamEvent& event) { validateRemovedProviderStreamEvent(delivery, event); };
    options.onFailure = [delivery](rt::Reason error, fs::OwnedResponsesStreamFailureDetails details) {
      const auto type = tel::classifyStreamFailure(error, delivery->ready.signal, delivery->downstreamSignal);
      if (type != "cancelled") tel::recordResponsesFailureTelemetry(contextFor(delivery), error, &details);
      else if (const auto telemetry = telemetryFor(delivery)) telemetry->responseCreatedObserved = details.responseCreatedObserved;
      if (details.failureKind == "empty_upstream_completion") {
        const auto* response = details.upstreamTerminal ? rt::get(details.upstreamTerminal->value, "response") : nullptr;
        const auto usage = utils::isRecord(response) ? tel::extractUsageTokens(rt::get(response, "usage")) : std::nullopt;
        tel::recordTerminalUsage(contextFor(delivery), usage, false);
      }
      reconcileCommittedFailure(delivery, type);
    };
    options.onCancel = [delivery](rt::Reason) { reconcileCommittedFailure(delivery, "cancelled"); };
    const auto body = fs::createOwnedResponsesStream(options); auto headers = delivery->ready.response.headers(); headers.delete_("Content-Encoding"); headers.delete_("Content-Length"); headers.set("Content-Type", "text/event-stream"); headers.set("x-uos-upstream", delivery->ready.provider);
    const auto response = rt::Response(rs::withSseKeepalive(body), {.status = 200, .headers = headers}); return request_policy::withUosWarning(response, delivery->clientWarnings);
  }
  rt::Task<rt::Response> deliverPreparedResponses(Delivery delivery) {
    const auto& prepared = delivery->ready.prepared;
    if (prepared.semantic || !prepared.terminal || !delivery->clientWantsStream || delivery->ready.provider != "chatgpt_codex") {
      tel::recordFirstSemanticCommitment(contextFor(delivery));
    }
    if (prepared.terminal) onResponsesTerminal(delivery, *prepared.terminal);
    if (!delivery->clientWantsStream) { auto task = deliverBufferedResponses(delivery); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult5 = co_await pending.wait(); co_return awaitedResult5; }
    co_return deliverStreamingResponses(delivery);
  }
  rt::Task<rt::Response> handleResponsesInternal(rt::Request req, const tel::UsageContext* context, const json::value* parsedBody) {
    auto requestTask = reqs::readResponsesRequest(req, parsedBody); rt::Promise<reqs::ResponsesStep<reqs::ReadResponsesRequestValue>> requestPending(std::move(requestTask)); const auto request = co_await requestPending.wait();
    if (const auto* error = std::get_if<reqs::ResponsesStepNotOk>(&request)) co_return error->response;
    const auto value = std::get<reqs::ResponsesStepOk<reqs::ReadResponsesRequestValue>>(request).value; const auto& rawRecord = value.rawRecord; const auto& rawBody = value.rawBody;
    auto extensionsTask = provider::openrouter_request_controls::openRouterPromptCacheRequestExtensions(rawRecord); rt::Promise<std::optional<rt::OrderedSet<std::string>>> extensionsPending(std::move(extensionsTask)); const auto extensions = co_await extensionsPending.wait();
    const auto invalid = reqs::validateResponsesRequestFields(rawRecord, rawBody, extensions ? &*extensions : nullptr); if (invalid) co_return *invalid;
    const auto dispatchContext = responses_native_headers::nativeHeaderUsageContext(req, rawRecord, context); const auto* dispatch = dispatchContext ? &*dispatchContext : nullptr;
    const auto requested = rt::trim(rt::getString(rawRecord, "model").value_or(""));
    if (provider::gemini::isGeminiModel(requested) && provider::gemini::geminiOperatorOptIn()) {
      auto selectionTask = provider::selection::loadProviderSelectionCached();
      const auto selection = co_await std::move(selectionTask);
      if (provider::selection::isProviderEnabled("gemini", selection)) {
        auto task = provider::gemini::handleGeminiResponses(req, rawRecord, requested, dispatch);
        rt::Promise<rt::Response> pending(std::move(task));
        co_return co_await pending.wait();
      }
    }
    if (!requested.empty() && deepseek::waterfall::isDeepSeekWaterfallModel(requested)) { auto task = deepseek::waterfall_handler::handleDeepSeekWaterfallResponses(req, rawRecord, rawBody, dispatch); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult6 = co_await pending.wait(); co_return awaitedResult6; }
    if (!requested.empty() && deepseek::index::deepSeekUpstreamModelFor(requested)) {
      const auto selection = co_await provider::selection::loadProviderSelectionCached(); co_await rt::microtask();
      if (provider::selection::isProviderEnabled("deepseek", selection)) { auto task = deepseek::handlers::handleDeepSeekResponses(req, rawRecord, requested, dispatch); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult7 = co_await pending.wait(); co_return awaitedResult7; }
    }
    if (!requested.empty() && provider::lithos::lithosUpstreamModelFor(requested)) {
      const auto selection = co_await provider::selection::loadProviderSelectionCached(); co_await rt::microtask();
      if (provider::selection::isProviderEnabled("lithos", selection)) { auto task = provider::lithos_handlers::handleLithosResponses(req, rawRecord, requested, dispatch); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult8 = co_await pending.wait(); co_return awaitedResult8; }
    }
    if (!requested.empty() && provider::cerebras::cerebrasUpstreamModelFor(requested)) {
      const auto selection = co_await provider::selection::loadProviderSelectionCached(); co_await rt::microtask();
      if (provider::selection::isProviderEnabled("cerebras", selection)) { auto task = provider::cerebras_responses::handleCerebrasResponses(req, rawRecord, requested, dispatch); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult9 = co_await pending.wait(); co_return awaitedResult9; }
    }
    if (!requested.empty()) {
      const auto selection = co_await provider::selection::loadProviderSelectionCached(); co_await rt::microtask();
      if (provider::selection::isProviderEnabled("openrouter", selection)) {
        auto resolveTask = provider::openrouter::resolveOpenRouterUpstreamModel(requested); rt::Promise<std::optional<std::string>> resolvePending(std::move(resolveTask)); const auto model = co_await resolvePending.wait();
        if (model && !model->empty()) { auto task = provider::openrouter_handlers::handleOpenRouterResponses(req, rawRecord, requested, dispatch); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult10 = co_await pending.wait(); co_return awaitedResult10; }
      }
    }
    auto task = runOrdinaryResponsesTail(req, rawRecord, rawBody, dispatch); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult11 = co_await pending.wait(); co_return awaitedResult11;
  }
  rt::Task<rt::Response> runResponsesTelemetryBody(rt::Request req, std::optional<json::value> parsedBody, tel::UsageContext context) {
    std::optional<rt::Response> response; rt::Reason error;
    try {
      auto task = handleResponsesInternal(req, &context, parsedBody ? &*parsedBody : nullptr); rt::Promise<rt::Response> pending(std::move(task)); response = co_await pending.wait();
      auto nativeTask = responses_native_headers::nativeFailureAfterEarlyHeaders(req, *response, context); rt::Promise<rt::Response> nativePending(std::move(nativeTask)); auto awaitedResult12 = co_await nativePending.wait(); co_return awaitedResult12;
    }
    catch (...) { error = std::current_exception(); }
    if (error) {
      const auto downstream = openai::downstreamSignalFor(req, &context); std::optional<std::string> type;
      if (tel::isTimeoutFailure(error, downstream.reason())) type = "deadline"; else if (downstream.aborted()) type = "cancelled";
      if (!type) std::rethrow_exception(error);
      tel::recordStreamTerminalType(&context, *type); co_await tel::recordErrorUsage(&context).wait(); response = upstream_wire::toPreHeaderErrorResponse(error, *type, context.responseTelemetry ? context.responseTelemetry->provider.value_or("chatgpt_codex") : "chatgpt_codex");
    }
    auto task = responses_native_headers::nativeFailureAfterEarlyHeaders(req, *response, context); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult13 = co_await pending.wait(); co_return awaitedResult13;
  }
}
void recordRemovedProviderCircuitTransition(const tel::UsageContext* context, const std::string& transition) { if (transition != "none") { tel::RecordRemovedProviderFieldsFields fields; fields.circuitTransition = transition; tel::recordRemovedProviderFields(context, fields); } }
rt::Task<rt::Response> runOrdinaryResponsesTail(rt::Request req, json::object rawRecord, ::uos::types::ResponsesRequest rawBody, const tel::UsageContext* context) {
  const ResponsesTailOptions options; auto task = runOrdinaryResponsesTail(req, rawRecord, rawBody, context, options); auto awaitedResult14 = co_await std::move(task); co_return awaitedResult14;
}
rt::Task<rt::Response> runOrdinaryResponsesTail(rt::Request req, json::object rawRecord, ::uos::types::ResponsesRequest rawBody, const tel::UsageContext* context, ResponsesTailOptions options) {
  auto task = prepareResponsesRequest(req, rawRecord, rawBody, context, options); rt::Promise<reqs::ResponsesStep<State>> pending(std::move(task)); const auto prepared = co_await pending.wait();
  if (const auto* error = std::get_if<reqs::ResponsesStepNotOk>(&prepared)) co_return error->response;
  const auto state = std::get<reqs::ResponsesStepOk<State>>(prepared).value;
  auto failoverTask = runResponsesFailover(state, options); rt::Promise<std::optional<rt::Response>> failoverPending(std::move(failoverTask)); const auto failover = co_await failoverPending.wait(); if (failover) co_return *failover;
  const auto delivery = buildResponsesDelivery(state); if (const auto* error = std::get_if<reqs::ResponsesStepNotOk>(&delivery)) co_return error->response;
  auto deliveryTask = deliverPreparedResponses(std::get<reqs::ResponsesStepOk<Delivery>>(delivery).value); rt::Promise<rt::Response> deliveryPending(std::move(deliveryTask)); auto awaitedResult15 = co_await deliveryPending.wait(); co_return awaitedResult15;
}
rt::Task<rt::Response> runResponsesHandler(rt::Request req, const tel::UsageContext* context, const json::value* parsedBody) {
  const std::optional<json::value> body = parsedBody ? std::optional(*parsedBody) : std::nullopt;
  std::function<rt::Task<rt::Response>(tel::UsageContext)> run = [req, body](tel::UsageContext telemetry) { return runResponsesTelemetryBody(req, body, std::move(telemetry)); };
  const auto source = context ? std::optional(*context) : std::nullopt;
  auto task = tel::runWithResponseTelemetry(source, run); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult16 = co_await pending.wait(); co_return awaitedResult16;
}
rt::Task<rt::Response> handleResponses(rt::Request req, const tel::UsageContext* context) { auto task = runResponsesHandler(req, context); auto awaitedResult17 = co_await std::move(task); co_return awaitedResult17; }
}
