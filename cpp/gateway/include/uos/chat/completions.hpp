#pragma once
// Port of src/chat/completions.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"

#include <functional>
#include <string>
#include <vector>

namespace uos::chat::completions {
  rt::Response streamChatCompletions(responses_stream::PreflightedResponsesStream source, std::string model, bool includeUsage,
                                     const openai_telemetry::UsageContext* usageContext, openai_telemetry::UpstreamProvider provider,
                                     openai_telemetry::MeteredTransportLifecycle lifecycle, rt::AbortSignal signal, rt::AbortSignal downstreamSignal,
                                     std::function<void(openai_telemetry::ResponseStreamTerminalType)> onResponseTerminal = {});
  rt::Task<rt::Response> completeChatCompletions(responses_stream::PreflightedResponsesStream source, std::string model,
                                                const openai_telemetry::UsageContext* usageContext, openai_telemetry::UpstreamProvider provider,
                                                openai_telemetry::MeteredTransportLifecycle lifecycle, rt::AbortSignal signal, rt::AbortSignal downstreamSignal,
                                                std::vector<std::string> warnings = {},
                                                std::function<void(openai_telemetry::ResponseStreamTerminalType)> onResponseTerminal = {});
}
