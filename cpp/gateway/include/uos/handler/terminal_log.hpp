#pragma once
// Port of src/handler/terminal-log.ts.
#include "uos/admin/error_log.hpp"
#include "uos/cache/prompt_analytics_core.hpp"
#include "uos/cache/telemetry_gate.hpp"
#include "uos/handler/http.hpp"
#include "uos/paid_fallback/rollups.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"

#include <functional>
#include <optional>
#include <string>

namespace uos::handler::terminal_log {
  using TerminalAdmissionStage = std::string;  // "pre_admission" | "accepted"

  struct QuotaAccountingFailureInput {
    std::string route;
    std::string requestId;
  };
  void warnQuotaAccountingFailure(const QuotaAccountingFailureInput& input, rt::Reason error);

  struct WithTerminalRequestLogInput {
    std::string route;
    std::optional<rt::Response> telemetryResponse{};
    double startedAtMonotonicMs;
    std::string requestId;
    std::optional<rt::Request> request{};
    std::optional<TerminalAdmissionStage> admissionStage{};
    std::function<rt::Promise<void>(std::string outcome, std::optional<std::string> reason)> onTerminal{};
    /** Releases the process-resource permit exactly once without awaiting accounting. */
    std::function<void()> onSettled{};
    std::optional<rt::Promise<void>> deliveryCompleted{};
    std::optional<rt::AbortSignal> deliverySignal{};
    std::optional<double> admissionWaitMs{};
    std::optional<double> requestStartedAtMs{};
    std::function<rt::Promise<bool>(authority::native_paid_contracts::TerminalUsageRollupInput input)> recordUsageRollup{};
    std::function<rt::Promise<cache::prompt_analytics_core::PromptCacheAnalyticsRecordResult>(cache::prompt_analytics_core::PromptCacheAnalyticsEvent input)>
      recordCacheAnalytics{};
    std::function<rt::Promise<cache::telemetry_gate::PromptCacheTelemetryRecordResult>(cache::telemetry_gate::PromptCacheTelemetryEvent input)> recordTelemetry{};
    std::optional<sentinel::replay_model::AcceptedSentinelReplayInput> sentinelReplayInput{};
    std::function<rt::Promise<sentinel::replay_observation::SentinelReplayPersistResult>(
      sentinel::replay_model::AcceptedSentinelReplayInput input, sentinel::replay_model::SentinelFailureObservation observation,
      sentinel::replay_model::SentinelClientFailureObservation clientObservation)> persistSentinelReplay{};
    std::optional<sentinel::replay_model::SentinelReplayCaptureOmissionReason> sentinelReplayOmission{};
    std::function<rt::Promise<void>(std::string requestId, sentinel::replay_model::SentinelReplayCaptureOmissionReason reason, double nowMs)>
      recordSentinelReplayOmission{};
    std::function<rt::Promise<bool>(double observedAtMs)> recordSentinelDegradation{};
    std::function<rt::Promise<void>(admin::error_log::AdminErrorInput input)> recordAdminError{};
    http::SentinelBackgroundTaskRegistrar waitUntil{};
  };

  // Plain entry functions start the source's synchronous setup at the call point.
  rt::Promise<rt::Response> withTerminalRequestLog(rt::Response response, WithTerminalRequestLogInput input);

  struct RejectionTerminalLogInput {
    std::string requestId;
    TerminalAdmissionStage admissionStage;
    double startedAtMonotonicMs;
    std::optional<http::RequestDeliveryInfo> delivery{};
    std::function<void()> onSettled{};
    std::optional<double> admissionWaitMs{};
  };
  rt::Promise<rt::Response> withRejectionTerminalLog(rt::Response response, std::optional<std::string> route, RejectionTerminalLogInput input);
}
