#pragma once
// Port of src/provider/lithos-handlers.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/rt/abort.hpp"
#include "uos/provider/lithos_rate_limits.hpp"
#include "uos/rt/ordered.hpp"
#include <functional>
#include <memory>
namespace uos::provider::lithos_handlers {
  extern const rt::OrderedSet<std::string> LITHOS_REASONING_LEVEL_SET;
  // C++ lifetime observation only; the observer never owns the dispatch controller.
  void setLithosPendingCancellationObserverForTest(std::function<void(std::weak_ptr<rt::AbortController>)> observer);
  rt::Task<rt::Response> handleLithosChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext = nullptr);
  struct HandleLithosResponsesOptions { std::optional<lithos_rate_limits::LithosRefusalWaitPolicy> refusalWaitPolicy{}; };
  rt::Task<rt::Response> handleLithosResponses(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<rt::Response> handleLithosResponses(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext, HandleLithosResponsesOptions options);
}
