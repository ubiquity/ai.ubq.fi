// Port of src/provider/cerebras-responses.ts.
#include "uos/provider/cerebras_responses.hpp"
#include "uos/deepseek/context_admission_failure.hpp"
#include "uos/openai.hpp"
#include "uos/defaults.hpp"
#include "uos/http.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/request_policy.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/cerebras_handlers.hpp"
#include "uos/provider/health.hpp"
#include "uos/bounded_response_body.hpp"
#include "uos/deepseek/chat_projection.hpp"
#include "uos/deepseek/forwarded_payload_policy.hpp"
#include "uos/deepseek/responses_payload.hpp"
#include "uos/deepseek/responses_stream.hpp"
#include "uos/deepseek/handlers.hpp"
#include "uos/chat/stream_translation.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"

namespace uos::provider::cerebras_responses {
  using namespace openai_telemetry;
  using namespace cerebras;
  using namespace cerebras_handlers;
  using namespace upstream_wire;
  using namespace deepseek::responses_payload;
  namespace {
    constexpr double CEREBRAS_BUFFERED_BODY_MAX_BYTES = 8 * 1024 * 1024;
    DeepSeekResponsesEcho responsesEcho(const json::object& rawRecord) {
      const auto field = [&](const char* key) { const auto* value = rt::get(rawRecord, key); return value ? *value : json::value(nullptr); };
      auto instructions = rt::getString(rawRecord, "instructions");
      return {field("tools"), field("tool_choice"), field("parallel_tool_calls"),
        instructions && !rt::trim(*instructions).empty() ? std::optional<std::string>(*instructions) : std::nullopt};
    }
    json::object responsesCompletion(const json::object& completion) {
      auto result = completion; json::array choices;
      if (const auto* input = rt::getArray(completion, "choices")) for (const auto& choice : *input) {
        if (!choice.is_object() || !rt::getObject(choice, "message")) { choices.push_back(choice); continue; }
        auto row = choice.as_object(); auto message = *rt::getObject(choice, "message");
        const auto reasoning = rt::getString(message, "reasoning");
        if (reasoning && !reasoning->empty() && !rt::hasOwn(message, "reasoning_content")) rt::objectSet(message, "reasoning_content", json::value(*reasoning));
        rt::objectSet(row, "message", message); choices.push_back(row);
      }
      rt::objectSet(result, "choices", choices); return result;
    }
    json::object responsesChunk(const json::object& completion) {
      // Insertion order follows the source object, including object before created/model.
      json::object ordered; if (const auto* value = rt::get(completion, "id")) ordered["id"] = *value;
      ordered["object"] = "chat.completion.chunk";
      for (const auto* key : {"created", "model"}) if (const auto* value = rt::get(completion, key)) ordered[key] = *value;
      json::array choices;
      if (const auto* input = rt::getArray(completion, "choices")) for (std::size_t position = 0; position < input->size(); ++position) {
        const auto& choice = (*input)[position]; json::object entry = choice.is_object() ? choice.as_object() : json::object{};
        const auto* rawMessage = rt::getObject(entry, "message"); json::object message = rawMessage ? *rawMessage : json::object{};
        json::object delta{{"role", "assistant"}};
        for (const auto* key : {"reasoning_content", "content", "refusal"}) {
          const auto value = rt::getString(message, key);
          if (value && (std::string_view(key) == "content" || !value->empty())) delta[key] = *value;
        }
        if (const auto* toolCalls = rt::getArray(message, "tool_calls")) {
          json::array calls;
          for (std::size_t index = 0; index < toolCalls->size(); ++index) {
            const auto& call = (*toolCalls)[index];
            if (!call.is_object()) { calls.push_back(call); continue; }
            auto row = call.as_object(); rt::objectSet(row, "index", rt::makeNumber(static_cast<double>(index))); calls.push_back(row);
          }
          delta["tool_calls"] = calls;
        }
        auto reason = rt::getString(entry, "finish_reason");
        choices.push_back(json::object{{"index", rt::makeNumber(rt::getNumber(entry, "index").value_or(static_cast<double>(position)))},
          {"delta", delta}, {"finish_reason", reason ? json::value(*reason) : json::value(nullptr)}});
      }
      ordered["choices"] = choices;
      if (const auto* value = rt::get(completion, "usage")) ordered["usage"] = *value;
      return ordered;
    }
    void recordCerebrasResponsesTerminal(const UsageContext* usageContext, const json::object& payload,
      std::optional<UsageTokens> usage, double upstreamStatus, std::optional<std::string> providerRequestId) {
      auto terminalType = deepseek::handlers::deepSeekTerminalTypeForPayload(rt::get(payload, "status"));
      recordTerminalUsage(usageContext, usage, terminalType == "response.completed"); recordStreamTerminalType(usageContext, terminalType);
      if (terminalType == "response.completed") { recordStreamTerminal(usageContext); recordCerebrasResponseHealth(upstreamStatus, providerRequestId); }
      else if (terminalType == "response.incomplete") { recordCerebrasFailureKind(usageContext, "incomplete_response"); recordCerebrasResponseHealth(upstreamStatus, providerRequestId); }
      else { recordCerebrasFailureKind(usageContext, "upstream_error"); health::recordCerebrasProviderHealth("upstream_error", upstreamStatus, {}, providerRequestId); }
    }
    struct Dispatched { json::object completion; std::optional<std::string> providerRequestId; double upstreamStatus; };
    using CerebrasResponsesDispatch = std::variant<Dispatched, rt::Response>;
    rt::Task<CerebrasResponsesDispatch> dispatchCerebrasResponses(rt::Request req, json::object chatBody, std::string upstreamModel, const UsageContext* usageContext) {
      auto downstreamSignal = openai::downstreamSignalFor(req, usageContext), requestSignal = openai::inferenceSignal(req, usageContext);
      CerebrasChatCompletionsOptions options; options.signal = requestSignal;
      options.beforeDispatch = [usageContext] {
        return usageContext && usageContext->beforeProviderDispatch ? usageContext->beforeProviderDispatch("cerebras")
          : rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>>::resolve(std::nullopt).wait();
      };
      options.onDispatch = [usageContext] { recordAttemptedProvider(usageContext, "cerebras"); recordFirstProviderDispatch(usageContext); };
      options.onHeaders = [usageContext] { recordFirstProviderHeaders(usageContext); };
      if (usageContext) options.sentinelUpstreamRecorder = usageContext->sentinelUpstreamRecorder;
      std::optional<rt::Response> upstream; rt::Reason failure;
      try { auto task = fetchCerebrasChatCompletions(chatBody, options); upstream = co_await std::move(task); } catch (...) { failure = std::current_exception(); }
      if (failure) { auto task = respondCerebrasChatDispatchFailure(failure, downstreamSignal, usageContext); auto response = co_await std::move(task); co_return response; }
      auto providerRequestId = getCerebrasProviderRequestId(*upstream);
      if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = providerRequestId;
      if (!upstream->ok()) { auto task = respondCerebrasChatUpstreamHttpFailure(*upstream, requestSignal, providerRequestId, usageContext); auto response = co_await std::move(task); co_return response; }
      bounded_response_body::ReadBoundedResponseBodyOptions captureOptions{.signal = requestSignal, .maxBytes = CEREBRAS_BUFFERED_BODY_MAX_BYTES,
        .timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS, .cancellationReason = "Cerebras Responses adapter body was incomplete"};
      auto captureTask = bounded_response_body::readBoundedResponseBody(*upstream, captureOptions); auto captured = co_await std::move(captureTask);
      if (!captured.complete) { auto task = respondCerebrasChatIncompleteCapture(usageContext, downstreamSignal, requestSignal, providerRequestId); auto response = co_await std::move(task); co_return response; }
      auto completionTask = readCerebrasChatCompletion(captured.bytes, upstream->status(), providerRequestId, usageContext, upstreamModel); auto completion = co_await std::move(completionTask);
      if (auto* refusal = std::get_if<CerebrasResponseNotOk>(&completion)) co_return refusal->response;
      auto value = std::get<CerebrasChatCompletionOk>(completion).value;
      if (!providerRequestId) providerRequestId = normalizeCerebrasProviderRequestId(rt::get(value, "id"));
      if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = providerRequestId;
      co_return Dispatched{std::move(value), providerRequestId, static_cast<double>(upstream->status())};
    }
  }
  rt::Task<rt::Response> handleCerebrasResponses(rt::Request req, json::object rawRecord, std::string modelRaw, const UsageContext* usageContext) {
    auto parsedStream = request_policy::parseStreamField(rt::get(rawRecord, "stream"));
    if (auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&parsedStream)) co_return http::openaiError(400, failure->message, "invalid_request_error", {.param = "stream"});
    auto clientWantsStream = std::get<request_policy::ParseStreamFieldOk>(parsedStream).value;
    auto upstreamModel = cerebrasUpstreamModelFor(modelRaw);
    if (!upstreamModel) co_return http::openaiError(400, "model '" + modelRaw + "' is not a Cerebras model", "invalid_request_error", {.param = "model"});
    // The upstream answers a buffered completion, so the translation never asks
    // for a stream: a client stream:true is answered by the replay below.
    auto translated = deepseek::chat_projection::toDeepSeekResponsesChatBody(rawRecord, modelRaw, false, deepseek::responses::CEREBRAS_RESPONSES_PROFILE, projectCerebrasRequest);
    if (auto* failure = std::get_if<deepseek::responses::DeepSeekResponsesFailure>(&translated)) co_return deepseek::context_admission_failure::chatResponsesProjectionFailure(req, rawRecord, *failure, usageContext);
    auto value = std::get<deepseek::responses::DeepSeekResponsesResultOk<deepseek::chat_projection::DeepSeekResponsesChatBody>>(translated).value;
    auto chatBody = value.body;
    if (!value.elisions.empty()) deepseek::chat_projection::logForwardedPayloadElisions(value.elisions);
    if (!value.exemptions.empty()) deepseek::forwarded_payload_policy::logForwardedPayloadExemptions(value.exemptions,
      {.requestId = usageContext ? usageContext->requestId : std::nullopt, .model = modelRaw, .promptAdmission = value.promptAdmission});
    auto tierRefusal = cerebrasReasoningEffortRefusal(*upstreamModel, rt::get(chatBody, "reasoning_effort")); if (tierRefusal) co_return *tierRefusal;
    auto echo = responsesEcho(rawRecord); std::optional<std::string> reasoningLabel;
    if (auto reasoning = rt::getString(chatBody, "reasoning_effort")) reasoningLabel = std::string(*reasoning);
    if (!reasoningLabel) {
      const auto declared = request_policy::cerebrasProviderHint(*upstreamModel).default_reasoning_effort;
      reasoningLabel = declared && declared->is_string() && !declared->as_string().empty() ? std::string(declared->as_string()) : defaults::DEFAULT_REASONING_EFFORT;
    }
    if (usageContext && usageContext->responseTelemetry) {
      usageContext->responseTelemetry->provider = "cerebras"; usageContext->responseTelemetry->reasoning = *reasoningLabel;
      usageContext->responseTelemetry->outputTokenAllowance = rt::getNumber(chatBody, "max_tokens");
    }
    RecordRequestUsageDetails details{modelRaw, "responses", clientWantsStream, *reasoningLabel}; auto requestUsage = recordRequestUsage(usageContext, details); co_await requestUsage.wait();
    auto dispatchTask = dispatchCerebrasResponses(req, chatBody, *upstreamModel, usageContext); auto dispatched = co_await std::move(dispatchTask);
    if (auto* refusal = std::get_if<rt::Response>(&dispatched)) co_return *refusal;
    auto result = std::get<Dispatched>(dispatched);
    // A token-budget stop can contain reasoning only. Refuse that unusable
    // answer rather than publish it as a completed or incomplete response.
    if (!chatCompletionHasAnswerBearingOutput(result.completion)) {
      auto task = respondCerebrasChatInvalidCompletion("invalid_completion_schema", result.upstreamStatus, result.providerRequestId, usageContext); auto response = co_await std::move(task); co_return response;
    }
    auto projected = responsesCompletion(result.completion); auto usage = extractChatUsageTokens(rt::get(projected, "usage"));
    auto rawId = result.providerRequestId ? *result.providerRequestId : rt::crypto::randomUUID(); std::string responseId = "resp_";
    for (const auto c : rawId) if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { if (responseId.size() == 45) break; responseId += c; }
    auto payload = toDeepSeekResponsesPayload(projected, modelRaw, responseId, echo, value.toolNames, value.customToolNames, deepseek::responses::CEREBRAS_RESPONSES_PROFILE);
    if (clientWantsStream) {
      auto translator = deepseek::responses_stream::createDeepSeekResponsesStreamTranslator(modelRaw, responseId, echo, rt::Math::floor(rt::dateNow() / 1000), value.toolNames, value.customToolNames, deepseek::responses::CEREBRAS_RESPONSES_PROFILE);
      auto events = translator.open(); auto pushed = translator.push(responsesChunk(projected)); events.insert(events.end(), pushed.begin(), pushed.end());
      auto terminal = translator.finish(); events.insert(events.end(), terminal.begin(), terminal.end());
      std::string body; double sequenceNumber = 0;
      for (auto& event : events) { rt::objectSet(event, "sequence_number", rt::makeNumber(sequenceNumber++)); body += deepseek::responses_stream::encodeResponsesEvent(event); }
      chat::stream_translation::markChatSemanticOutput(usageContext); recordFirstSemanticCommitment(usageContext);
      recordCerebrasResponsesTerminal(usageContext, payload, usage, result.upstreamStatus, result.providerRequestId);
      auto headers = cerebrasResponseHeaders(result.providerRequestId, std::string(GPT_OSS_STREAM_DOWNGRADED_WARNING)); headers.set("Content-Type", "text/event-stream"); headers.set("Cache-Control", "no-cache");
      co_return rt::Response(body, {.status = 200, .headers = headers});
    }
    chat::stream_translation::markChatSemanticOutput(usageContext); recordCerebrasResponsesTerminal(usageContext, payload, usage, result.upstreamStatus, result.providerRequestId);
    co_return http::json(200, payload, cerebrasResponseHeaders(result.providerRequestId));
  }
}
