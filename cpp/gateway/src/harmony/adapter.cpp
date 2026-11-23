#include "uos/harmony/adapter.hpp"

#include "uos/harmony/conversation.hpp"
#include "uos/harmony/parse.hpp"
#include "uos/harmony/render.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <cmath>
#include <stdexcept>

namespace uos::harmony::adapter {
  namespace {
    template <class T> std::optional<std::string> stringOf(const T& value, std::string_view key) {
      const auto text = rt::getString(value, key);
      return text ? std::optional<std::string>(std::string(*text)) : std::nullopt;
    }
    bool toolStrictnessFor(const types::ToolDefinition& tool) { return tool.strict == true; }

    std::vector<types::ToolDefinition> applyStrictnessMode(const std::vector<types::ToolDefinition>& tools, const std::string& mode) {
      if (mode == "normalize-false") return normalizeToolStrictness(tools, false);
      if (mode == "normalize-true") return normalizeToolStrictness(tools, true);
      return tools;
    }

    json::object jsonSchemaTool(const types::ToolDefinition& tool) {
      json::object function{{"name", tool.name}, {"description", tool.description}, {"parameters", tool.parameters}};
      rt::objectSet(function, "strict", tool.strict);
      return {{"type", "function"}, {"function", std::move(function)}};
    }

    std::vector<std::string> wireMessageRoles(const json::object& body) {
      const json::object projected = provider::cerebras::projectCerebrasRequest(body);
      const json::array* messages = rt::getArray(projected, "messages");
      std::vector<std::string> roles;
      if (messages) for (const json::value& message : *messages) {
        roles.push_back(message.is_object() ? stringOf(message, "role").value_or("?") : "?");
      }
      return roles;
    }

    BuiltHarmonyRequest::Metadata metadataFor(const HarmonyRequestOptions& options, const std::vector<types::ToolDefinition>& tools, const json::object& body, bool native) {
      std::vector<BuiltHarmonyRequest::Metadata::ToolEntry> entries;
      std::vector<bool> strictness;
      for (const auto& tool : tools) {
        entries.push_back({.name = tool.name, .strict = toolStrictnessFor(tool)});
        strictness.push_back(toolStrictnessFor(tool));
      }
      double assistantToolTurns = 0;
      double toolResultTurns = 0;
      for (const auto& turn : options.turns) {
        if (const auto* assistant = std::get_if<types::AssistantTurn>(&turn); assistant && !assistant->toolCalls.empty()) assistantToolTurns += 1;
        if (std::holds_alternative<types::ToolResultTurn>(turn)) toolResultTurns += 1;
      }
      std::string responseFormat = "none";
      if (native && options.nativeResponseFormat) responseFormat = "developer";
      if (!native && options.responseFormat) responseFormat = std::visit([](const auto& format) { return std::string(format.type); }, *options.responseFormat);
      return {
        .model = std::string(provider::cerebras::CEREBRAS_GPT_OSS_120B_MODEL), .messageRoles = wireMessageRoles(body),
        .toolsRendered = tools.empty() ? "none" : native ? "developer" : "parameter", .toolEntries = std::move(entries),
        .toolStrictnessValues = std::move(strictness), .reasoningEffortTopLevel = native ? std::nullopt : options.reasoningEffort,
        .reasoningEffortInSystem = native, .responseFormat = responseFormat, .parallelToolCalls = native ? std::nullopt : options.parallelToolCalls,
        .maxCompletionTokens = options.maxCompletionTokens, .analysisInWire = false, .assistantToolTurns = assistantToolTurns, .toolResultTurns = toolResultTurns,
      };
    }

