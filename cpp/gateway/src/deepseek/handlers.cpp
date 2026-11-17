// Port of src/deepseek/handlers.ts.
#include "uos/deepseek/handlers.hpp"
#include "uos/openai.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/provider/health.hpp"
#include "uos/defaults.hpp"
#include "uos/request_policy.hpp"
#include "uos/bounded_response_body.hpp"
#include "uos/http.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/chat/stream_translation.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/clock.hpp"
#include "uos/utils.hpp"
#include "uos/deepseek/chat_projection.hpp"
#include "uos/deepseek/context_admission_failure.hpp"
#include "uos/deepseek/responses_payload.hpp"

namespace uos::deepseek::handlers {
  using namespace openai_telemetry;
  using namespace upstream_wire;
  using DeepSeekFailureKind = std::string;
  using namespace index;
  namespace {
    ResponseStreamTerminalType deepSeekTerminalTypeForError(rt::Reason error, const rt::AbortSignal& downstreamSignal) {
      if (downstreamSignal.aborted()) return "cancelled";
      if (error) try { std::rethrow_exception(error); } catch (const stream::DeepSeekStreamError& value) {
        if (value.kind == "inactivity_timeout") return "deadline";
        if (value.kind == "premature_eof") return "eof";
        return "error";
      } catch (const DeepSeekError& value) {
        if (value.status == 504) return "deadline";
      } catch (...) {}
      if (rt::isError(error) && rt::errorName(error) == "TimeoutError") return "deadline";
      if (rt::isError(error) && rt::errorName(error) == "AbortError") return "cancelled";
      return "error";
    }

  }

