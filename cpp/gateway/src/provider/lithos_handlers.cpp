// Port of src/provider/lithos-handlers.ts.
#include "uos/provider/lithos_handlers.hpp"
#include "uos/deepseek/context_admission_failure.hpp"
#include "uos/deepseek/handlers.hpp"
#include "uos/deepseek/chat_projection.hpp"
#include "uos/deepseek/responses_payload.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/lithos_streams.hpp"
#include "uos/provider/health.hpp"
#include "uos/openai.hpp"
#include "uos/http.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/request_policy.hpp"
#include "uos/bounded_response_body.hpp"
#include "uos/chat/stream_translation.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/clock.hpp"
#include <memory>

namespace uos::provider::lithos {
  class RequestDeadline {
  public:
    RequestDeadline()
      : timer_(rt::detail::createExactTimer([controller = controller_] {
          controller.abort(rt::timeoutErrorReason());
        }, inference_deadline::INFERENCE_DEADLINE_MS)) {
      timer_.unref();
    }
    ~RequestDeadline() { rt::clearTimeout(timer_); }
    RequestDeadline(const RequestDeadline&) = delete;
    RequestDeadline& operator=(const RequestDeadline&) = delete;

    rt::AbortSignal signal() const { return controller_.signal(); }

  private:
    rt::AbortController controller_;
    rt::Timeout timer_;
  };
}

