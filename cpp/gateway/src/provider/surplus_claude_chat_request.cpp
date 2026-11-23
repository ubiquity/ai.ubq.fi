#include "uos/provider/surplus_claude_chat_request.hpp"

#include "uos/provider/anthropic_replay.hpp"
#include "uos/provider/surplus_claude_request.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::provider::surplus_claude_chat_request {
  namespace {
    using JsonRecord = json::object;
    using deepseek::responses::OriginalToolName;

    // `{ tools: JsonRecord[]; toolNames: Map<string, OriginalToolName>; customToolNames: Set<string> }`
    struct ToolProjection {
      std::vector<JsonRecord> tools;
      rt::OrderedMap<std::string, OriginalToolName> toolNames;
      rt::OrderedSet<std::string> customToolNames;
    };
    // `{ mode: PromptCacheMode; count: number }`
    struct CachePolicy {
      openai_telemetry::PromptCacheMode mode;
      double count;
    };
    // `unknown | null | undefined` results: `rejected` is null, an empty `value` undefined.
    struct Projection {
      bool rejected = false;
      std::optional<json::value> value;
    };

    json::value text(std::string_view value) { return json::value(json::string(value)); }
    bool equals(const json::value* value, std::string_view expected) { return value && value->is_string() && value->get_string() == expected; }

    bool record(const json::value* value) { return rt::isRecord(value); }
    bool has(const JsonRecord& value, std::string_view key) { return rt::hasOwn(value, key); }
    bool name(std::string_view value) {
      static const rt::Regex NAME("^[A-Za-z0-9_-]{1,64}$");
      return NAME.test(value);
    }
    bool name(const json::value* value) { return value && value->is_string() && name(std::string_view(value->get_string())); }
    bool positiveInteger(const json::value* value) {
      return value && rt::isNumber(*value) && rt::Number::isSafeInteger(*value) && rt::toNumber(*value) > 0;
    }

    std::optional<JsonRecord> control(const json::value* value) {
      if (!record(value) || !equals(rt::get(*value, "type"), "ephemeral")) return std::nullopt;
      for (const std::string& key : rt::objectKeys(value->get_object()))
        if (key != "type" && key != "ttl") return std::nullopt;
      const json::value* ttl = rt::get(*value, "ttl");
      if (!ttl || equals(ttl, "5m") || equals(ttl, "1h")) return rt::objectSpread(value->get_object());
      return std::nullopt;
    }

    std::optional<JsonRecord> cachedBlock(const JsonRecord& projected, const JsonRecord& original) {
      const json::value* marker = rt::get(original, "prompt_cache_breakpoint");
      if (has(original, "prompt_cache_breakpoint") && (!record(marker) || !equals(rt::get(*marker, "mode"), "explicit") || marker->get_object().size() != 1))
        return std::nullopt;
      if (has(original, "cache_control")) {
        std::optional<JsonRecord> policy = control(rt::get(original, "cache_control"));
        if (!policy) return std::nullopt;
        JsonRecord cached = rt::objectSpread(projected);
        rt::objectDefine(cached, "cache_control", json::value(std::move(*policy)));
        return cached;
      }
      if (!marker) return projected;
      JsonRecord cached = rt::objectSpread(projected);
      JsonRecord ephemeral;
      ephemeral["type"] = "ephemeral";
      rt::objectDefine(cached, "cache_control", json::value(std::move(ephemeral)));
      return cached;
    }

    std::optional<JsonRecord> chatPart(const json::value& value) {
      if (!record(&value)) return std::nullopt;
      const JsonRecord& part = value.get_object();
      const std::string type = rt::toString(rt::get(part, "type"));
      const json::value* partText = rt::get(part, "text");
      if ((type == "input_text" || type == "output_text" || type == "text") && partText && partText->is_string()) {
        JsonRecord block;
        block["type"] = "text";
        block["text"] = *partText;
        return cachedBlock(block, part);
      }
      const json::value* imageUrl = rt::get(part, "image_url");
      if (!equals(rt::get(part, "type"), "input_image") || !imageUrl || !imageUrl->is_string() || rt::get(part, "file_id")) return std::nullopt;
      static const rt::Regex REMOTE("^https?:\\/\\/");
      static const rt::Regex INLINE("^data:image\\/(?:jpeg|png|gif|webp);base64,");
      const std::string_view url(imageUrl->get_string());
      if (!REMOTE.test(url) && !INLINE.test(url)) return std::nullopt;
      JsonRecord image;
      image["url"] = *imageUrl;
      if (const json::value* detail = rt::get(part, "detail")) image["detail"] = *detail;
      JsonRecord block;
      block["type"] = "image_url";
      block["image_url"] = std::move(image);
      return cachedBlock(block, part);
    }

    std::optional<std::vector<JsonRecord>> chatContent(const json::value* value) {
      if (value && value->is_string()) {
        JsonRecord part;
        part["type"] = "text";
        part["text"] = *value;
        return std::vector<JsonRecord>{std::move(part)};
      }
      if (!value || !value->is_array()) return std::nullopt;
      std::vector<JsonRecord> parts;
      for (const json::value& original : value->get_array()) {
        std::optional<JsonRecord> part = chatPart(original);
        if (!part) return std::nullopt;
        parts.push_back(std::move(*part));
      }
      return parts;
    }

    json::value recordsJson(std::vector<JsonRecord> records) {
      json::array out;
      for (JsonRecord& entry : records) out.push_back(json::value(std::move(entry)));
      return out;
    }

    bool collectFunction(ToolProjection& projection, const JsonRecord& tool, const std::optional<std::string>& namespace_) {
      const json::value* toolName = rt::get(tool, "name");
      const json::value* parameters = rt::get(tool, "parameters");
      if (!name(toolName) || (parameters && !record(parameters))) return false;
      const json::value* strict = rt::get(tool, "strict");
      if (strict && !strict->is_null() && !strict->is_bool()) return false;
      const json::value* description = rt::get(tool, "description");
      if (description && !description->is_string()) return false;
      const std::string toolNameText(toolName->get_string());
      const std::string preferred = namespace_ && !namespace_->empty() ? *namespace_ + "_" + toolNameText : toolNameText;
      if (!name(std::string_view(preferred))) return false;
      for (const JsonRecord& entry : projection.tools) {
        const json::value* function = rt::get(entry, "function");
        if (record(function) && equals(rt::get(*function, "name"), preferred)) return false;
      }
      JsonRecord declaration;
      declaration["name"] = text(preferred);
      if (description) declaration["description"] = *description;
      if (parameters) declaration["parameters"] = *parameters;
      if (strict) declaration["strict"] = *strict;
      std::optional<JsonRecord> fn = cachedBlock(declaration, tool);
      if (!fn) return false;
      JsonRecord entry;
      entry["type"] = "function";
      entry["function"] = std::move(*fn);
      projection.tools.push_back(std::move(entry));
      projection.toolNames.set(preferred, OriginalToolName{.name = toolNameText, .namespace_ = namespace_});
      return true;
    }

    bool collectCustom(ToolProjection& projection, const JsonRecord& tool) {
      // Chat cannot enforce a caller's context-free grammar. Retain Responses for that control.
      const json::value* format = rt::get(tool, "format");
      if (format && (!record(format) || !equals(rt::get(*format, "type"), "text") || format->get_object().size() != 1)) return false;
      JsonRecord fn = rt::objectSpread(tool);
      rt::objectDefine(fn, "parameters",
                       rt::jsonParse(R"({"type":"object","properties":{"input":{"type":"string"}},"required":["input"],"additionalProperties":false})"));
      if (!collectFunction(projection, fn, std::nullopt)) return false;
      const json::value* toolName = rt::get(tool, "name");
      if (!toolName || !toolName->is_string()) return false;
      projection.customToolNames.add(std::string(toolName->get_string()));
      return true;
    }

    bool collectTool(ToolProjection& projection, const json::value& value) {
      if (!record(&value)) return false;
      const JsonRecord& tool = value.get_object();
      const json::value* type = rt::get(tool, "type");
      if (equals(type, "function")) return collectFunction(projection, tool, std::nullopt);
      if (equals(type, "custom")) return collectCustom(projection, tool);
      const json::value* toolName = rt::get(tool, "name");
      const json::array* tools = rt::getArray(tool, "tools");
      if (!equals(type, "namespace") || !name(toolName) || !tools) return false;
      const std::optional<std::string> namespace_(std::string(toolName->get_string()));
      for (const json::value& nested : *tools)
        if (!record(&nested) || !equals(rt::get(nested, "type"), "function") || !collectFunction(projection, nested.get_object(), namespace_)) return false;
      return true;
    }

    std::optional<ToolProjection> chatTools(const json::value* value) {
      ToolProjection projection;
      if (!value) return projection;
      if (!value->is_array()) return std::nullopt;
      for (const json::value& tool : value->get_array())
        if (!collectTool(projection, tool)) return std::nullopt;
      return projection;
    }

    std::optional<std::string> projectedToolName(const JsonRecord& item, const ToolProjection& projection) {
      const json::value* itemName = rt::get(item, "name");
      if (!name(itemName)) return std::nullopt;
      const json::value* namespaceValue = rt::get(item, "namespace");
      const bool nullNamespace = rt::isNullish(namespaceValue);
      if (!nullNamespace && !name(namespaceValue)) return std::nullopt;
      const std::optional<std::string> namespace_ =
        nullNamespace ? std::nullopt : std::optional<std::string>(std::string(namespaceValue->get_string()));
      for (const auto& [projected, original] : projection.toolNames)
        if (original.name == std::string_view(itemName->get_string()) && original.namespace_ == namespace_) return projected;
      return nullNamespace ? std::optional<std::string>(std::string(itemName->get_string())) : std::nullopt;
    }

    void appendCall(std::vector<JsonRecord>& messages, JsonRecord call) {
      JsonRecord* previous = messages.empty() ? nullptr : &messages.back();
      if (previous && equals(rt::get(*previous, "role"), "assistant")) {
        if (json::array* toolCalls = rt::getArray(*previous, "tool_calls")) {
          toolCalls->push_back(json::value(std::move(call)));
          return;
        }
      }
      json::array toolCalls;
      toolCalls.push_back(json::value(std::move(call)));
      JsonRecord message;
      message["role"] = "assistant";
      message["content"] = nullptr;
      message["tool_calls"] = std::move(toolCalls);
      messages.push_back(std::move(message));
    }

    bool appendToolCall(const JsonRecord& item, std::vector<JsonRecord>& messages, const ToolProjection& projection) {
      // The Chat tool-call union has no documented explicit block cache marker.
      if (has(item, "cache_control") || has(item, "prompt_cache_breakpoint")) return false;
      const std::optional<std::string> projected = projectedToolName(item, projection);
      const json::value* callId = rt::get(item, "call_id");
      if (!projected || projected->empty() || !callId || !callId->is_string() || callId->get_string().empty()) return false;
      std::string argumentsText;
      if (equals(rt::get(item, "type"), "custom_tool_call")) {
        const json::value* input = rt::get(item, "input");
        if (!projection.customToolNames.has(*projected) || !input || !input->is_string()) return false;
        JsonRecord wrapped;
        wrapped["input"] = *input;
        argumentsText = rt::jsonStringify(json::value(std::move(wrapped)));
      } else {
        const json::value* arguments = rt::get(item, "arguments");
        if (!arguments || !arguments->is_string() || projection.customToolNames.has(*projected)) return false;
        argumentsText = std::string(arguments->get_string());
      }
      JsonRecord function;
      function["name"] = text(*projected);
      function["arguments"] = text(argumentsText);
      JsonRecord call;
      call["id"] = *callId;
      call["type"] = "function";
      call["function"] = std::move(function);
      appendCall(messages, std::move(call));
      return true;
    }

    bool appendToolResult(const JsonRecord& item, std::vector<JsonRecord>& messages) {
      const json::value* callId = rt::get(item, "call_id");
      if (!callId || !callId->is_string() || callId->get_string().empty()) return false;
      std::optional<std::vector<JsonRecord>> content = chatContent(rt::get(item, "output"));
      if (!content) return false;
      if (has(item, "cache_control") || has(item, "prompt_cache_breakpoint")) {
        if (content->empty()) return false;
        std::optional<JsonRecord> cached = cachedBlock(content->back(), item);
        if (!cached) return false;
        content->back() = std::move(*cached);
      }
      JsonRecord message;
      message["role"] = "tool";
      message["tool_call_id"] = *callId;
      message["content"] = recordsJson(std::move(*content));
      messages.push_back(std::move(message));
      return true;
    }

    bool appendOriginal(const json::value& value, std::vector<JsonRecord>& messages, const ToolProjection& projection) {
      if (!record(&value)) return false;
      const JsonRecord& item = value.get_object();
      const json::value* type = rt::get(item, "type");
      if (equals(type, "function_call") || equals(type, "custom_tool_call")) return appendToolCall(item, messages, projection);
      if (equals(type, "function_call_output") || equals(type, "custom_tool_call_output")) return appendToolResult(item, messages);
      if (type && !equals(type, "message")) return false;
      const json::value* role = rt::get(item, "role");
      const std::string roleText = rt::toString(role);
      if (roleText != "system" && roleText != "developer" && roleText != "user" && roleText != "assistant") return false;
      std::optional<std::vector<JsonRecord>> content = chatContent(rt::get(item, "content"));
      if (!content) return false;
      JsonRecord message;
      message["role"] = *role;
      message["content"] = recordsJson(std::move(*content));
      messages.push_back(std::move(message));
      return true;
    }

    bool appendInstructions(const json::value* instructions, std::vector<JsonRecord>& messages) {
      if (rt::isNullish(instructions)) return true;
      std::optional<std::vector<JsonRecord>> content = chatContent(instructions);
      if (!content) return false;
      for (const JsonRecord& part : *content)
        if (!equals(rt::get(part, "type"), "text")) return false;
      JsonRecord message;
      message["role"] = "system";
      message["content"] = recordsJson(std::move(*content));
      messages.push_back(std::move(message));
      return true;
    }

    bool appendReplayEntries(const std::vector<anthropic_replay::AnthropicChatReplayEntry>& entries, std::vector<JsonRecord>& messages,
                             const ToolProjection& projection) {
      for (const anthropic_replay::AnthropicChatReplayEntry& entry : entries) {
        if (const auto* native = std::get_if<anthropic_replay::AnthropicChatReplayEntryNativeChatAssistant>(&entry)) {
          messages.push_back(native->message);
        } else {
          const json::value item(std::get<anthropic_replay::AnthropicChatReplayEntryOriginal>(entry).item);
          if (!appendOriginal(item, messages, projection)) return false;
        }
      }
      return true;
    }

    std::optional<std::vector<JsonRecord>> chatMessages(const JsonRecord& body, const ToolProjection& projection, const ClaudeChatRequestOptions& options) {
      std::vector<JsonRecord> messages;
      if (!appendInstructions(rt::get(body, "instructions"), messages)) return std::nullopt;
      const json::value* input = rt::get(body, "input");
      if (input && input->is_string()) {
        JsonRecord part;
        part["type"] = "text";
        part["text"] = *input;
        json::array content;
        content.push_back(json::value(std::move(part)));
        JsonRecord message;
        message["role"] = "user";
        message["content"] = std::move(content);
        messages.push_back(std::move(message));
        return messages;
      }
      if (!input || !input->is_array()) return std::nullopt;
      if (options.reasoningReplayScope && !options.reasoningReplayScope->empty()) {
        const anthropic_replay::AnthropicReplayContext context{
          .scope = *options.reasoningReplayScope,
          .provider = options.provider.value_or("surplus"),
          .model = rt::toString(rt::get(body, "model")),
        };
        const std::optional<std::vector<anthropic_replay::AnthropicChatReplayEntry>> entries =
          anthropic_replay::restoreAnthropicChatReplay(input->get_array(), context);
        if (!entries) return std::nullopt;
        if (!appendReplayEntries(*entries, messages, projection)) return std::nullopt;
      } else {
        for (const json::value& item : input->get_array())
          if (!appendOriginal(item, messages, projection)) return std::nullopt;
      }
      if (messages.empty()) return std::nullopt;
      return messages;
    }

    std::optional<CachePolicy> cachePolicy(const JsonRecord& body, std::vector<JsonRecord>& messages, ToolProjection& tools) {
      const json::value* retention = rt::get(body, "prompt_cache_retention");
      if (retention && !equals(retention, "in_memory")) return std::nullopt;
      const json::value* options = rt::get(body, "prompt_cache_options");
      if (options) {
        if (!record(options)) return std::nullopt;
        for (const std::string& key : rt::objectKeys(options->get_object()))
          if (key != "mode") return std::nullopt;
        const json::value* mode = rt::get(*options, "mode");
        if (mode && !equals(mode, "implicit") && !equals(mode, "explicit")) return std::nullopt;
      }
      std::optional<JsonRecord> rootControl;
      if (has(body, "cache_control")) {
        rootControl = control(rt::get(body, "cache_control"));
      } else {
        rootControl = JsonRecord{};
        (*rootControl)["type"] = "ephemeral";
      }
      if (!rootControl) return std::nullopt;
      std::vector<JsonRecord*> parts;
      for (JsonRecord& message : messages) {
        if (json::array* content = rt::getArray(message, "content"))
          for (json::value& part : *content)
            if (record(&part)) parts.push_back(&part.get_object());
      }
      std::vector<JsonRecord*> toolBlocks;
      for (JsonRecord& tool : tools.tools) {
        json::value* function = rt::get(tool, "function");
        if (record(function)) toolBlocks.push_back(&function->get_object());
      }
      double count = 0;
      for (const JsonRecord* part : parts)
        if (has(*part, "cache_control")) count += 1;
      for (const JsonRecord* block : toolBlocks)
        if (has(*block, "cache_control")) count += 1;
      if (count > 4) return std::nullopt;
      if (count > 0 || (record(options) && equals(rt::get(*options, "mode"), "explicit"))) return CachePolicy{.mode = "explicit", .count = count};
      JsonRecord* last = nullptr;
      for (auto part = parts.rbegin(); part != parts.rend(); ++part) {
        const json::value* type = rt::get(**part, "type");
        if (equals(type, "text") || equals(type, "image_url")) {
          last = *part;
          break;
        }
      }
      if (!last && !toolBlocks.empty()) last = toolBlocks.back();
      if (!last) return std::nullopt;
      rt::objectSet(*last, "cache_control", json::value(std::move(*rootControl)));
      return CachePolicy{.mode = "implicit", .count = 0};
    }

    // `{ effort?: string } | null`: std::nullopt is null; an empty `effort` is absent.
    std::optional<std::optional<std::string>> reasoningEffort(const json::value* value) {
      if (rt::isNullish(value)) return std::optional<std::string>();
      if (!record(value)) return std::nullopt;
      for (const std::string& key : rt::objectKeys(value->get_object()))
        if (key != "effort" && key != "summary") return std::nullopt;
      const json::value* summary = rt::get(*value, "summary");
      if (summary && !equals(summary, "auto") && !equals(summary, "detailed") && !equals(summary, "none")) return std::nullopt;
      const json::value* effort = rt::get(*value, "effort");
      if (!effort) return std::optional<std::string>();
      if (!effort->is_string() || rt::trim(effort->get_string()).empty()) return std::nullopt;
      return std::optional<std::string>(equals(effort, "ultra") ? std::string("max") : std::string(effort->get_string()));
    }

    Projection chatToolChoice(const json::value* value, const ToolProjection& tools) {
      if (!value) return {};
      if (equals(value, "auto") || equals(value, "none") || equals(value, "required")) return {.rejected = false, .value = *value};
      if (!record(value)) return {.rejected = true, .value = std::nullopt};
      const std::string type = rt::toString(rt::get(*value, "type"));
      if (type != "function" && type != "custom") return {.rejected = true, .value = std::nullopt};
      const std::optional<std::string> projected = projectedToolName(value->get_object(), tools);
      if (!projected || projected->empty()) return {.rejected = true, .value = std::nullopt};
      JsonRecord function;
      function["name"] = text(*projected);
      JsonRecord choice;
      choice["type"] = "function";
      choice["function"] = std::move(function);
      return {.rejected = false, .value = json::value(std::move(choice))};
    }

    Projection chatResponseFormat(const json::value* value) {
      if (!value) return {};
      if (!record(value)) return {.rejected = true, .value = std::nullopt};
      for (const std::string& key : rt::objectKeys(value->get_object()))
        if (key != "format") return {.rejected = true, .value = std::nullopt};
      const json::value* format = rt::get(*value, "format");
      if (!format) return {};
      if (!record(format)) return {.rejected = true, .value = std::nullopt};
      const JsonRecord& formatRecord = format->get_object();
      const json::value* type = rt::get(formatRecord, "type");
      if (equals(type, "text") && formatRecord.size() == 1) return {};
      if (equals(type, "json_object") && formatRecord.size() == 1) {
        JsonRecord result;
        result["type"] = "json_object";
        return {.rejected = false, .value = json::value(std::move(result))};
      }
      if (!equals(type, "json_schema")) return {.rejected = true, .value = std::nullopt};
      JsonRecord jsonSchema = rt::objectSpread(formatRecord);
      rt::objectDelete(jsonSchema, "type");
      JsonRecord result;
      result["type"] = "json_schema";
      result["json_schema"] = std::move(jsonSchema);
      return {.rejected = false, .value = json::value(std::move(result))};
    }

    bool supportedControls(const JsonRecord& body) {
      const json::value* store = rt::get(body, "store");
      if (!surplus_claude_request::isSurplusClaudeModel(rt::get(body, "model")) || (store && !(store->is_bool() && !store->get_bool()))) return false;
      if (!rt::isNullish(rt::get(body, "previous_response_id"))) return false;
      for (const std::string_view key : {"context_management", "max_tool_calls", "truncation", "metadata", "service_tier", "safety_identifier", "user"})
        if (rt::get(body, key)) return false;
      const json::value* include = rt::get(body, "include");
      if (include) {
        if (!include->is_array()) return false;
        for (const json::value& field : include->get_array())
          if (!equals(&field, "reasoning.encrypted_content")) return false;
      }
      const json::value* parallel = rt::get(body, "parallel_tool_calls");
      return !parallel || parallel->is_bool();
    }

    JsonRecord projectedBody(const JsonRecord& body, std::vector<JsonRecord> messages, std::vector<JsonRecord> tools, const std::optional<double>& maxTokens,
                             const std::optional<std::string>& effort, const std::optional<json::value>& choice, const std::optional<json::value>& format) {
      const json::value* stream = rt::get(body, "stream");
      const bool streaming = stream && stream->is_bool() && stream->get_bool();
      JsonRecord projected;
      projected["model"] = *rt::get(body, "model");
      projected["messages"] = recordsJson(std::move(messages));
      projected["stream"] = streaming;
      if (streaming) {
        JsonRecord streamOptions;
        streamOptions["include_usage"] = true;
        rt::objectSet(projected, "stream_options", json::value(std::move(streamOptions)));
      }
      if (maxTokens) rt::objectSet(projected, "max_tokens", rt::makeNumber(*maxTokens));
      if (effort) rt::objectSet(projected, "reasoning_effort", text(*effort));
      if (!tools.empty()) rt::objectSet(projected, "tools", recordsJson(std::move(tools)));
      if (choice) rt::objectSet(projected, "tool_choice", *choice);
      if (format) rt::objectSet(projected, "response_format", *format);
      for (const std::string_view key : {"parallel_tool_calls", "temperature", "top_p", "prompt_cache_key"})
        if (const json::value* value = rt::get(body, key)) rt::objectSet(projected, key, *value);
      return projected;
    }
  }

  std::optional<SurplusClaudeChatRequest> buildSurplusClaudeChatRequest(const json::object& body) {
    return buildSurplusClaudeChatRequest(body, std::nullopt, ClaudeChatRequestOptions{});
  }

  std::optional<SurplusClaudeChatRequest> buildSurplusClaudeChatRequest(const json::object& body, std::optional<double> catalogMaxOutputTokens) {
    return buildSurplusClaudeChatRequest(body, catalogMaxOutputTokens, ClaudeChatRequestOptions{});
  }

  /** Claude's canonical Chat route preserves both explicit block caching and requested effort. */
  std::optional<SurplusClaudeChatRequest> buildSurplusClaudeChatRequest(const json::object& body, std::optional<double> catalogMaxOutputTokens,
                                                                        const ClaudeChatRequestOptions& options) {
    if (!supportedControls(body)) return std::nullopt;
    std::optional<ToolProjection> tools = chatTools(rt::get(body, "tools"));
    if (!tools) return std::nullopt;
    std::optional<std::vector<JsonRecord>> messages = chatMessages(body, *tools, options);
    if (!messages) return std::nullopt;
    const std::optional<std::optional<std::string>> reasoning = reasoningEffort(rt::get(body, "reasoning"));
    if (!reasoning) return std::nullopt;
    const Projection choice = chatToolChoice(rt::get(body, "tool_choice"), *tools);
    if (choice.rejected) return std::nullopt;
    const Projection format = chatResponseFormat(rt::get(body, "text"));
    if (format.rejected) return std::nullopt;
    // `body.max_output_tokens ?? catalogMaxOutputTokens`
    const json::value* bodyMaxTokens = rt::get(body, "max_output_tokens");
    std::optional<double> maxTokens;
    if (!rt::isNullish(bodyMaxTokens)) {
      if (!positiveInteger(bodyMaxTokens)) return std::nullopt;
      maxTokens = rt::toNumber(*bodyMaxTokens);
    } else if (catalogMaxOutputTokens) {
      const json::value catalogMaxTokens = rt::makeNumber(*catalogMaxOutputTokens);
      if (!positiveInteger(&catalogMaxTokens)) return std::nullopt;
      maxTokens = *catalogMaxOutputTokens;
    }
    const std::optional<CachePolicy> cache = cachePolicy(body, *messages, *tools);
    if (!cache) return std::nullopt;
    JsonRecord projected = projectedBody(body, std::move(*messages), std::move(tools->tools), maxTokens, *reasoning, choice.value, format.value);
    return SurplusClaudeChatRequest{
      .body = std::move(projected),
      .promptCacheMode = cache->mode,
      .explicitBreakpointCount = cache->count,
      .toolNames = std::move(tools->toolNames),
      .customToolNames = std::move(tools->customToolNames),
    };
  }
}
