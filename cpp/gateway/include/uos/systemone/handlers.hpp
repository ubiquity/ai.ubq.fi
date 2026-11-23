#pragma once
// Port of src/systemone/handlers.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace uos::systemone::handlers {
  inline constexpr std::string_view SYSTEMONE_DEFAULT_MODEL = "~typesafe/jev-latest";
  inline constexpr double SYSTEMONE_MAX_BODY_BYTES = 262'144;
  inline constexpr double SYSTEMONE_MAX_QUESTIONS = 24;
  inline constexpr double SYSTEMONE_MAX_MODEL_LENGTH = 80;

  struct SystemOneHandlerDeps {
    provider::openrouter::OpenRouterFetch fetcher{};
    std::function<std::optional<std::string>()> apiKey{};
  };

  void recordSystemOneResponseHealth(double status);
  rt::Task<rt::Response> handleSystemOne(rt::Request req, std::optional<openai_telemetry::UsageContext> usageContext = std::nullopt);
  rt::Task<rt::Response> handleSystemOne(rt::Request req, std::optional<openai_telemetry::UsageContext> usageContext, const SystemOneHandlerDeps& deps);
}
