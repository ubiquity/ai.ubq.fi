#pragma once
// Port of src/paid-fallback/routing.ts.
#include "uos/openai_telemetry.hpp"
namespace uos::paid_fallback::routing {
  struct CodexStreamPreflight {
    std::optional<responses_failover_stream::PreparedResponsesStream> prepared;
    rt::Reason error;
  };

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
    std::shared_ptr<CodexStreamPreflight> codexStreamPreflight{};
  };
  rt::Task<openai_telemetry::RoutedResponsesUpstream> fetchResponsesWithPaidFallback(json::object body,
    FetchResponsesWithPaidFallbackOptions options);
}
