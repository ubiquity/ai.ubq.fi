#pragma once
// Port of src/provider/lithos-streams.ts.
#include "uos/provider/stream_relay.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace uos::provider::lithos { class RequestDeadline; }

namespace uos::provider::lithos_streams {
  using LithosFailureKind = std::string;
  void recordLithosFailureKind(const openai_telemetry::UsageContext* context, const LithosFailureKind& failureKind);
  openai_telemetry::ResponseStreamTerminalType lithosTerminalTypeForError(const rt::Reason& error, const rt::AbortSignal& downstreamSignal);
  LithosFailureKind lithosTransportFailureKind(const rt::Reason& error, const openai_telemetry::ResponseStreamTerminalType& terminalType);
  void recordLithosResponseHealth(double status, std::optional<std::string> providerRequestId);

  using LithosServedModel = std::variant<std::string, std::function<std::string()>>;

  rt::Response streamLithosChatCompletion(stream_relay::ProviderStreamSource upstream, std::optional<std::string> providerRequestId,
    const openai_telemetry::UsageContext* usageContext, rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal,
    std::string upstreamModel, LithosServedModel servedModel, stream_relay::CancelPending cancelPending = {},
    std::shared_ptr<const lithos::RequestDeadline> deadline = {});

  rt::Response streamLithosResponses(stream_relay::ProviderStreamSource upstream, std::string requestedModel, std::string responseId,
    double createdAtSeconds, deepseek::responses_payload::DeepSeekResponsesEcho echo,
    rt::OrderedMap<std::string, deepseek::responses::OriginalToolName> toolNames, rt::OrderedSet<std::string> customToolNames,
    std::optional<std::string> providerRequestId, const openai_telemetry::UsageContext* usageContext,
    rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal, std::string upstreamModel,
    LithosServedModel servedModel, stream_relay::CancelPending cancelPending = {},
    std::shared_ptr<const lithos::RequestDeadline> deadline = {});
}
