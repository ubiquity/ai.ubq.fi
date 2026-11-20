#pragma once
// Port of src/responses-attempts.ts.
#include "uos/inference_deadline.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/provider/removed_provider_circuit.hpp"
#include "uos/responses_failover_stream.hpp"
#include <variant>
namespace uos::paid_fallback::routing { struct CodexStreamPreflight; }
namespace uos::responses_attempts {
  using ResponsesAttemptTrigger = std::string;
  struct PreparedResponsesAttempt {
    openai_telemetry::UpstreamProvider provider;
    rt::Response response;
    responses_failover_stream::PreparedResponsesStream prepared;
    std::optional<std::string> responseId;
    std::optional<std::string> selectedModel;
    std::optional<std::string> taskType;
    rt::AbortSignal signal;
    std::function<void(rt::Reason)> abort;
    std::function<void()> clearDeadline;
  };
  struct FailedResponsesAttempt {
    openai_telemetry::UpstreamProvider provider;
    rt::Response response;
    ResponsesAttemptTrigger trigger;
    rt::Nullable<responses_stream::ResponsesStreamEvent> terminal{};
    rt::AbortSignal signal;
    std::function<void()> clearDeadline;
  };
  struct ResponsesAttemptResultReady { static constexpr std::string_view kind = "ready"; PreparedResponsesAttempt attempt; };
  struct ResponsesAttemptResultFailed { static constexpr std::string_view kind = "failed"; FailedResponsesAttempt attempt; };
  using ResponsesAttemptResult = std::variant<ResponsesAttemptResultReady, ResponsesAttemptResultFailed>;
  bool isEligibleResponsesAttemptStatus(const rt::Response& response);
  std::optional<responses_stream::ResponsesStreamFailureKind> failureKindForResponsesAttemptTrigger(const ResponsesAttemptTrigger& trigger);
  struct PrepareResponsesAttemptOptions {
    const openai_telemetry::UsageContext* usageContext = nullptr;
    std::optional<bool> requireEligibleModel{};
    std::optional<bool> rejectFailedTerminal{};
    std::optional<bool> rejectPresemanticFailureTerminal{};
    std::optional<bool> releaseOnProgress{};
    std::shared_ptr<paid_fallback::routing::CodexStreamPreflight> codexStreamPreflight{};
  };
  rt::Task<ResponsesAttemptResult> prepareResponsesAttempt(rt::Response response, std::string provider,
    inference_deadline::StreamDeadline deadline, rt::AbortSignal requestSignal, std::vector<std::string> warnings);
  rt::Task<ResponsesAttemptResult> prepareResponsesAttempt(rt::Response response, std::string provider,
    inference_deadline::StreamDeadline deadline, rt::AbortSignal requestSignal, std::vector<std::string> warnings,
    PrepareResponsesAttemptOptions options);
  struct ResponsesRouteAttempt { openai_telemetry::RoutedResponsesUpstream routed; PreparedResponsesAttempt prepared; openai_telemetry::MeteredTransportLifecycle lifecycle; };
  struct ResponsesRouteFailure { openai_telemetry::RoutedResponsesUpstream routed; FailedResponsesAttempt failed; openai_telemetry::MeteredTransportLifecycle lifecycle; };
  openai_telemetry::ResponseStreamTerminalType responseFailureTerminalType(const ResponsesAttemptTrigger& trigger,
    const rt::AbortSignal& signal, const rt::AbortSignal& downstreamSignal);
  struct PrimaryResponsesOptions {
    std::string model;
    std::optional<std::string> reasoning;
    bool clientWantsStream;
    const openai_telemetry::UsageContext* usageContext = nullptr;
    rt::Nullable<std::string> clientVersion{};
    rt::AbortSignal requestSignal;
    rt::AbortSignal downstreamSignal;
    std::vector<std::string> warnings;
    inference_deadline::StreamDeadline attemptDeadline;
    std::optional<rt::AbortSignal> fallbackSignal{};
    std::function<inference_deadline::StreamDeadline()> createFallbackDeadline{};
    std::optional<bool> rejectPresemanticFailureTerminal{};
    std::optional<bool> releaseOnProgress{};
    std::optional<std::vector<std::string>> allowedPaidProviders{};
    std::optional<std::string> deepSeekWaterfallPaidHop{};
  };
  struct PrimaryResponsesReady { static constexpr std::string_view kind = "ready"; ResponsesRouteAttempt value; };
  struct PrimaryResponsesFailed { static constexpr std::string_view kind = "failed"; ResponsesRouteFailure value; };
  using PrimaryResponsesResult = std::variant<PrimaryResponsesReady, PrimaryResponsesFailed>;
  rt::Task<PrimaryResponsesResult> fetchAndPreparePrimaryResponses(json::object body, PrimaryResponsesOptions options);
  struct RemovedProviderResponsesOptions {
    const openai_telemetry::UsageContext* usageContext = nullptr;
    rt::AbortSignal requestSignal;
    std::optional<std::string> sessionId;
    std::string apiKey;
    inference_deadline::StreamDeadline attemptDeadline;
  };
  rt::Task<ResponsesAttemptResult> fetchAndPrepareRemovedProviderResponses(json::object body,
    RemovedProviderResponsesOptions options);
  struct FinalizeAbandonedPrimaryAttemptOptions { std::optional<bool> cancelled{}; std::optional<ResponsesAttemptTrigger> failureTrigger{}; };
  rt::Task<void> finalizeAbandonedPrimaryAttempt(openai_telemetry::RoutedResponsesUpstream routed,
    openai_telemetry::MeteredTransportLifecycle lifecycle);
  rt::Task<void> finalizeAbandonedPrimaryAttempt(openai_telemetry::RoutedResponsesUpstream routed,
    openai_telemetry::MeteredTransportLifecycle lifecycle, FinalizeAbandonedPrimaryAttemptOptions options);
  void markPrimarySemanticRecovery(const openai_telemetry::RoutedResponsesUpstream& routed,
    const std::optional<provider::removed_provider_circuit::RemovedProviderCircuitProbe>& circuitProbe,
    const openai_telemetry::UsageContext* usageContext = nullptr, std::optional<std::string> terminalType = std::nullopt);
}
