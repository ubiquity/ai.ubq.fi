#pragma once
// Port of src/deepseek/responses-stream.ts.
#include "uos/deepseek/responses.hpp"
#include "uos/deepseek/responses_payload.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace uos::deepseek::responses_stream {
  struct DeepSeekResponsesAnswerBearingOutput { std::string text; std::string refusal; double toolCallCount; };
  using DeepSeekResponsesTerminalKind = std::string;
  DeepSeekResponsesTerminalKind deepSeekResponsesTerminalKind(const json::value* finishReason, const responses::ChatOnlyResponsesProfile& profile = responses::DEEPSEEK_RESPONSES_PROFILE);
  DeepSeekResponsesTerminalKind deepSeekResponsesTerminalKind(const json::value& finishReason, const responses::ChatOnlyResponsesProfile& profile = responses::DEEPSEEK_RESPONSES_PROFILE);
  struct DeepSeekResponsesStreamTranslator {
    std::function<std::vector<json::object>()> open;
    std::function<std::vector<json::object>(const json::object&)> push;
    /**
     * The accumulated translated output facts a terminal-validity decision needs.
     *
     * Only the caller decides whether a completion is usable; this is the view it
     * decides on. Reasoning is deliberately absent: it is streaming progress, not
     * an answer a client can act on, so a stream whose only output is reasoning
     * still reports empty text and no tool calls here.
     */
    std::function<DeepSeekResponsesAnswerBearingOutput()> answerBearingOutput;
    /**
     * The gateway terminal a Chat `finish_reason` maps onto, decided once here.
     *
     * `stop`, `tool_calls` and an absent reason are normal completions. `length`
     * means the provider stopped at an output or context boundary, which the
     * Responses schema reports as `incomplete` with the single reason
     * `max_output_tokens` (the schema spells it that way; it is not `max_tokens`).
     * Any other value must not be laundered into a normal stop: it becomes a
     * failed terminal, and the route records the raw value in telemetry.
     */
    std::function<DeepSeekResponsesTerminalKind()> terminalKind;
    std::function<std::optional<std::string>()> upstreamFinishReason;
    std::function<std::vector<json::object>()> finish;
  };
  DeepSeekResponsesStreamTranslator createDeepSeekResponsesStreamTranslator(const std::string& requestedModel, const std::string& responseId,
    const responses_payload::DeepSeekResponsesEcho& echo, double createdAtSeconds, const rt::OrderedMap<std::string, responses::OriginalToolName>& toolNames = {},
    const rt::OrderedSet<std::string>& customToolNames = {}, const responses::ChatOnlyResponsesProfile& profile = responses::DEEPSEEK_RESPONSES_PROFILE);
  std::string encodeResponsesEvent(const json::object& value);
}
