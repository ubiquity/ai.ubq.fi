// Port of src/deepseek/waterfall-handler.ts.
#include "uos/deepseek/waterfall_handler.hpp"
#include "uos/deepseek/context_admission_failure.hpp"
#include "uos/deepseek/handlers.hpp"
#include "uos/provider/lithos_handlers.hpp"
#include "uos/provider/lithos_rate_limits.hpp"
#include "uos/provider/openrouter_handlers.hpp"
#include "uos/provider/presentation.hpp"
#include "uos/http.hpp"
#include "uos/responses_failover_stream.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
namespace uos::deepseek::waterfall_handler {
namespace {
  namespace wf = waterfall;
  namespace rs = responses_stream;
  namespace fs = responses_failover_stream;
  rt::Response withAttemptedProviders(rt::Response response, const std::vector<std::string>& attempted) {
    const auto value = rt::join(attempted, ",");
    if (context_admission_failure::isLocalContextAdmissionFailure(response)) { response.headers().set("x-uos-attempted-providers", value); return response; }
    auto headers = response.headers(); headers.set("x-uos-attempted-providers", value);
    return rt::Response(rt::bodyInit(response.body()), {.status = response.status(), .statusText = response.statusText(), .headers = headers});
  }
  rt::Response unsupportedEffortResponse(const std::string& effort) {
    std::vector<std::string> levels; for (const auto level : wf::DEEPSEEK_WATERFALL_REASONING_LEVELS) levels.emplace_back(level);
    return http::openaiError(400, "The model '" + std::string(wf::DEEPSEEK_WATERFALL_MODEL_ID) + "' supports reasoning efforts " + rt::join(levels, ", ") + "; '" + effort + "' is not supported.", "unsupported_reasoning_effort", {.param = std::optional<std::string>("reasoning.effort")});
  }
  rt::Response unavailableResponse() { return http::openaiError(503, "No DeepSeek waterfall provider is enabled and configured.", "deepseek_waterfall_unavailable", {.type = "server_error"}); }
  rt::Response transportFailureResponse(const std::string& provider) { return http::openaiError(502, "DeepSeek waterfall provider '" + provider + "' failed before responding.", "deepseek_waterfall_transport_failure", {.type = "server_error"}); }
  rt::Response streamFailureResponse(const std::string& provider) { return http::openaiError(502, "DeepSeek waterfall provider '" + provider + "' ended its stream before producing output.", "deepseek_waterfall_stream_failure", {.type = "server_error"}); }
  rt::Task<rt::Response> dispatchPaidHop(DeepSeekWaterfallPaidTail tail, DeepSeekWaterfallDispatchInput input) {
    auto paidBody = input.rawBody; paidBody["model"] = input.model; auto headers = input.req.headers(); headers.delete_("content-length");
    const rt::Request paidRequest(input.req.url(), {.method = "POST", .headers = headers, .body = rt::jsonStringify(paidBody), .signal = input.req.signal()});
    const auto* pin = wf::DEEPSEEK_WATERFALL_PAID_PIN.get(input.provider);
    const std::string hop = input.provider == "openlux" ? "openlux" : "surplus";
    std::optional<openai_telemetry::UsageContext> context = input.usageContext ? std::optional(*input.usageContext) : std::nullopt;
    if (context && context->requestId && !context->requestId->empty()) context->paidFallbackRequestId = *context->requestId + ":deepseek_waterfall:" + hop;
    const responses_handler::ResponsesTailOptions options{pin ? std::optional(*pin) : std::nullopt, hop};
    auto task = tail(paidRequest, paidBody, paidBody, context ? &*context : nullptr, options); rt::Promise<rt::Response> pending(std::move(task)); auto awaitedResult1 = co_await pending.wait(); co_return awaitedResult1;
  }
  DeepSeekWaterfallDispatch defaultDispatchFor(DeepSeekWaterfallPaidTail paidTail) {
    return [paidTail](const DeepSeekWaterfallDispatchInput& input) -> rt::Task<rt::Response> {
      if (input.provider == "surplus" || input.provider == "openlux") return dispatchPaidHop(paidTail, input);
      if (input.provider == "openrouter") return provider::openrouter_handlers::handleOpenRouterResponses(input.req, input.rawRecord, input.model, input.usageContext);
      if (input.provider == "lithos") {
        provider::lithos_handlers::HandleLithosResponsesOptions options; options.refusalWaitPolicy = provider::lithos_rate_limits::LITHOS_IMMEDIATE_FAILOVER_WAIT_POLICY;
        return provider::lithos_handlers::handleLithosResponses(input.req, input.rawRecord, input.model, input.usageContext, options);
      }
      return handlers::handleDeepSeekResponses(input.req, input.rawRecord, input.model, input.usageContext);
    };
  }
  rt::Task<void> pumpPrepared(rs::ResponsesStreamIterator iterator, rt::ReadableStreamDefaultController<rt::Bytes> controller) {
    try { for (;;) { auto next = co_await iterator.next(); if (next.done) break; controller.enqueue(rt::TextEncoder().encode(next.value->raw)); } controller.close(); }
    catch (...) { controller.error(std::current_exception()); }
  }
  rt::Response replayPreparedResponses(rt::Response response, fs::PreparedResponsesStream prepared) {
    rt::UnderlyingSource<rt::Bytes> source;
    source.start = [prepared](rt::ReadableStreamDefaultController<rt::Bytes> controller) {
      for (const auto& event : prepared.buffered) controller.enqueue(rt::TextEncoder().encode(event.raw));
      rt::spawn(pumpPrepared(prepared.iterator, controller));
    };
    source.cancel = [iterator = prepared.iterator](rt::Reason) { rt::spawn(iterator.return_(), rt::ignoreRejection); };
    rt::ByteStream stream(source);
    return rt::Response(stream, {.status = response.status(), .statusText = response.statusText(), .headers = response.headers()});
  }
  struct Gated { bool committed; rt::Response response; };
  rt::Task<Gated> gateStreamingHop(rt::Response response, rt::AbortSignal signal) {
    if (!response.ok() || !response.body() || !rt::includes(response.headers().get("content-type").value_or(""), "text/event-stream")) co_return Gated{true, response};
    auto iterator = rs::readResponsesStream(*response.body(), signal); std::optional<fs::PreparedResponsesStream> prepared; rt::Reason error;
    try { auto task = fs::prepareResponsesStreamForCommit(iterator); rt::Promise<fs::PreparedResponsesStream> pending(std::move(task)); prepared = co_await pending.wait(); }
    catch (...) { error = std::current_exception(); }
    if (error) { if (signal.aborted()) std::rethrow_exception(error); co_return Gated{false, response}; }
    if (!prepared->semantic && prepared->terminal && (prepared->terminal->type == "response.failed" || prepared->terminal->type == "error")) {
      try { co_await iterator.return_(); } catch (...) {} co_await rt::microtask(); co_return Gated{false, response};
    }
    co_return Gated{true, replayPreparedResponses(response, *prepared)};
  }
  struct HopResult { bool delivered; rt::Response response; std::variant<double, std::string> failure; };
  rt::Task<HopResult> attemptHop(DeepSeekWaterfallDispatch dispatch, std::string provider, std::string model,
    rt::Request req, json::object rawRecord, ::uos::types::ResponsesRequest rawBody, const openai_telemetry::UsageContext* context) {
    std::optional<HopResult> result; rt::Reason error;
    try {
      auto record = rawRecord; record["model"] = model; const DeepSeekWaterfallDispatchInput input{provider, req, record, rawBody, model, context};
      auto task = dispatch(input); rt::Promise<rt::Response> pending(std::move(task)); const auto response = co_await pending.wait();
      if (context_admission_failure::isLocalContextAdmissionFailure(response)) co_return HopResult{true, response, 0.0};
      if (wf::isDeepSeekWaterfallRetryStatus(response.status())) co_return HopResult{false, response, static_cast<double>(response.status())};
      auto gatedTask = gateStreamingHop(response, req.signal()); rt::Promise<Gated> gatedPending(std::move(gatedTask)); const auto gated = co_await gatedPending.wait();
      if (!gated.committed) co_return HopResult{false, streamFailureResponse(provider), std::string("stream")};
      co_return HopResult{true, gated.response, 0.0};
    } catch (...) { error = std::current_exception(); }
    if (req.signal().aborted()) std::rethrow_exception(error);
    co_return HopResult{false, transportFailureResponse(provider), std::string("transport")};
  }
  const std::string& modelFor(const flash_aliases::DeepSeekFlashAliases::Models& models, const std::string& provider) {
    if (provider == "lithos") return models.lithos;
    if (provider == "deepseek") return models.deepseek;
    if (provider == "openrouter") return models.openrouter;
    if (provider == "surplus") return models.surplus;
    return models.openlux;
  }
}
rt::Task<rt::Response> handleDeepSeekWaterfallResponses(rt::Request req, json::object record) {
  const ::uos::types::ResponsesRequest body; auto task = handleDeepSeekWaterfallResponses(req, record, body); auto awaitedResult2 = co_await std::move(task); co_return awaitedResult2;
}
rt::Task<rt::Response> handleDeepSeekWaterfallResponses(rt::Request req, json::object record, ::uos::types::ResponsesRequest body, const openai_telemetry::UsageContext* context) {
  const DeepSeekWaterfallOptions options; auto task = handleDeepSeekWaterfallResponses(req, record, body, context, options); auto awaitedResult3 = co_await std::move(task); co_return awaitedResult3;
}
rt::Task<rt::Response> handleDeepSeekWaterfallResponses(rt::Request req, json::object rawRecord, ::uos::types::ResponsesRequest rawBody,
  const openai_telemetry::UsageContext* usageContext, DeepSeekWaterfallOptions options) {
  const auto effort = wf::deepSeekWaterfallRequestedEffort(rawRecord); if (effort && !wf::isDeepSeekWaterfallReasoningLevel(*effort)) co_return unsupportedEffortResponse(*effort);
  flash_aliases::warmDeepSeekFlashCatalogs(); const auto aliases = flash_aliases::currentDeepSeekFlashAliases();
  std::optional<provider::selection::ProviderSelection> selection;
  if (options.selection.isUndefined()) { selection = co_await provider::selection::loadProviderSelectionCached(); co_await rt::microtask(); } else selection = options.selection.toOptional();
  const std::function<bool(const std::string&)> enabled = options.enabled ? options.enabled : std::function<bool(const std::string&)>([selection](const std::string&) { return provider::selection::isProviderEnabled(std::string(provider::presentation::GATEWAY_PROVIDER_ID), selection); });
  const std::function<bool(const std::string&)> configured = options.configured ? options.configured : std::function<bool(const std::string&)>(wf::deepSeekWaterfallProviderConfigured);
  const wf::DeepSeekWaterfallPlanInput planInput{enabled, configured}; const auto plan = wf::deepSeekWaterfallPlan(planInput); if (plan.empty()) co_return unavailableResponse();
  const DeepSeekWaterfallPaidTail paidTail = options.paidTail ? options.paidTail : DeepSeekWaterfallPaidTail([](rt::Request request, const json::object& record, const ::uos::types::ResponsesRequest& body, const openai_telemetry::UsageContext* context, const responses_handler::ResponsesTailOptions& tailOptions) { return responses_handler::runOrdinaryResponsesTail(request, record, body, context, tailOptions); });
  const auto dispatch = options.dispatch ? options.dispatch : defaultDispatchFor(paidTail);
  std::vector<std::string> attempted; std::optional<std::string> fallbackReason; auto finalResponse = unavailableResponse();
  for (const auto& provider : plan) {
    if (req.signal().aborted()) break;
    auto task = attemptHop(dispatch, provider, modelFor(aliases.models, provider), req, rawRecord, rawBody, usageContext); rt::Promise<HopResult> pending(std::move(task)); const auto result = co_await pending.wait();
    attempted.push_back(provider); openai_telemetry::recordAttemptedProvider(usageContext, provider); finalResponse = result.response; if (result.delivered) break;
    if (!fallbackReason) fallbackReason = wf::deepSeekWaterfallFailureReason(provider, result.failure);
  }
  if (fallbackReason && usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->fallbackReason = fallbackReason;
  co_return withAttemptedProviders(finalResponse, attempted);
}
}
