#pragma once
// Port of src/deepseek/waterfall-handler.ts.
#include "uos/deepseek/waterfall.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/provider/selection.hpp"
#include "uos/responses_handler.hpp"
namespace uos::deepseek::waterfall_handler {
  struct DeepSeekWaterfallDispatchInput {
    waterfall::DeepSeekWaterfallProvider provider;
    rt::Request req;
    json::object rawRecord;
    ::uos::types::ResponsesRequest rawBody;
    std::string model;
    const openai_telemetry::UsageContext* usageContext = nullptr;
  };
  using DeepSeekWaterfallDispatch = std::function<rt::Task<rt::Response>(const DeepSeekWaterfallDispatchInput&)>;
  using DeepSeekWaterfallPaidTail = std::function<rt::Task<rt::Response>(rt::Request, const json::object&, const ::uos::types::ResponsesRequest&,
    const openai_telemetry::UsageContext*, const responses_handler::ResponsesTailOptions&)>;
  struct DeepSeekWaterfallOptions {
    DeepSeekWaterfallDispatch dispatch{};
    DeepSeekWaterfallPaidTail paidTail{};
    rt::Nullable<provider::selection::ProviderSelection> selection{};
    std::function<bool(const waterfall::DeepSeekWaterfallProvider&)> enabled{};
    std::function<bool(const waterfall::DeepSeekWaterfallProvider&)> configured{};
  };
  rt::Task<rt::Response> handleDeepSeekWaterfallResponses(rt::Request req, json::object rawRecord);
  rt::Task<rt::Response> handleDeepSeekWaterfallResponses(rt::Request req, json::object rawRecord,
    ::uos::types::ResponsesRequest rawBody, const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<rt::Response> handleDeepSeekWaterfallResponses(rt::Request req, json::object rawRecord,
    ::uos::types::ResponsesRequest rawBody, const openai_telemetry::UsageContext* usageContext, DeepSeekWaterfallOptions options);
}