    BuiltHarmonyRequest buildGenericHarmonyRequest(const HarmonyRequestOptions& options, const std::vector<types::ToolDefinition>& strictTools) {
      const auto messages = conversation::wireMessagesFromConversation({.turns = options.turns});
      json::object body{{"model", provider::cerebras::CEREBRAS_GPT_OSS_120B_MODEL}, {"messages", json::value_from(messages)}, {"stream", false}};
      if (!strictTools.empty()) {
        json::array tools;
        for (const auto& tool : strictTools) tools.push_back(jsonSchemaTool(tool));
        body["tools"] = std::move(tools);
      }
      if (options.reasoningEffort && !options.reasoningEffort->empty()) body["reasoning_effort"] = *options.reasoningEffort;
      if (options.responseFormat) body["response_format"] = json::value_from(*options.responseFormat);
      rt::objectSet(body, "parallel_tool_calls", options.parallelToolCalls);
      rt::objectSet(body, "max_completion_tokens", options.maxCompletionTokens);
      auto metadata = metadataFor(options, strictTools, body, false);
      return {.style = "generic", .body = std::move(body), .metadata = std::move(metadata)};
    }

    std::optional<json::object> nativeHarmonyMessage(const types::ConversationTurn& turn, const std::string& toolResultStyle) {
      if (std::holds_alternative<types::ConversationTurnSystem>(turn) || std::holds_alternative<types::ConversationTurnDeveloper>(turn)) return std::nullopt;
      if (const auto* assistant = std::get_if<types::AssistantTurn>(&turn)) {
        if (!assistant->content && assistant->toolCalls.empty()) return std::nullopt;
        json::object message{{"role", "assistant"}, {"content", assistant->content.value_or("")}};
        if (!assistant->toolCalls.empty()) {
          json::array calls;
          for (const auto& call : assistant->toolCalls) calls.push_back(json::object{
            {"id", call.id}, {"type", "function"}, {"function", json::object{{"name", call.name}, {"arguments", call.arguments}}},
          });
          message["tool_calls"] = std::move(calls);
        }
        return message;
      }
      if (const auto* tool = std::get_if<types::ToolResultTurn>(&turn)) {
        if (toolResultStyle == "tool-role") return json::object{{"role", "tool"}, {"tool_call_id", tool->toolCallId}, {"content", tool->content}};
        return json::object{{"role", "user"}, {"content", "Tool result from " + tool->name + ":\n" + tool->content}};
      }
      return json::object{{"role", "user"}, {"content", std::get<types::ConversationTurnUser>(turn).content}};
    }

    BuiltHarmonyRequest buildNativeHarmonyRequest(const HarmonyRequestOptions& options, const std::vector<types::ToolDefinition>& strictTools) {
      const auto systemContent = render::renderSystemMessage({.currentDate = options.currentDate.value_or("2026-01-01"), .reasoningEffort = options.reasoningEffort.value_or("medium")});
      const auto developerContent = render::renderDeveloperMessage({.instructions = options.instructions.value_or("Follow the user's request."), .tools = strictTools,
        .responseFormat = options.nativeResponseFormat, .namespace_ = options.namespace_});
      json::array messages{json::object{{"role", "system"}, {"content", systemContent}}, json::object{{"role", "developer"}, {"content", developerContent}}};
      for (const auto& turn : options.turns) {
        const auto message = nativeHarmonyMessage(turn, options.nativeToolResultStyle.value_or("tool-role"));
        if (message) messages.push_back(*message);
      }
      json::object body{{"model", provider::cerebras::CEREBRAS_GPT_OSS_120B_MODEL}, {"messages", std::move(messages)}, {"stream", false}};
      rt::objectSet(body, "max_completion_tokens", options.maxCompletionTokens);
      auto metadata = metadataFor(options, strictTools, body, true);
      return {.style = "native", .body = std::move(body), .metadata = std::move(metadata)};
    }

