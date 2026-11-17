#pragma once
// Port of src/provider/stream-relay.ts.
#include "uos/deepseek/responses.hpp"
#include "uos/deepseek/responses_payload.hpp"
#include "uos/deepseek/stream.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/ordered.hpp"

#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace uos::provider::stream_relay {
  using ProviderStreamFrame = deepseek::stream::DeepSeekStreamFrame;

  // The source's mutable async-generator methods remain observable to adapters and fixtures.
  struct ProviderStreamIterator {
    std::function<rt::Task<rt::IteratorResult<ProviderStreamFrame>>()> next;
    std::function<rt::Task<rt::IteratorResult<ProviderStreamFrame>>()> return_;
    ProviderStreamIterator(rt::AsyncGenerator<ProviderStreamFrame> frames)
      : next([frames] { return frames.next(); }), return_([frames] { return frames.return_(); }) {}
    ProviderStreamIterator(decltype(next) next_, decltype(return_) return__)
      : next(std::move(next_)), return_(std::move(return__)) {}
  };

  struct ProviderStreamFrameOptions { rt::AbortSignal signal; };

  /** The adapter a chat-wire relay needs; the Responses translator profile is not part of it. */
  struct ProviderChatStreamAdapter {
    std::function<rt::Headers(std::optional<std::string>)> responseHeaders;
    std::function<ProviderStreamIterator(rt::Response, std::string, ProviderStreamFrameOptions)> frames;
    std::function<void(double, std::optional<std::string>)> recordResponseHealth;
    std::function<void(std::optional<double>, std::optional<std::string>)> recordProviderError;
    std::function<void(const openai_telemetry::UsageContext*)> recordCancellation;
    std::function<void(const openai_telemetry::UsageContext*)> recordIncompleteResponse;
    std::function<void(const openai_telemetry::UsageContext*, std::optional<std::string>)> recordFinishFailureKind;
    std::function<void(const openai_telemetry::UsageContext*, rt::Reason, openai_telemetry::ResponseStreamTerminalType)> recordTransportFailure;
    std::function<openai_telemetry::ResponseStreamTerminalType(rt::Reason, rt::AbortSignal)> terminalTypeForError;
    std::string streamErrorCode;
  };

  struct ProviderStreamAdapter : ProviderChatStreamAdapter {
    deepseek::responses::ChatOnlyResponsesProfile responsesProfile;
  };

  using ProviderStreamSource = std::variant<rt::Response, rt::Promise<rt::Response>>;
  using CancelPending = std::function<void(rt::Reason)>;

  rt::Response relayChatCompletionStream(ProviderChatStreamAdapter adapter, ProviderStreamSource upstreamSource,
    std::optional<std::string> providerRequestId, const openai_telemetry::UsageContext* usageContext,
    rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal, std::string upstreamModel, CancelPending cancelPending = {});

  struct RelayResponsesStreamOptions {
    ProviderStreamSource upstream;
    std::string requestedModel;
    std::string responseId;
    double createdAtSeconds;
    deepseek::responses_payload::DeepSeekResponsesEcho echo;
    rt::OrderedMap<std::string, deepseek::responses::OriginalToolName> toolNames;
    rt::OrderedSet<std::string> customToolNames;
    std::optional<std::string> providerRequestId;
    const openai_telemetry::UsageContext* usageContext;
    rt::AbortSignal downstreamSignal;
    rt::AbortSignal requestSignal;
    std::string upstreamModel;
    CancelPending cancelPending{};
  };

  rt::Response relayResponsesStream(ProviderStreamAdapter adapter, const RelayResponsesStreamOptions& options);
}
