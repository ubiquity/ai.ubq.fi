#pragma once
// Port of src/paid-fallback/routing.ts.
#include "uos/openai_telemetry.hpp"
namespace uos::paid_fallback::routing {
  struct FetchResponsesWithPaidFallbackOptions {
    std::string model;
    std::string route;
    bool stream;
    std::optional<std::string> reasoning;
    const openai_telemetry::UsageContext* usageContext = nullptr;
    rt::Nullable<std::string> clientVersion{};
    std::optional<rt::AbortSignal> signal{};
    std::optional<rt::AbortSignal> fallbackSignal{};
    std::optional<std::vector<std::string>> allowedPaidProviders{};
    std::optional<std::string> deepSeekWaterfallPaidHop{};
  };
  rt::Task<openai_telemetry::RoutedResponsesUpstream> fetchResponsesWithPaidFallback(json::object body,
    FetchResponsesWithPaidFallbackOptions options);
}
