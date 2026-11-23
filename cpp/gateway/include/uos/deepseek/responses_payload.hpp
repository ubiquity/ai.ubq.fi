#pragma once
// Port of src/deepseek/responses-payload.ts.
#include "uos/deepseek/responses.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include <optional>
#include <string>

namespace uos::deepseek::responses_payload {
  json::object reasoningItem(const std::string& id, const std::string& text);
  json::object functionCallItem(const std::string& id, const std::string& callId, const std::string& name, const std::string& args, const std::optional<std::string>& namespace_ = std::nullopt);
  json::object customToolCallItem(const std::string& id, const std::string& callId, const std::string& name, const std::string& input);
  std::string freeformInputFromArguments(const std::string& args);
  std::optional<json::object> toResponsesUsage(const json::value* value, const responses::ChatOnlyResponsesProfile& profile = responses::DEEPSEEK_RESPONSES_PROFILE);
  std::optional<json::object> toResponsesUsage(const json::value& value, const responses::ChatOnlyResponsesProfile& profile = responses::DEEPSEEK_RESPONSES_PROFILE);
  struct DeepSeekResponsesEcho {
    json::value tools;
    json::value tool_choice;
    json::value parallel_tool_calls;
    std::optional<std::string> instructions;
  };
  json::object responsesEnvelope(const std::string& responseId, const std::string& requestedModel, double createdAtSeconds, const std::string& status, const DeepSeekResponsesEcho& echo);
  std::string deepSeekTerminalTypeForDisposition(const index::DeepSeekFinishDisposition& disposition);
  struct DeepSeekTerminalEnvelope { std::string type; json::object response; };
  DeepSeekTerminalEnvelope deepSeekTerminalEnvelope(const std::string& responseId, const std::string& requestedModel, double createdAtSeconds,
    const std::string& status, const DeepSeekResponsesEcho& echo, const json::value* finishReason,
    const responses::ChatOnlyResponsesProfile& profile = responses::DEEPSEEK_RESPONSES_PROFILE);
  DeepSeekTerminalEnvelope deepSeekTerminalEnvelope(const std::string& responseId, const std::string& requestedModel, double createdAtSeconds,
    const std::string& status, const DeepSeekResponsesEcho& echo, const json::value& finishReason,
    const responses::ChatOnlyResponsesProfile& profile = responses::DEEPSEEK_RESPONSES_PROFILE);
  json::object toDeepSeekResponsesPayload(const json::object& completion, const std::string& requestedModel, const std::string& responseId,
    const DeepSeekResponsesEcho& echo, const rt::OrderedMap<std::string, responses::OriginalToolName>& toolNames = {},
    const rt::OrderedSet<std::string>& customToolNames = {}, const responses::ChatOnlyResponsesProfile& profile = responses::DEEPSEEK_RESPONSES_PROFILE);
}