    std::optional<std::string> exactString(const json::value* value) {
      return value && value->is_string() && !value->as_string().empty() ? std::optional<std::string>(value->as_string()) : std::nullopt;
    }
    std::optional<double> nonNegativeInteger(const json::value* value) {
      if (!value || !rt::isNumber(*value)) return std::nullopt;
      const double number = rt::toNumber(*value);
      if (!std::isfinite(number)) return std::nullopt;
      const double normalized = rt::Math::trunc(number);
      return normalized >= 0 ? std::optional<double>(normalized) : std::nullopt;
    }
    std::optional<types::ToolCall> normalizeToolCallWire(const json::value& value, std::size_t index) {
      if (!value.is_object()) return std::nullopt;
      const json::object* function = rt::getObject(value, "function");
      if (!function) return std::nullopt;
      const auto name = exactString(rt::get(*function, "name"));
      const auto argumentsText = stringOf(*function, "arguments");
      if (!name || !argumentsText) return std::nullopt;
      const auto id = exactString(rt::get(value, "id")).value_or("harmony-call-" + rt::numberToString(static_cast<double>(index + 1)));
      const auto providerName = rt::startsWith(*name, "functions.") ? rt::slice(*name, 10) : *name;
      return types::ToolCall{.id = id, .name = providerName, .arguments = parse::normalizeToolArguments(*argumentsText)};
    }
    std::string reasoningFieldOf(const json::object& message) {
      if (stringOf(message, "reasoning_content")) return "reasoning_content";
      if (stringOf(message, "reasoning")) return "reasoning";
      return "none";
    }
    using NormalizedToolCalls = std::variant<std::vector<types::ToolCall>, HarmonyNormalizationError>;
    NormalizedToolCalls normalizeToolCallsWire(const json::value* value) {
      std::vector<types::ToolCall> calls;
      if (rt::isNullish(value)) return calls;
      if (!value->is_array()) return HarmonyNormalizationError{"tool_calls is not an array"};
      const auto& list = value->as_array();
      for (std::size_t i = 0; i < list.size(); ++i) {
        const auto call = normalizeToolCallWire(list[i], i);
        if (!call) return HarmonyNormalizationError{"tool call " + rt::numberToString(static_cast<double>(i)) + " is invalid"};
        calls.push_back(*call);
      }
      return calls;
    }
    std::variant<NormalizedChoice, HarmonyNormalizationError> normalizeChoice(const json::value& value) {
      if (!value.is_object()) return HarmonyNormalizationError{"choice is not an object"};
      const json::object* message = rt::getObject(value, "message");
      if (!message) return HarmonyNormalizationError{"assistant message is missing"};
      const auto* role = rt::get(*message, "role");
      if (role && (!role->is_string() || role->as_string() != "assistant")) return HarmonyNormalizationError{"message role is not assistant"};
      const auto* content = rt::get(*message, "content");
      if (content && !content->is_null() && !content->is_string()) return HarmonyNormalizationError{"message content is not a string"};
      const auto reasoningField = reasoningFieldOf(*message);
      const auto calls = normalizeToolCallsWire(rt::get(*message, "tool_calls"));
      if (const auto* error = std::get_if<HarmonyNormalizationError>(&calls)) return *error;
      return NormalizedChoice{.content = stringOf(*message, "content"), .refusal = stringOf(*message, "refusal"),
        .reasoning = reasoningField == "none" ? std::nullopt : stringOf(*message, reasoningField), .toolCalls = std::get<std::vector<types::ToolCall>>(calls),
        .finishReason = stringOf(value, "finish_reason"), .reasoningField = reasoningField};
    }

    std::optional<UpstreamError> upstreamErrorFromRecord(const json::object& value) {
      auto code = stringOf(value, "code");
      if (!code) code = stringOf(value, "error");
      const auto message = stringOf(value, "message");
      if (code || message) return UpstreamError{.code = code, .message = message};
      const auto* inner = rt::getObject(value, "error");
      if (!inner) return std::nullopt;
      return UpstreamError{.code = stringOf(*inner, "code"), .message = stringOf(*inner, "message")};
    }
  }

  std::vector<types::ToolDefinition> normalizeToolStrictness(const std::vector<types::ToolDefinition>& tools, bool strictness) {
    auto normalized = tools;
    for (auto& tool : normalized) tool.strict = strictness;
    return normalized;
  }

