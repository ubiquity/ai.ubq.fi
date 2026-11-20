// Port of src/responses-attempts.ts.
#include "uos/responses_attempts.hpp"
#include "uos/responses_handler.hpp"
#include "uos/paid_fallback/health.hpp"
#include "uos/paid_fallback/routing.hpp"
#include "uos/paid_fallback/removed_provider.hpp"
#include "uos/codex/index.hpp"
#include "uos/request_policy.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"
#include "uos/rt/number.hpp"

#include <utility>
namespace uos::responses_attempts {
namespace {
  namespace tel = openai_telemetry;
  namespace fs = responses_failover_stream;
  namespace rs = responses_stream;
  namespace removed = paid_fallback::removed_provider;
  namespace circuit = provider::removed_provider_circuit;
  std::string responsesHttpErrorTrigger(double status) { return status >= 500 ? "http_5xx" : status >= 400 ? "http_4xx" : "http_error"; }
  std::string triggerForResponsesError(const rt::Reason& error, const rt::AbortSignal& signal) {
    if (signal.aborted()) { try { if (signal.reason()) std::rethrow_exception(signal.reason()); } catch (const rt::JsError& e) { if (e.name == "TimeoutError") return "semantic_timeout"; } catch (...) {} }
    if (const auto* e = rs::responsesStreamErrorOf(error)) {
      if (e->kind == "event_too_large" || e->kind == "malformed_event" || e->kind == "premature_eof") return e->kind;
      if (e->kind == "inactivity_timeout") return "semantic_timeout";
    }
    return "read_error";
  }
  rt::Response safeFailedAttemptResponse(rt::Response response, const std::string& provider, const std::string& trigger, const std::vector<std::string>& warnings) {
    if (!response.ok()) return response;
    if (trigger == "empty_upstream_completion") return tel::streamErrorResponse(502, "The upstream completed without visible output.", "empty_upstream_completion", provider, warnings, "server_error", nullptr);
    if (trigger == "semantic_timeout") return tel::streamErrorResponse(504, "Upstream stream exceeded the gateway deadline before semantic output.", "gateway_timeout", provider, warnings, "server_error");
    if (trigger == "missing_body") return tel::streamErrorResponse(502, "Upstream response missing body.", "server_error", provider, warnings);
    return tel::streamErrorResponse(502, "Upstream Responses stream ended unexpectedly.", "server_error", provider, warnings);
  }
  std::string responsesAttemptTriggerFor(const std::optional<rs::ResponsesStreamEvent>& semantic, const std::string& fallback) { return semantic ? "terminal_failure" : fallback; }
  struct Identity { std::optional<std::string> selectedModel; std::optional<std::string> taskType; bool modelConflict; };
  Identity applyResponsesEventIdentity(const rs::ResponsesStreamEvent& event, std::optional<std::string> selectedModel, std::optional<std::string> taskType) {
    const auto candidate = removed::removedProviderModelFromEvent(event.value);
    if (candidate && !candidate->empty()) {
      if (selectedModel && !selectedModel->empty() && selectedModel != candidate) return {selectedModel, taskType, true};
      selectedModel = candidate;
    }
    if ((!taskType || taskType->empty()) && utils::isRecord(rt::get(event.value, "response"))) {
      const auto* response = rt::getObject(event.value, "response");
      if (response) taskType = removed::removedProviderTaskTypeFromResponse(*response);
    }
    return {selectedModel, taskType, false};
  }
  rt::Task<void> closeIterator(rs::ResponsesStreamIterator iterator) {
    try { co_await iterator.return_(); } catch (...) {}
    co_await rt::microtask();
  }
  rs::ResponsesStreamIterator sanitizedIterator(rs::ResponsesStreamIterator iterator) {
    return rs::ResponsesStreamIterator([iterator](rt::Yield<rs::ResponsesStreamEvent> yield) -> rt::Task<void> {
      rt::Reason error;
      try {
        for (;;) {
          auto next = co_await iterator.next(); if (next.done) break;
          auto event = *next.value; const auto& value = removed::stripRemovedProviderMetadata(event.value);
          if (&value != &event.value) event = fs::responseEventFromValue(value);
          co_await yield(event);
        }
      } catch (...) { error = std::current_exception(); }
      if (error) { auto closeTask = closeIterator(iterator); co_await std::move(closeTask); std::rethrow_exception(error); }
    });
  }
  bool isDispatchError(const rt::Reason& error) { try { std::rethrow_exception(error); } catch (const api_key_policy::ApiKeyQuotaDispatchError&) { return true; } catch (...) { return false; } }
}
bool isEligibleResponsesAttemptStatus(const rt::Response& response) { return response.status() >= 500; }
std::optional<rs::ResponsesStreamFailureKind> failureKindForResponsesAttemptTrigger(const ResponsesAttemptTrigger& trigger) {
  if (trigger == "http_4xx") return "upstream_http_4xx";
  if (trigger == "http_5xx") return "upstream_http_5xx";
  if (trigger == "http_error") return "upstream_http_error";
  if (trigger == "premature_eof" || trigger == "malformed_event" || trigger == "event_too_large" || trigger == "empty_upstream_completion") return trigger;
  if (trigger == "semantic_timeout") return "inactivity_timeout";
  if (trigger == "read_error" || trigger == "missing_body") return "read_error";
  return std::nullopt;
}
rt::Task<ResponsesAttemptResult> prepareResponsesAttempt(rt::Response response, std::string provider, inference_deadline::StreamDeadline deadline,
  rt::AbortSignal requestSignal, std::vector<std::string> warnings) {
  const PrepareResponsesAttemptOptions options;
  auto task = prepareResponsesAttempt(response, provider, deadline, requestSignal, warnings, options); auto awaitedResult1 = co_await std::move(task); co_return awaitedResult1;
}
rt::Task<ResponsesAttemptResult> prepareResponsesAttempt(rt::Response response, std::string provider, inference_deadline::StreamDeadline deadline,
  rt::AbortSignal requestSignal, std::vector<std::string> warnings, PrepareResponsesAttemptOptions options) {
  auto fail = [&](const std::string& trigger, rt::Response failedResponse, rt::Nullable<rs::ResponsesStreamEvent> terminal = nullptr) -> ResponsesAttemptResult {
    deadline.clear();
    return ResponsesAttemptResultFailed{{provider, safeFailedAttemptResponse(failedResponse, provider, trigger, warnings), trigger, terminal, deadline.signal, deadline.clear}};
  };
  if (!response.ok()) {
    const auto trigger = responsesHttpErrorTrigger(response.status());
    auto task = upstream_wire::toOpenAiUpstreamErrorResponse(response, provider, deadline.signal);
    auto normalized = co_await std::move(task); co_await rt::microtask(); deadline.clear(); co_return fail(trigger, normalized);
  }
  if (!response.body()) { deadline.clear(); co_return fail("missing_body", response); }
  std::optional<fs::PreparedResponsesStream> preparedStream;
  rt::Reason error;
  try {
    if (options.codexStreamPreflight && options.codexStreamPreflight->error) rt::rethrow(options.codexStreamPreflight->error);
    if (options.codexStreamPreflight && options.codexStreamPreflight->prepared) {
      preparedStream = std::exchange(options.codexStreamPreflight->prepared, std::nullopt);
    } else {
      rs::ReadResponsesStreamOptions readOptions;
      readOptions.firstEventTimeoutMs = rt::Math::ceil(deadline.remainingMs());
      auto iterator = rs::readResponsesStream(*response.body(), deadline.signal, readOptions);
      fs::PrepareResponsesStreamForCommitOptions prepareOptions;
      prepareOptions.onEvent = [context = options.usageContext](const rs::ResponsesStreamEvent& event) { tel::recordFirstUpstreamSseEvent(context); tel::recordResponsesEventTelemetry(context, event); };
      prepareOptions.releaseOnProgress = options.releaseOnProgress;
      auto task = fs::prepareResponsesStreamForCommit(iterator, prepareOptions);
      preparedStream = co_await std::move(task);
    }
    co_await rt::microtask();
    auto& prepared = *preparedStream;
    auto iterator = prepared.iterator;
    if (prepared.terminal && prepared.terminal->type == "response.completed" && !prepared.semantic) {
      auto closeTask = closeIterator(iterator); co_await std::move(closeTask); co_return fail("empty_upstream_completion", response, *prepared.terminal);
    }
    if (prepared.terminal && !prepared.semantic && (options.rejectFailedTerminal.value_or(false) || options.rejectPresemanticFailureTerminal.value_or(false)) &&
      (prepared.terminal->type == "response.failed" || prepared.terminal->type == "error")) {
      deadline.clear(); co_return fail(options.rejectPresemanticFailureTerminal.value_or(false) ? "terminal_failure" : "read_error", response);
    }
    std::optional<std::string> selectedModel; std::optional<std::string> taskType;
    for (const auto& event : prepared.buffered) {
      const auto identity = applyResponsesEventIdentity(event, selectedModel, taskType);
      if (identity.modelConflict) { auto closeTask = closeIterator(iterator); co_await std::move(closeTask); co_return fail(responsesAttemptTriggerFor(prepared.semantic, "invalid_model"), response); }
      selectedModel = identity.selectedModel; taskType = identity.taskType;
    }
    co_await rt::microtask();
    auto responseId = fs::responseIdFromEvents(prepared.buffered);
    auto bufferedChars = prepared.bufferedChars; auto terminal = prepared.terminal;
    while (options.requireEligibleModel.value_or(false) && (!selectedModel || selectedModel->empty() || !responseId || responseId->empty()) && !terminal) {
      auto next = co_await iterator.next(); if (next.done) break;
      tel::recordResponsesEventTelemetry(options.usageContext, *next.value);
      bufferedChars = fs::appendResponsesPrecommitEvent(prepared.buffered, *next.value, bufferedChars);
      const std::vector<rs::ResponsesStreamEvent> events{*next.value}; const auto candidateId = fs::responseIdFromEvents(events);
      if (candidateId && !candidateId->empty() && responseId && !responseId->empty() && candidateId != responseId) {
        auto closeTask = closeIterator(iterator); co_await std::move(closeTask); co_return fail(responsesAttemptTriggerFor(prepared.semantic, "malformed_event"), response);
      }
      if (!responseId) responseId = candidateId;
      const auto identity = applyResponsesEventIdentity(*next.value, selectedModel, taskType);
      if (identity.modelConflict) { auto closeTask = closeIterator(iterator); co_await std::move(closeTask); co_return fail(responsesAttemptTriggerFor(prepared.semantic, "invalid_model"), response); }
      selectedModel = identity.selectedModel; taskType = identity.taskType;
      if (next.value->terminal) terminal = *next.value;
    }
    co_await rt::microtask();
    if (options.rejectFailedTerminal.value_or(false) && terminal && !prepared.semantic && (terminal->type == "response.failed" || terminal->type == "error")) {
      auto closeTask = closeIterator(iterator); co_await std::move(closeTask); co_return fail("read_error", response);
    }
    if (options.requireEligibleModel.value_or(false)) {
      const bool created = std::any_of(prepared.buffered.begin(), prepared.buffered.end(), [](const auto& event) { return event.type == "response.created"; });
      if (!created || !responseId || responseId->empty()) { auto closeTask = closeIterator(iterator); co_await std::move(closeTask); co_return fail(responsesAttemptTriggerFor(prepared.semantic, "malformed_event"), response); }
    }
    deadline.clear();
    if (options.requireEligibleModel.value_or(false) && (!selectedModel || selectedModel->empty() || !removed::isEligibleRemovedProviderModel(*selectedModel))) {
      auto closeTask = closeIterator(iterator); co_await std::move(closeTask); co_return fail(responsesAttemptTriggerFor(prepared.semantic, "invalid_model"), response);
    }
    co_await rt::microtask();
    auto sanitized = prepared.buffered;
    if (options.requireEligibleModel.value_or(false)) for (auto& event : sanitized) {
      const auto& value = removed::stripRemovedProviderMetadata(event.value); if (&value != &event.value) event = fs::responseEventFromValue(value);
    }
    if (terminal) for (std::size_t i = 0; i < prepared.buffered.size(); ++i) if (prepared.buffered[i].identity() == terminal->identity()) { terminal = sanitized[i]; break; }
    prepared.iterator = options.requireEligibleModel.value_or(false) ? sanitizedIterator(iterator) : iterator;
    prepared.buffered = std::move(sanitized); prepared.bufferedChars = bufferedChars; prepared.terminal = terminal;
    co_return ResponsesAttemptResultReady{{provider, response, prepared, responseId, selectedModel, taskType, deadline.signal, deadline.abort, deadline.clear}};
  } catch (...) { error = std::current_exception(); }
  if (preparedStream) { auto closeTask = closeIterator(preparedStream->iterator); co_await std::move(closeTask); }
  deadline.clear();
  if (requestSignal.aborted()) std::rethrow_exception(requestSignal.reason() ? requestSignal.reason() : error);
  co_return fail(preparedStream && preparedStream->semantic ? "terminal_failure" : triggerForResponsesError(error, deadline.signal), response);
}
tel::ResponseStreamTerminalType responseFailureTerminalType(const ResponsesAttemptTrigger& trigger, const rt::AbortSignal& signal, const rt::AbortSignal& downstreamSignal) {
  if (trigger == "semantic_timeout" || tel::isTimeoutFailure(signal.reason(), downstreamSignal.reason())) return "deadline";
  if (downstreamSignal.aborted()) return "cancelled";
  if (signal.aborted()) return "deadline";
  if (trigger == "premature_eof") return "eof";
  if (trigger == "terminal_failure" || trigger == "empty_upstream_completion") return "response.failed";
  return "error";
}
rt::Task<PrimaryResponsesResult> fetchAndPreparePrimaryResponses(json::object body, PrimaryResponsesOptions options) {
  auto deadline = options.attemptDeadline;
  const auto codexPreflight = options.clientWantsStream ? std::make_shared<paid_fallback::routing::CodexStreamPreflight>() : nullptr;
  std::optional<tel::RoutedResponsesUpstream> routed;
  rt::Reason error;
  try {
    paid_fallback::routing::FetchResponsesWithPaidFallbackOptions routeOptions;
    routeOptions.model = options.model; routeOptions.route = "responses"; routeOptions.stream = options.clientWantsStream;
    routeOptions.reasoning = options.reasoning; routeOptions.usageContext = options.usageContext; routeOptions.clientVersion = options.clientVersion;
    routeOptions.signal = deadline.signal; routeOptions.fallbackSignal = options.fallbackSignal;
    routeOptions.allowedPaidProviders = options.allowedPaidProviders; routeOptions.deepSeekWaterfallPaidHop = options.deepSeekWaterfallPaidHop;
    routeOptions.codexStreamPreflight = codexPreflight;
    auto task = paid_fallback::routing::fetchResponsesWithPaidFallback(body, routeOptions); routed = co_await std::move(task); co_await rt::microtask();
  } catch (...) { error = std::current_exception(); }
  if (error) {
    deadline.clear();
    if (options.requestSignal.aborted() || isDispatchError(error)) std::rethrow_exception(error);
    const auto* codexError = codex::index::codexErrorOf(error);
    if (!codexError || (codexError->code != "gateway_timeout" && codexError->code != "codex_upstream_unreachable")) std::rethrow_exception(error);
    upstream_wire::logRedactedUpstreamError("[ai.ubq.fi] Upstream fetch failed:", error);
    auto response = upstream_wire::toCodexErrorResponse(error, "chatgpt_codex");
    const std::string trigger = codexError->code == "gateway_timeout" ? "semantic_timeout" : "read_error";
    tel::RoutedResponsesUpstream failedRoute{.response = response, .provider = "chatgpt_codex", .paidFallback = std::nullopt, .gatewayResponse = false, .fallbackReason = std::nullopt};
    FailedResponsesAttempt failed{.provider = "chatgpt_codex", .response = response, .trigger = trigger, .signal = deadline.signal, .clearDeadline = deadline.clear};
    co_return PrimaryResponsesFailed{{failedRoute, failed, paid_fallback::health::createMeteredTransportLifecycle(std::nullopt)}};
  }
  auto preparationDeadline = deadline;
  if (routed->provider != "chatgpt_codex" && options.createFallbackDeadline) { deadline.clear(); preparationDeadline = options.createFallbackDeadline(); }
  auto lifecycle = paid_fallback::health::createMeteredTransportLifecycle(routed->paidFallback, routed->provider,
    routed->paidFallbackProviderRequestId.toOptional(), routed->paidFallbackBilling.toOptional(), options.model,
    routed->providerHealthOnly.value_or(false), routed->paidFallbackErrorHealth);
  if (routed->gatewayResponse) {
    preparationDeadline.clear();
    auto trigger = routed->allowRemovedProviderRecovery == false ? std::string("read_error") : routed->response.status() == 504 ? std::string("semantic_timeout") : responsesHttpErrorTrigger(routed->response.status());
    FailedResponsesAttempt failed{.provider = routed->provider, .response = routed->response, .trigger = trigger, .signal = preparationDeadline.signal, .clearDeadline = preparationDeadline.clear};
    co_return PrimaryResponsesFailed{{*routed, failed, lifecycle}};
  }
  std::optional<ResponsesAttemptResult> prepared;
  try {
    auto warnings = options.warnings; const auto extra = request_policy::responseWarnings(routed->response); warnings.insert(warnings.end(), extra.begin(), extra.end());
    PrepareResponsesAttemptOptions prepareOptions;
    prepareOptions.usageContext = options.usageContext; prepareOptions.rejectPresemanticFailureTerminal = options.rejectPresemanticFailureTerminal;
    prepareOptions.releaseOnProgress = options.releaseOnProgress.value_or(false) && tel::supportsReasoningProgressRelease(routed->provider);
    if (routed->provider == "chatgpt_codex" && codexPreflight) {
      prepareOptions.codexStreamPreflight = codexPreflight;
      if (codexPreflight->prepared) prepareOptions.rejectPresemanticFailureTerminal = false;
    }
    auto task = prepareResponsesAttempt(routed->response, routed->provider, preparationDeadline, options.requestSignal, warnings, prepareOptions);
    prepared = co_await std::move(task); co_await rt::microtask();
  } catch (...) { error = std::current_exception(); }
  if (error) {
    if (options.requestSignal.aborted()) {
      FinalizeAbandonedPrimaryAttemptOptions abandon; abandon.cancelled = tel::classifyPreHeaderFailure(error, options.requestSignal, options.downstreamSignal) == "cancelled";
      auto task = finalizeAbandonedPrimaryAttempt(*routed, lifecycle, abandon); co_await std::move(task); co_await rt::microtask();
    }
    std::rethrow_exception(error);
  }
  if (const auto* ready = std::get_if<ResponsesAttemptResultReady>(&*prepared)) co_return PrimaryResponsesReady{{*routed, ready->attempt, lifecycle}};
  co_return PrimaryResponsesFailed{{*routed, std::get<ResponsesAttemptResultFailed>(*prepared).attempt, lifecycle}};
}
rt::Task<ResponsesAttemptResult> fetchAndPrepareRemovedProviderResponses(json::object body, RemovedProviderResponsesOptions options) {
  const auto deadline = options.attemptDeadline;
  tel::recordAttemptedProvider(options.usageContext, "removed_provider"); tel::selectRemovedProviderTelemetry(options.usageContext);
  std::optional<rt::Response> response; rt::Reason error;
  try {
    removed::RemovedProviderResponsesOptions fetchOptions{.apiKey = options.apiKey, .sessionId = options.sessionId, .signal = deadline.signal, .timing = std::nullopt, .beforeDispatch = {}};
    fetchOptions.timing = removed::RemovedProviderResponseTiming{[context = options.usageContext] { tel::recordFirstProviderDispatch(context); }, [context = options.usageContext] { tel::recordFirstProviderHeaders(context); }};
    fetchOptions.beforeDispatch = [context = options.usageContext] {
      if (!context || !context->beforeProviderDispatch) return rt::Promise<void>::resolve();
      auto task = context->beforeProviderDispatch("removed_provider");
      return rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>>(std::move(task)).then([](const auto&) {});
    };
    auto result = co_await removed::fetchRemovedProviderResponses(body, fetchOptions).wait(); response = result.response;
  } catch (...) { error = std::current_exception(); }
  if (error) {
    deadline.clear(); if (isDispatchError(error)) std::rethrow_exception(error);
    if (options.requestSignal.aborted()) std::rethrow_exception(options.requestSignal.reason() ? options.requestSignal.reason() : error);
    auto failedResponse = tel::streamErrorResponse(502, "RemovedProvider request failed before response headers were received.", "server_error", "removed_provider", {});
    co_return ResponsesAttemptResultFailed{{.provider = "removed_provider", .response = failedResponse, .trigger = triggerForResponsesError(error, deadline.signal), .signal = deadline.signal, .clearDeadline = deadline.clear}};
  }
  PrepareResponsesAttemptOptions prepareOptions; prepareOptions.usageContext = options.usageContext; prepareOptions.requireEligibleModel = true; prepareOptions.rejectFailedTerminal = true;
  const std::string provider = "removed_provider"; const std::vector<std::string> warnings;
  auto task = prepareResponsesAttempt(*response, provider, deadline, options.requestSignal, warnings, prepareOptions); auto awaitedResult2 = co_await std::move(task); co_return awaitedResult2;
}
rt::Task<void> finalizeAbandonedPrimaryAttempt(tel::RoutedResponsesUpstream routed, tel::MeteredTransportLifecycle lifecycle) {
  const FinalizeAbandonedPrimaryAttemptOptions options;
  auto task = finalizeAbandonedPrimaryAttempt(routed, lifecycle, options); co_await std::move(task);
}
rt::Task<void> finalizeAbandonedPrimaryAttempt(tel::RoutedResponsesUpstream routed, tel::MeteredTransportLifecycle lifecycle, FinalizeAbandonedPrimaryAttemptOptions options) {
  if (routed.provider == "chatgpt_codex") {
    auto transition = routed.response.ok() && !options.cancelled.value_or(false) ? codex::index::markCodexResponseUpstreamError(routed.response) : codex::index::releaseCodexResponseProbe(routed.response);
    try { rt::Promise<void> pending(std::move(transition)); co_await pending.wait(); } catch (...) {}
    co_await rt::microtask();
  } else if ((routed.provider == "metered" || routed.provider == "surplus") && !routed.gatewayResponse) {
    if (options.cancelled.value_or(false)) lifecycle.cancelled();
    else if (options.failureTrigger == "http_5xx" || options.failureTrigger == "terminal_failure" || options.failureTrigger == "empty_upstream_completion") lifecycle.terminal("response.failed", std::nullopt);
    else lifecycle.ambiguous();
  }
}
void markPrimarySemanticRecovery(const tel::RoutedResponsesUpstream& routed, const std::optional<circuit::RemovedProviderCircuitProbe>& circuitProbe,
  const tel::UsageContext* usageContext, std::optional<std::string> terminalType) {
  if (!circuitProbe) return;
  auto transition = routed.provider != "chatgpt_codex" ? circuit::releaseRemovedProviderCircuitProbe(circuitProbe) :
    terminalType == "response.failed" ? circuit::recordRemovedProviderEligibleFailure(circuitProbe) : circuit::closeRemovedProviderCircuit(circuitProbe);
  (void)transition.then([usageContext](const std::string& value) { responses_handler::recordRemovedProviderCircuitTransition(usageContext, value); }).catch_([](rt::Reason) {});
}
}
