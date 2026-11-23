// Port of src/provider/openrouter-handlers.ts.
#include "uos/provider/openrouter_handlers.hpp"
#include "uos/bounded_response_body.hpp"
#include "uos/chat/stream_translation.hpp"
#include "uos/http.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/openai.hpp"
#include "uos/responses_failover_stream.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/openrouter_web_tools.hpp"
#include "uos/provider/openrouter_streams.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

namespace uos::provider::openrouter_handlers {
  using namespace openai_telemetry;
  using namespace upstream_wire;
  using namespace openrouter;
  using namespace openrouter_streams;
  namespace {
    const std::string OPENROUTER_UPSTREAM_LABEL = "openrouter";
    void enqueueOpenRouterCacheBlocks(const json::value* item, std::vector<const json::value*>& pending) {
      if (!item) return;
      if (item->is_array()) { for (const auto& part : item->as_array()) pending.push_back(&part); return; }
      if (!item->is_object()) return;
      // Walk request content and tool blocks, excluding tool JSON schemas.
      for (const auto* key : {"instructions", "input", "messages", "tools", "content", "output", "function"})
        if (const auto* value = rt::get(*item, key)) pending.push_back(value);
    }
    struct CacheControls { bool nestedControlPresent; double explicitBreakpointCount; };
    CacheControls openRouterExplicitCacheControls(const json::object& body) {
      json::value root(body); std::vector<const json::value*> pending{&root};
      double explicitBreakpointCount = 0; bool nestedControlPresent = false;
      while (!pending.empty()) {
        const auto* item = pending.back(); pending.pop_back(); enqueueOpenRouterCacheBlocks(item, pending);
        if (!item->is_object() || item == &root) continue;
        if (rt::hasOwn(item->as_object(), "cache_control") || rt::hasOwn(item->as_object(), "prompt_cache_breakpoint")) {
          nestedControlPresent = true;
          const auto* cache = rt::get(*item, "cache_control"); const auto* breakpoint = rt::get(*item, "prompt_cache_breakpoint");
          if ((utils::isRecord(cache) && rt::getString(*cache, "type") == "ephemeral") ||
              (utils::isRecord(breakpoint) && rt::getString(*breakpoint, "mode") == "explicit")) ++explicitBreakpointCount;
        }
      }
      return {nestedControlPresent, explicitBreakpointCount};
    }
    struct CacheRequest { json::object body; PromptCacheMode promptCacheMode; double explicitBreakpointCount; };
    // Apply Claude's default five-minute cache policy only on the upstream wire.
    CacheRequest withOpenRouterClaudePromptCache(json::object body, const std::string& model) {
      const auto controls = openRouterExplicitCacheControls(body); auto declaredMode = promptCacheModeFor(body);
      bool explicit_ = controls.nestedControlPresent || declaredMode == "explicit";
      bool automaticControlPresent = rt::hasOwn(body, "cache_control");
      bool enableAutomatic = (rt::startsWith(model, "anthropic/claude-") || rt::startsWith(model, "~anthropic/claude-")) && !automaticControlPresent && !explicit_;
      auto promptCacheMode = declaredMode;
      if (explicit_) promptCacheMode = "explicit";
      else if (automaticControlPresent || enableAutomatic) promptCacheMode = "implicit";
      if (enableAutomatic) body["cache_control"] = json::object{{"type", "ephemeral"}};
      return {std::move(body), promptCacheMode, controls.explicitBreakpointCount};
    }
    bool isAbortLike(rt::Reason error);
    rt::Response dispatchFailure(rt::Reason error) {
      if (error) try { std::rethrow_exception(error); } catch (const OpenRouterError& value) {
        if (value.code == "openrouter_api_key_missing") return http::openaiError(503, "OpenRouter is not configured", "openrouter_api_key_missing");
      } catch (...) {}
      health::recordOpenRouterProviderHealth("upstream_error", std::nullopt);
      if (isAbortLike(error)) return http::openaiError(499, "Request was cancelled.", "request_cancelled",
        {.type = "server_error", .param = std::optional<std::string>{}});
      return http::openaiError(502, "OpenRouter upstream unreachable", "openrouter_upstream_unreachable");
    }
    bool isAbortLike(rt::Reason error) { return rt::isError(error) && (rt::errorName(error) == "AbortError" || rt::errorName(error) == "TimeoutError"); }
    struct Dispatched { rt::Response response; std::optional<std::string> providerRequestId; };
    rt::Task<Dispatched> dispatchUpstream(std::function<rt::Task<rt::Response>()> attempt, rt::AbortSignal requestSignal, const UsageContext* usageContext) {
      std::optional<rt::Response> upstream; rt::Reason failure;
      try { rt::Promise<rt::Response> promise(attempt()); upstream = co_await promise.wait(); }
      catch (...) { failure = std::current_exception(); }
      if (failure) {
        try { std::rethrow_exception(failure); } catch (const api_key_policy::ApiKeyQuotaDispatchError&) { throw; } catch (...) {}
        co_return Dispatched{dispatchFailure(failure), std::nullopt};
      }
      auto providerRequestId = providerRequestIdFromResponse(*upstream);
      if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = providerRequestId;
      recordOpenRouterResponseHealth(upstream->status());
      if (!upstream->ok()) {
        auto task = toOpenAiUpstreamErrorResponse(*upstream, OPENROUTER_UPSTREAM_LABEL, requestSignal); auto response = co_await std::move(task);
        co_return Dispatched{std::move(response), providerRequestId};
      }
      co_return Dispatched{*upstream, providerRequestId};
    }
    OpenRouterDispatchHooks hooksFor(const UsageContext* usageContext) {
      OpenRouterDispatchHooks hooks;
      hooks.beforeDispatch = [usageContext] { return usageContext && usageContext->beforeProviderDispatch ? usageContext->beforeProviderDispatch("openrouter") : rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>>::resolve(std::nullopt).wait(); };
      hooks.onDispatch = [usageContext] { recordAttemptedProvider(usageContext, "openrouter"); recordFirstProviderDispatch(usageContext); };
      hooks.onHeaders = [usageContext] { recordFirstProviderHeaders(usageContext); };
      if (usageContext) hooks.sentinelUpstreamRecorder = usageContext->sentinelUpstreamRecorder;
      return hooks;
    }
    rt::Response respondOpenRouterEmptyBufferedCompletion(const UsageContext* usageContext, std::optional<UsageTokens> usage, const std::optional<std::string>& providerRequestId) {
      if (usageContext && usageContext->responseTelemetry) { usageContext->responseTelemetry->failureKind = "empty_upstream_completion"; usageContext->responseTelemetry->semanticOutputObserved = false; }
      recordTerminalUsage(usageContext, usage, false); recordStreamTerminalType(usageContext, "response.failed");
      return http::openaiError(502, std::string(chat::stream_translation::EMPTY_UPSTREAM_COMPLETION_MESSAGE), "empty_upstream_completion",
        {.type = "server_error", .headers = openRouterResponseHeaders(providerRequestId)});
    }
    std::optional<json::object> parseJsonRecord(const rt::Bytes& bytes) {
      try { auto parsed = rt::jsonParse(rt::decodeUtf8(bytes)); if (parsed.is_object()) return parsed.as_object(); } catch (...) {}
      return std::nullopt;
    }
  }
  rt::Task<rt::Response> handleOpenRouterChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw, const UsageContext* usageContext) {
    return handleOpenRouterChatCompletions(std::move(req), std::move(rawRecord), std::move(modelRaw), usageContext, {});
  }
  rt::Task<rt::Response> handleOpenRouterChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw, const UsageContext* usageContext, OpenRouterHandlerDeps deps) {
    auto upstreamModel = co_await resolveOpenRouterUpstreamModel(modelRaw);
    if (!upstreamModel) co_return http::openaiError(400, "The requested model is not served by OpenRouter.", "openrouter_request_invalid", {.param = "model"});
    bool clientWantsStream = rt::getBool(rawRecord, "stream") == true;
    auto body = rawRecord; body["model"] = *upstreamModel; body["stream"] = clientWantsStream;
    auto cacheRequest = withOpenRouterClaudePromptCache(openrouter_web_tools::withOpenRouterOpenAiWebTools(body, *upstreamModel), *upstreamModel); body = cacheRequest.body;
    if (clientWantsStream) { if (!rt::hasOwn(body, "stream_options")) body["stream_options"] = json::object{{"include_usage", true}}; }
    else rt::objectDelete(body, "stream_options");
    if (usageContext && usageContext->responseTelemetry) { usageContext->responseTelemetry->provider = OPENROUTER_UPSTREAM_LABEL; usageContext->responseTelemetry->outputTokenAllowance = rt::getNumber(rawRecord, "max_completion_tokens"); }
    RecordRequestUsageDetails details{modelRaw, "chat.completions", clientWantsStream, utils::getString(rt::get(rawRecord, "reasoning_effort")),
      promptCacheKeyPresent(rawRecord), cacheRequest.promptCacheMode, cacheRequest.explicitBreakpointCount};
    auto requestUsage = recordRequestUsage(usageContext, details); co_await requestUsage.wait();
    auto requestSignal = openai::inferenceSignal(req, usageContext), downstreamSignal = openai::downstreamSignalFor(req, usageContext);
    OpenRouterRouteTransport transport = deps.fetchChat ? deps.fetchChat : fetchOpenRouterChatCompletions;
    OpenRouterDispatchInit init{requestSignal, hooksFor(usageContext)};
    std::function<rt::Task<rt::Response>()> attempt = [transport, body, init] { return transport(body, init); };
    auto dispatchTask = dispatchUpstream(attempt, requestSignal, usageContext); auto dispatched = co_await std::move(dispatchTask);
    if (!dispatched.response.ok()) co_return dispatched.response;
    if (clientWantsStream) co_return streamOpenRouterChatCompletion(dispatched.response, dispatched.providerRequestId, usageContext, downstreamSignal, requestSignal, *upstreamModel);
    bounded_response_body::ReadBoundedResponseBodyOptions options{.signal = requestSignal, .maxBytes = std::nullopt,
      .timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS, .cancellationReason = "OpenRouter Chat Completions response was incomplete"};
    auto captureTask = bounded_response_body::readBoundedResponseBody(dispatched.response, options); auto captured = co_await std::move(captureTask);
    if (!captured.complete) { recordStreamTerminalType(usageContext, "error"); co_return http::openaiError(502, "OpenRouter Chat Completions response was incomplete", "openrouter_upstream_incomplete"); }
    auto payload = parseJsonRecord(captured.bytes);
    if (!payload) { recordStreamTerminalType(usageContext, "error"); co_return http::openaiError(502, "OpenRouter returned an unusable body", "openrouter_upstream_invalid_response"); }
    if (chatCompletionHasAnswerBearingOutput(*payload)) chat::stream_translation::markChatSemanticOutput(usageContext);
    auto usage = extractChatUsageTokens(rt::get(*payload, "usage")); auto completionUsage = recordCompletionUsage(usageContext, usage); co_await completionUsage.wait();
    recordStreamTerminalType(usageContext, "response.completed"); co_return http::json(200, *payload, rt::Headers{{"x-uos-upstream", OPENROUTER_UPSTREAM_LABEL}});
  }
  rt::Task<rt::Response> handleOpenRouterResponses(rt::Request req, json::object rawRecord, std::string modelRaw, const UsageContext* usageContext) {
    return handleOpenRouterResponses(std::move(req), std::move(rawRecord), std::move(modelRaw), usageContext, {});
  }
  rt::Task<rt::Response> handleOpenRouterResponses(rt::Request req, json::object rawRecord, std::string modelRaw, const UsageContext* usageContext, OpenRouterHandlerDeps deps) {
    auto upstreamModel = co_await resolveOpenRouterUpstreamModel(modelRaw);
    if (!upstreamModel) co_return http::openaiError(400, "The requested model is not served by OpenRouter.", "openrouter_request_invalid", {.param = "model"});
    bool clientWantsStream = rt::getBool(rawRecord, "stream") == true;
    auto body = rawRecord; body["model"] = *upstreamModel;
    auto cacheRequest = withOpenRouterClaudePromptCache(openrouter_web_tools::withOpenRouterOpenAiWebTools(body, *upstreamModel), *upstreamModel); body = cacheRequest.body;
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->provider = OPENROUTER_UPSTREAM_LABEL;
    const auto* reasoning = rt::get(rawRecord, "reasoning");
    RecordRequestUsageDetails details{modelRaw, "responses", clientWantsStream,
      utils::isRecord(reasoning) ? utils::getString(rt::get(*reasoning, "effort")) : std::nullopt,
      promptCacheKeyPresent(rawRecord), cacheRequest.promptCacheMode, cacheRequest.explicitBreakpointCount};
    auto requestUsage = recordRequestUsage(usageContext, details); co_await requestUsage.wait();
    auto requestSignal = openai::inferenceSignal(req, usageContext), downstreamSignal = openai::downstreamSignalFor(req, usageContext);
    OpenRouterRouteTransport transport = deps.fetchResponses ? deps.fetchResponses : fetchOpenRouterResponses;
    OpenRouterDispatchInit init{requestSignal, hooksFor(usageContext)};
    std::function<rt::Task<rt::Response>()> attempt = [transport, body, init] { return transport(body, init); };
    auto dispatchTask = dispatchUpstream(attempt, requestSignal, usageContext); auto dispatched = co_await std::move(dispatchTask);
    if (!dispatched.response.ok()) co_return dispatched.response;
    if (clientWantsStream) co_return streamOpenRouterResponses(dispatched.response, dispatched.providerRequestId, usageContext, downstreamSignal, requestSignal);
    bounded_response_body::ReadBoundedResponseBodyOptions options{.signal = requestSignal, .maxBytes = std::nullopt,
      .timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS, .cancellationReason = "OpenRouter Responses response was incomplete"};
    auto captureTask = bounded_response_body::readBoundedResponseBody(dispatched.response, options); auto captured = co_await std::move(captureTask);
    if (!captured.complete) { recordStreamTerminalType(usageContext, "error"); co_return http::openaiError(502, "OpenRouter Responses response was incomplete", "openrouter_upstream_incomplete"); }
    auto payload = parseJsonRecord(captured.bytes);
    if (!payload) { recordStreamTerminalType(usageContext, "error"); co_return http::openaiError(502, "OpenRouter returned an unusable body", "openrouter_upstream_invalid_response"); }
    bool hasAnswerBearingOutput = responses_failover_stream::responsesPayloadHasAnswerBearingOutput(*payload);
    if (hasAnswerBearingOutput) chat::stream_translation::markChatSemanticOutput(usageContext);
    auto usage = extractUsageTokens(rt::get(*payload, "usage")); auto status = rt::getString(*payload, "status");
    if (status == "completed" && !hasAnswerBearingOutput) co_return respondOpenRouterEmptyBufferedCompletion(usageContext, usage, dispatched.providerRequestId);
    std::string terminalType = status == "incomplete" ? "response.incomplete" : status == "failed" ? "response.failed" : "response.completed";
    auto completionUsage = recordCompletionUsage(usageContext, usage, terminalType == "response.completed"); co_await completionUsage.wait();
    recordStreamTerminalType(usageContext, terminalType); co_return http::json(200, *payload, rt::Headers{{"x-uos-upstream", OPENROUTER_UPSTREAM_LABEL}});
  }
}
