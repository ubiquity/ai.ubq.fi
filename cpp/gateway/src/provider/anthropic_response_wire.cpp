#include "uos/provider/anthropic_response_wire.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"

#include <utility>

namespace uos::provider::anthropic_response_wire {
  namespace {
    using JsonRecord = json::object;

    bool record(const json::value* value) { return rt::isRecord(value); }

    json::value jsonString(std::string_view text) { return json::value(json::string(text)); }

    // `{ ...a, ...b }` member order: a's members, then b's new members.
    JsonRecord spread(const JsonRecord& base, const JsonRecord& patch) { return rt::objectSpread(base, patch); }
  }

  std::string encodeClaudeResponsesEvent(const json::object& event) {
    return "event: " + rt::toString(rt::get(event, "type")) + "\ndata: " + rt::jsonStringify(json::value(event)) + "\n\n";
  }

  json::object claudeResponsesEnvelope(std::string_view id, std::string_view model, double created, std::string_view status, const ClaudeResponseEcho& echo) {
    JsonRecord envelope;
    rt::objectDefine(envelope, "id", jsonString(id));
    rt::objectDefine(envelope, "object", jsonString("response"));
    rt::objectDefine(envelope, "created_at", rt::makeNumber(created));
    rt::objectDefine(envelope, "status", jsonString(status));
    rt::objectDefine(envelope, "model", jsonString(model));
    rt::objectDefine(envelope, "output", json::array{});
    rt::objectDefine(envelope, "error", nullptr);
    rt::objectDefine(envelope, "incomplete_details", nullptr);
    rt::objectDefine(envelope, "instructions", echo.instructions ? jsonString(*echo.instructions) : json::value(nullptr));
    rt::objectDefine(envelope, "metadata", json::object{});
    rt::objectDefine(envelope, "parallel_tool_calls",
                     echo.parallel_tool_calls && echo.parallel_tool_calls->is_bool() ? json::value(echo.parallel_tool_calls->get_bool()) : json::value(true));
    rt::objectDefine(envelope, "tool_choice", echo.tool_choice && !echo.tool_choice->is_null() ? *echo.tool_choice : jsonString("auto"));
    rt::objectDefine(envelope, "tools", echo.tools && echo.tools->is_array() ? *echo.tools : json::value(json::array{}));
    rt::objectDefine(envelope, "temperature", nullptr);
    rt::objectDefine(envelope, "top_p", nullptr);
    rt::objectDefine(envelope, "max_output_tokens", nullptr);
    rt::objectDefine(envelope, "previous_response_id", nullptr);
    rt::objectDefine(envelope, "reasoning", nullptr);
    rt::objectDefine(envelope, "store", false);
    rt::objectDefine(envelope, "truncation", jsonString("disabled"));
    rt::objectDefine(envelope, "usage", nullptr);
    return envelope;
  }

  namespace {
    struct ActiveItem {
      std::string kind;  // "reasoning" | "message"
      std::string id;
      double index = 0;
      std::string text;
      std::string refusal;
      double textPart = -1;
      double refusalPart = -1;
    };

    struct PreparedTool {
      double index = 0;
      std::string id;
      bool custom = false;
      std::string args;
      std::string input;
      JsonRecord item;
    };

    std::string customInput(const std::string& args) {
      try {
        const json::value parsed = rt::jsonParse(args);
        const std::optional<std::string_view> input = rt::getString(rt::get(parsed, "input"));
        return record(&parsed) && input ? std::string(*input) : args;
      } catch (const rt::JsError&) {
        return args;
      }
    }

    std::optional<PreparedTool> prepareTool(const JsonRecord& call, double index, std::string_view status, const WriterOptions& options) {
      const json::value* function = rt::get(call, "function");
      const std::optional<std::string_view> callId = rt::getString(rt::get(call, "id"));
      if (!record(function) || !callId || !rt::getString(rt::get(*function, "name")) || !rt::getString(rt::get(*function, "arguments"))) return std::nullopt;
      const std::string name(*rt::getString(rt::get(*function, "name")));
      const ClaudeToolName* mapped = options.toolNames ? options.toolNames->get(name) : nullptr;
      const ClaudeToolName original = mapped ? *mapped : ClaudeToolName{name, std::nullopt};
      const bool custom = options.customToolNames && options.customToolNames->has(name);
      const std::string id = std::string(custom ? "ctc" : "fc") + "_" + options.id + "_" + rt::numberToString(index);
      const std::string args(*rt::getString(rt::get(*function, "arguments")));
      const std::string input = custom ? customInput(args) : args;
      JsonRecord item;
      rt::objectDefine(item, "id", jsonString(id));
      rt::objectDefine(item, "type", jsonString(custom ? "custom_tool_call" : "function_call"));
      rt::objectDefine(item, "status", jsonString(status));
      rt::objectDefine(item, "call_id", jsonString(*callId));
      rt::objectDefine(item, "name", jsonString(original.name));
      if (custom) {
        rt::objectSet(item, "input", jsonString(input));
      } else {
        rt::objectSet(item, "arguments", jsonString(args));
        if (rt::truthy(original.namespace_)) rt::objectSet(item, "namespace", jsonString(*original.namespace_));
      }
      return PreparedTool{index, id, custom, args, input, std::move(item)};
    }