  BuiltHarmonyRequest buildCerebrasHarmonyRequest(const HarmonyRequestOptions& options) {
    const auto tools = options.tools.value_or(std::vector<types::ToolDefinition>{});
    const bool toolsPlusFormat = !tools.empty() && (options.responseFormat || options.nativeResponseFormat);
    if (toolsPlusFormat && options.combinationPolicy.value_or("error") != "probe") throw types::HarmonyAdapterError(
      "Combining tools with a structured response format is not proven for gpt-oss-120b; use combinationPolicy 'probe' only for protocol evidence.", "unproven-combination");
    if (options.style == "generic" && options.nativeResponseFormat) throw types::HarmonyAdapterError("nativeResponseFormat applies only to the native Harmony style.", "invalid-request");
    if (options.style == "native" && options.responseFormat) throw types::HarmonyAdapterError("responseFormat applies only to the generic style; use nativeResponseFormat.", "invalid-request");
    const auto strictTools = applyStrictnessMode(tools, options.toolStrictnessMode.value_or("normalize-false"));
    return options.style == "generic" ? buildGenericHarmonyRequest(options, strictTools) : buildNativeHarmonyRequest(options, strictTools);
  }

  std::vector<types::HarmonyTurn> harmonyTurnsFromChoice(const NormalizedChoice& choice) {
    std::vector<types::HarmonyTurn> turns;
    if (choice.reasoning && !choice.reasoning->empty()) turns.push_back(types::HarmonyTurnReasoning{.text = *choice.reasoning});
    if (!choice.toolCalls.empty()) {
      if (choice.content && !choice.content->empty()) turns.push_back(types::HarmonyTurnCommentary{.text = *choice.content});
      for (const auto& call : choice.toolCalls) turns.push_back(types::HarmonyTurnToolCall{.recipient = "functions." + call.name, .name = call.name, .arguments = call.arguments});
    } else if (choice.content && !choice.content->empty()) {
      const auto parsed = parse::parseHarmonyOutput(*choice.content);
      if (!parsed.turns.empty()) turns.insert(turns.end(), parsed.turns.begin(), parsed.turns.end());
      else turns.push_back(types::HarmonyTurnFinal{.text = *choice.content});
    }
    return turns;
  }

  NormalizeHarmonyChatCompletionResult normalizeHarmonyChatCompletion(const json::value* value, const std::string& requestedModel) {
    if (!value || !value->is_object()) return HarmonyNormalizationError{"upstream reply is not a Chat Completion object"};
    const auto id = exactString(rt::get(*value, "id"));
    const auto created = nonNegativeInteger(rt::get(*value, "created"));
    if (!id) return HarmonyNormalizationError{"upstream reply is missing an id"};
    if (!created) return HarmonyNormalizationError{"upstream reply has an invalid creation time"};
    const auto model = exactString(rt::get(*value, "model")).value_or(requestedModel);
    if (model != requestedModel) return HarmonyNormalizationError{"upstream returned model " + model + " instead of " + requestedModel};
    const auto* choices = rt::getArray(*value, "choices");
    if (!choices || choices->empty()) return HarmonyNormalizationError{"upstream reply has no choices"};
    const auto choiceResult = normalizeChoice((*choices)[0]);
    if (const auto* error = std::get_if<HarmonyNormalizationError>(&choiceResult)) return *error;
    const auto& choice = std::get<NormalizedChoice>(choiceResult);
    auto turns = harmonyTurnsFromChoice(choice);
    std::vector<std::string> analysis;
    std::vector<std::string> visible;
    std::vector<types::ToolCall> calls;
    for (const auto& turn : turns) {
      if (const auto* reasoning = std::get_if<types::HarmonyTurnReasoning>(&turn)) analysis.push_back(reasoning->text);
      if (const auto* commentary = std::get_if<types::HarmonyTurnCommentary>(&turn)) visible.push_back(commentary->text);
      if (const auto* final = std::get_if<types::HarmonyTurnFinal>(&turn)) visible.push_back(final->text);
      if (const auto* call = std::get_if<types::HarmonyTurnToolCall>(&turn)) {
        const std::size_t index = calls.size();
        calls.push_back({.id = index < choice.toolCalls.size() ? choice.toolCalls[index].id : "harmony-call-" + rt::numberToString(static_cast<double>(index + 1)),
          .name = call->name, .arguments = call->arguments});
      }
    }
    const auto content = visible.empty() ? std::nullopt : std::optional<std::string>(rt::join(visible, "\n"));
    const types::AssistantResponseShape shape{.contentPresent = content.has_value(), .contentChars = content ? static_cast<double>(rt::length(*content)) : 0,
      .reasoningField = choice.reasoningField, .reasoningChars = choice.reasoning ? static_cast<double>(rt::length(*choice.reasoning)) : 0,
      .toolCallsField = !choice.toolCalls.empty(), .toolCallCount = static_cast<double>(choice.toolCalls.size()), .finishReason = choice.finishReason, .refusal = choice.refusal.has_value()};
    return types::NormalizedAssistantResponse{.turns = std::move(turns), .analysis = std::move(analysis), .content = content, .toolCalls = std::move(calls),
      .finishReason = choice.finishReason, .refusal = choice.refusal, .shape = shape, .id = *id, .model = model, .created = *created};
  }
  NormalizeHarmonyChatCompletionResult normalizeHarmonyChatCompletion(const json::value& value, const std::string& model) { return normalizeHarmonyChatCompletion(&value, model); }
  NormalizeHarmonyChatCompletionResult normalizeHarmonyChatCompletion(const json::value* value) { return normalizeHarmonyChatCompletion(value, std::string(provider::cerebras::CEREBRAS_GPT_OSS_120B_MODEL)); }
  NormalizeHarmonyChatCompletionResult normalizeHarmonyChatCompletion(const json::value& value) { return normalizeHarmonyChatCompletion(&value); }