namespace uos::provider::lithos_handlers {
  using namespace openai_telemetry;
  using namespace upstream_wire;
  using namespace lithos;
  using namespace lithos_streams;
  using namespace lithos_rate_limits;
  const rt::OrderedSet<std::string> LITHOS_REASONING_LEVEL_SET = [] { rt::OrderedSet<std::string> result; for (auto level : LITHOS_REASONING_LEVELS) result.add(std::string(level)); return result; }();
  namespace {
    std::function<void(std::weak_ptr<rt::AbortController>)> pendingCancellationObserver;
    constexpr double LITHOS_BUFFERED_BODY_MAX_BYTES = 8 * 1024 * 1024;
    struct LithosChatCompletionOk { json::object value; };
    struct LithosResponseNotOk { rt::Response response; };
    using LithosChatCompletionResult = std::variant<LithosChatCompletionOk, LithosResponseNotOk>;
    std::optional<std::string> lithosReasoningLevel(const std::string& value) {
      auto level = rt::toLowerCase(rt::trim(value)); return LITHOS_REASONING_LEVEL_SET.has(level) ? std::optional<std::string>(level) : std::nullopt;
    }
    struct ChatFields { std::string reasoning; bool clientWantsStream; };
    std::variant<ChatFields, rt::Response> validateLithosChatRequestFields(const json::object& rawRecord) {
      const auto* messages = rt::getArray(rawRecord, "messages");
      if (!messages) return http::openaiError(400, "messages must be an array", "invalid_request_error");
      if (messages->empty()) return http::openaiError(400, "messages must be a non-empty array", "invalid_request_error");
      for (const auto& message : *messages) if (!message.is_object()) return http::openaiError(400, "messages must contain objects", "invalid_request_error", {.param = "messages"});
      auto effort = request_policy::parseReasoningEffortField(rt::get(rawRecord, "reasoning_effort"), "reasoning_effort");
      if (auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&effort)) return http::openaiError(400, failure->message, "invalid_request_error", {.param = "reasoning_effort"});
      auto requested = std::get<request_policy::ParseReasoningEffortFieldOk>(effort).value;
      auto reasoning = requested ? lithosReasoningLevel(*requested) : std::optional<std::string>(LITHOS_DEFAULT_REASONING_EFFORT);
      if (!reasoning) return http::openaiError(400, "reasoning_effort '" + *requested + "' is not supported by LithosAI. Use none, minimal, low, medium, high, xhigh, or max.", "invalid_request_error", {.param = "reasoning_effort"});
      auto stream = request_policy::parseStreamField(rt::get(rawRecord, "stream"));
      if (auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&stream)) return http::openaiError(400, failure->message, "invalid_request_error", {.param = "stream"});
      auto options = request_policy::parseChatStreamOptions(rt::get(rawRecord, "stream_options"));
      if (auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&options)) return http::openaiError(400, failure->message, "invalid_request_error", {.param = "stream_options"});
      return ChatFields{*reasoning, std::get<request_policy::ParseStreamFieldOk>(stream).value};
    }
  rt::Task<rt::Response> respondLithosChatInvalidCompletion(std::string failureKind, double upstreamStatus,
    std::optional<std::string> providerRequestId, const UsageContext* usageContext) {
    recordStreamTerminalType(usageContext, "error");
    recordLithosFailureKind(usageContext, failureKind);
    health::recordLithosProviderHealth("upstream_error", upstreamStatus, {}, providerRequestId);
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    co_return http::openaiError(502, "Upstream returned an invalid Chat Completions response.", "lithos_upstream_invalid_response",
      {.type = "server_error", .headers = lithosResponseHeaders(providerRequestId)});
  }
  rt::Task<LithosChatCompletionResult> readLithosChatCompletion(rt::Bytes bytes, double upstreamStatus,
    std::optional<std::string> providerRequestId, const UsageContext* usageContext, std::string model, std::string servedModel) {
    json::value payload; bool invalidJson = false;
    try { payload = rt::jsonParse(rt::decodeUtf8(bytes)); } catch (...) { invalidJson = true; }
    if (invalidJson) {
      auto task = respondLithosChatInvalidCompletion("invalid_json", upstreamStatus, providerRequestId, usageContext);
      auto response = co_await std::move(task); co_return LithosResponseNotOk{std::move(response)};
    }
    auto normalized = normalizeLithosChatCompletion(payload, model, servedModel);
    if (std::holds_alternative<NormalizationResultNotOk>(normalized)) {
      auto task = respondLithosChatInvalidCompletion("invalid_completion_schema", upstreamStatus, providerRequestId, usageContext);
      auto response = co_await std::move(task); co_return LithosResponseNotOk{std::move(response)};
    }
    co_return LithosChatCompletionOk{std::get<NormalizationResultOk<json::object>>(normalized).value};
  }
  rt::Task<rt::Response> respondLithosChatDispatchFailure(rt::Reason error, rt::AbortSignal downstreamSignal, const UsageContext* usageContext) {
    auto terminalType = lithosTerminalTypeForError(error, downstreamSignal);
    recordLithosFailureKind(usageContext, lithosTransportFailureKind(error, terminalType));
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType != "cancelled") health::recordLithosProviderHealth("upstream_error", std::nullopt);
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    co_return toLithosErrorResponse(error);
  }
  rt::Task<rt::Response> respondLithosChatUpstreamHttpFailure(rt::Response upstream, rt::AbortSignal requestSignal,
    std::optional<std::string> providerRequestId, const UsageContext* usageContext, json::object body) {
    recordLithosResponseHealth(upstream.status(), providerRequestId);
    recordLithosFailureKind(usageContext, "upstream_http_error");
    recordStreamTerminalType(usageContext, "response.failed");
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    auto bodyDiagnostic = deepSeekChatBodyDiagnostic(body);
    auto task = toLithosUpstreamErrorResponse(upstream, requestSignal, bodyDiagnostic); auto response = co_await std::move(task); co_return response;
  }
  rt::Task<rt::Response> respondLithosChatIncompleteCapture(const UsageContext* usageContext, rt::AbortSignal downstreamSignal,
    rt::AbortSignal requestSignal, std::optional<std::string> providerRequestId) {
    std::string terminalType = "error", failureKind = "incomplete_response";
    if (downstreamSignal.aborted()) { terminalType = "cancelled"; failureKind = "cancellation"; }
    else if (requestSignal.aborted()) { terminalType = "deadline"; failureKind = "deadline"; }
    recordLithosFailureKind(usageContext, failureKind); recordStreamTerminalType(usageContext, terminalType);
    if (terminalType != "cancelled") health::recordLithosProviderHealth("upstream_error", std::nullopt, {}, providerRequestId);
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    if (terminalType == "cancelled") co_return http::openaiError(499, "Request was cancelled.", "request_cancelled",
      {.type = "server_error", .headers = lithosResponseHeaders(providerRequestId)});
    co_return http::openaiError(terminalType == "deadline" ? 504 : 502,
      terminalType == "deadline" ? "Upstream request exceeded the gateway deadline." : "Upstream returned an incomplete response.",
      terminalType == "deadline" ? "gateway_timeout" : "lithos_upstream_invalid_response",
      {.type = "server_error", .headers = lithosResponseHeaders(providerRequestId)});
  }
    struct LithosDispatchOk {
      rt::Response upstream; std::optional<std::string> providerRequestId; rt::AbortSignal requestSignal; rt::AbortSignal downstreamSignal; std::string servedModel;
    };
    struct LithosDispatchFailed { rt::Response response; rt::AbortSignal requestSignal; rt::AbortSignal downstreamSignal; };
    using LithosDispatchOutcome = std::variant<LithosDispatchOk, LithosDispatchFailed>;
    struct LithosDispatchInput { json::object body; std::string modelRaw; rt::AbortSignal requestSignal; rt::AbortSignal downstreamSignal; const UsageContext* usageContext; std::shared_ptr<const UsageContext> ownedUsageContext{}; std::shared_ptr<rt::AbortController> ownedCancellation{}; };
    struct LithosDispatchProgress {
      double attempt; std::string attemptModel; std::size_t ladderIndex; rt::OrderedSet<std::string> noRetryModels; double waitedMs; double elapsedWaitMs; double waits;
    };
    rt::Task<double> waitForLithosRateLimit(LithosDispatchInput input, double plannedWaitMs, double elapsedWaitMs) {
      auto elapsed = std::make_shared<double>(0); rt::Reason failure;
      std::function<void(double)> measured = [elapsed](double measuredMs) { *elapsed = measuredMs; };
      auto pending = waitForLithosRetry(plannedWaitMs, input.requestSignal, measured);
      try { co_await pending.wait(); } catch (...) { failure = std::current_exception(); }
      recordLithosRateLimitWaitMs(input.usageContext, elapsedWaitMs + *elapsed);
      if (failure) std::rethrow_exception(failure);
      co_return *elapsed;
    }
    rt::Task<rt::Response> lithosDispatchAttempt(json::object body, std::string modelRaw, rt::AbortSignal requestSignal, const UsageContext* usageContext) {
      LithosChatCompletionsOptions options; options.signal = requestSignal;
      // Idempotent per request: a retry after a real dispatch is the same
      // reservation, so the second call cannot double-count the attempt.
      options.beforeDispatch = [usageContext] { return usageContext && usageContext->beforeProviderDispatch ? usageContext->beforeProviderDispatch("lithos")
        : rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>>::resolve(std::nullopt).wait(); };
      options.onDispatch = [usageContext] { recordAttemptedProvider(usageContext, "lithos"); recordFirstProviderDispatch(usageContext); };
      options.onHeaders = [usageContext] { recordFirstProviderHeaders(usageContext); };
      if (usageContext) options.sentinelUpstreamRecorder = usageContext->sentinelUpstreamRecorder;
      auto task = fetchLithosChatCompletions(body, modelRaw, options); auto response = co_await std::move(task); co_return response;
    }
    rt::Task<LithosDispatchOutcome> lithosAttemptOutcome(LithosDispatchInput input, rt::Response upstream, std::string servedModel) {
      auto providerRequestId = getLithosProviderRequestId(upstream);
      if (input.usageContext && input.usageContext->responseTelemetry) input.usageContext->responseTelemetry->providerRequestId = providerRequestId;
      if (!upstream.ok()) {
        auto task = respondLithosChatUpstreamHttpFailure(upstream, input.requestSignal, providerRequestId, input.usageContext, input.body); auto response = co_await std::move(task);
        co_return LithosDispatchFailed{std::move(response), input.requestSignal, input.downstreamSignal};
      }
      recordLithosFailoverModel(input.usageContext, servedModel == input.modelRaw ? std::nullopt : std::optional<std::string>(servedModel));
      co_return LithosDispatchOk{std::move(upstream), providerRequestId, input.requestSignal, input.downstreamSignal, std::move(servedModel)};
    }
    void lithosOpenFailoverWindowFrom(const std::string& modelRaw, const rt::Response& refusal, const std::string& target, const std::string& refusedRung) {
      auto window = lithosRateLimitWait(refusal.headers(), rt::dateNow()); if (!window) return;
      lithosOpenFailoverWindow(modelRaw, rt::dateNow(), window->waitMs, target, refusedRung);
    }
    json::value requestId(const UsageContext* usageContext) { return usageContext && usageContext->requestId ? json::value(*usageContext->requestId) : json::value(nullptr); }
    std::optional<LithosDispatchProgress> lithosFailoverProgress(const LithosDispatchInput& input, const LithosDispatchProgress& state) {
      auto ladder = lithosFailoverLadderFor(input.modelRaw);
      for (auto index = state.ladderIndex; index < ladder.size(); ++index) {
        const auto& next = ladder[index]; if (state.noRetryModels.has(next)) continue;
        logLithosRateLimitFailover(json::object{{"request_id", requestId(input.usageContext)}, {"model", input.modelRaw}, {"sibling_model", next}, {"attempt", rt::makeNumber(state.attempt)}});
        recordLithosFailoverModel(input.usageContext, next); auto result = state; result.attemptModel = next; result.ladderIndex = index + 1; return result;
      }
      return std::nullopt;
    }
    struct RetryStart { std::string attemptModel; std::size_t ladderIndex; };
    std::optional<RetryStart> lithosRetryStart(const std::string& modelRaw, const rt::OrderedSet<std::string>& noRetryModels) {
      if (!noRetryModels.has(modelRaw)) return RetryStart{modelRaw, 0};
      auto ladder = lithosFailoverLadderFor(modelRaw);
      for (std::size_t index = 0; index < ladder.size(); ++index) if (!noRetryModels.has(ladder[index])) return RetryStart{ladder[index], index + 1};
      return std::nullopt;
    }
    struct DispatchPassResult { LithosDispatchOutcome result; };
    struct DispatchPassRetry { LithosDispatchProgress state; };
    struct DispatchPassDefer { LithosRateLimitWait planned; LithosDispatchProgress state; };
    using LithosDispatchPass = std::variant<DispatchPassResult, DispatchPassRetry, DispatchPassDefer>;
    struct LithosPendingDispatch { rt::Promise<LithosDispatchOutcome> pending; rt::AbortSignal requestSignal; rt::AbortSignal downstreamSignal; stream_relay::CancelPending cancel; };
    using LithosDispatchResult = std::variant<LithosDispatchOk, LithosDispatchFailed, LithosPendingDispatch>;
    void logWait(const LithosDispatchInput& input, const LithosDispatchProgress& state, const LithosRateLimitWait& planned) {
      logLithosRateLimitWait(json::object{{"request_id", requestId(input.usageContext)}, {"model", input.modelRaw}, {"attempt", rt::makeNumber(state.attempt)},
        {"wait_ms", rt::makeNumber(planned.waitMs)}, {"wait_source", planned.source}, {"waited_ms", rt::makeNumber(state.waitedMs)}, {"cap_ms", rt::makeNumber(LITHOS_REFUSAL_WAIT_CAP_MS)}});
    }
    rt::Task<LithosDispatchPass> lithosDispatchPass(LithosDispatchInput input, LithosDispatchProgress state, LithosRefusalWaitPolicy policy, bool defer) {
      auto attemptTask = lithosDispatchAttempt(input.body, state.attemptModel, input.requestSignal, input.usageContext); auto attempt = co_await std::move(attemptTask);
      auto progressed = state; progressed.attempt += 1;
      if (attempt.status() != 429) { auto task = lithosAttemptOutcome(input, attempt, progressed.attemptModel); auto result = co_await std::move(task); co_return DispatchPassResult{std::move(result)}; }
      json::object logFields{{"request_id", requestId(input.usageContext)}, {"model", progressed.attemptModel}, {"attempt", rt::makeNumber(progressed.attempt)}};
      rt::objectAssign(logFields, lithosRateLimitSnapshot(attempt.headers())); logLithosRateLimitRefusal(logFields);
      auto noRetryModels = progressed.noRetryModels;
      if (lithosRetryProhibited(attempt.headers())) noRetryModels.add(progressed.attemptModel);
      auto eligible = progressed; eligible.noRetryModels = noRetryModels;
      auto failover = lithosFailoverProgress(input, eligible);
      // A refusal with a window deepens later requests to the next eligible rung.
      if (failover) { lithosOpenFailoverWindowFrom(input.modelRaw, attempt, failover->attemptModel, progressed.attemptModel); cancelResponseBody(attempt); co_return DispatchPassRetry{*failover}; }
      auto planned = lithosRefusalWait(attempt.headers(), progressed.waitedMs, progressed.waits, policy);
      if (planned) {
        auto retryStart = lithosRetryStart(input.modelRaw, noRetryModels);
        if (!retryStart) { auto task = lithosAttemptOutcome(input, attempt, progressed.attemptModel); auto result = co_await std::move(task); co_return DispatchPassResult{std::move(result)}; }
        LithosDispatchProgress nextState{progressed.attempt, retryStart->attemptModel, retryStart->ladderIndex, noRetryModels,
          progressed.waitedMs + planned->waitMs, progressed.elapsedWaitMs, progressed.waits + 1};
        // Open the SSE response now and spend the bounded refusal window behind it.
        if (defer) { cancelResponseBody(attempt); co_return DispatchPassDefer{*planned, nextState}; }
        logWait(input, nextState, *planned); cancelResponseBody(attempt);
        auto waitTask = waitForLithosRateLimit(input, planned->waitMs, progressed.elapsedWaitMs); auto elapsedWaitMs = co_await std::move(waitTask);
        nextState.elapsedWaitMs = progressed.elapsedWaitMs + elapsedWaitMs; co_return DispatchPassRetry{std::move(nextState)};
      }
      // A terminal refusal body belongs to the responder and must not be cancelled here.
      auto task = lithosAttemptOutcome(input, attempt, progressed.attemptModel); auto result = co_await std::move(task); co_return DispatchPassResult{std::move(result)};
    }
    rt::Task<LithosDispatchResult> runLithosDispatch(LithosDispatchInput input, LithosDispatchProgress start, LithosRefusalWaitPolicy policy, bool allowDefer);
    rt::Task<LithosDispatchOutcome> resumeLithosDispatch(LithosDispatchInput input, DispatchPassDefer pass, LithosRefusalWaitPolicy policy) {
      rt::Reason failure; std::optional<LithosDispatchOutcome> result;
      try {
        auto waitTask = waitForLithosRateLimit(input, pass.planned.waitMs, pass.state.elapsedWaitMs); auto elapsedWaitMs = co_await std::move(waitTask);
        auto next = pass.state; next.elapsedWaitMs += elapsedWaitMs;
        auto task = runLithosDispatch(input, next, policy, false); auto continued = co_await std::move(task);
        if (std::holds_alternative<LithosPendingDispatch>(continued)) throw rt::JsError("A resumed LithosAI dispatch cannot defer again.");
        if (auto* ready = std::get_if<LithosDispatchOk>(&continued)) result = *ready; else result = std::get<LithosDispatchFailed>(continued);
      } catch (...) { failure = std::current_exception(); }
      if (failure) { auto task = respondLithosChatDispatchFailure(failure, input.downstreamSignal, input.usageContext); auto response = co_await std::move(task); co_return LithosDispatchFailed{std::move(response), input.requestSignal, input.downstreamSignal}; }
      co_return *result;
    }
    rt::Task<LithosDispatchResult> runLithosDispatch(LithosDispatchInput input, LithosDispatchProgress start, LithosRefusalWaitPolicy policy, bool allowDefer) {
      auto state = start;
      for (;;) {
        std::optional<LithosDispatchPass> pass; rt::Reason failure;
        try { auto task = lithosDispatchPass(input, state, policy, allowDefer); pass = co_await std::move(task); } catch (...) { failure = std::current_exception(); }
        if (failure) { auto task = respondLithosChatDispatchFailure(failure, input.downstreamSignal, input.usageContext); auto response = co_await std::move(task); co_return LithosDispatchFailed{std::move(response), input.requestSignal, input.downstreamSignal}; }
        if (auto* terminal = std::get_if<DispatchPassResult>(&*pass)) {
          if (auto* ready = std::get_if<LithosDispatchOk>(&terminal->result)) co_return *ready;
          co_return std::get<LithosDispatchFailed>(terminal->result);
        }
        if (auto* deferred = std::get_if<DispatchPassDefer>(&*pass)) {
          logWait(input, deferred->state, deferred->planned); const auto cancellation = std::make_shared<rt::AbortController>();
          if (pendingCancellationObserver) pendingCancellationObserver(cancellation);
          auto requestSignal = rt::AbortSignal::any({input.requestSignal, cancellation->signal()});
          auto downstreamSignal = rt::AbortSignal::any({input.downstreamSignal, cancellation->signal()});
          auto pendingInput = input; pendingInput.requestSignal = requestSignal; pendingInput.downstreamSignal = downstreamSignal;
          pendingInput.ownedCancellation = cancellation;
          if (input.usageContext) {
            pendingInput.ownedUsageContext = std::make_shared<const UsageContext>(*input.usageContext);
            pendingInput.usageContext = pendingInput.ownedUsageContext.get();
          }
          rt::Promise<LithosDispatchOutcome> pending(resumeLithosDispatch(pendingInput, *deferred, policy));
          // The running dispatch owns its interrupt; a downstream listener must not own its own signal source.
          stream_relay::CancelPending cancel = [weak = std::weak_ptr(cancellation)](rt::Reason reason) {
            if (const auto active = weak.lock(); active && !active->signal().aborted()) active->abort(reason);
          };
          co_return LithosPendingDispatch{pending, requestSignal, downstreamSignal, std::move(cancel)};
        }
        state = std::get<DispatchPassRetry>(*pass).state;
      }
    }
    rt::Task<LithosDispatchResult> dispatchLithosUpstreamResult(rt::Request req, json::object body, std::string modelRaw,
      const UsageContext* usageContext, const RequestDeadline& deadline, bool streamed,
      std::optional<LithosRefusalWaitPolicy> refusalWaitPolicy = std::nullopt) {
      auto downstreamSignal = openai::downstreamSignalFor(req, usageContext);
      auto requestSignal = rt::AbortSignal::any({downstreamSignal, deadline.signal()});
      auto canonicalModel = lithosUpstreamModelFor(modelRaw).value_or(rt::trim(modelRaw));
      auto ladder = lithosFailoverLadderFor(modelRaw); auto stickyTarget = lithosFailoverTargetAt(modelRaw, rt::dateNow());
      std::size_t ladderIndex = 0;
      if (stickyTarget) {
        const auto position = std::find(ladder.begin(), ladder.end(), *stickyTarget); if (position != ladder.end()) ladderIndex = static_cast<std::size_t>(position - ladder.begin()) + 1;
        logLithosRateLimitFailover(json::object{{"request_id", requestId(usageContext)}, {"model", canonicalModel}, {"sibling_model", *stickyTarget}, {"attempt", 1}});
        recordLithosFailoverModel(usageContext, stickyTarget);
      }
      LithosDispatchInput input{std::move(body), canonicalModel, requestSignal, downstreamSignal, usageContext};
      LithosDispatchProgress start{0, stickyTarget.value_or(canonicalModel), ladderIndex, {}, 0, 0, 0};
      auto policy = refusalWaitPolicy.value_or(streamed ? LITHOS_STREAMED_REFUSAL_WAIT_POLICY : LITHOS_BUFFERED_REFUSAL_WAIT_POLICY);
      auto task = runLithosDispatch(input, start, policy, streamed); auto result = co_await std::move(task); co_return result;
    }
    std::string responseIdFor(const std::optional<std::string>& providerRequestId) {
      auto raw = providerRequestId ? *providerRequestId : rt::crypto::randomUUID(); std::string result = "resp_";
      for (const auto c : raw) if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { if (result.size() == 45) break; result += c; }
      return result;
    }
  }
  void setLithosPendingCancellationObserverForTest(std::function<void(std::weak_ptr<rt::AbortController>)> observer) {
    pendingCancellationObserver = std::move(observer);
  }
  rt::Task<rt::Response> handleLithosChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw, const UsageContext* usageContext) {
    auto parsed = validateLithosChatRequestFields(rawRecord); if (auto* refusal = std::get_if<rt::Response>(&parsed)) co_return *refusal;
    auto fields = std::get<ChatFields>(parsed); auto upstreamModel = lithosUpstreamModelFor(modelRaw);
    if (!upstreamModel) co_return http::openaiError(400, "The requested model is not configured.", "lithos_request_invalid", {.param = "model"});
    auto body = rawRecord; body["reasoning_effort"] = fields.reasoning; body["stream"] = fields.clientWantsStream;
    if (!fields.clientWantsStream) rt::objectDelete(body, "stream_options");
    if (usageContext && usageContext->responseTelemetry) { usageContext->responseTelemetry->provider = "lithos";
      usageContext->responseTelemetry->reasoning = fields.reasoning; usageContext->responseTelemetry->outputTokenAllowance = deepseek::handlers::deepSeekChatClientOutputAllowance(rawRecord); }
    RecordRequestUsageDetails details{modelRaw, "chat.completions", fields.clientWantsStream, fields.reasoning}; auto requestUsage = recordRequestUsage(usageContext, details); co_await requestUsage.wait();
    const auto deadline = std::make_shared<RequestDeadline>();
    auto dispatchTask = dispatchLithosUpstreamResult(req, body, modelRaw, usageContext, *deadline, fields.clientWantsStream); auto dispatched = co_await std::move(dispatchTask);
    if (auto* pending = std::get_if<LithosPendingDispatch>(&dispatched)) {
      auto servedModel = std::make_shared<std::string>(modelRaw);
      auto upstream = pending->pending.then([servedModel](const LithosDispatchOutcome& outcome) {
        if (const auto* ready = std::get_if<LithosDispatchOk>(&outcome)) { *servedModel = ready->servedModel; return ready->upstream; }
        return std::get<LithosDispatchFailed>(outcome).response;
      });
      std::function<std::string()> served = [servedModel] { return *servedModel; };
      co_return streamLithosChatCompletion(upstream, std::nullopt, usageContext, pending->downstreamSignal, pending->requestSignal, *upstreamModel, served, pending->cancel, deadline);
    }
    if (auto* failure = std::get_if<LithosDispatchFailed>(&dispatched)) {
      if (fields.clientWantsStream) co_return streamLithosChatCompletion(failure->response, std::nullopt, usageContext, failure->downstreamSignal, failure->requestSignal, *upstreamModel, modelRaw, {}, deadline);
      co_return failure->response;
    }
    auto result = std::get<LithosDispatchOk>(dispatched);
    if (fields.clientWantsStream) co_return streamLithosChatCompletion(result.upstream, result.providerRequestId, usageContext, result.downstreamSignal, result.requestSignal, *upstreamModel, result.servedModel, {}, deadline);
    bounded_response_body::ReadBoundedResponseBodyOptions captureOptions{.signal = result.requestSignal, .maxBytes = LITHOS_BUFFERED_BODY_MAX_BYTES,
      .timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS, .cancellationReason = "LithosAI Chat Completions response was incomplete"};
    auto captureTask = bounded_response_body::readBoundedResponseBody(result.upstream, captureOptions); auto captured = co_await std::move(captureTask);
    if (!captured.complete) { auto task = respondLithosChatIncompleteCapture(usageContext, result.downstreamSignal, result.requestSignal, result.providerRequestId); auto response = co_await std::move(task); co_return response; }
    auto completionTask = readLithosChatCompletion(captured.bytes, result.upstream.status(), result.providerRequestId, usageContext, *upstreamModel, result.servedModel); auto completion = co_await std::move(completionTask);
    if (auto* refusal = std::get_if<LithosResponseNotOk>(&completion)) co_return refusal->response;
    auto payload = std::get<LithosChatCompletionOk>(completion).value;
    if (chatCompletionHasAnswerBearingOutput(payload)) chat::stream_translation::markChatSemanticOutput(usageContext);
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = result.providerRequestId;
    auto usage = extractChatUsageTokens(rt::get(payload, "usage")); auto completionUsage = recordCompletionUsage(usageContext, usage); co_await completionUsage.wait();
    recordStreamTerminalType(usageContext, "response.completed"); recordLithosResponseHealth(result.upstream.status(), result.providerRequestId);
    co_return http::json(200, payload, lithosResponseHeaders(result.providerRequestId));
  }
  namespace {
    rt::Response respondLithosEmptyBufferedCompletion(const UsageContext* usageContext, std::optional<UsageTokens> usage, double upstreamStatus, const std::optional<std::string>& providerRequestId) {
      if (usageContext && usageContext->responseTelemetry) { usageContext->responseTelemetry->failureKind = "empty_upstream_completion"; usageContext->responseTelemetry->semanticOutputObserved = false; }
      recordTerminalUsage(usageContext, usage, false); recordStreamTerminalType(usageContext, "response.failed"); recordLithosResponseHealth(upstreamStatus, providerRequestId);
      return http::openaiError(502, std::string(chat::stream_translation::EMPTY_UPSTREAM_COMPLETION_MESSAGE), "empty_upstream_completion", {.type = "server_error", .headers = lithosResponseHeaders(providerRequestId)});
    }
    void recordBufferedLithosResponsesTerminal(const UsageContext* usageContext, const json::object& payload, std::optional<UsageTokens> usage,
      double upstreamStatus, const std::optional<std::string>& providerRequestId) {
      auto terminalType = deepseek::handlers::deepSeekTerminalTypeForPayload(rt::get(payload, "status"));
      recordTerminalUsage(usageContext, usage, terminalType == "response.completed"); recordStreamTerminalType(usageContext, terminalType);
      if (terminalType == "response.completed") recordStreamTerminal(usageContext);
      else { recordLithosFailureKind(usageContext, terminalType == "response.incomplete" ? "incomplete_response" : "upstream_error"); health::recordLithosProviderHealth("upstream_error", upstreamStatus, {}, providerRequestId); }
      recordLithosResponseHealth(upstreamStatus, providerRequestId);
    }
    struct BufferedLithosResponsesOptions {
      rt::Response upstream; std::string modelRaw; std::string responseId; deepseek::responses_payload::DeepSeekResponsesEcho echo;
      rt::OrderedMap<std::string, deepseek::responses::OriginalToolName> toolNames; rt::OrderedSet<std::string> customToolNames;
      std::string upstreamModel; std::string servedModel; std::optional<std::string> providerRequestId; rt::AbortSignal requestSignal; rt::AbortSignal downstreamSignal; const UsageContext* usageContext;
    };
    rt::Task<rt::Response> finalizeBufferedLithosResponses(BufferedLithosResponsesOptions options) {
      bounded_response_body::ReadBoundedResponseBodyOptions captureOptions{.signal = options.requestSignal, .maxBytes = LITHOS_BUFFERED_BODY_MAX_BYTES,
        .timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS, .cancellationReason = "LithosAI Responses adapter body was incomplete"};
      auto captureTask = bounded_response_body::readBoundedResponseBody(options.upstream, captureOptions); auto captured = co_await std::move(captureTask);
      if (!captured.complete) { auto task = respondLithosChatIncompleteCapture(options.usageContext, options.downstreamSignal, options.requestSignal, options.providerRequestId); auto response = co_await std::move(task); co_return response; }
      auto completionTask = readLithosChatCompletion(captured.bytes, options.upstream.status(), options.providerRequestId, options.usageContext, options.upstreamModel, options.servedModel); auto completion = co_await std::move(completionTask);
      if (auto* refusal = std::get_if<LithosResponseNotOk>(&completion)) co_return refusal->response;
      auto firstCompletion = std::get<LithosChatCompletionOk>(completion).value;
      if (options.usageContext && options.usageContext->responseTelemetry) options.usageContext->responseTelemetry->providerRequestId = options.providerRequestId;
      auto payload = deepseek::responses_payload::toDeepSeekResponsesPayload(firstCompletion, options.modelRaw, options.responseId, options.echo, options.toolNames, options.customToolNames, deepseek::responses::LITHOS_RESPONSES_PROFILE);
      auto usage = extractChatUsageTokens(rt::get(firstCompletion, "usage"));
      if (deepseek::handlers::deepSeekTerminalTypeForPayload(rt::get(payload, "status")) == "response.completed" && !chatCompletionHasAnswerBearingOutput(firstCompletion))
        co_return respondLithosEmptyBufferedCompletion(options.usageContext, usage, options.upstream.status(), options.providerRequestId);
      recordBufferedLithosResponsesTerminal(options.usageContext, payload, usage, options.upstream.status(), options.providerRequestId);
      co_return http::json(200, payload, lithosResponseHeaders(options.providerRequestId));
    }
  }
  rt::Task<rt::Response> handleLithosResponses(rt::Request req, json::object rawRecord, std::string modelRaw, const UsageContext* usageContext) {
    return handleLithosResponses(std::move(req), std::move(rawRecord), std::move(modelRaw), usageContext, {});
  }
  rt::Task<rt::Response> handleLithosResponses(rt::Request req, json::object rawRecord, std::string modelRaw, const UsageContext* usageContext, HandleLithosResponsesOptions options) {
    auto parsedStream = request_policy::parseStreamField(rt::get(rawRecord, "stream"));
    if (auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&parsedStream)) co_return http::openaiError(400, failure->message, "invalid_request_error", {.param = "stream"});
    auto clientWantsStream = std::get<request_policy::ParseStreamFieldOk>(parsedStream).value;
    auto upstreamModel = lithosUpstreamModelFor(modelRaw);
    if (!upstreamModel) co_return http::openaiError(400, "model '" + modelRaw + "' is not a LithosAI official model", "invalid_request_error", {.param = "model"});
    auto translated = deepseek::chat_projection::toDeepSeekResponsesChatBody(rawRecord, modelRaw, clientWantsStream, deepseek::responses::LITHOS_RESPONSES_PROFILE);
    if (auto* failure = std::get_if<deepseek::responses::DeepSeekResponsesFailure>(&translated)) co_return deepseek::context_admission_failure::chatResponsesProjectionFailure(req, rawRecord, *failure, usageContext);
    auto value = std::get<deepseek::responses::DeepSeekResponsesResultOk<deepseek::chat_projection::DeepSeekResponsesChatBody>>(translated).value;
    if (!value.elisions.empty()) deepseek::chat_projection::logForwardedPayloadElisions(value.elisions);
    if (!value.exemptions.empty()) deepseek::forwarded_payload_policy::logForwardedPayloadExemptions(value.exemptions,
      {.requestId = usageContext ? usageContext->requestId : std::nullopt, .model = modelRaw, .promptAdmission = value.promptAdmission});
    auto field = [&](const char* key) { const auto* item = rt::get(rawRecord, key); return item ? *item : json::value(nullptr); };
    auto instructions = rt::getString(rawRecord, "instructions");
    deepseek::responses_payload::DeepSeekResponsesEcho echo{field("tools"), field("tool_choice"), field("parallel_tool_calls"),
      instructions && !rt::trim(*instructions).empty() ? std::optional<std::string>(*instructions) : std::nullopt};
    auto reasoningLabel = std::string(rt::getString(value.body, "reasoning_effort").value_or(LITHOS_DEFAULT_REASONING_EFFORT)); value.body["reasoning_effort"] = reasoningLabel;
    if (usageContext && usageContext->responseTelemetry) { usageContext->responseTelemetry->provider = "lithos";
      usageContext->responseTelemetry->reasoning = reasoningLabel; usageContext->responseTelemetry->outputTokenAllowance = rt::getNumber(value.body, "max_tokens"); }
    RecordRequestUsageDetails details{modelRaw, "responses", clientWantsStream, reasoningLabel}; auto requestUsage = recordRequestUsage(usageContext, details); co_await requestUsage.wait();
    auto createdAtSeconds = rt::Math::floor(rt::dateNow() / 1000);
    const auto deadline = std::make_shared<RequestDeadline>();
    auto dispatchTask = dispatchLithosUpstreamResult(req, value.body, modelRaw, usageContext, *deadline, clientWantsStream, options.refusalWaitPolicy); auto dispatched = co_await std::move(dispatchTask);
    if (auto* pending = std::get_if<LithosPendingDispatch>(&dispatched)) {
      auto servedModel = std::make_shared<std::string>(modelRaw);
      auto upstream = pending->pending.then([servedModel](const LithosDispatchOutcome& outcome) {
        if (const auto* ready = std::get_if<LithosDispatchOk>(&outcome)) { *servedModel = ready->servedModel; return ready->upstream; }
        return std::get<LithosDispatchFailed>(outcome).response;
      });
      std::function<std::string()> served = [servedModel] { return *servedModel; };
      co_return streamLithosResponses(upstream, modelRaw, responseIdFor(std::nullopt), createdAtSeconds, echo, value.toolNames, value.customToolNames,
        std::nullopt, usageContext, pending->downstreamSignal, pending->requestSignal, *upstreamModel, served, pending->cancel, deadline);
    }
    if (auto* failure = std::get_if<LithosDispatchFailed>(&dispatched)) {
      if (clientWantsStream) co_return streamLithosResponses(failure->response, modelRaw, responseIdFor(std::nullopt), createdAtSeconds, echo, value.toolNames,
        value.customToolNames, std::nullopt, usageContext, failure->downstreamSignal, failure->requestSignal, *upstreamModel, modelRaw, {}, deadline);
      co_return failure->response;
    }
    auto result = std::get<LithosDispatchOk>(dispatched); auto responseId = responseIdFor(result.providerRequestId);
    if (clientWantsStream) co_return streamLithosResponses(result.upstream, modelRaw, responseId, createdAtSeconds, echo, value.toolNames, value.customToolNames,
      result.providerRequestId, usageContext, result.downstreamSignal, result.requestSignal, *upstreamModel, result.servedModel, {}, deadline);
    BufferedLithosResponsesOptions buffered{result.upstream, modelRaw, responseId, echo, value.toolNames, value.customToolNames, *upstreamModel,
      result.servedModel, result.providerRequestId, result.requestSignal, result.downstreamSignal, usageContext};
    auto task = finalizeBufferedLithosResponses(buffered); auto response = co_await std::move(task); co_return response;
  }
}
