#pragma once
// Port of src/provider/cerebras-responses.ts.
#include "uos/openai_telemetry.hpp"
namespace uos::provider::cerebras_responses {
  rt::Task<rt::Response> handleCerebrasResponses(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext = nullptr);
}