  rt::Task<rt::Response> HarmonyTransport::operator()(json::object body) const {
    if (!function_) throw std::logic_error("HarmonyTransport is undefined");
    return keepAlive(function_, (*function_)(std::move(body), std::nullopt));
  }
  rt::Task<rt::Response> HarmonyTransport::operator()(json::object body, HarmonyTransportRequestOptions options) const {
    if (!function_) throw std::logic_error("HarmonyTransport is undefined");
    return keepAlive(function_, (*function_)(std::move(body), std::move(options)));
  }
  rt::Task<rt::Response> HarmonyTransport::keepAlive(std::shared_ptr<const Function> function, rt::Task<rt::Response> task) {
    (void)function;
    auto result = co_await std::move(task);
    co_return result;
  }
  HarmonyTransport createCerebrasTransport() { return createCerebrasTransport(HarmonyTransportOptions{}); }
  HarmonyTransport createCerebrasTransport(const HarmonyTransportOptions& options) {
    return [options](json::object body, std::optional<HarmonyTransportRequestOptions> requestOptions) {
      provider::cerebras::CerebrasChatCompletionsOptions transportOptions;
      transportOptions.apiKey = options.apiKey;
      transportOptions.fetcher = options.fetcher;
      if (requestOptions && requestOptions->signal) transportOptions.signal = requestOptions->signal;
      return provider::cerebras::fetchCerebrasChatCompletions(std::move(body), std::move(transportOptions));
    };
  }

  rt::Task<RunTurnResult> runHarmonyTurn(HarmonyRequestOptions options, HarmonyTransport transport) {
    const auto built = buildCerebrasHarmonyRequest(options);
    rt::Promise<rt::Response> pending(transport(built.body));
    auto response = co_await pending.wait();
    const double status = response.status();
    json::value body;
    try {
      rt::Promise<json::value> parsed(response.json());
      const auto ignoreParseError = [](rt::Reason) { return json::value(nullptr); };
      auto safe = parsed.catch_(ignoreParseError);
      body = co_await safe.wait();
    } catch (...) { body = nullptr; }
    if (!response.ok()) {
      const auto error = body.is_object() ? upstreamErrorFromRecord(body.as_object()) : std::nullopt;
      co_return RunTurnResultNotOk{.status = status, .upstreamError = error, .normalizationError = std::nullopt};
    }
    if (body.is_null()) co_return RunTurnResultNotOk{.status = status, .upstreamError = std::nullopt, .normalizationError = "upstream reply is not JSON"};
    const auto normalized = normalizeHarmonyChatCompletion(body);
    if (const auto* error = std::get_if<HarmonyNormalizationError>(&normalized)) co_return RunTurnResultNotOk{.status = status, .upstreamError = std::nullopt, .normalizationError = error->error};
    co_return RunTurnResultOk{.status = status, .normalized = std::get<types::NormalizedAssistantResponse>(normalized)};
  }
}
