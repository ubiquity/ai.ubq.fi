#pragma once
// Port of src/handler/terminal-route.ts.
#include "uos/handler/http.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace uos::handler::terminal_route {
  struct PersistInferenceExceptionReplayOptions {
    std::function<rt::Promise<sentinel::replay_observation::SentinelReplayPersistResult>(
      sentinel::replay_model::AcceptedSentinelReplayInput input, sentinel::replay_model::SentinelFailureObservation observation)> persistSentinelReplay{};
    http::SentinelBackgroundTaskRegistrar waitUntil{};
  };

  rt::Task<void> persistInferenceExceptionReplay(sentinel::replay_model::AcceptedSentinelReplayInput sentinelReplayInput, rt::Reason runError);
  rt::Task<void> persistInferenceExceptionReplay(sentinel::replay_model::AcceptedSentinelReplayInput sentinelReplayInput, rt::Reason runError,
                                               PersistInferenceExceptionReplayOptions options);

  struct UosTerminalRouteEntry {
    std::string_view method;
    std::string_view path;
    bool prefix;
    std::string_view handler;
  };
  inline constexpr std::array<UosTerminalRouteEntry, 3> UOS_TERMINAL_ROUTES = {{
    {"POST", "/uos/embeddings", false, "embeddings"},
    {"POST", "/uos/embedding-jobs", false, "embeddingJobCreate"},
    {"GET", "/uos/embedding-jobs/", true, "embeddingJobGet"},
  }};

  rt::Task<rt::Response> handleTerminalRoute(rt::Request req, std::string path, std::optional<http::RequestDeliveryInfo> delivery,
                                            std::string requestId, double requestStartedAtMs, double requestStartedAtMonotonicMs);
}
