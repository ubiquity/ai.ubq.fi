#pragma once
// Port of src/chat/envelope.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/rt/http.hpp"
namespace uos::chat::envelope {
  rt::Task<rt::Response> handleChatCompletionsInternal(rt::Request req, const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<rt::Response> handleChatCompletions(rt::Request req, const openai_telemetry::UsageContext* usageContext = nullptr);
}