    std::vector<JsonRecord> toolLifecycle(const PreparedTool& tool) {
      const auto& [index, id, custom, args, input, item] = tool;
      JsonRecord patch;
      rt::objectDefine(patch, "status", jsonString("in_progress"));
      if (custom) rt::objectDefine(patch, "input", jsonString(""));
      else rt::objectDefine(patch, "arguments", jsonString(""));
      const JsonRecord empty = spread(item, patch);
      std::vector<JsonRecord> events;
      events.push_back(JsonRecord{{"type", "response.output_item.added"}, {"output_index", rt::makeNumber(index)}, {"item", empty}});
      if (custom) {
        events.push_back(JsonRecord{{"type", "response.custom_tool_call_input.delta"},
                                    {"item_id", jsonString(id)},
                                    {"output_index", rt::makeNumber(index)},
                                    {"call_id", *rt::get(item, "call_id")},
                                    {"delta", jsonString(input)}});
      } else {
        events.push_back(JsonRecord{{"type", "response.function_call_arguments.delta"}, {"item_id", jsonString(id)}, {"output_index", rt::makeNumber(index)}, {"delta", jsonString(args)}});
        events.push_back(JsonRecord{{"type", "response.function_call_arguments.done"}, {"item_id", jsonString(id)}, {"output_index", rt::makeNumber(index)}, {"arguments", jsonString(args)}});
      }
      events.push_back(JsonRecord{{"type", "response.output_item.done"}, {"output_index", rt::makeNumber(index)}, {"item", item}});
      return events;
    }

    void append(std::vector<JsonRecord>& events, std::vector<JsonRecord> more) {
      for (JsonRecord& event : more) events.push_back(std::move(event));
    }
  }

  struct ClaudeResponsesWriter::State {
    WriterOptions options;
    ClaudeResponseEcho echo;
    std::vector<JsonRecord> output;
    std::optional<ActiveItem> active;
    bool started = false;
    std::string pendingReasoning;
    std::optional<JsonRecord> finalResponse;

    JsonRecord envelope(std::string_view status) const { return claudeResponsesEnvelope(options.id, options.model, options.created, status, echo); }

    json::array outputArray() const {
      json::array array;
      for (const JsonRecord& item : output) array.push_back(item);
      return array;
    }

    std::vector<JsonRecord> open() {
      if (started) return {};
      started = true;
      std::vector<JsonRecord> events;
      events.push_back(JsonRecord{{"type", "response.created"}, {"response", envelope("in_progress")}});
      events.push_back(JsonRecord{{"type", "response.in_progress"}, {"response", envelope("in_progress")}});
      return events;
    }

    static JsonRecord itemFor(const ActiveItem& item) {
      if (item.kind == "reasoning") {
        return JsonRecord{{"id", jsonString(item.id)},
                          {"type", "reasoning"},
                          {"status", "completed"},
                          {"summary", json::array{JsonRecord{{"type", "summary_text"}, {"text", jsonString(item.text)}}}}};
      }
      json::array content;
      if (item.textPart >= 0) content.push_back(JsonRecord{{"type", "output_text"}, {"text", jsonString(item.text)}, {"annotations", json::array{}}});
      if (item.refusalPart >= 0) content.push_back(JsonRecord{{"type", "refusal"}, {"refusal", jsonString(item.refusal)}});
      return JsonRecord{{"id", jsonString(item.id)}, {"type", "message"}, {"status", "completed"}, {"role", "assistant"}, {"content", std::move(content)}};
    }

