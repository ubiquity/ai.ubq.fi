#include "uos/provider/surplus_claude_request.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::provider::surplus_claude_request {
  namespace {
    using JsonRecord = json::object;
    using anthropic_response_wire::ClaudeToolName;

    // `{ role: "user" | "assistant"; content: JsonRecord[] }`
    struct NativeMessage {
      std::string role;
      std::vector<JsonRecord> content;
    };
    // `Readonly<{ tools: JsonRecord[]; toolNames: Map<string, ClaudeToolName> }>`
    struct ToolProjection {
      std::vector<JsonRecord> tools;
      rt::OrderedMap<std::string, ClaudeToolName> toolNames;
    };
    // `Pick<SurplusClaudeRequest, "promptCacheMode" | "explicitBreakpointCount">`
    struct CachePolicy {
      openai_telemetry::PromptCacheMode promptCacheMode;
      double explicitBreakpointCount;
    };
    // `JsonRecord | null | undefined`: `rejected` is null, an empty `format` undefined.
    struct OutputFormat {
      bool rejected = false;
      std::optional<JsonRecord> format;
    };

    json::value text(std::string_view value) { return json::value(json::string(value)); }
    bool equals(const json::value* value, std::string_view expected) { return value && value->is_string() && value->get_string() == expected; }

    bool isRecord(const json::value* value) { return rt::isRecord(value); }
    bool has(const JsonRecord& value, std::string_view key) { return rt::hasOwn(value, key); }
    bool positiveInteger(const json::value* value) {
      return value && rt::isNumber(*value) && rt::Number::isSafeInteger(*value) && rt::toNumber(*value) > 0;
    }
    bool toolName(std::string_view value) {
      static const rt::Regex TOOL_NAME("^[A-Za-z0-9_-]{1,64}$");
      return TOOL_NAME.test(value);
    }
    bool toolName(const json::value* value) { return value && value->is_string() && toolName(std::string_view(value->get_string())); }

    std::optional<JsonRecord> cacheControl(const json::value* value) {
      if (!isRecord(value) || !equals(rt::get(*value, "type"), "ephemeral")) return std::nullopt;
      for (const std::string& key : rt::objectKeys(value->get_object()))
        if (key != "type" && key != "ttl") return std::nullopt;
      const json::value* ttl = rt::get(*value, "ttl");
      if (ttl && !equals(ttl, "5m") && !equals(ttl, "1h")) return std::nullopt;
      return rt::objectSpread(value->get_object());
    }

    std::optional<JsonRecord> withBlockCache(const JsonRecord& block, const JsonRecord& original) {
      if (has(original, "prompt_cache_breakpoint")) {
        const json::value* marker = rt::get(original, "prompt_cache_breakpoint");
        if (!isRecord(marker) || !equals(rt::get(*marker, "mode"), "explicit") || marker->get_object().size() != 1) return std::nullopt;
      }
      if (has(original, "cache_control")) {
        std::optional<JsonRecord> control = cacheControl(rt::get(original, "cache_control"));
        if (!control) return std::nullopt;
        JsonRecord cached = rt::objectSpread(block);
        rt::objectDefine(cached, "cache_control", json::value(std::move(*control)));
        return cached;
      }
      if (has(original, "prompt_cache_breakpoint")) {
        JsonRecord cached = rt::objectSpread(block);
        JsonRecord ephemeral;
        ephemeral["type"] = "ephemeral";
        rt::objectDefine(cached, "cache_control", json::value(std::move(ephemeral)));
        return cached;
      }
      return block;
    }

    JsonRecord textBlock(std::string_view value) {
      JsonRecord block;
      block["type"] = "text";
      block["text"] = text(value);
      return block;
    }

    std::optional<JsonRecord> imageSource(std::string_view url) {
      static const rt::Regex INLINE("^data:(image\\/(?:jpeg|png|gif|webp));base64,([A-Za-z0-9+/=\\s]+)$");
      static const rt::Regex REMOTE("^https?:\\/\\/");
      if (const std::optional<rt::RegexMatch> inline_ = INLINE.exec(url)) {
        JsonRecord source;
        source["type"] = "base64";
        source["media_type"] = text(*(*inline_)[1]);
        source["data"] = text(*(*inline_)[2]);
        return source;
      }
      if (!REMOTE.test(url)) return std::nullopt;
      JsonRecord source;
      source["type"] = "url";
      source["url"] = text(url);
      return source;
    }

    std::optional<JsonRecord> contentBlock(const json::value& value) {
      if (!isRecord(&value)) return std::nullopt;
      const JsonRecord& item = value.get_object();
      const json::value* type = rt::get(item, "type");
      const json::value* itemText = rt::get(item, "text");
      if ((equals(type, "input_text") || equals(type, "output_text") || equals(type, "text")) && itemText && itemText->is_string()) {
        return withBlockCache(textBlock(itemText->get_string()), item);
      }
      const json::value* imageUrl = rt::get(item, "image_url");
      if (equals(type, "input_image") && imageUrl && imageUrl->is_string() && !rt::get(item, "file_id")) {
        // Native Messages has no equivalent for a caller-selected OpenAI image detail.
        const json::value* detail = rt::get(item, "detail");
        if (detail && !detail->is_null() && !equals(detail, "auto")) return std::nullopt;
        std::optional<JsonRecord> source = imageSource(imageUrl->get_string());
        if (!source) return std::nullopt;
        JsonRecord block;
        block["type"] = "image";
        block["source"] = std::move(*source);
        return withBlockCache(block, item);
      }
      return std::nullopt;
    }

    std::optional<std::vector<JsonRecord>> contentBlocks(const json::value* value) {
      if (value && value->is_string()) return std::vector<JsonRecord>{textBlock(value->get_string())};
      if (!value || !value->is_array()) return std::nullopt;
      std::vector<JsonRecord> blocks;
      for (const json::value& original : value->get_array()) {
        std::optional<JsonRecord> block = contentBlock(original);
        if (!block) return std::nullopt;
        blocks.push_back(std::move(*block));
      }
      return blocks;
    }

    std::optional<JsonRecord> functionArguments(const json::value* value) {
      if (!value || !value->is_string()) return std::nullopt;
      try {
        json::value parsed = rt::jsonParse(value->get_string());
        if (!isRecord(&parsed)) return std::nullopt;
        // JSON numbers beyond the JavaScript range would otherwise silently become null.
        std::vector<const json::value*> pending{&parsed};
        while (!pending.empty()) {
          const json::value* item = pending.back();
          pending.pop_back();
          if (rt::isNumber(*item) && !rt::Number::isFinite(*item)) return std::nullopt;
          if (item->is_array()) {
            for (const json::value& element : item->get_array()) pending.push_back(&element);
          } else if (isRecord(item)) {
            for (const json::value* member : rt::objectValues(item->get_object())) pending.push_back(member);
          }
        }
        return std::move(parsed.as_object());
      } catch (...) {
        return std::nullopt;
      }
    }

    void appendMessage(std::vector<NativeMessage>& messages, std::string_view role, std::vector<JsonRecord> blocks) {
      NativeMessage* previous = messages.empty() ? nullptr : &messages.back();
      if (previous && previous->role == role) {
        for (JsonRecord& block : blocks) previous->content.push_back(std::move(block));
      } else {
        messages.push_back(NativeMessage{.role = std::string(role), .content = std::move(blocks)});
      }
    }

    bool appendInputMessage(const JsonRecord& item, std::vector<JsonRecord>& system, std::vector<NativeMessage>& messages) {
      std::optional<std::vector<JsonRecord>> blocks = contentBlocks(rt::get(item, "content"));
      if (!blocks) return false;
      const json::value* role = rt::get(item, "role");
      if (equals(role, "system") || equals(role, "developer")) {
        // Messages has one system channel. Preserve every developer/system text block
        // there, retaining their relative order; the target has no interleaved role.
        for (const JsonRecord& block : *blocks)
          if (!equals(rt::get(block, "type"), "text")) return false;
        for (JsonRecord& block : *blocks) system.push_back(std::move(block));
        return true;
      }
      if (!equals(role, "user") && !equals(role, "assistant")) return false;
      appendMessage(messages, std::string_view(role->get_string()), std::move(*blocks));
      return true;
    }

    std::optional<std::string> projectedToolName(const JsonRecord& item, const ToolProjection& projection) {
      const json::value* name = rt::get(item, "name");
      if (!toolName(name)) return std::nullopt;
      const json::value* namespaceValue = rt::get(item, "namespace");
      const bool nullNamespace = rt::isNullish(namespaceValue);
      if (!nullNamespace && !toolName(namespaceValue)) return std::nullopt;
      const std::optional<std::string> namespace_ =
        nullNamespace ? std::nullopt : std::optional<std::string>(std::string(namespaceValue->get_string()));
      for (const auto& [projected, original] : projection.toolNames) {
        if (original.name == std::string_view(name->get_string()) && original.namespace_ == namespace_) return projected;
      }
      return nullNamespace ? std::optional<std::string>(std::string(name->get_string())) : std::nullopt;
    }

    std::optional<JsonRecord> nativeToolUse(const JsonRecord& item, const ToolProjection& projection) {
      const json::value* callId = rt::get(item, "call_id");
      if (!callId || !callId->is_string() || callId->get_string().empty()) return std::nullopt;
      std::optional<std::string> name = projectedToolName(item, projection);
      if (!name || name->empty()) return std::nullopt;
      std::optional<JsonRecord> args = functionArguments(rt::get(item, "arguments"));
      if (!args) return std::nullopt;
      JsonRecord block;
      block["type"] = "tool_use";
      block["id"] = *callId;
      block["name"] = text(*name);
      block["input"] = std::move(*args);
      return withBlockCache(block, item);
    }

    std::optional<JsonRecord> nativeToolResult(const JsonRecord& item) {
      const json::value* callId = rt::get(item, "call_id");
      if (!callId || !callId->is_string() || callId->get_string().empty()) return std::nullopt;
      const json::value* output = rt::get(item, "output");
      json::value content;
      if (output && output->is_string()) {
        content = *output;
      } else {
        std::optional<std::vector<JsonRecord>> blocks = contentBlocks(output);
        if (!blocks) return std::nullopt;
        json::array array;
        for (JsonRecord& block : *blocks) array.push_back(json::value(std::move(block)));
        content = std::move(array);
      }
      JsonRecord block;
      block["type"] = "tool_result";
      block["tool_use_id"] = *callId;
      block["content"] = std::move(content);
      return withBlockCache(block, item);
    }

    bool appendInputItem(const json::value& value, std::vector<JsonRecord>& system, std::vector<NativeMessage>& messages, const ToolProjection& projection) {
      if (!isRecord(&value)) return false;
      const JsonRecord& item = value.get_object();
      const json::value* type = rt::get(item, "type");
      if (equals(type, "message") || !type) return appendInputMessage(item, system, messages);
      const std::string_view role = equals(type, "function_call") ? "assistant" : "user";
      std::optional<JsonRecord> block;
      if (equals(type, "function_call")) block = nativeToolUse(item, projection);
      if (equals(type, "function_call_output")) block = nativeToolResult(item);
      if (!block) return false;
      std::vector<JsonRecord> blocks;
      blocks.push_back(std::move(*block));
      appendMessage(messages, role, std::move(blocks));
      return true;
    }

    bool appendReplayEntries(const std::vector<ClaudeNativeReplayEntry>& entries, std::vector<JsonRecord>& system, std::vector<NativeMessage>& messages,
                             const ToolProjection& projection) {
      for (const ClaudeNativeReplayEntry& entry : entries) {
        if (const auto* native = std::get_if<anthropic_replay::NativeReplayEntryNativeAssistant>(&entry)) {
          // structuredClone(entry.blocks): the store keeps only record blocks.
          std::vector<JsonRecord> blocks;
          for (const json::value& block : native->blocks) blocks.push_back(block.as_object());
          appendMessage(messages, "assistant", std::move(blocks));
        } else {
          const json::value item(std::get<anthropic_replay::NativeReplayEntryOriginal>(entry).item);
          if (!appendInputItem(item, system, messages, projection)) return false;
        }
      }
      return true;
    }

    std::optional<std::vector<NativeMessage>> inputMessages(const json::value* input, std::vector<JsonRecord>& system, const ToolProjection& projection,
                                                            const std::vector<ClaudeNativeReplayEntry>* replayEntries) {
      if (input && input->is_string()) {
        std::vector<NativeMessage> messages;
        std::vector<JsonRecord> content;
        content.push_back(textBlock(input->get_string()));
        messages.push_back(NativeMessage{.role = "user", .content = std::move(content)});
        return messages;
      }
      if (!input || !input->is_array()) return std::nullopt;
      std::vector<NativeMessage> messages;
      if (replayEntries) {
        if (!appendReplayEntries(*replayEntries, system, messages, projection)) return std::nullopt;
      } else {
        for (const json::value& item : input->get_array())
          if (!appendInputItem(item, system, messages, projection)) return std::nullopt;
      }
      // Hosted tools and opaque OpenAI reasoning without a scope-bound receipt retain Responses transport.
      if (messages.empty()) return std::nullopt;
      return messages;
    }

    bool collectNativeFunction(ToolProjection& projection, const JsonRecord& tool, const std::optional<std::string>& namespace_) {
      const json::value* name = rt::get(tool, "name");
      const json::value* strict = rt::get(tool, "strict");
      if (!toolName(name) || (strict && strict->is_bool() && strict->get_bool())) return false;
      const json::value* description = rt::get(tool, "description");
      if (description && !description->is_string()) return false;
      const json::value* parameters = rt::get(tool, "parameters");
      if (parameters && !isRecord(parameters)) return false;
      // `namespace ?? tool.namespace ?? null`
      std::optional<std::string> explicitNamespace = namespace_;
      if (!explicitNamespace) {
        const json::value* toolNamespace = rt::get(tool, "namespace");
        if (!rt::isNullish(toolNamespace)) {
          if (!toolName(toolNamespace)) return false;
          explicitNamespace = std::string(toolNamespace->get_string());
        }
      } else if (!toolName(std::string_view(*explicitNamespace))) {
        return false;
      }
      const std::string toolNameText(name->get_string());
      const std::string projected = explicitNamespace ? *explicitNamespace + "_" + toolNameText : toolNameText;
      if (!toolName(std::string_view(projected)) || projection.toolNames.has(projected)) return false;
      JsonRecord declaration;
      declaration["name"] = text(projected);
      if (description) declaration["description"] = *description;
      if (parameters) {
        declaration["input_schema"] = *parameters;
      } else {
        JsonRecord schema;
        schema["type"] = "object";
        declaration["input_schema"] = std::move(schema);
      }
      std::optional<JsonRecord> native = withBlockCache(declaration, tool);
      if (!native) return false;
      projection.tools.push_back(std::move(*native));
      projection.toolNames.set(projected, ClaudeToolName{.name = toolNameText, .namespace_ = explicitNamespace});
      return true;
    }

    bool collectNativeTool(ToolProjection& projection, const json::value& value, bool omitHostedWebSearch) {
      if (!isRecord(&value)) return false;
      const JsonRecord& tool = value.get_object();
      const json::value* type = rt::get(tool, "type");
      if (equals(type, "web_search")) {
        const json::value* external = rt::get(tool, "external_web_access");
        return omitHostedWebSearch || (external && external->is_bool() && !external->get_bool());
      }
      if (equals(type, "function")) return collectNativeFunction(projection, tool, std::nullopt);
      const json::value* name = rt::get(tool, "name");
      const json::array* tools = rt::getArray(tool, "tools");
      if (!equals(type, "namespace") || !toolName(name) || !tools || tools->empty()) return false;
      const std::optional<std::string> namespace_(std::string(name->get_string()));
      for (const json::value& nested : *tools) {
        if (!isRecord(&nested) || !equals(rt::get(nested, "type"), "function") || !collectNativeFunction(projection, nested.get_object(), namespace_))
          return false;
      }
      return true;
    }

    /**
     * Flatten Codex namespace tools into native Messages function names and retain
     * the reverse mapping for response translation. The native Messages lane has
     * no lossless hosted web_search representation. Reseller adapters reject any
     * enabled declaration; the direct Claude lane omits the hosted declaration so
     * the otherwise representable turn can run without advertising unavailable
     * provider-side search.
     */
    std::optional<ToolProjection> nativeTools(const json::value* value, bool omitHostedWebSearch) {
      ToolProjection projection;
      if (!value) return projection;
      if (!value->is_array()) return std::nullopt;
      for (const json::value& tool : value->get_array())
        if (!collectNativeTool(projection, tool, omitHostedWebSearch)) return std::nullopt;
      return projection;
    }

    std::optional<JsonRecord> nativeToolChoice(const json::value& value, const ToolProjection& projection) {
      if (equals(&value, "auto") || equals(&value, "none")) {
        JsonRecord choice;
        choice["type"] = value;
        return choice;
      }
      if (equals(&value, "required")) {
        JsonRecord choice;
        choice["type"] = "any";
        return choice;
      }
      if (!isRecord(&value)) return std::nullopt;
      const json::value* type = rt::get(value, "type");
      if (!equals(type, "function") && !equals(type, "custom")) return std::nullopt;
      std::optional<std::string> projected = projectedToolName(value.get_object(), projection);
      if (!projected || projected->empty()) return std::nullopt;
      JsonRecord choice;
      choice["type"] = "tool";
      choice["name"] = text(*projected);
      return choice;
    }

    std::optional<JsonRecord> nativeReasoning(const json::value* value, bool supportsEffort, bool supportsDisabledThinking,
                                              const std::optional<std::vector<std::string>>& observedEfforts) {
      if (rt::isNullish(value)) return JsonRecord{};
      if (!isRecord(value)) return std::nullopt;
      for (const std::string& key : rt::objectKeys(value->get_object()))
        if (key != "effort" && key != "summary") return std::nullopt;
      const json::value* summary = rt::get(*value, "summary");
      if (summary) {
        if (!summary->is_string()) return std::nullopt;
        const std::string_view summaryText(summary->get_string());
        if (summaryText != "auto" && summaryText != "concise" && summaryText != "detailed" && summaryText != "none") return std::nullopt;
      }
      const json::value* effortValue = rt::get(*value, "effort");
      if (!effortValue) return JsonRecord{};
      if (equals(effortValue, "none")) {
        if (!supportsDisabledThinking) return std::nullopt;
        JsonRecord thinking;
        thinking["type"] = "disabled";
        JsonRecord reasoning;
        reasoning["thinking"] = std::move(thinking);
        return reasoning;
      }
      if (!supportsEffort) return std::nullopt;
      const json::value effort = equals(effortValue, "ultra") && !observedEfforts ? json::value("max") : *effortValue;
      if (!effort.is_string()) return std::nullopt;
      const std::string effortText(effort.get_string());
      static const std::vector<std::string> DEFAULT_EFFORTS{"low", "medium", "high", "xhigh", "max"};
      const std::vector<std::string>& allowed = observedEfforts ? *observedEfforts : DEFAULT_EFFORTS;
      bool listed = false;
      for (const std::string& candidate : allowed) listed = listed || candidate == effortText;
      if (!listed) return std::nullopt;
      JsonRecord thinking;
      thinking["type"] = "adaptive";
      JsonRecord outputConfig;
      outputConfig["effort"] = effort;
      JsonRecord reasoning;
      reasoning["thinking"] = std::move(thinking);
      reasoning["output_config"] = std::move(outputConfig);
      return reasoning;
    }

    bool supportedCachePolicy(const JsonRecord& body) {
      if (has(body, "cache_control") && !cacheControl(rt::get(body, "cache_control"))) return false;
      const json::value* options = rt::get(body, "prompt_cache_options");
      if (options) {
        if (!isRecord(options) || rt::get(*options, "ttl")) return false;
        for (const std::string& key : rt::objectKeys(options->get_object()))
          if (key != "mode") return false;
        const json::value* mode = rt::get(*options, "mode");
        if (mode && !equals(mode, "implicit") && !equals(mode, "explicit")) return false;
      }
      // Neither the native five-minute nor one-hour cache can honor an explicit 24-hour/30-minute lifetime.
      const json::value* retention = rt::get(body, "prompt_cache_retention");
      return !retention || equals(retention, "in_memory");
    }

    double explicitCacheBlocks(const std::vector<JsonRecord>& system, const std::vector<NativeMessage>& messages, const std::vector<JsonRecord>& tools) {
      std::vector<const JsonRecord*> blocks;
      for (const JsonRecord& block : system) blocks.push_back(&block);
      for (const JsonRecord& block : tools) blocks.push_back(&block);
      for (const NativeMessage& message : messages)
        for (const JsonRecord& block : message.content) blocks.push_back(&block);
      double controls = 0;
      // The loop also visits the blocks it appends, like for...of over a growing array.
      for (std::size_t index = 0; index < blocks.size(); ++index) {
        const JsonRecord& block = *blocks[index];
        if (has(block, "cache_control")) controls += 1;
        const json::array* content = rt::getArray(block, "content");
        if (equals(rt::get(block, "type"), "tool_result") && content) {
          for (const json::value& nested : *content)
            if (isRecord(&nested)) blocks.push_back(&nested.get_object());
        }
      }
      return controls;
    }

    std::optional<CachePolicy> applyNativeCachePolicy(const JsonRecord& body, std::vector<JsonRecord>& system, std::vector<NativeMessage>& messages,
                                                      std::vector<JsonRecord>& tools) {
      if (!supportedCachePolicy(body)) return std::nullopt;
      const json::value* options = rt::get(body, "prompt_cache_options");
      const double explicitBreakpointCount = explicitCacheBlocks(system, messages, tools);
      if (explicitBreakpointCount > 4) return std::nullopt;
      const bool declaredExplicit = isRecord(options) && equals(rt::get(*options, "mode"), "explicit");
      const bool explicit_ = explicitBreakpointCount > 0 || declaredExplicit;
      if (!explicit_) {
        std::optional<JsonRecord> control;
        if (has(body, "cache_control")) {
          control = cacheControl(rt::get(body, "cache_control"));
        } else {
          control = JsonRecord{};
          (*control)["type"] = "ephemeral";
        }
        if (!control) return std::nullopt;
        JsonRecord* last = nullptr;
        for (auto message = messages.rbegin(); message != messages.rend() && !last; ++message) {
          for (auto block = message->content.rbegin(); block != message->content.rend(); ++block) {
            const std::string type = rt::toString(rt::get(*block, "type"));
            if (type == "text" || type == "image" || type == "tool_use" || type == "tool_result") {
              last = &*block;
              break;
            }
          }
        }
        if (!last && !system.empty()) last = &system.back();
        if (!last && !tools.empty()) last = &tools.back();
        if (!last) return std::nullopt;
        rt::objectSet(*last, "cache_control", json::value(std::move(*control)));
      }
      return CachePolicy{.promptCacheMode = explicit_ ? "explicit" : "implicit", .explicitBreakpointCount = explicitBreakpointCount};
    }

    OutputFormat structuredOutputFormat(const json::value* value) {
      if (!value) return {};
      if (!isRecord(value)) return {.rejected = true, .format = std::nullopt};
      for (const std::string& key : rt::objectKeys(value->get_object()))
        if (key != "format") return {.rejected = true, .format = std::nullopt};
      const json::value* format = rt::get(*value, "format");
      if (!format) return {};
      const json::value* schema = rt::get(*format, "schema");
      if (!isRecord(format) || !equals(rt::get(*format, "type"), "json_schema") || !isRecord(schema)) return {.rejected = true, .format = std::nullopt};
      const json::value* strict = rt::get(*format, "strict");
      if (strict && !strict->is_bool()) return {.rejected = true, .format = std::nullopt};
      const json::value* name = rt::get(*format, "name");
      if (name && !name->is_string()) return {.rejected = true, .format = std::nullopt};
      JsonRecord result;
      result["type"] = "json_schema";
      result["schema"] = *schema;
      return {.rejected = false, .format = std::move(result)};
    }

    bool supportedTextFormat(const json::value* value) { return !structuredOutputFormat(value).rejected; }

    bool supportedBaseControls(const JsonRecord& body) {
      const json::value* store = rt::get(body, "store");
      if (!isSurplusClaudeModel(rt::get(body, "model")) || (store && !(store->is_bool() && !store->get_bool()))) return false;
      if (!rt::isNullish(rt::get(body, "previous_response_id"))) return false;
      for (const std::string_view key : {"context_management", "max_tool_calls", "truncation", "metadata", "service_tier", "safety_identifier", "user"})
        if (rt::get(body, key)) return false;
      if (!supportedTextFormat(rt::get(body, "text"))) return false;
      const json::value* include = rt::get(body, "include");
      if (!include) return true;
      if (!include->is_array()) return false;
      for (const json::value& field : include->get_array())
        if (!equals(&field, "reasoning.encrypted_content")) return false;
      return true;
    }

    std::optional<std::vector<NativeMessage>> nativeRequestMessages(const JsonRecord& body, std::vector<JsonRecord>& system, const ToolProjection& projection,
                                                                    const SurplusClaudeRequestOptions& options) {
      const json::value* input = rt::get(body, "input");
      if (!input || !input->is_array() || !options.reasoningReplayScope || options.reasoningReplayScope->empty())
        return inputMessages(input, system, projection, nullptr);
      const anthropic_replay::AnthropicReplayContext context{
        .scope = *options.reasoningReplayScope,
        .provider = options.provider.value_or("surplus"),
        .model = rt::toString(rt::get(body, "model")),
      };
      const std::optional<std::vector<ClaudeNativeReplayEntry>> entries = anthropic_replay::restoreAnthropicReplay(input->get_array(), context);
      if (!entries) return std::nullopt;
      return inputMessages(input, system, projection, &*entries);
    }

    std::optional<JsonRecord> parallelToolChoice(const JsonRecord& body, const ToolProjection& projection) {
      const json::value* toolChoice = rt::get(body, "tool_choice");
      std::optional<JsonRecord> choice = toolChoice ? nativeToolChoice(*toolChoice, projection) : JsonRecord{};
      if (!choice) return std::nullopt;
      const json::value* parallel = rt::get(body, "parallel_tool_calls");
      if (parallel) {
        if (!parallel->is_bool()) return std::nullopt;
        if (!has(*choice, "type")) rt::objectSet(*choice, "type", json::value("auto"));
        rt::objectSet(*choice, "disable_parallel_tool_use", json::value(!parallel->get_bool()));
      }
      return choice;
    }

    json::value messagesJson(std::vector<NativeMessage> messages) {
      json::array out;
      for (NativeMessage& message : messages) {
        json::array content;
        for (JsonRecord& block : message.content) content.push_back(json::value(std::move(block)));
        JsonRecord entry;
        entry["role"] = text(message.role);
        entry["content"] = std::move(content);
        out.push_back(json::value(std::move(entry)));
      }
      return out;
    }

    json::value recordsJson(std::vector<JsonRecord> records) {
      json::array out;
      for (JsonRecord& record : records) out.push_back(json::value(std::move(record)));
      return out;
    }

    JsonRecord nativeBody(const JsonRecord& body, double maxTokens, std::vector<JsonRecord> system, std::vector<NativeMessage> messages,
                          std::vector<JsonRecord> tools, const JsonRecord& choice, const JsonRecord& reasoning, const std::optional<JsonRecord>& format) {
      JsonRecord native;
      native["model"] = *rt::get(body, "model");
      native["max_tokens"] = rt::makeNumber(maxTokens);
      native["messages"] = messagesJson(std::move(messages));
      const json::value* stream = rt::get(body, "stream");
      native["stream"] = stream && stream->is_bool() && stream->get_bool();
      for (const json::key_value_pair* member : rt::objectEntries(reasoning)) rt::objectDefine(native, member->key(), member->value());
      if (!system.empty()) rt::objectSet(native, "system", recordsJson(std::move(system)));
      if (!tools.empty()) rt::objectSet(native, "tools", recordsJson(std::move(tools)));
      if (!choice.empty()) rt::objectSet(native, "tool_choice", json::value(choice));
      if (format) {
        const json::value* current = rt::get(native, "output_config");
        JsonRecord outputConfig = isRecord(current) ? rt::objectSpread(current->get_object()) : JsonRecord{};
        rt::objectDefine(outputConfig, "format", json::value(*format));
        rt::objectSet(native, "output_config", json::value(std::move(outputConfig)));
      }
      // The caller's affinity key remains a Surplus router hint, never a native Codex header.
      for (const std::string_view key : {"temperature", "top_p", "prompt_cache_key"})
        if (const json::value* value = rt::get(body, key)) rt::objectSet(native, key, *value);
      return native;
    }
  }

  bool isSurplusClaudeModel(std::string_view model) {
    static const rt::Regex CLAUDE_MODEL("^(?:~?anthropic\\/)?claude-");
    return CLAUDE_MODEL.test(model);
  }

  bool isSurplusClaudeModel(const json::value& model) { return model.is_string() && isSurplusClaudeModel(std::string_view(model.get_string())); }

  bool isSurplusClaudeModel(const json::value* model) { return model && isSurplusClaudeModel(*model); }

  std::optional<SurplusClaudeRequest> buildSurplusClaudeRequest(const json::object& body) {
    return buildSurplusClaudeRequest(body, std::nullopt, SurplusClaudeRequestOptions{});
  }

  std::optional<SurplusClaudeRequest> buildSurplusClaudeRequest(const json::object& body, std::optional<double> catalogMaxOutputTokens) {
    return buildSurplusClaudeRequest(body, catalogMaxOutputTokens, SurplusClaudeRequestOptions{});
  }

  std::optional<SurplusClaudeRequest> buildSurplusClaudeRequest(const json::object& body, std::optional<double> catalogMaxOutputTokens,
                                                                const SurplusClaudeRequestOptions& options) {
    if (!supportedBaseControls(body)) return std::nullopt;
    // `body.max_output_tokens ?? catalogMaxOutputTokens`
    const json::value* bodyMaxTokens = rt::get(body, "max_output_tokens");
    json::value catalogMaxTokens;
    const json::value* maxTokens = bodyMaxTokens;
    if (rt::isNullish(bodyMaxTokens)) {
      maxTokens = nullptr;
      if (catalogMaxOutputTokens) {
        catalogMaxTokens = rt::makeNumber(*catalogMaxOutputTokens);
        maxTokens = &catalogMaxTokens;
      }
    }
    if (!positiveInteger(maxTokens)) return std::nullopt;
    const json::value* instructions = rt::get(body, "instructions");
    std::optional<std::vector<JsonRecord>> system = rt::isNullish(instructions) ? std::vector<JsonRecord>{} : contentBlocks(instructions);
    if (!system) return std::nullopt;
    for (const JsonRecord& block : *system)
      if (!equals(rt::get(block, "type"), "text")) return std::nullopt;
    std::optional<ToolProjection> projection = nativeTools(rt::get(body, "tools"), options.omitHostedWebSearch == true);
    if (!projection) return std::nullopt;
    std::optional<std::vector<NativeMessage>> messages = nativeRequestMessages(body, *system, *projection, options);
    if (!messages) return std::nullopt;
    const std::optional<JsonRecord> reasoning =
      nativeReasoning(rt::get(body, "reasoning"), options.supportsNativeReasoningEffort == true, options.supportsNativeDisabledThinking == true,
                      options.nativeReasoningEfforts);
    if (!reasoning) return std::nullopt;
    const OutputFormat format = structuredOutputFormat(rt::get(body, "text"));
    if (format.rejected) return std::nullopt;
    const std::optional<JsonRecord> choice = parallelToolChoice(body, *projection);
    if (!choice) return std::nullopt;
    const std::optional<CachePolicy> policy = applyNativeCachePolicy(body, *system, *messages, projection->tools);
    if (!policy) return std::nullopt;
    const double maxTokenCount = rt::toNumber(*maxTokens);
    return SurplusClaudeRequest{
      .body = nativeBody(body, maxTokenCount, std::move(*system), std::move(*messages), std::move(projection->tools), *choice, *reasoning, format.format),
      .promptCacheMode = policy->promptCacheMode,
      .explicitBreakpointCount = policy->explicitBreakpointCount,
      .toolNames = std::move(projection->toolNames),
    };
  }
}
