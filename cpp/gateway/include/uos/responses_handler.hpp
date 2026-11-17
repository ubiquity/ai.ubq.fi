#pragma once
// Port of src/responses-handler.ts.
#include "uos/responses_request.hpp"
namespace uos::responses_handler {
  void recordRemovedProviderCircuitTransition(const openai_telemetry::UsageContext* usageContext, const std::string& transition);
  struct ResponsesTailOptions { std::optional<std::vector<std::string>> allowedPaidProviders{}; std::optional<std::string> deepSeekWaterfallPaidHop{}; };
  rt::Task<rt::Response> runOrdinaryResponsesTail(rt::Request req, json::object rawRecord, ::uos::types::ResponsesRequest rawBody,
    const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<rt::Response> runOrdinaryResponsesTail(rt::Request req, json::object rawRecord, ::uos::types::ResponsesRequest rawBody,
    const openai_telemetry::UsageContext* usageContext, ResponsesTailOptions options);
  rt::Task<rt::Response> runResponsesHandler(rt::Request req, const openai_telemetry::UsageContext* usageContext = nullptr,
    const json::value* parsedBody = nullptr);
  rt::Task<rt::Response> handleResponses(rt::Request req, const openai_telemetry::UsageContext* usageContext = nullptr);
}
