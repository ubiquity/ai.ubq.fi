#pragma once
// Port of src/provider/openrouter-streams.ts.
#include "uos/provider/stream_relay.hpp"
#include "uos/rt/error.hpp"

#include <optional>
#include <string>

namespace uos::provider::openrouter_streams {
  inline constexpr double OPENROUTER_SSE_FRAME_MAX_BYTES = 4 * 1024 * 1024;
  using OpenRouterStreamFailureKind = std::string;
  struct OpenRouterStreamErrorOptions {
    std::optional<rt::Reason> cause{};
    std::optional<OpenRouterStreamFailureKind> kind{};
  };
  class OpenRouterStreamError : public rt::JsError {
  public:
    OpenRouterStreamFailureKind kind;
    explicit OpenRouterStreamError(std::string message_) : OpenRouterStreamError(std::move(message_), OpenRouterStreamErrorOptions{}) {}
    OpenRouterStreamError(std::string message_, const OpenRouterStreamErrorOptions& options)
      : rt::JsError(std::move(message_), "OpenRouterStreamError"), kind(options.kind.value_or("read_error")) {
      if (options.cause) {
        causeValue = *options.cause;
        try { if (*options.cause) std::rethrow_exception(*options.cause); }
        catch (const rt::JsError& inner) { cause = std::make_shared<rt::JsError>(inner); }
        catch (...) {}
      }
    }
  };

  void recordOpenRouterResponseHealth(double status, std::optional<std::string> providerRequestId = std::nullopt);

  struct OpenRouterStreamOptions {
    std::optional<rt::AbortSignal> signal{};
    std::optional<double> firstEventTimeoutMs{};
    std::optional<double> inactivityTimeoutMs{};
  };
  rt::AsyncGenerator<stream_relay::ProviderStreamFrame> iterateOpenRouterChatCompletionStream(rt::Response response);
  rt::AsyncGenerator<stream_relay::ProviderStreamFrame> iterateOpenRouterChatCompletionStream(rt::Response response, const OpenRouterStreamOptions& options);

  rt::Response streamOpenRouterChatCompletion(stream_relay::ProviderStreamSource upstream, std::optional<std::string> providerRequestId,
    const openai_telemetry::UsageContext* usageContext, rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal, std::string upstreamModel);

  rt::Response streamOpenRouterResponses(rt::Response upstream, std::optional<std::string> providerRequestId,
    const openai_telemetry::UsageContext* usageContext, rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal);
}