  void recordDeepSeekResponseHealth(double status, std::optional<std::string> providerRequestId) {
    const std::string event = status == 401 || status == 403 ? "auth_invalid" : status == 429 ? "quota_exhausted"
      : status >= 500 ? "upstream_error" : status >= 400 ? "reachable" : "success";
    provider::health::recordDeepSeekProviderHealth(event, status, {}, std::move(providerRequestId));
  }
  namespace {
  void recordDeepSeekFailureKind(const UsageContext* context, const DeepSeekFailureKind& failureKind) {
    if (context && context->responseTelemetry) context->responseTelemetry->failureKind = failureKind;
  }
    std::string deepSeekFinishReasonFailureKind(const std::optional<std::string>& finishReason) {
      static const rt::Regex accepted("^[A-Za-z0-9_.:-]{1,64}$");
      return "deepseek_finish_reason:" + (finishReason && accepted.test(*finishReason) ? *finishReason : "unrecognized");
    }
    DeepSeekFailureKind deepSeekTransportFailureKind(rt::Reason error, const ResponseStreamTerminalType& terminalType) {
      if (terminalType == "cancelled") return "cancellation";
      if (terminalType == "deadline") return "deadline";
      if (terminalType == "eof") return "incomplete_response";
      if (error) try { std::rethrow_exception(error); } catch (const api_key_policy::ApiKeyQuotaDispatchError&) {
        return "api_key_quota_reservation_unavailable";
      } catch (const stream::DeepSeekStreamError& value) {
        if (value.kind == "malformed_event") return "invalid_json";
        if (value.kind == "invalid_chunk") return "invalid_stream_chunk";
        if (value.kind == "premature_eof") return "incomplete_response";
        if (value.kind == "inactivity_timeout") return "deadline";
      } catch (const DeepSeekError& value) {
        if (value.code == "deepseek_api_key_missing" || value.code == "deepseek_request_invalid") return value.code;
        if (value.code == "deepseek_upstream_unreachable") return "upstream_unreachable";
        if (value.code == "gateway_timeout") return "deadline";
      } catch (...) {}
      return "upstream_unreachable";
    }
  rt::Task<rt::Response> respondDeepSeekChatInvalidCompletion(std::string failureKind, double upstreamStatus,
    std::optional<std::string> providerRequestId, const UsageContext* usageContext) {
    recordStreamTerminalType(usageContext, "error");
    recordDeepSeekFailureKind(usageContext, failureKind);
    provider::health::recordDeepSeekProviderHealth("upstream_error", upstreamStatus, {}, providerRequestId);
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    co_return http::openaiError(502, "Upstream returned an invalid Chat Completions response.", "deepseek_upstream_invalid_response",
      {.type = "server_error", .headers = deepseekResponseHeaders(providerRequestId)});
  }
  }
  rt::Task<DeepSeekChatCompletionResult> readDeepSeekChatCompletion(rt::Bytes bytes, double upstreamStatus,
    std::optional<std::string> providerRequestId, const UsageContext* usageContext, std::string model) {
    json::value payload; bool invalidJson = false;
    try { payload = rt::jsonParse(rt::decodeUtf8(bytes)); } catch (...) { invalidJson = true; }
    if (invalidJson) {
      auto task = respondDeepSeekChatInvalidCompletion("invalid_json", upstreamStatus, providerRequestId, usageContext);
      auto response = co_await std::move(task); co_return DeepSeekResponseNotOk{std::move(response)};
    }
    auto normalized = normalizeDeepSeekChatCompletion(payload, model);
    if (std::holds_alternative<NormalizationResultNotOk>(normalized)) {
      auto task = respondDeepSeekChatInvalidCompletion("invalid_completion_schema", upstreamStatus, providerRequestId, usageContext);
      auto response = co_await std::move(task); co_return DeepSeekResponseNotOk{std::move(response)};
    }
    co_return DeepSeekChatCompletionOk{std::get<NormalizationResultOk<json::object>>(normalized).value};
  }
  namespace {
  rt::Task<rt::Response> respondDeepSeekChatDispatchFailure(rt::Reason error, rt::AbortSignal downstreamSignal, const UsageContext* usageContext) {
    auto terminalType = deepSeekTerminalTypeForError(error, downstreamSignal);
    recordDeepSeekFailureKind(usageContext, deepSeekTransportFailureKind(error, terminalType));
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType != "cancelled") provider::health::recordDeepSeekProviderHealth("upstream_error", std::nullopt);
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    co_return toDeepSeekErrorResponse(error);
  }
  rt::Task<rt::Response> respondDeepSeekChatUpstreamHttpFailure(rt::Response upstream, rt::AbortSignal requestSignal,
    std::optional<std::string> providerRequestId, const UsageContext* usageContext, std::optional<json::object> bodyDiagnostic) {
    recordDeepSeekResponseHealth(upstream.status(), providerRequestId);
    recordDeepSeekFailureKind(usageContext, "upstream_http_error");
    recordStreamTerminalType(usageContext, "response.failed");
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    auto task = toDeepSeekUpstreamErrorResponse(upstream, requestSignal, bodyDiagnostic); auto response = co_await std::move(task); co_return response;
  }
  }
  rt::Task<rt::Response> respondDeepSeekChatIncompleteCapture(const UsageContext* usageContext, rt::AbortSignal downstreamSignal,
    rt::AbortSignal requestSignal, std::optional<std::string> providerRequestId) {
    std::string terminalType = "error", failureKind = "incomplete_response";
    if (downstreamSignal.aborted()) { terminalType = "cancelled"; failureKind = "cancellation"; }
    else if (requestSignal.aborted()) { terminalType = "deadline"; failureKind = "deadline"; }
    recordDeepSeekFailureKind(usageContext, failureKind); recordStreamTerminalType(usageContext, terminalType);
    if (terminalType != "cancelled") provider::health::recordDeepSeekProviderHealth("upstream_error", std::nullopt, {}, providerRequestId);
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    if (terminalType == "cancelled") co_return http::openaiError(499, "Request was cancelled.", "request_cancelled",
      {.type = "server_error", .headers = deepseekResponseHeaders(providerRequestId)});
    co_return http::openaiError(terminalType == "deadline" ? 504 : 502,
      terminalType == "deadline" ? "Upstream request exceeded the gateway deadline." : "Upstream returned an incomplete response.",
      terminalType == "deadline" ? "gateway_timeout" : "deepseek_upstream_invalid_response",
      {.type = "server_error", .headers = deepseekResponseHeaders(providerRequestId)});
  }

  rt::Response respondDeepSeekEmptyBufferedCompletion(const UsageContext* usageContext, std::optional<UsageTokens> usage,
    double upstreamStatus, std::optional<std::string> providerRequestId) {
    if (usageContext && usageContext->responseTelemetry) { usageContext->responseTelemetry->failureKind = "empty_upstream_completion"; usageContext->responseTelemetry->semanticOutputObserved = false; }
    recordTerminalUsage(usageContext, usage, false); recordStreamTerminalType(usageContext, "response.failed");
    recordDeepSeekResponseHealth(upstreamStatus, providerRequestId);
    return http::openaiError(502, std::string(chat::stream_translation::EMPTY_UPSTREAM_COMPLETION_MESSAGE), "empty_upstream_completion",
      {.type = "server_error", .headers = deepseekResponseHeaders(providerRequestId)});
  }
  void recordBufferedDeepSeekResponsesTerminal(const UsageContext* usageContext, const json::object& payload, std::optional<UsageTokens> usage,
    double upstreamStatus, std::optional<std::string> providerRequestId) {
    const auto terminalType = deepSeekTerminalTypeForPayload(rt::get(payload, "status"));
    recordTerminalUsage(usageContext, usage, terminalType == "response.completed"); recordStreamTerminalType(usageContext, terminalType);
    if (terminalType == "response.completed") recordStreamTerminal(usageContext);
    else { recordDeepSeekFailureKind(usageContext, terminalType == "response.incomplete" ? "incomplete_response" : "upstream_error");
      provider::health::recordDeepSeekProviderHealth("upstream_error", upstreamStatus, {}, providerRequestId); }
    recordDeepSeekResponseHealth(upstreamStatus, providerRequestId);
  }
  ResponseStreamTerminalType deepSeekTerminalTypeForPayload(const json::value* status) {
    if (status && status->is_string()) {
      if (status->as_string() == "incomplete") return "response.incomplete";
      if (status->as_string() == "failed") return "response.failed";
    }
    return "response.completed";
  }
  ResponseStreamTerminalType deepSeekTerminalTypeForPayload(const json::value& status) { return deepSeekTerminalTypeForPayload(&status); }
  DeepSeekChatRequestResult validateDeepSeekChatRequestFields(const json::object& rawRecord) {
    const auto* messages = rt::getArray(rawRecord, "messages");
    if (!messages) return DeepSeekResponseNotOk{http::openaiError(400, "messages must be an array", "invalid_request_error")};
    if (messages->empty()) return DeepSeekResponseNotOk{http::openaiError(400, "messages must be a non-empty array", "invalid_request_error")};
    for (const auto& message : *messages) if (!message.is_object()) return DeepSeekResponseNotOk{http::openaiError(400, "messages must contain objects", "invalid_request_error", {.param = "messages"})};
    const auto effort = request_policy::parseReasoningEffortField(rt::get(rawRecord, "reasoning_effort"), "reasoning_effort");
    if (const auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&effort)) return DeepSeekResponseNotOk{http::openaiError(400, failure->message, "invalid_request_error", {.param = "reasoning_effort"})};
    const auto parsedStream = request_policy::parseStreamField(rt::get(rawRecord, "stream"));
    if (const auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&parsedStream)) return DeepSeekResponseNotOk{http::openaiError(400, failure->message, "invalid_request_error", {.param = "stream"})};
    const auto options = request_policy::parseChatStreamOptions(rt::get(rawRecord, "stream_options"));
    if (const auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&options)) return DeepSeekResponseNotOk{http::openaiError(400, failure->message, "invalid_request_error", {.param = "stream_options"})};
    auto reasoning = std::get<request_policy::ParseReasoningEffortFieldOk>(effort).value;
    return DeepSeekChatRequestOk{{!reasoning && !deepSeekThinkingModeActive(nullptr, rt::get(rawRecord, "thinking")) ? "none"
      : reasoning.value_or(std::string(DEEPSEEK_DEFAULT_REASONING_EFFORT)), std::get<request_policy::ParseStreamFieldOk>(parsedStream).value}};
  }
  std::optional<double> deepSeekChatClientOutputAllowance(const json::object& rawRecord) {
    auto completionTokens = request_policy::parseMaxCompletionTokensField(rt::get(rawRecord, "max_completion_tokens"));
    if (std::holds_alternative<request_policy::ParseFieldNotOk>(completionTokens)) return std::nullopt;
    if (auto value = std::get<request_policy::ParseMaxCompletionTokensFieldOk>(completionTokens).value) return value;
    auto maxTokens = request_policy::parseMaxCompletionTokensField(rt::get(rawRecord, "max_tokens"));
    if (auto* parsed = std::get_if<request_policy::ParseMaxCompletionTokensFieldOk>(&maxTokens)) return parsed->value;
    return std::nullopt;
  }
  const provider::stream_relay::ProviderStreamAdapter deepseekStreamAdapter = [] {
    provider::stream_relay::ProviderStreamAdapter adapter;
    adapter.responseHeaders = [](std::optional<std::string> id) { return deepseekResponseHeaders(id); };
    adapter.frames = [](rt::Response response, std::string model, provider::stream_relay::ProviderStreamFrameOptions options) {
      stream::DeepSeekStreamOptions streamOptions; streamOptions.signal = options.signal;
      return provider::stream_relay::ProviderStreamIterator(stream::iterateDeepSeekChatCompletionStream(std::move(response), std::move(model), streamOptions));
    };
    adapter.recordResponseHealth = recordDeepSeekResponseHealth;
    adapter.recordProviderError = [](std::optional<double> status, std::optional<std::string> id) { provider::health::recordDeepSeekProviderHealth("upstream_error", status, {}, id); };
    adapter.recordCancellation = [](const UsageContext* context) { recordDeepSeekFailureKind(context, "cancellation"); };
    adapter.recordIncompleteResponse = [](const UsageContext* context) { recordDeepSeekFailureKind(context, "incomplete_response"); };
    adapter.recordFinishFailureKind = [](const UsageContext* context, std::optional<std::string> reason) { recordDeepSeekFailureKind(context, deepSeekFinishReasonFailureKind(reason)); };
    adapter.recordTransportFailure = [](const UsageContext* context, rt::Reason error, ResponseStreamTerminalType terminal) { recordDeepSeekFailureKind(context, deepSeekTransportFailureKind(error, terminal)); };
    adapter.terminalTypeForError = deepSeekTerminalTypeForError; adapter.streamErrorCode = "deepseek_upstream_stream_error";
    adapter.responsesProfile = responses::DEEPSEEK_RESPONSES_PROFILE; return adapter;
  }();
  rt::Response streamDeepSeekChatCompletion(rt::Response upstream, std::optional<std::string> providerRequestId, const UsageContext* usageContext,
    rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal, std::string upstreamModel) {
    return provider::stream_relay::relayChatCompletionStream(deepseekStreamAdapter, upstream, providerRequestId, usageContext, downstreamSignal, requestSignal, upstreamModel);
  }
  rt::Task<DeepSeekDispatchResult> dispatchDeepSeekUpstream(rt::Request req, json::object body, std::string modelRaw, const UsageContext* usageContext) {
    const auto downstreamSignal = openai::downstreamSignalFor(req, usageContext);
    // Streaming owns a renewable first-event/inactivity deadline in the stream
    // reader, while the DeepSeek transport separately bounds dispatch and
    // response headers. The buffered signal would cut off a healthy long stream.
    const auto requestSignal = rt::getBool(body, "stream") == true ? downstreamSignal : openai::inferenceSignal(req, usageContext);
    auto bodyDiagnostic = std::make_shared<std::optional<json::object>>();
    DeepSeekChatCompletionsOptions options; options.signal = requestSignal;
    options.onProjectedBody = [bodyDiagnostic](const json::object& projectedBody) { *bodyDiagnostic = deepSeekChatBodyDiagnostic(projectedBody); };
    options.beforeDispatch = [usageContext] { return usageContext && usageContext->beforeProviderDispatch ? usageContext->beforeProviderDispatch("deepseek")
      : rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>>::resolve(std::nullopt).wait(); };
    options.onDispatch = [usageContext] { recordAttemptedProvider(usageContext, "deepseek"); recordFirstProviderDispatch(usageContext); };
    options.onHeaders = [usageContext] { recordFirstProviderHeaders(usageContext); };
    if (usageContext) options.sentinelUpstreamRecorder = usageContext->sentinelUpstreamRecorder;
    std::optional<rt::Response> upstream; rt::Reason failure;
    try { auto task = fetchDeepSeekChatCompletions(std::move(body), modelRaw, options); upstream = co_await std::move(task); } catch (...) { failure = std::current_exception(); }
    if (failure) { auto task = respondDeepSeekChatDispatchFailure(failure, downstreamSignal, usageContext); auto response = co_await std::move(task); co_return DeepSeekResponseNotOk{std::move(response)}; }
    auto providerRequestId = getDeepSeekProviderRequestId(*upstream);
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = providerRequestId;
    if (!upstream->ok()) { auto task = respondDeepSeekChatUpstreamHttpFailure(*upstream, requestSignal, providerRequestId, usageContext, *bodyDiagnostic); auto response = co_await std::move(task); co_return DeepSeekResponseNotOk{std::move(response)}; }
    co_return DeepSeekDispatchOk{*upstream, providerRequestId, requestSignal, downstreamSignal};
  }
  rt::Task<rt::Response> handleDeepSeekChatCompletions(rt::Request req, json::object body, std::string modelRaw, const UsageContext* usageContext) {
    auto parsed = validateDeepSeekChatRequestFields(body);
    if (auto* refusal = std::get_if<DeepSeekResponseNotOk>(&parsed)) co_return refusal->response;
    const auto fields = std::get<DeepSeekChatRequestOk>(parsed).value; json::value effort(fields.reasoning);
    auto conflict = deepSeekThinkingToolChoiceConflict(&effort, rt::get(body, "thinking"), rt::get(body, "tool_choice"));
    if (conflict) co_return http::openaiError(400, deepSeekToolChoiceThinkingConflictMessage(*conflict, "reasoning_effort"), "invalid_request_error", {.param = "tool_choice"});
    auto upstreamModel = deepSeekUpstreamModelFor(modelRaw).value_or(std::string(DEEPSEEK_FLASH_MODEL));
    // thinking is an input to effort resolution, not a second wire representation.
    body["reasoning_effort"] = fields.reasoning; body["stream"] = fields.clientWantsStream; rt::objectDelete(body, "thinking");
    if (fields.clientWantsStream) { if (!utils::isRecord(rt::get(body, "stream_options"))) body["stream_options"] = json::object{{"include_usage", true}}; }
    else rt::objectDelete(body, "stream_options");
    if (usageContext && usageContext->responseTelemetry) {
      usageContext->responseTelemetry->provider = "deepseek"; usageContext->responseTelemetry->reasoning = fields.reasoning;
      // Observability only: an omitted client cap stays omitted on the upstream wire.
      auto allowance = deepSeekChatClientOutputAllowance(body); if (!allowance) allowance = deepSeekDefaultOutputAllowance(fields.reasoning);
      usageContext->responseTelemetry->outputTokenAllowance = allowance;
    }
    RecordRequestUsageDetails details{modelRaw, "chat.completions", fields.clientWantsStream, fields.reasoning}; auto requestUsage = recordRequestUsage(usageContext, details); co_await requestUsage.wait();
    auto dispatchTask = dispatchDeepSeekUpstream(req, std::move(body), modelRaw, usageContext); auto dispatched = co_await std::move(dispatchTask);
    if (auto* refusal = std::get_if<DeepSeekResponseNotOk>(&dispatched)) co_return refusal->response;
    auto value = std::get<DeepSeekDispatchOk>(dispatched); auto providerRequestId = value.providerRequestId;
    if (fields.clientWantsStream) co_return streamDeepSeekChatCompletion(value.upstream, providerRequestId, usageContext, value.downstreamSignal, value.requestSignal, upstreamModel);
    bounded_response_body::ReadBoundedResponseBodyOptions captureOptions{.signal = value.requestSignal, .maxBytes = DEEPSEEK_BUFFERED_BODY_MAX_BYTES,
      .timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS, .cancellationReason = "DeepSeek Chat Completions response was incomplete"};
    auto captureTask = bounded_response_body::readBoundedResponseBody(value.upstream, captureOptions); auto captured = co_await std::move(captureTask);
    if (!captured.complete) { auto task = respondDeepSeekChatIncompleteCapture(usageContext, value.downstreamSignal, value.requestSignal, providerRequestId); auto response = co_await std::move(task); co_return response; }
    auto completionTask = readDeepSeekChatCompletion(captured.bytes, value.upstream.status(), providerRequestId, usageContext, upstreamModel); auto completion = co_await std::move(completionTask);
    if (auto* refusal = std::get_if<DeepSeekResponseNotOk>(&completion)) co_return refusal->response;
    auto payload = std::get<DeepSeekChatCompletionOk>(completion).value;
    if (chatCompletionHasAnswerBearingOutput(payload)) chat::stream_translation::markChatSemanticOutput(usageContext);
    if (!providerRequestId) providerRequestId = normalizeDeepSeekProviderRequestId(rt::get(payload, "id"));
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = providerRequestId;
    auto usage = extractChatUsageTokens(rt::get(payload, "usage")); auto completionUsage = recordCompletionUsage(usageContext, usage); co_await completionUsage.wait();
    recordStreamTerminalType(usageContext, "response.completed"); recordDeepSeekResponseHealth(value.upstream.status(), providerRequestId);
    co_return http::json(200, payload, deepseekResponseHeaders(providerRequestId));
  }
  namespace {
    struct BufferedDeepSeekResponsesOptions {
      rt::Response upstream; std::string modelRaw; std::string responseId; responses_payload::DeepSeekResponsesEcho echo;
      rt::OrderedMap<std::string, responses::OriginalToolName> toolNames; rt::OrderedSet<std::string> customToolNames;
      std::string upstreamModel; std::optional<std::string> providerRequestId; rt::AbortSignal requestSignal; rt::AbortSignal downstreamSignal;
      const UsageContext* usageContext;
    };
    rt::Task<rt::Response> finalizeBufferedDeepSeekResponses(BufferedDeepSeekResponsesOptions options) {
      bounded_response_body::ReadBoundedResponseBodyOptions captureOptions{.signal = options.requestSignal, .maxBytes = DEEPSEEK_BUFFERED_BODY_MAX_BYTES,
        .timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS, .cancellationReason = "DeepSeek Responses adapter body was incomplete"};
      auto captureTask = bounded_response_body::readBoundedResponseBody(options.upstream, captureOptions); auto captured = co_await std::move(captureTask);
      if (!captured.complete) { auto task = respondDeepSeekChatIncompleteCapture(options.usageContext, options.downstreamSignal, options.requestSignal, options.providerRequestId); auto response = co_await std::move(task); co_return response; }
      auto completionTask = readDeepSeekChatCompletion(captured.bytes, options.upstream.status(), options.providerRequestId, options.usageContext, options.upstreamModel); auto completion = co_await std::move(completionTask);
      if (auto* refusal = std::get_if<DeepSeekResponseNotOk>(&completion)) co_return refusal->response;
      auto firstCompletion = std::get<DeepSeekChatCompletionOk>(completion).value; auto providerRequestId = options.providerRequestId;
      if (!providerRequestId) providerRequestId = normalizeDeepSeekProviderRequestId(rt::get(firstCompletion, "id"));
      if (options.usageContext && options.usageContext->responseTelemetry) options.usageContext->responseTelemetry->providerRequestId = providerRequestId;
      auto payload = responses_payload::toDeepSeekResponsesPayload(firstCompletion, options.modelRaw, options.responseId, options.echo, options.toolNames, options.customToolNames);
      auto usage = extractChatUsageTokens(rt::get(firstCompletion, "usage"));
      // The provider terminal decides first: explicit truncation stays incomplete;
      // only a would-be completion is then measured for answer-bearing output.
      if (deepSeekTerminalTypeForPayload(rt::get(payload, "status")) == "response.completed" && !chatCompletionHasAnswerBearingOutput(firstCompletion))
        co_return respondDeepSeekEmptyBufferedCompletion(options.usageContext, usage, options.upstream.status(), providerRequestId);
      recordBufferedDeepSeekResponsesTerminal(options.usageContext, payload, usage, options.upstream.status(), providerRequestId);
      co_return http::json(200, payload, deepseekResponseHeaders(providerRequestId));
    }
  }
  rt::Task<rt::Response> handleDeepSeekResponses(rt::Request req, json::object rawRecord, std::string modelRaw, const UsageContext* usageContext) {
    auto parsedStream = request_policy::parseStreamField(rt::get(rawRecord, "stream"));
    if (auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&parsedStream)) co_return http::openaiError(400, failure->message, "invalid_request_error", {.param = "stream"});
    auto clientWantsStream = std::get<request_policy::ParseStreamFieldOk>(parsedStream).value;
    auto translated = chat_projection::toDeepSeekResponsesChatBody(rawRecord, modelRaw, clientWantsStream);
    auto upstreamModel = deepSeekUpstreamModelFor(modelRaw).value_or(std::string(DEEPSEEK_FLASH_MODEL));
    if (auto* failure = std::get_if<responses::DeepSeekResponsesFailure>(&translated)) co_return context_admission_failure::chatResponsesProjectionFailure(req, rawRecord, *failure, usageContext);
    auto value = std::get<responses::DeepSeekResponsesResultOk<chat_projection::DeepSeekResponsesChatBody>>(translated).value;
    if (!value.elisions.empty()) chat_projection::logForwardedPayloadElisions(value.elisions);
    if (!value.exemptions.empty()) forwarded_payload_policy::logForwardedPayloadExemptions(value.exemptions,
      {.requestId = usageContext ? usageContext->requestId : std::nullopt, .model = modelRaw, .promptAdmission = value.promptAdmission});
    auto field = [&](const char* key) { const auto* item = rt::get(rawRecord, key); return item ? *item : json::value(nullptr); };
    auto instructions = rt::getString(rawRecord, "instructions");
    responses_payload::DeepSeekResponsesEcho echo{field("tools"), field("tool_choice"), field("parallel_tool_calls"),
      instructions && !rt::trim(*instructions).empty() ? std::optional<std::string>(*instructions) : std::nullopt};
    std::string reasoningLabel = std::string(rt::getString(value.body, "reasoning_effort").value_or(DEEPSEEK_DEFAULT_REASONING_EFFORT));
    auto outputAllowance = rt::getNumber(value.body, "max_tokens"); if (!outputAllowance) outputAllowance = deepSeekDefaultOutputAllowance(reasoningLabel);
    if (usageContext && usageContext->responseTelemetry) { usageContext->responseTelemetry->provider = "deepseek";
      usageContext->responseTelemetry->reasoning = reasoningLabel; usageContext->responseTelemetry->outputTokenAllowance = outputAllowance; }
    RecordRequestUsageDetails details{modelRaw, "responses", clientWantsStream, reasoningLabel}; auto requestUsage = recordRequestUsage(usageContext, details); co_await requestUsage.wait();
    auto dispatchTask = dispatchDeepSeekUpstream(req, value.body, modelRaw, usageContext); auto dispatched = co_await std::move(dispatchTask);
    if (auto* refusal = std::get_if<DeepSeekResponseNotOk>(&dispatched)) co_return request_policy::withUosWarning(refusal->response, value.warnings);
    auto result = std::get<DeepSeekDispatchOk>(dispatched);
    auto rawId = result.providerRequestId ? *result.providerRequestId : rt::crypto::randomUUID(); std::string responseId = "resp_";
    for (const auto c : rawId) if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { if (responseId.size() == 45) break; responseId += c; }
    const auto createdAtSeconds = rt::Math::floor(rt::dateNow() / 1000);
    if (clientWantsStream) {
      provider::stream_relay::RelayResponsesStreamOptions options{result.upstream, modelRaw, responseId, createdAtSeconds, echo,
        value.toolNames, value.customToolNames, result.providerRequestId, usageContext, result.downstreamSignal, result.requestSignal, upstreamModel};
      auto response = provider::stream_relay::relayResponsesStream(deepseekStreamAdapter, options);
      co_return request_policy::withUosWarning(response, value.warnings);
    }
    BufferedDeepSeekResponsesOptions options{result.upstream, modelRaw, responseId, echo, value.toolNames, value.customToolNames,
      upstreamModel, result.providerRequestId, result.requestSignal, result.downstreamSignal, usageContext};
    auto task = finalizeBufferedDeepSeekResponses(options); auto response = co_await std::move(task); co_return request_policy::withUosWarning(response, value.warnings);
  }
}
