#pragma once
// Port of src/provider/cerebras-handlers.ts.
#include "uos/openai_telemetry.hpp"
namespace uos::provider::cerebras_handlers {
  void recordCerebrasResponseHealth(double status, std::optional<std::string> providerRequestId);
  using CerebrasFailureKind = std::string;
  void recordCerebrasFailureKind(const openai_telemetry::UsageContext* context, const CerebrasFailureKind& failureKind);
  std::optional<rt::Response> cerebrasReasoningEffortRefusal(const std::string& upstreamModel, const json::value* reasoningEffort);
  std::optional<rt::Response> cerebrasReasoningEffortRefusal(const std::string& upstreamModel, const json::value& reasoningEffort);
  rt::Task<rt::Response> respondCerebrasChatInvalidCompletion(std::string failureKind, double upstreamStatus,
    std::optional<std::string> providerRequestId, const openai_telemetry::UsageContext* usageContext);
  struct CerebrasChatCompletionOk { static constexpr bool ok = true; json::object value; };
  struct CerebrasResponseNotOk { static constexpr bool ok = false; rt::Response response; };
  using CerebrasChatCompletionResult = std::variant<CerebrasChatCompletionOk, CerebrasResponseNotOk>;
  rt::Task<CerebrasChatCompletionResult> readCerebrasChatCompletion(rt::Bytes bytes, double upstreamStatus,
    std::optional<std::string> providerRequestId, const openai_telemetry::UsageContext* usageContext, std::string model);
  rt::Task<rt::Response> respondCerebrasChatDispatchFailure(rt::Reason error, rt::AbortSignal downstreamSignal,
    const openai_telemetry::UsageContext* usageContext);
  rt::Task<rt::Response> respondCerebrasChatUpstreamHttpFailure(rt::Response upstream, rt::AbortSignal requestSignal,
    std::optional<std::string> providerRequestId, const openai_telemetry::UsageContext* usageContext);
  rt::Task<rt::Response> respondCerebrasChatIncompleteCapture(const openai_telemetry::UsageContext* usageContext,
    rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal, std::optional<std::string> providerRequestId);
  rt::Task<rt::Response> handleCerebrasChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext = nullptr, std::string upstreamModel = "gpt-oss-120b");
}
