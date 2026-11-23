// Port of src/provider/cerebras-handlers.ts.
#include "uos/provider/cerebras_handlers.hpp"
#include "uos/openai.hpp"
#include "uos/provider/cerebras.hpp"
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

namespace uos::provider::cerebras_handlers {
  using namespace openai_telemetry;
  using namespace upstream_wire;
  using namespace cerebras;
  namespace {
    ResponseStreamTerminalType cerebrasTerminalTypeForError(rt::Reason error, const rt::AbortSignal& downstreamSignal) {
      if (downstreamSignal.aborted()) return "cancelled";
      if (error) try { std::rethrow_exception(error); } catch (const CerebrasError& value) {
        if (value.status == 504) return "deadline";
      } catch (...) {}
      if (rt::isError(error) && rt::errorName(error) == "TimeoutError") return "deadline";
      if (rt::isError(error) && rt::errorName(error) == "AbortError") return "cancelled";
      return "error";
    }
    CerebrasFailureKind cerebrasTransportFailureKind(rt::Reason error, const ResponseStreamTerminalType& terminalType) {
      if (terminalType == "cancelled") return "cancellation";
      if (terminalType == "deadline") return "deadline";
      if (error) try { std::rethrow_exception(error); } catch (const api_key_policy::ApiKeyQuotaDispatchError&) {
        return "api_key_quota_reservation_unavailable";
      } catch (const CerebrasError& value) {
        if (value.code == "cerebras_api_key_missing" || value.code == "cerebras_request_invalid") return value.code;
        if (value.code == "cerebras_upstream_unreachable") return "upstream_unreachable";
        if (value.code == "gateway_timeout") return "deadline";
      } catch (...) {}
      return "upstream_unreachable";
    }
    struct CerebrasChatRequestFields { std::string reasoning; bool clientWantsStream; bool includeUsage; };
    using Validation = std::variant<CerebrasChatRequestFields, rt::Response>;
    Validation validateCerebrasChatRequestFields(const json::object& rawRecord, const std::string& upstreamModel) {
      const auto* messages = rt::getArray(rawRecord, "messages");
      if (!messages) return http::openaiError(400, "messages must be an array", "invalid_request_error");
      if (messages->empty()) return http::openaiError(400, "messages must be a non-empty array", "invalid_request_error");
      for (const auto& message : *messages) if (!message.is_object())
        return http::openaiError(400, "messages must contain objects", "invalid_request_error", {.param = "messages"});
      auto reasoningEffort = request_policy::parseReasoningEffortField(rt::get(rawRecord, "reasoning_effort"), "reasoning_effort");
      if (auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&reasoningEffort))
        return http::openaiError(400, failure->message, "invalid_request_error", {.param = "reasoning_effort"});
      auto effort = std::get<request_policy::ParseReasoningEffortFieldOk>(reasoningEffort).value;
      std::optional<json::value> effortValue;
      if (effort) effortValue = *effort;
      auto tierRefusal = cerebrasReasoningEffortRefusal(upstreamModel, effortValue ? &*effortValue : nullptr);
      if (tierRefusal) return *tierRefusal;
      auto hint = request_policy::cerebrasProviderHint(upstreamModel);
      auto declaredDefault = hint.default_reasoning_effort;
      auto defaultReasoning = declaredDefault && declaredDefault->is_string() && !declaredDefault->as_string().empty()
        ? std::string(declaredDefault->as_string()) : defaults::DEFAULT_REASONING_EFFORT;
      auto parsedStream = request_policy::parseStreamField(rt::get(rawRecord, "stream"));
      if (auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&parsedStream))
        return http::openaiError(400, failure->message, "invalid_request_error", {.param = "stream"});
      auto streamOptions = request_policy::parseChatStreamOptions(rt::get(rawRecord, "stream_options"));
      if (auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&streamOptions))
        return http::openaiError(400, failure->message, "invalid_request_error", {.param = "stream_options"});
      return CerebrasChatRequestFields{effort.value_or(defaultReasoning), std::get<request_policy::ParseStreamFieldOk>(parsedStream).value,
        std::get<request_policy::ParseChatStreamOptionsOk>(streamOptions).includeUsage};
    }
  }

  void recordCerebrasResponseHealth(double status, std::optional<std::string> providerRequestId) {
    const std::string event = status == 401 || status == 403 ? "auth_invalid" : status == 429 ? "quota_exhausted"
      : status >= 500 ? "upstream_error" : status >= 400 ? "reachable" : "success";
    health::recordCerebrasProviderHealth(event, status, {}, std::move(providerRequestId));
  }
  void recordCerebrasFailureKind(const UsageContext* context, const CerebrasFailureKind& failureKind) {
    if (context && context->responseTelemetry) context->responseTelemetry->failureKind = failureKind;
  }
  std::optional<rt::Response> cerebrasReasoningEffortRefusal(const std::string& upstreamModel, const json::value* reasoningEffort) {
    if (!reasoningEffort || !reasoningEffort->is_string()) return std::nullopt;
    const auto hint = request_policy::cerebrasProviderHint(upstreamModel);
    const auto& levels = hint.supported_reasoning_levels;
    if (levels.isNullish() || levels->empty()) return std::nullopt;
    std::vector<std::string> names;
    for (const auto& level : *levels) {
      if (rt::strictEquals(level, *reasoningEffort)) return std::nullopt;
      names.push_back(rt::toString(level));
    }
    return http::openaiError(400, "reasoning_effort '" + std::string(reasoningEffort->as_string()) + "' is not supported for " + upstreamModel +
      ". Use " + rt::join(names, ", ") + ".", "invalid_request_error", {.param = "reasoning_effort"});
  }
  std::optional<rt::Response> cerebrasReasoningEffortRefusal(const std::string& upstreamModel, const json::value& reasoningEffort) {
    return cerebrasReasoningEffortRefusal(upstreamModel, &reasoningEffort);
  }
  rt::Task<rt::Response> respondCerebrasChatInvalidCompletion(std::string failureKind, double upstreamStatus,
    std::optional<std::string> providerRequestId, const UsageContext* usageContext) {
    recordStreamTerminalType(usageContext, "error");
    recordCerebrasFailureKind(usageContext, failureKind);
    health::recordCerebrasProviderHealth("upstream_error", upstreamStatus, {}, providerRequestId);
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    co_return http::openaiError(502, "Upstream returned an invalid Chat Completions response.", "cerebras_upstream_invalid_response",
      {.type = "server_error", .headers = cerebrasResponseHeaders(providerRequestId)});
  }
  rt::Task<CerebrasChatCompletionResult> readCerebrasChatCompletion(rt::Bytes bytes, double upstreamStatus,
    std::optional<std::string> providerRequestId, const UsageContext* usageContext, std::string model) {
    json::value payload; bool invalidJson = false;
    try { payload = rt::jsonParse(rt::decodeUtf8(bytes)); } catch (...) { invalidJson = true; }
    if (invalidJson) {
      auto task = respondCerebrasChatInvalidCompletion("invalid_json", upstreamStatus, providerRequestId, usageContext);
      auto response = co_await std::move(task); co_return CerebrasResponseNotOk{std::move(response)};
    }
    auto normalized = normalizeCerebrasChatCompletion(payload, model);
    if (std::holds_alternative<NormalizationResultNotOk>(normalized)) {
      auto task = respondCerebrasChatInvalidCompletion("invalid_completion_schema", upstreamStatus, providerRequestId, usageContext);
      auto response = co_await std::move(task); co_return CerebrasResponseNotOk{std::move(response)};
    }
    co_return CerebrasChatCompletionOk{std::get<NormalizationResultOk<json::object>>(normalized).value};
  }
  rt::Task<rt::Response> respondCerebrasChatDispatchFailure(rt::Reason error, rt::AbortSignal downstreamSignal, const UsageContext* usageContext) {
    auto terminalType = cerebrasTerminalTypeForError(error, downstreamSignal);
    recordCerebrasFailureKind(usageContext, cerebrasTransportFailureKind(error, terminalType));
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType != "cancelled") health::recordCerebrasProviderHealth("upstream_error", std::nullopt);
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    co_return toCerebrasErrorResponse(error);
  }
  rt::Task<rt::Response> respondCerebrasChatUpstreamHttpFailure(rt::Response upstream, rt::AbortSignal requestSignal,
    std::optional<std::string> providerRequestId, const UsageContext* usageContext) {
    recordCerebrasResponseHealth(upstream.status(), providerRequestId);
    recordCerebrasFailureKind(usageContext, "upstream_http_error");
    recordStreamTerminalType(usageContext, "response.failed");
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    auto task = toCerebrasUpstreamErrorResponse(upstream, requestSignal); auto response = co_await std::move(task); co_return response;
  }
  rt::Task<rt::Response> respondCerebrasChatIncompleteCapture(const UsageContext* usageContext, rt::AbortSignal downstreamSignal,
    rt::AbortSignal requestSignal, std::optional<std::string> providerRequestId) {
    std::string terminalType = "error", failureKind = "incomplete_response";
    if (downstreamSignal.aborted()) { terminalType = "cancelled"; failureKind = "cancellation"; }
    else if (requestSignal.aborted()) { terminalType = "deadline"; failureKind = "deadline"; }
    recordCerebrasFailureKind(usageContext, failureKind); recordStreamTerminalType(usageContext, terminalType);
    if (terminalType != "cancelled") health::recordCerebrasProviderHealth("upstream_error", std::nullopt, {}, providerRequestId);
    auto usageTask = recordErrorUsage(usageContext); co_await usageTask.wait();
    if (terminalType == "cancelled") co_return http::openaiError(499, "Request was cancelled.", "request_cancelled",
      {.type = "server_error", .headers = cerebrasResponseHeaders(providerRequestId)});
    co_return http::openaiError(terminalType == "deadline" ? 504 : 502,
      terminalType == "deadline" ? "Upstream request exceeded the gateway deadline." : "Upstream returned an incomplete response.",
      terminalType == "deadline" ? "gateway_timeout" : "cerebras_upstream_invalid_response",
      {.type = "server_error", .headers = cerebrasResponseHeaders(providerRequestId)});
  }
  rt::Task<rt::Response> handleCerebrasChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw,
    const UsageContext* usageContext, std::string upstreamModel) {
    // Preserve the official nested Chat tools/tool_choice contract. In
    // particular, do not run the Codex-specific flattening that follows this
    // early branch in handleChatCompletionsInternal.
    auto parsedRequest = validateCerebrasChatRequestFields(rawRecord, upstreamModel);
    if (auto* refusal = std::get_if<rt::Response>(&parsedRequest)) co_return *refusal;
    const auto fields = std::get<CerebrasChatRequestFields>(parsedRequest);
    json::object cerebrasBody = rawRecord;
    rt::objectSet(cerebrasBody, "model", json::value(upstreamModel)); rt::objectSet(cerebrasBody, "reasoning_effort", json::value(fields.reasoning));
    // The upstream answers a buffered completion; a streamed client receives a replayed burst.
    rt::objectSet(cerebrasBody, "stream", false);
    // Cerebras validates the whole body instead of ignoring what it does not
    // implement. store and metadata are refused even as explicit nulls.
    rt::objectDelete(cerebrasBody, "stream_options"); rt::objectDelete(cerebrasBody, "store"); rt::objectDelete(cerebrasBody, "metadata");
    // top_logprobs is rejected only on its own, so it survives a request that also asked for logprobs.
    if (!rt::truthy(rt::get(cerebrasBody, "logprobs"))) rt::objectDelete(cerebrasBody, "top_logprobs");
    if (usageContext && usageContext->responseTelemetry) {
      usageContext->responseTelemetry->provider = "cerebras"; usageContext->responseTelemetry->reasoning = fields.reasoning;
    }
    RecordRequestUsageDetails details{modelRaw, "chat.completions", fields.clientWantsStream, fields.reasoning};
    auto requestUsage = recordRequestUsage(usageContext, details); co_await requestUsage.wait();
    auto downstreamSignal = openai::downstreamSignalFor(req, usageContext), requestSignal = openai::inferenceSignal(req, usageContext);
    CerebrasChatCompletionsOptions options;
    options.signal = requestSignal;
    options.beforeDispatch = [usageContext] {
      return usageContext && usageContext->beforeProviderDispatch ? usageContext->beforeProviderDispatch("cerebras")
        : rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>>::resolve(std::nullopt).wait();
    };
    options.onDispatch = [usageContext] { recordAttemptedProvider(usageContext, "cerebras"); recordFirstProviderDispatch(usageContext); };
    options.onHeaders = [usageContext] { recordFirstProviderHeaders(usageContext); };
    if (usageContext) options.sentinelUpstreamRecorder = usageContext->sentinelUpstreamRecorder;
    std::optional<rt::Response> upstream; rt::Reason failure;
    try { auto task = fetchCerebrasChatCompletions(cerebrasBody, options); upstream = co_await std::move(task); }
    catch (...) { failure = std::current_exception(); }
    if (failure) { auto task = respondCerebrasChatDispatchFailure(failure, downstreamSignal, usageContext); auto response = co_await std::move(task); co_return response; }
    auto providerRequestId = getCerebrasProviderRequestId(*upstream);
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = providerRequestId;
    if (!upstream->ok()) {
      auto task = respondCerebrasChatUpstreamHttpFailure(*upstream, requestSignal, providerRequestId, usageContext);
      auto response = co_await std::move(task); co_return response;
    }
    bounded_response_body::ReadBoundedResponseBodyOptions captureOptions{.signal = requestSignal, .maxBytes = 128 * 1024,
      .timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS, .cancellationReason = "Cerebras Chat Completions response was incomplete"};
    auto captureTask = bounded_response_body::readBoundedResponseBody(*upstream, captureOptions); auto captured = co_await std::move(captureTask);
    if (!captured.complete) {
      auto task = respondCerebrasChatIncompleteCapture(usageContext, downstreamSignal, requestSignal, providerRequestId);
      auto response = co_await std::move(task); co_return response;
    }
    auto completionTask = readCerebrasChatCompletion(captured.bytes, upstream->status(), providerRequestId, usageContext, upstreamModel);
    auto completion = co_await std::move(completionTask);
    if (auto* refusal = std::get_if<CerebrasResponseNotOk>(&completion)) co_return refusal->response;
    auto value = std::get<CerebrasChatCompletionOk>(completion).value;
    if (chatCompletionHasAnswerBearingOutput(value)) chat::stream_translation::markChatSemanticOutput(usageContext);
    if (!providerRequestId) providerRequestId = normalizeCerebrasProviderRequestId(rt::get(value, "id"));
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = providerRequestId;
    auto usage = extractChatUsageTokens(rt::get(value, "usage"));
    auto completionUsage = recordCompletionUsage(usageContext, usage); co_await completionUsage.wait();
    if (fields.clientWantsStream) recordFirstSemanticCommitment(usageContext);
    recordStreamTerminalType(usageContext, "response.completed"); recordCerebrasResponseHealth(upstream->status(), providerRequestId);
    auto responseHeaders = cerebrasResponseHeaders(providerRequestId, fields.clientWantsStream ? std::optional<std::string>(GPT_OSS_STREAM_DOWNGRADED_WARNING) : std::nullopt);
    if (fields.clientWantsStream) { recordStreamTerminal(usageContext); co_return streamCerebrasChatCompletion(value, fields.includeUsage, responseHeaders); }
    co_return http::json(200, value, responseHeaders);
  }
}
