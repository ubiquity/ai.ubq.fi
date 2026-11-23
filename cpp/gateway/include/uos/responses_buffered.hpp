#pragma once
// Port of src/responses-buffered.ts.
#include "uos/responses_attempts.hpp"
namespace uos::responses_buffered {
  struct BufferedResponsesOptions {
    std::optional<std::string> warningModel{};
    const openai_telemetry::UsageContext* usageContext = nullptr;
    std::function<void(const responses_stream::ResponsesStreamEvent&)> onTerminal{};
    std::function<void(const responses_stream::ResponsesStreamEvent&)> onEvent{};
    std::function<void(const responses_stream::ResponsesStreamEvent&)> validateEvent{};
    std::function<std::optional<rt::Response>(const rt::Reason&, const responses_failover_stream::OwnedResponsesStreamFailureDetails*)> onFailure{};
  };
  rt::Task<rt::Response> collectBufferedResponses(responses_attempts::PreparedResponsesAttempt attempt);
  rt::Task<rt::Response> collectBufferedResponses(responses_attempts::PreparedResponsesAttempt attempt, BufferedResponsesOptions options);
}
