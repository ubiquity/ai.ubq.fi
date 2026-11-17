#include "uos/deepseek/responses_payload.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/number.hpp"
#include "uos/utils.hpp"

namespace uos::deepseek::responses_payload {
namespace {
  template<class T> std::optional<std::string> stringField(const T& value, std::string_view key) { return utils::getString(rt::get(value, key)); }
  json::array responseMessageContent(const std::string& text, const std::string& refusal) {
    json::array content;
    if (!text.empty()) content.push_back(json::object{{"type", "output_text"}, {"text", text}, {"annotations", json::array{}}});
    if (!refusal.empty()) content.push_back(json::object{{"type", "refusal"}, {"refusal", refusal}});
    return content;
  }
  json::object responseMessageItem(const std::string& id, const std::string& text, const std::string& refusal = "") {
    return {{"id", id}, {"type", "message"}, {"status", "completed"}, {"role", "assistant"}, {"content", responseMessageContent(text, refusal)}};
  }
}
json::object reasoningItem(const std::string& id, const std::string& text) {
  return {{"id", id}, {"type", "reasoning"}, {"status", "completed"}, {"summary", json::array{json::object{{"type", "summary_text"}, {"text", text}}}}};
}
json::object functionCallItem(const std::string& id, const std::string& callId, const std::string& name, const std::string& args, const std::optional<std::string>& namespace_) {
  json::object item{{"id", id}, {"type", "function_call"}, {"status", "completed"}, {"call_id", callId}, {"name", name}};
  if (namespace_ && !namespace_->empty()) item["namespace"] = *namespace_;
  item["arguments"] = args;
  return item;
}
/** Codex's freeform tool item; the text travels in `input`, not JSON arguments. */
json::object customToolCallItem(const std::string& id, const std::string& callId, const std::string& name, const std::string& input) {
  return {{"id", id}, {"type", "custom_tool_call"}, {"status", "completed"}, {"call_id", callId}, {"name", name}, {"input", input}};
}
/**
 * Unwraps the single freeform `input` parameter back into the tool's raw text.
 * The provider answers with JSON arguments because Chat Completions has no
 * freeform tool shape, and the client expects the original text.
 */
std::string freeformInputFromArguments(const std::string& args) {
  try {
    const auto parsed = rt::jsonParse(args);
    if (parsed.is_string()) return rt::toString(parsed);
    if (parsed.is_object()) if (auto input = stringField(parsed, "input")) return *input;
  } catch (...) {}
  return args;
}
/**
 * Maps one Chat Completions usage object onto the Responses usage shape.
 *
 * The counters are the reason this is not a field-by-field copy: Codex reads
 * `input_tokens_details.cached_tokens` and `output_tokens_details.
 * reasoning_tokens`, and each provider publishes those measurements under its
 * own names (DeepSeek `prompt_cache_hit_tokens` / `completion_tokens_details.
 * reasoning_tokens`; LithosAI `prompt_tokens_details.cached_tokens` /
 * `completion_tokens_details.reasoning_tokens`), so the profile's counters are
 * the ones consulted. A counter the upstream did not report leaves its detail
 * object absent, so the client and the gateway telemetry both read an unknown
 * value instead of a measured zero.
 */
std::optional<json::object> toResponsesUsage(const json::value* value, const responses::ChatOnlyResponsesProfile& profile) {
  if (!value || !value->is_object()) return std::nullopt;
  const auto input = rt::getNumber(*value, "prompt_tokens");
  const auto output = rt::getNumber(*value, "completion_tokens");
  if (!input || !output) return std::nullopt;
  const auto total = rt::getNumber(*value, "total_tokens").value_or(*input + *output);
  const auto cached = profile.cachedPromptTokens(value->as_object(), *input);
  const auto reasoning = profile.reasoningTokens(value->as_object(), *output);
  json::object usage;
  rt::objectSet(usage, "input_tokens", *input);
  if (cached) usage["input_tokens_details"] = json::object{{"cached_tokens", rt::makeNumber(*cached)}};
  rt::objectSet(usage, "output_tokens", *output);
  if (reasoning) usage["output_tokens_details"] = json::object{{"reasoning_tokens", rt::makeNumber(*reasoning)}};
  rt::objectSet(usage, "total_tokens", total);
  return usage;
}
std::optional<json::object> toResponsesUsage(const json::value& value, const responses::ChatOnlyResponsesProfile& profile) { return toResponsesUsage(&value, profile); }
/** Fields Codex reads back off a Responses object; the request is echoed verbatim. */
json::object responsesEnvelope(const std::string& responseId, const std::string& requestedModel, double createdAtSeconds, const std::string& status, const DeepSeekResponsesEcho& echo) {
  json::object envelope{{"id", responseId}, {"object", "response"}, {"created_at", rt::makeNumber(createdAtSeconds)}, {"status", status}, {"model", requestedModel},
    {"output", json::array{}}, {"error", nullptr}, {"incomplete_details", nullptr}};
  rt::objectSet(envelope, "instructions", rt::orNull(echo.instructions));
  envelope["metadata"] = json::object{};
  envelope["parallel_tool_calls"] = echo.parallel_tool_calls.is_bool() ? echo.parallel_tool_calls : json::value(true);
  envelope["tool_choice"] = echo.tool_choice.is_null() ? json::value("auto") : echo.tool_choice;
  envelope["tools"] = echo.tools.is_array() ? echo.tools : json::value(json::array{});
  for (std::string_view key : {"temperature", "top_p", "max_output_tokens", "previous_response_id", "reasoning"}) envelope[key] = nullptr;
  envelope["store"] = false;
  envelope["truncation"] = "disabled";
  envelope["usage"] = nullptr;
  return envelope;
}
/**
 * The terminal event and status a DeepSeek `finish_reason` implies.
 *
 * The provider reports one vocabulary for both the Chat endpoint and its native
 * Responses endpoint; the official Responses schema defines only
 * `max_output_tokens`, `max_messages`, `content_filter` and `steered` as
 * `incomplete_details.reason` values, so the two incomplete-capable reasons map
 * onto the schema's own names. A resource interruption and an interruption of
 * unspecified cause are reported as a failed terminal rather than a clean
 * completion, because neither leaves a usable answer.
 */
std::string deepSeekTerminalTypeForDisposition(const index::DeepSeekFinishDisposition& disposition) {
  if (std::holds_alternative<index::DeepSeekFinishDispositionCompleted>(disposition)) return "response.completed";
  if (std::holds_alternative<index::DeepSeekFinishDispositionIncomplete>(disposition)) return "response.incomplete";
  return "response.failed";
}
/**
 * Builds the terminal Responses object for one DeepSeek finish disposition.
 * Only a completed disposition reports `completed`; an incomplete disposition
 * carries `incomplete_details.reason`, and every other disposition names the
 * provider's own reason as the error code so the cause stays visible.
 */
DeepSeekTerminalEnvelope deepSeekTerminalEnvelope(const std::string& responseId, const std::string& requestedModel, double createdAtSeconds,
  const std::string& status, const DeepSeekResponsesEcho& echo, const json::value* finishReason, const responses::ChatOnlyResponsesProfile& profile) {
  const auto disposition = profile.finishDisposition(finishReason);
  const auto terminalType = deepSeekTerminalTypeForDisposition(disposition);
  if (std::holds_alternative<index::DeepSeekFinishDispositionCompleted>(disposition)) return {terminalType, responsesEnvelope(responseId, requestedModel, createdAtSeconds, status, echo)};
  if (const auto* incomplete = std::get_if<index::DeepSeekFinishDispositionIncomplete>(&disposition)) {
    auto response = responsesEnvelope(responseId, requestedModel, createdAtSeconds, "incomplete", echo);
    response["incomplete_details"] = json::object{{"reason", incomplete->reason}};
    return {"response.incomplete", std::move(response)};
  }
  const auto* failed = std::get_if<index::DeepSeekFinishDispositionFailed>(&disposition);
  const auto code = failed ? failed->code : "unrecognized_finish_reason:" + std::get<index::DeepSeekFinishDispositionUnknown>(disposition).value;
  auto response = responsesEnvelope(responseId, requestedModel, createdAtSeconds, "failed", echo);
  response["error"] = json::object{{"code", code}, {"message", std::string(profile.label) + " stopped generating: " + code}};
  return {"response.failed", std::move(response)};
}
DeepSeekTerminalEnvelope deepSeekTerminalEnvelope(const std::string& responseId, const std::string& requestedModel, double createdAtSeconds,
  const std::string& status, const DeepSeekResponsesEcho& echo, const json::value& finishReason, const responses::ChatOnlyResponsesProfile& profile) {
  return deepSeekTerminalEnvelope(responseId, requestedModel, createdAtSeconds, status, echo, &finishReason, profile);
}
namespace {
  /**
   * Output items for one Chat choice: reasoning, message, then any tool calls.
   * Item ids carry the official type prefix (`rs_`, `msg_`, `fc_`, `ctc_`) so a
   * history replayed to a Codex upstream passes its id validation.
   */
  json::array outputItemsForChoice(const json::object& message, double choiceIndex, const std::string& responseId,
    const rt::OrderedMap<std::string, responses::OriginalToolName>& toolNames, const rt::OrderedSet<std::string>& customToolNames) {
    json::array items;
    const auto suffix = responseId + "_" + rt::numberToString(choiceIndex);
    const auto reasoning = stringField(message, "reasoning_content").value_or("");
    if (!reasoning.empty()) items.push_back(reasoningItem("rs_" + suffix, reasoning));
    const auto text = stringField(message, "content").value_or("");
    const auto refusal = stringField(message, "refusal").value_or("");
    if (!text.empty() || !refusal.empty()) items.push_back(responseMessageItem("msg_" + suffix, text, refusal));
    const auto* calls = rt::getArray(message, "tool_calls");
    if (!calls) return items;
    for (std::size_t i = 0; i < calls->size(); ++i) {
      const auto& call = (*calls)[i];
      const auto* fn = rt::getObject(call, "function");
      if (!call.is_object() || !fn) continue;
      const auto index = rt::numberToString(static_cast<double>(i));
      const auto callId = stringField(call, "id").value_or(responseId + "_call_" + index);
      const auto chatName = stringField(*fn, "name").value_or("");
      const auto original = responses::originalTool(chatName, toolNames);
      const auto args = stringField(*fn, "arguments").value_or("");
      if (customToolNames.has(chatName)) items.push_back(customToolCallItem("ctc_" + suffix + "_" + index, callId, original.name, freeformInputFromArguments(args)));
      else items.push_back(functionCallItem("fc_" + suffix + "_" + index, callId, original.name, args, original.namespace_));
    }
    return items;
  }
}
/**
 * Builds the buffered Responses object for a completed Chat completion under
 * one provider profile. Chat tool calls become `function_call` output items,
 * and the provider's `reasoning_content` becomes a `reasoning` item so nothing
 * is silently lost.
 */
json::object toDeepSeekResponsesPayload(const json::object& completion, const std::string& requestedModel, const std::string& responseId,
  const DeepSeekResponsesEcho& echo, const rt::OrderedMap<std::string, responses::OriginalToolName>& toolNames,
  const rt::OrderedSet<std::string>& customToolNames, const responses::ChatOnlyResponsesProfile& profile) {
  const auto created = rt::getNumber(completion, "created").value_or(rt::Math::floor(rt::dateNow() / 1000));
  const auto* choices = rt::getArray(completion, "choices");
  const json::value* finish = nullptr;
  if (choices) for (const auto& choice : *choices) if (choice.is_object()) { finish = rt::get(choice, "finish_reason"); break; }
  auto payload = deepSeekTerminalEnvelope(responseId, requestedModel, created, "completed", echo, finish, profile).response;
  json::array output;
  if (choices) for (std::size_t i = 0; i < choices->size(); ++i) {
    const auto& choice = (*choices)[i];
    const auto* message = rt::getObject(choice, "message");
    if (!choice.is_object() || !message) continue;
    auto items = outputItemsForChoice(*message, static_cast<double>(i), responseId, toolNames, customToolNames);
    for (auto& item : items) output.push_back(std::move(item));
  }
  payload["output"] = std::move(output);
  const auto usage = toResponsesUsage(rt::get(completion, "usage"), profile);
  payload["usage"] = usage ? json::value(*usage) : json::value(nullptr);
  return payload;
}
}