    std::vector<JsonRecord> close() {
      if (!active) return {};
      const ActiveItem item = *active;
      active.reset();
      const JsonRecord common{{"item_id", jsonString(item.id)}, {"output_index", rt::makeNumber(item.index)}};
      std::vector<JsonRecord> events;
      const auto event = [&common](std::string_view type, JsonRecord rest) {
        JsonRecord head{{"type", jsonString(type)}};
        return rt::objectSpread(head, common, rest);
      };
      if (item.kind == "reasoning") {
        events.push_back(event("response.reasoning_summary_text.done", JsonRecord{{"summary_index", 0}, {"text", jsonString(item.text)}}));
        events.push_back(event("response.reasoning_summary_part.done",
                               JsonRecord{{"summary_index", 0}, {"part", JsonRecord{{"type", "summary_text"}, {"text", jsonString(item.text)}}}}));
      } else {
        if (item.textPart >= 0) {
          events.push_back(event("response.output_text.done",
                                 JsonRecord{{"content_index", rt::makeNumber(item.textPart)}, {"text", jsonString(item.text)}, {"logprobs", json::array{}}}));
          events.push_back(event("response.content_part.done",
                                 JsonRecord{{"content_index", rt::makeNumber(item.textPart)},
                                            {"part", JsonRecord{{"type", "output_text"}, {"text", jsonString(item.text)}, {"annotations", json::array{}}}}}));
        }
        if (item.refusalPart >= 0) {
          events.push_back(event("response.refusal.done", JsonRecord{{"content_index", rt::makeNumber(item.refusalPart)}, {"refusal", jsonString(item.refusal)}}));
          events.push_back(event("response.content_part.done",
                                 JsonRecord{{"content_index", rt::makeNumber(item.refusalPart)},
                                            {"part", JsonRecord{{"type", "refusal"}, {"refusal", jsonString(item.refusal)}}}}));
        }
      }
      const JsonRecord completed = itemFor(item);
      output[static_cast<std::size_t>(item.index)] = completed;
      events.push_back(JsonRecord{{"type", "response.output_item.done"}, {"output_index", rt::makeNumber(item.index)}, {"item", completed}});
      return events;
    }

    std::vector<JsonRecord> ensure(const std::string& kind) {
      if (active && active->kind == kind) return {};
      std::vector<JsonRecord> events = close();
      const double index = static_cast<double>(output.size());
      const std::string id = std::string(kind == "reasoning" ? "rs" : "msg") + "_" + options.id + "_" + rt::numberToString(index);
      const JsonRecord item = kind == "reasoning"
                                ? JsonRecord{{"id", jsonString(id)}, {"type", jsonString(kind)}, {"status", "in_progress"}, {"summary", json::array{}}}
                                : JsonRecord{{"id", jsonString(id)}, {"type", jsonString(kind)}, {"status", "in_progress"}, {"role", "assistant"}, {"content", json::array{}}};
      active = ActiveItem{kind, id, index, "", "", -1, -1};
      output.push_back(item);
      events.push_back(JsonRecord{{"type", "response.output_item.added"}, {"output_index", rt::makeNumber(index)}, {"item", item}});
      if (kind == "reasoning") {
        events.push_back(JsonRecord{{"type", "response.reasoning_summary_part.added"},
                                    {"item_id", jsonString(id)},
                                    {"output_index", rt::makeNumber(index)},
                                    {"summary_index", 0},
                                    {"part", JsonRecord{{"type", "summary_text"}, {"text", ""}}}});
      }
      return events;
    }

    std::vector<JsonRecord> reasoningDelta(const std::string& text) {
      if (text.empty()) return {};
      if (active && active->kind == "message") {
        pendingReasoning += text;
        return {};
      }
      std::vector<JsonRecord> events = ensure("reasoning");
      if (!active) return events;
      active->text += text;
      events.push_back(JsonRecord{{"type", "response.reasoning_summary_text.delta"},
                                  {"item_id", jsonString(active->id)},
                                  {"output_index", rt::makeNumber(active->index)},
                                  {"summary_index", 0},
                                  {"delta", jsonString(text)}});
      return events;
    }

    std::vector<JsonRecord> messageDelta(const std::string& text, bool refusal = false) {
      if (text.empty()) return {};
      std::vector<JsonRecord> events = ensure("message");
      if (!active) return events;
      const JsonRecord common{{"item_id", jsonString(active->id)}, {"output_index", rt::makeNumber(active->index)}};
      double& part = refusal ? active->refusalPart : active->textPart;
      if (part < 0) {
        part = (active->textPart >= 0 ? 1 : 0) + (active->refusalPart >= 0 ? 1 : 0);
        const JsonRecord partValue =
          refusal ? JsonRecord{{"type", "refusal"}, {"refusal", ""}} : JsonRecord{{"type", "output_text"}, {"text", ""}, {"annotations", json::array{}}};
        JsonRecord head{{"type", "response.content_part.added"}};
        events.push_back(rt::objectSpread(head, common, JsonRecord{{"content_index", rt::makeNumber(part)}, {"part", partValue}}));
      }
      if (refusal) active->refusal += text;
      else active->text += text;
      JsonRecord head{{"type", refusal ? "response.refusal.delta" : "response.output_text.delta"}};
      JsonRecord tail{{"content_index", rt::makeNumber(part)}, {"delta", jsonString(text)}};
      if (!refusal) rt::objectDefine(tail, "logprobs", json::array{});
      events.push_back(rt::objectSpread(head, common, tail));
      return events;
    }

