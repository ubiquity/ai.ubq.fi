// LithosAI stream plumbing, extracted from src/provider/lithos-handlers.ts.
#include "uos/provider/lithos_streams.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/responses_stream.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/regex.hpp"

#include <utility>

namespace uos::provider::lithos_streams {
  namespace {
    namespace telemetry = openai_telemetry;
    LithosFailureKind lithosFinishReasonFailureKind(const std::optional<std::string>& finishReason) {
      static const rt::Regex valid("^[A-Za-z0-9_.:-]{1,64}$");
      return "lithos_finish_reason:" + (finishReason && valid.test(*finishReason) ? *finishReason : "unrecognized");
    }
    rt::Task<void> lithosChatStreamFramesBody(rt::Response upstream, std::string upstreamModel,
      lithos::IterateLithosChatCompletionStreamOptions options, rt::Yield<stream_relay::ProviderStreamFrame> yield) {
      const auto chunks = lithos::iterateLithosChatCompletionStream(upstream, upstreamModel, options);
      const auto forward = [yield](json::object chunk) {
        stream_relay::ProviderStreamFrame frame = deepseek::stream::DeepSeekStreamFrameChunk{std::move(chunk)};
        return yield(std::move(frame));
      };
      co_await rt::forAwait(chunks, forward);
    }
    rt::AsyncGenerator<stream_relay::ProviderStreamFrame> lithosChatStreamFrames(rt::Response upstream, std::string upstreamModel,
      lithos::IterateLithosChatCompletionStreamOptions options) {
      return rt::AsyncGenerator<stream_relay::ProviderStreamFrame>([upstream, upstreamModel, options](rt::Yield<stream_relay::ProviderStreamFrame> yield) {
        return lithosChatStreamFramesBody(upstream, upstreamModel, options, std::move(yield));
      });
    }
    bool isClassifiedLithosRefusal(const std::optional<double>& status) {
      return status == 401 || status == 402 || status == 403 || status == 429;
    }
    rt::Response withLithosSseKeepalive(rt::Response response) {
      const auto body = response.body();
      if (!body) return response;
      const rt::ResponseInit init{.status = response.status(), .headers = response.headers()};
      return rt::Response(responses_stream::withSseKeepalive(*body), init);
    }
    stream_relay::ProviderStreamAdapter lithosStreamAdapter() {
      stream_relay::ProviderStreamAdapter adapter;
      adapter.responseHeaders = upstream_wire::lithosResponseHeaders;
      adapter.frames = [](rt::Response upstream, std::string model, stream_relay::ProviderStreamFrameOptions options) -> stream_relay::ProviderStreamIterator {
        lithos::IterateLithosChatCompletionStreamOptions settings; settings.signal = options.signal;
        return lithosChatStreamFrames(upstream, std::move(model), settings);
      };
      adapter.recordResponseHealth = recordLithosResponseHealth;
      adapter.recordProviderError = [](std::optional<double> status, std::optional<std::string> providerRequestId) {
        if (isClassifiedLithosRefusal(status)) return;
        (void)health::recordLithosProviderHealth("upstream_error", status, rt::dateNow, providerRequestId);
      };
      adapter.recordCancellation = [](const telemetry::UsageContext* context) { recordLithosFailureKind(context, "cancellation"); };
      adapter.recordIncompleteResponse = [](const telemetry::UsageContext* context) { recordLithosFailureKind(context, "incomplete_response"); };
      adapter.recordFinishFailureKind = [](const telemetry::UsageContext* context, std::optional<std::string> reason) {
        recordLithosFailureKind(context, lithosFinishReasonFailureKind(reason));
      };
      adapter.recordTransportFailure = [](const telemetry::UsageContext* context, rt::Reason error, std::string terminalType) {
        recordLithosFailureKind(context, lithosTransportFailureKind(error, terminalType));
      };
      adapter.terminalTypeForError = lithosTerminalTypeForError;
      adapter.streamErrorCode = "lithos_upstream_stream_error";
      adapter.responsesProfile = deepseek::responses::LITHOS_RESPONSES_PROFILE;
      return adapter;
    }
    stream_relay::ProviderStreamAdapter lithosStreamAdapterServing(LithosServedModel servedModel, std::shared_ptr<const lithos::RequestDeadline> deadline) {
      std::function<std::string()> servedModelAtFrames;
      if (const auto* getter = std::get_if<std::function<std::string()>>(&servedModel)) servedModelAtFrames = *getter;
      else servedModelAtFrames = [model = std::get<std::string>(std::move(servedModel))] { return model; };
      auto adapter = lithosStreamAdapter();
      adapter.frames = [servedModelAtFrames, deadline = std::move(deadline)](rt::Response upstream, std::string model, stream_relay::ProviderStreamFrameOptions options) -> stream_relay::ProviderStreamIterator {
        // The relay owns the request deadline until it releases the upstream.
        (void)deadline;
        lithos::IterateLithosChatCompletionStreamOptions settings; settings.signal = options.signal; settings.servedModel = servedModelAtFrames();
        return lithosChatStreamFrames(upstream, std::move(model), settings);
      };
      return adapter;
    }
  }
  void recordLithosFailureKind(const telemetry::UsageContext* context, const LithosFailureKind& failureKind) {
    if (context && context->responseTelemetry) context->responseTelemetry->failureKind = failureKind;
  }
  telemetry::ResponseStreamTerminalType lithosTerminalTypeForError(const rt::Reason& error, const rt::AbortSignal& downstreamSignal) {
    if (downstreamSignal.aborted()) return "cancelled";
    try { if (error) std::rethrow_exception(error); }
    catch (const lithos::LithosError& failure) { if (failure.code == "gateway_timeout") return "deadline"; }
    catch (...) {}
    if (rt::isError(error) && rt::errorName(error) == "TimeoutError") return "deadline";
    if (rt::isError(error) && rt::errorName(error) == "AbortError") return "cancelled";
    return "error";
  }
  LithosFailureKind lithosTransportFailureKind(const rt::Reason& error, const telemetry::ResponseStreamTerminalType& terminalType) {
    if (terminalType == "cancelled") return "cancellation";
    if (terminalType == "deadline") return "deadline";
    try { if (error) std::rethrow_exception(error); }
    catch (const api_key_policy::ApiKeyQuotaDispatchError&) { return "api_key_quota_reservation_unavailable"; }
    catch (const lithos::LithosError& failure) {
      if (failure.code == "lithos_api_key_missing" || failure.code == "lithos_request_invalid") return failure.code;
      if (failure.code == "lithos_upstream_unreachable") return "upstream_unreachable";
      if (failure.code == "lithos_upstream_invalid_response") return "lithos_upstream_invalid_response";
    } catch (...) {}
    return "upstream_error";
  }
  void recordLithosResponseHealth(double status, std::optional<std::string> providerRequestId) {
    const std::string event = status == 401 || status == 403 ? "auth_invalid" : status == 402 || status == 429 ? "quota_exhausted"
      : status >= 500 ? "upstream_error" : status >= 400 ? "reachable" : "success";
    (void)health::recordLithosProviderHealth(event, status, rt::dateNow, providerRequestId);
  }
  rt::Response streamLithosChatCompletion(stream_relay::ProviderStreamSource upstream, std::optional<std::string> providerRequestId,
    const telemetry::UsageContext* usageContext, rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal,
    std::string upstreamModel, LithosServedModel servedModel, stream_relay::CancelPending cancelPending,
    std::shared_ptr<const lithos::RequestDeadline> deadline) {
    return withLithosSseKeepalive(stream_relay::relayChatCompletionStream(lithosStreamAdapterServing(std::move(servedModel), std::move(deadline)), std::move(upstream),
      std::move(providerRequestId), usageContext, std::move(downstreamSignal), std::move(requestSignal), std::move(upstreamModel), std::move(cancelPending)));
  }
  rt::Response streamLithosResponses(stream_relay::ProviderStreamSource upstream, std::string requestedModel, std::string responseId,
    double createdAtSeconds, deepseek::responses_payload::DeepSeekResponsesEcho echo,
    rt::OrderedMap<std::string, deepseek::responses::OriginalToolName> toolNames, rt::OrderedSet<std::string> customToolNames,
    std::optional<std::string> providerRequestId, const telemetry::UsageContext* usageContext,
    rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal, std::string upstreamModel,
    LithosServedModel servedModel, stream_relay::CancelPending cancelPending, std::shared_ptr<const lithos::RequestDeadline> deadline) {
    const stream_relay::RelayResponsesStreamOptions options{std::move(upstream), std::move(requestedModel), std::move(responseId), createdAtSeconds,
      std::move(echo), std::move(toolNames), std::move(customToolNames), std::move(providerRequestId), usageContext, std::move(downstreamSignal),
      std::move(requestSignal), std::move(upstreamModel), std::move(cancelPending)};
    return withLithosSseKeepalive(stream_relay::relayResponsesStream(lithosStreamAdapterServing(std::move(servedModel), std::move(deadline)), options));
  }
}
