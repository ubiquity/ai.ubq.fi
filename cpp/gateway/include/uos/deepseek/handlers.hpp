#pragma once
// Port of src/deepseek/handlers.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/provider/stream_relay.hpp"
namespace uos::deepseek::handlers {
  void recordDeepSeekResponseHealth(double status, std::optional<std::string> providerRequestId);
  rt::Response respondDeepSeekEmptyBufferedCompletion(const openai_telemetry::UsageContext* usageContext,
    std::optional<openai_telemetry::UsageTokens> usage, double upstreamStatus, std::optional<std::string> providerRequestId);
  void recordBufferedDeepSeekResponsesTerminal(const openai_telemetry::UsageContext* usageContext,
    const json::object& payload, std::optional<openai_telemetry::UsageTokens> usage, double upstreamStatus,
    std::optional<std::string> providerRequestId);
  openai_telemetry::ResponseStreamTerminalType deepSeekTerminalTypeForPayload(const json::value* status);
  openai_telemetry::ResponseStreamTerminalType deepSeekTerminalTypeForPayload(const json::value& status);
  inline constexpr double DEEPSEEK_BUFFERED_BODY_MAX_BYTES = 8 * 1024 * 1024;
  struct DeepSeekChatRequestFields { std::string reasoning; bool clientWantsStream; };
  struct DeepSeekChatRequestOk { static constexpr bool ok = true; DeepSeekChatRequestFields value; };
  struct DeepSeekResponseNotOk { static constexpr bool ok = false; rt::Response response; };
  using DeepSeekChatRequestResult = std::variant<DeepSeekChatRequestOk, DeepSeekResponseNotOk>;
  DeepSeekChatRequestResult validateDeepSeekChatRequestFields(const json::object& rawRecord);
  std::optional<double> deepSeekChatClientOutputAllowance(const json::object& rawRecord);
  struct DeepSeekChatCompletionOk { static constexpr bool ok = true; json::object value; };
  using DeepSeekChatCompletionResult = std::variant<DeepSeekChatCompletionOk, DeepSeekResponseNotOk>;
  rt::Task<DeepSeekChatCompletionResult> readDeepSeekChatCompletion(rt::Bytes bytes, double upstreamStatus,
    std::optional<std::string> providerRequestId, const openai_telemetry::UsageContext* usageContext, std::string upstreamModel);
  rt::Task<rt::Response> respondDeepSeekChatIncompleteCapture(const openai_telemetry::UsageContext* usageContext,
    rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal, std::optional<std::string> providerRequestId);
  extern const provider::stream_relay::ProviderStreamAdapter deepseekStreamAdapter;
  rt::Response streamDeepSeekChatCompletion(rt::Response upstream, std::optional<std::string> providerRequestId,
    const openai_telemetry::UsageContext* usageContext, rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal, std::string upstreamModel);
  struct DeepSeekDispatchOk { static constexpr bool ok = true; rt::Response upstream; std::optional<std::string> providerRequestId;
    rt::AbortSignal requestSignal; rt::AbortSignal downstreamSignal; };
  using DeepSeekDispatchResult = std::variant<DeepSeekDispatchOk, DeepSeekResponseNotOk>;
  rt::Task<DeepSeekDispatchResult> dispatchDeepSeekUpstream(rt::Request req, json::object body, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext);
  rt::Task<rt::Response> handleDeepSeekChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<rt::Response> handleDeepSeekResponses(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext = nullptr);
}
