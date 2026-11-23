#pragma once
// Port of src/decisions/handlers.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"

#include <functional>
#include <optional>
#include <string>

namespace uos::decisions::handlers {
  struct DecisionsHandlerDeps {
    provider::openrouter::OpenRouterFetch fetcher{};
    std::function<std::optional<std::string>()> apiKey{};
  };

  rt::Task<rt::Response> handleDecisions(rt::Request req, const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<rt::Response> handleDecisions(rt::Request req, const openai_telemetry::UsageContext* usageContext, const DecisionsHandlerDeps& deps);
}