    std::vector<JsonRecord> push(const JsonRecord& delta) {
      std::vector<JsonRecord> events = open();
      const std::optional<std::string_view> reasoningContent = rt::getString(rt::get(delta, "reasoning_content"));
      append(events, reasoningDelta(reasoningContent ? std::string(*reasoningContent) : std::string()));
      const std::optional<std::string_view> content = rt::getString(rt::get(delta, "content"));
      append(events, messageDelta(content ? std::string(*content) : std::string()));
      const std::optional<std::string_view> refusalText = rt::getString(rt::get(delta, "refusal"));
      append(events, messageDelta(refusalText ? std::string(*refusalText) : std::string(), true));
      return events;
    }

    std::vector<JsonRecord> toolEvents(const std::vector<JsonRecord>& calls, std::string_view status) {
      std::vector<JsonRecord> events;
      for (const JsonRecord& call : calls) {
        const std::optional<PreparedTool> tool = prepareTool(call, static_cast<double>(output.size()), status, options);
        if (!tool) continue;
        output.push_back(tool->item);
        append(events, toolLifecycle(*tool));
      }
      return events;
    }

    std::vector<JsonRecord> finish(const std::vector<JsonRecord>& calls, const std::optional<json::value>& reason, const std::optional<json::value>& usage) {
      std::vector<JsonRecord> events = open();
      append(events, close());
      if (!pendingReasoning.empty()) {
        const std::string pending = pendingReasoning;
        pendingReasoning.clear();
        std::vector<JsonRecord> reasoning = reasoningDelta(pending);
        std::vector<JsonRecord> closing = close();
        append(events, std::move(reasoning));
        append(events, std::move(closing));
      }
      const auto reasonIs = [&reason](std::string_view text) { return reason && reason->is_string() && reason->get_string() == text; };
      std::string status = "failed";
      if (reasonIs("stop") || reasonIs("tool_calls")) status = "completed";
      if (reasonIs("length") || reasonIs("content_filter")) status = "incomplete";
      append(events, toolEvents(calls, status == "completed" ? "completed" : "incomplete"));
      JsonRecord response = envelope(status);
      rt::objectDefine(response, "output", outputArray());
      rt::objectDefine(response, "usage", rt::Nullable<json::value>(rt::orUndefined(usage)));
      if (status == "incomplete")
        rt::objectSet(response, "incomplete_details", JsonRecord{{"reason", reasonIs("content_filter") ? "content_filter" : "max_output_tokens"}});
      if (status == "failed") {
        const std::string text = reason ? rt::toString(*reason) : "undefined";
        rt::objectSet(response, "error", JsonRecord{{"code", "claude_chat_finish_reason"}, {"message", jsonString("Claude Chat stopped with reason " + text + ".")}});
      }
      finalResponse = response;
      events.push_back(JsonRecord{{"type", jsonString("response." + status)}, {"response", response}});
      return events;
    }

    JsonRecord observedUsage(const std::optional<json::value>& usage) const {
      JsonRecord response = envelope("in_progress");
      rt::objectDefine(response, "usage", rt::Nullable<json::value>(rt::orUndefined(usage)));
      return JsonRecord{{"type", "response.in_progress"}, {"response", std::move(response)}};
    }
  };

  std::vector<json::object> ClaudeResponsesWriter::open() const { return state_->open(); }

  std::vector<json::object> ClaudeResponsesWriter::push(const json::object& delta) const { return state_->push(delta); }

  std::vector<json::object> ClaudeResponsesWriter::finish(const std::vector<json::object>& calls, const std::optional<json::value>& reason,
                                                          const std::optional<json::value>& usage) const {
    return state_->finish(calls, reason, usage);
  }

  json::object ClaudeResponsesWriter::observedUsage(const std::optional<json::value>& usage) const { return state_->observedUsage(usage); }

  std::optional<json::object> ClaudeResponsesWriter::response() const { return state_->finalResponse; }

  ClaudeResponsesWriter createClaudeResponsesWriter(WriterOptions options) {
    auto state = std::make_shared<ClaudeResponsesWriter::State>();
    state->options = std::move(options);
    return ClaudeResponsesWriter(std::move(state));
  }
}
