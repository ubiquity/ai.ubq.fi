#pragma once
// Port of src/provider/openrouter-handlers.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/provider/openrouter.hpp"
namespace uos::provider::openrouter_handlers {
  using OpenRouterRouteTransport = std::function<rt::Task<rt::Response>(const json::object&, const openrouter::OpenRouterDispatchInit&)>;
  struct OpenRouterHandlerDeps { OpenRouterRouteTransport fetchChat{}; OpenRouterRouteTransport fetchResponses{}; };
  rt::Task<rt::Response> handleOpenRouterChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<rt::Response> handleOpenRouterChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext, OpenRouterHandlerDeps deps);
  rt::Task<rt::Response> handleOpenRouterResponses(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<rt::Response> handleOpenRouterResponses(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext, OpenRouterHandlerDeps deps);
}
