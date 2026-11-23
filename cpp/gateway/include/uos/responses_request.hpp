#pragma once
// Port of src/responses-request.ts.
#include "uos/request_policy.hpp"
#include "uos/responses_attempts.hpp"
namespace uos::responses_request {
  template<class T> struct ResponsesStepOk { static constexpr bool ok = true; T value; };
  struct ResponsesStepNotOk { static constexpr bool ok = false; rt::Response response; };
  template<class T> using ResponsesStep = std::variant<ResponsesStepOk<T>, ResponsesStepNotOk>;
  struct ResponsesRequestState {
    std::optional<openai_telemetry::UsageContext> usageContext;
    json::object rawRecord;
    std::vector<std::string> warnings;
    bool clientWantsStream;
    std::string modelRaw;
    std::string model;
    request_policy::CodexModelMetadata modelMetadata;
    double explicitBreakpointCount;
    defaults::ReasoningEffort reasoningLabel;
    json::object codexBody;
    json::object removedProviderBody;
  };
  struct ResponsesCircuitRoute { std::string route; std::optional<provider::removed_provider_circuit::RemovedProviderCircuitProbe> probe; };
  struct ResponsesFailureCorrelation {
    std::optional<std::string> provider;
    std::optional<double> accountSlot;
    std::optional<std::string> accountCohortId;
    std::optional<double> activeGeneration;
    openai_telemetry::ActiveTransitionReason activeTransitionReason;
    std::optional<std::string> providerRequestId;
  };
  struct ResponsesRoutingState {
    rt::AbortSignal downstreamSignal;
    rt::AbortSignal requestInferenceSignal;
    inference_deadline::StreamDeadline preHeaderDeadline;
    std::optional<std::string> apiKey;
    bool paidFallbackAvailable;
    std::optional<std::string> sessionId;
    std::string route;
    std::optional<provider::removed_provider_circuit::RemovedProviderCircuitProbe> probe;
    std::optional<rt::Response> primaryFailureResponse;
    std::optional<ResponsesFailureCorrelation> primaryFailureCorrelation;
    std::optional<responses_attempts::ResponsesRouteAttempt> primaryResult;
    std::optional<responses_attempts::PreparedResponsesAttempt> removedProviderAttempt;
    std::optional<std::string> selectedModel;
    double fallbackStartedAt;
  };
  struct ResponsesHandlerState : ResponsesRequestState, ResponsesRoutingState {};
  struct ResponsesDeliveryState {
    std::optional<openai_telemetry::UsageContext> usageContext;
    responses_attempts::PreparedResponsesAttempt ready;
    openai_telemetry::MeteredTransportLifecycle lifecycle;
    std::optional<openai_telemetry::RoutedResponsesUpstream> routed;
    bool clientWantsStream;
    rt::AbortSignal downstreamSignal;
    std::vector<std::string> clientWarnings;
    std::optional<std::string> warningModel;
    std::optional<std::string> selectedModel;
    std::optional<provider::removed_provider_circuit::RemovedProviderCircuitProbe> probe;
    std::optional<responses_attempts::PreparedResponsesAttempt> removedProviderAttempt;
    double fallbackStartedAt;
    bool providerTerminalValidated;
    std::function<void()> clearProbeRenewal;
  };
  struct ReadResponsesRequestValue { json::object rawRecord; };
  rt::Task<ResponsesStep<ReadResponsesRequestValue>> readResponsesRequest(rt::Request req, const json::value* parsedBody = nullptr);
  std::optional<rt::Response> validateResponsesRequestFields(const json::object& rawRecord, const ::uos::types::ResponsesRequest& rawBody,
    const rt::OrderedSet<std::string>* providerExtensions = nullptr);
  struct ResponsesStreamSettings { std::vector<std::string> warnings; bool clientWantsStream; };
  ResponsesStep<ResponsesStreamSettings> resolveResponsesStreamSettings(const json::object& rawRecord, const ::uos::types::ResponsesRequest& rawBody);
  struct ResponsesModel { std::string modelRaw; std::string model; request_policy::CodexModelMetadata modelMetadata; };
  struct ResolveResponsesModelOptions { std::optional<std::vector<std::string>> allowedPaidProviders{}; std::optional<std::string> deepSeekWaterfallPaidHop{}; };
  rt::Task<ResponsesStep<ResponsesModel>> resolveResponsesModel(const json::object& rawRecord, const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<ResponsesStep<ResponsesModel>> resolveResponsesModel(const json::object& rawRecord, const openai_telemetry::UsageContext* usageContext,
    ResolveResponsesModelOptions options);
  ResponsesStep<std::vector<::uos::types::ResponseInputItem>> normalizeResponsesInput(const json::value* inputRaw);
  ResponsesStep<std::vector<::uos::types::ResponseInputItem>> normalizeResponsesInput(const json::value& inputRaw);
  struct ResponsesReasoning { defaults::ReasoningEffort reasoningLabel; std::optional<std::string> instructions; std::optional<json::object> reasoning; };
  rt::Task<ResponsesStep<ResponsesReasoning>> resolveResponsesReasoning(const json::object& rawRecord, const ::uos::types::ResponsesRequest& rawBody,
    const request_policy::CodexModelMetadata& modelMetadata, const openai_telemetry::UsageContext* usageContext = nullptr);
  struct ResponsesUpstreamBodies { json::object codexBody; json::object removedProviderBody; };
  ResponsesUpstreamBodies buildResponsesUpstreamBodies(const std::string& model, const std::vector<::uos::types::ResponseInputItem>& input,
    const std::optional<std::string>& instructions, const std::optional<json::object>& reasoning, const json::object& rawRecord);
}
