// Port of lib/jev_compaction/codex_items.ts.
#include "uos/lib/jev_compaction/codex_items.hpp"

#include "uos/lib/jev_compaction/compact.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include <array>
#include <deque>
#include <utility>

namespace uos::lib::jev_compaction::codex_items {
  using compact::truncatedResultText;
  using types::CallDecision;
  using types::Message;
  using types::ToolResult;
  using types::ToolUse;

  namespace {
    // `Record<string, unknown>`
    using UnknownRecord = json::object;

    // Body of Codex's built-in local-compaction prompt
    // (`codex-rs/prompts/templates/compact/prompt.md`). Used only to recognise the
    // prompt Codex appends as the final input item, never to detect whether a
    // request is a compaction request (that is header-only).
    constexpr std::array<std::string_view, 2> COMPACTION_PROMPT_MARKERS = {"CONTEXT CHECKPOINT COMPACTION", "Create a handoff summary for another LLM"};

    const UnknownRecord* record(const json::value* value) { return rt::getObject(value); }
    const UnknownRecord* record(const json::value& value) { return rt::getObject(value); }

    std::string str(const json::value* value) {
      const std::optional<std::string_view> text = rt::getString(value);
      return text ? std::string(*text) : std::string();
    }

    std::string join(const std::vector<std::string>& parts, std::string_view separator) {
      std::string out;
      for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) out += separator;
        out += parts[index];
      }
      return out;
    }

    // Text of a `message` item's content parts; images/files become placeholders.
    std::string messageText(const json::value* content) {
      if (const std::optional<std::string_view> text = rt::getString(content)) return std::string(*text);
      const json::array* items = rt::getArray(content);
      if (items == nullptr) return "";
      std::vector<std::string> parts;
      for (const json::value& raw : *items) {
        if (raw.is_string()) {
          parts.emplace_back(raw.as_string());
          continue;
        }
        const UnknownRecord* part = record(raw);
        if (part == nullptr) continue;
        const std::string type = str(rt::get(*part, "type"));
        const json::value* text = rt::get(*part, "text");
        const json::value* refusal = rt::get(*part, "refusal");
        if (text != nullptr && text->is_string()) parts.emplace_back(text->as_string());
        else if (type == "input_image" || type == "image") parts.emplace_back("[image]");
        else if (type == "input_file") parts.emplace_back("[file]");
        else if (type == "refusal" && refusal != nullptr && refusal->is_string()) parts.emplace_back(refusal->as_string());
      }
      return join(parts, "\n");
    }

    // Tool input: the parsed `arguments` JSON, or a wrapper when it is not an object.
    UnknownRecord parseToolArguments(const std::string& argumentsText) {
      const std::string trimmed = rt::trim(argumentsText);
      if (rt::length(trimmed) == 0) return {};
      json::value parsed;
      try {
        parsed = rt::jsonParse(trimmed);
      } catch (const rt::JsError&) {
        UnknownRecord wrapper;
        rt::objectDefine(wrapper, "unparsed_arguments", json::value(argumentsText));
        return wrapper;
      }
      if (const UnknownRecord* obj = record(parsed)) return *obj;
      UnknownRecord wrapper;
      rt::objectDefine(wrapper, "value", std::move(parsed));
      return wrapper;
    }

    bool isErrorOutput(const UnknownRecord& item) {
      return rt::getBool(rt::get(item, "is_error")) == std::optional<bool>(true) || rt::getBool(rt::get(item, "success")) == std::optional<bool>(false);
    }

    // Output text of a `*_call_output` item; objects are serialised, never dropped.
    // (`JSON.stringify(output) ?? String(output)`: JSON data always serialises to a
    // string, so the `String(output)` fallback has no json::value to apply to.)
    std::string outputText(const json::value* output) {
      if (const std::optional<std::string_view> text = rt::getString(output)) return std::string(*text);
      if (rt::isNullish(output)) return "[no output]";
      try {
        return rt::jsonStringify(*output);
      } catch (const rt::JsError&) {
        return "[unserialisable output]";
      }
    }

    const rt::Regex& OPAQUE_KEY() {
      static const rt::Regex regex("encrypt|ciphertext|opaque|blob|signature", "i");
      return regex;
    }
    std::string OPAQUE_NOTICE(std::int64_t chars) { return "[opaque payload, " + std::to_string(chars) + " chars omitted from this memory]"; }

    json::value redact(const json::value& value, int depth = 0) {
      if (value.is_string()) return value;
      if (value.is_null() || !(value.is_object() || value.is_array())) return value;
      if (depth > 6) return json::value("[deeply nested value omitted]");
      if (value.is_array()) {
        json::array out;
        for (const json::value& entry : value.as_array()) out.push_back(redact(entry, depth + 1));
        return out;
      }
      UnknownRecord out;
      for (const json::key_value_pair* member : rt::objectEntries(value.as_object())) {
        const std::string_view key(member->key().data(), member->key().size());
        const json::value& entry = member->value();
        if (OPAQUE_KEY().test(key)) {
          rt::objectSet(out, key, json::value(entry.is_string() ? OPAQUE_NOTICE(rt::length(std::string_view(entry.as_string()))) : OPAQUE_NOTICE(0)));
          continue;
        }
        rt::objectSet(out, key, redact(entry, depth + 1));
      }
      return out;
    }

    struct Placeholder {
      std::string text;
      bool opaque = false;
    };

    // Bounded, redacted rendering of an item the adapter does not model. Opaque
    // payloads (encrypted reasoning/compaction blobs) become a notice instead of
    // usable plaintext; unknown items stay represented so nothing vanishes mutely.
    // (`JSON.stringify(redact(item)) ?? ""`: a redacted record always serialises.)
    Placeholder opaquePlaceholder(const std::string& itemType, const UnknownRecord& item, std::int64_t limit = 2000) {
      std::string json;
      try {
        json = rt::jsonStringify(redact(json::value(item)));
      } catch (const rt::JsError&) {
        json = "[unserialisable item]";
      }
      const bool opaque = rt::includes(json, "opaque payload,");
      const std::int64_t length = rt::length(json);
      const std::string clipped = length > limit ? rt::slice(json, 0, limit) + "… [" + std::to_string(length - limit) + " chars omitted]" : json;
      return Placeholder{"[" + (itemType.empty() ? std::string("unknown item") : itemType) + "]" + (opaque ? " (contains an opaque payload)" : "") + " " + clipped,
                         opaque};
    }

    TextEntry messageEntry(const UnknownRecord& item) {
      const std::string role = str(rt::get(item, "role"));
      std::string normalized = "user";
      if (role == "assistant") normalized = "assistant";
      else if (role == "developer" || role == "system") normalized = role;
      return TextEntry{"message", normalized, messageText(rt::get(item, "content"))};
    }

    CallEntry callEntry(const UnknownRecord& item) {
      const bool functionCall = str(rt::get(item, "type")) == "function_call";
      const json::value* input = rt::get(item, "input");
      UnknownRecord rawInput;
      if (functionCall) {
        rawInput = parseToolArguments(str(rt::get(item, "arguments")));
      } else if (input != nullptr && input->is_string()) {
        rawInput = parseToolArguments(std::string(input->as_string()));
      } else {
        const UnknownRecord* object = record(input);
        rawInput = object != nullptr ? *object : UnknownRecord();
      }
      std::string inputJson;
      try {
        inputJson = rt::jsonStringify(json::value(rawInput));
      } catch (const rt::JsError&) {
        inputJson = "[unserialisable tool input]";
      }
      const std::string name = str(rt::get(item, "name"));
      return CallEntry{str(rt::get(item, "type")), str(rt::get(item, "call_id")), functionCall || !name.empty() ? name : std::string("custom_tool"),
                       std::move(rawInput), std::move(inputJson)};
    }

    ResultEntry resultEntry(const UnknownRecord& item) {
      return ResultEntry{str(rt::get(item, "type")), str(rt::get(item, "call_id")), outputText(rt::get(item, "output")), isErrorOutput(item)};
    }

    Message entryToMessage(const TranscriptEntry& entry) {
      if (const auto* text = std::get_if<TextEntry>(&entry)) {
        return Message{text->role == "assistant" ? "assistant" : "user", text->text, {}, std::nullopt};
      }
      if (const auto* call = std::get_if<CallEntry>(&entry)) {
        ToolUse toolUse{call->callId, call->tool, call->input, std::nullopt, std::nullopt};
        return Message{"assistant", "", {std::move(toolUse)}, std::nullopt};
      }
      const auto& result = std::get<ResultEntry>(entry);
      ToolResult toolResult{result.callId, result.text, std::nullopt};
      if (result.isError) toolResult.isError = true;
      return Message{"user", "", {}, std::vector<ToolResult>{std::move(toolResult)}};
    }

    // The trailing item Codex appends for local compaction. Codex always appends
    // its prompt (built-in `SUMMARIZATION_PROMPT` or the configured
    // `compact_prompt`) as the final user message, so only that item is excluded —
    // earlier real user content is kept even when it looks similar.
    std::optional<PromptExclusion> compactionPromptIndex(const std::vector<const UnknownRecord*>& items) {
      if (items.empty()) return std::nullopt;
      const auto index = static_cast<std::int64_t>(items.size()) - 1;
      const UnknownRecord& item = *items.back();
      if (str(rt::get(item, "type")) != "message" || str(rt::get(item, "role")) != "user") return std::nullopt;
      const std::string text = messageText(rt::get(item, "content"));
      if (rt::length(rt::trim(text)) == 0) return std::nullopt;
      bool known = false;
      for (const std::string_view marker : COMPACTION_PROMPT_MARKERS) known = known || rt::includes(text, marker);
      return PromptExclusion{index, rt::length(text), known ? "known-prompt" : "trailing-prompt-assumed"};
    }
  }

  std::optional<CodexTranscript> parseCodexInput(const json::value& input) {
    const json::array* items = rt::getArray(input);
    if (items == nullptr) return std::nullopt;
    std::deque<UnknownRecord> wrapped;
    std::vector<const UnknownRecord*> rawItems;
    for (const json::value& item : *items) {
      if (const UnknownRecord* object = record(item)) {
        rawItems.push_back(object);
        continue;
      }
      UnknownRecord& wrapper = wrapped.emplace_back();
      rt::objectDefine(wrapper, "type", json::value("unknown"));
      rt::objectDefine(wrapper, "value", item);
      rawItems.push_back(&wrapper);
    }
    const std::optional<PromptExclusion> exclusion = compactionPromptIndex(rawItems);
    std::vector<TranscriptEntry> entries;
    std::int64_t opaqueItems = 0;
    for (std::size_t position = 0; position < rawItems.size(); ++position) {
      const UnknownRecord& item = *rawItems[position];
      if (exclusion && static_cast<std::int64_t>(position) == exclusion->index) continue;
      const std::string type = str(rt::get(item, "type"));
      if (type == "message") {
        entries.emplace_back(messageEntry(item));
        continue;
      }
      if (type == "function_call" || type == "custom_tool_call") {
        entries.emplace_back(callEntry(item));
        continue;
      }
      if (type == "function_call_output" || type == "custom_tool_call_output") {
        entries.emplace_back(resultEntry(item));
        continue;
      }
      const Placeholder placeholder = opaquePlaceholder(type, item);
      if (placeholder.opaque) opaqueItems += 1;
      entries.emplace_back(TextEntry{type.empty() ? std::string("unknown") : type, "user", placeholder.text});
    }
    CodexTranscript transcript;
    for (const TranscriptEntry& entry : entries) transcript.messages.push_back(entryToMessage(entry));
    transcript.entries = std::move(entries);
    transcript.exclusion = exclusion;
    transcript.opaqueItems = opaqueItems;
    return transcript;
  }

  std::optional<CodexTranscript> parseCodexInput(const json::value* input) {
    if (input == nullptr) return std::nullopt;
    return parseCodexInput(*input);
  }

  namespace {
    std::string ROLE_LABEL(std::string_view role) {
      if (role == "user") return "user";
      if (role == "assistant") return "assistant";
      if (role == "developer") return "developer instructions";
      if (role == "system") return "system instructions";
      return "undefined";
    }

    // Bounded call input for a dropped call: the call line itself is provenance we
    // never erase, but a long script or command list is truncated like a dropped
    // result is. Kept calls render their input verbatim.
    std::string truncatedCallInput(std::string_view text, double limit) {
      const auto length = static_cast<double>(rt::length(text));
      if (length <= limit + 60) return std::string(text);
      return rt::slice(text, 0, limit) + "\n[fast-jev-compaction truncated " + rt::numberToString(length - limit) +
             " chars of this tool input; the call is kept as provenance]";
    }
  }

  std::string renderSummary(const CodexTranscript& transcript, const RenderOptions& options) {
    rt::OrderedMap<std::string, CallDecision> decisionByCall;
    for (const CallDecision& decision : options.decisions) {
      const std::string* mapped = options.callIds.get(decision.id);
      const std::string callId = mapped != nullptr ? *mapped : decision.id;
      decisionByCall.set(callId, decision);
    }
    std::vector<std::string> out = {
      std::string(SUMMARY_MARKER),
      "Tool calls and results below were classified by TypeSafe Jev (keep / truncate).",
      "A dropped call keeps its provenance line and a bounded head of its result; nothing is erased",
      "outright. Text kept verbatim is copied from the original transcript; this memory is text only, so",
      "structured tool-call replay and opaque items (encrypted reasoning or compaction payloads) are not restored.",
      "",
    };
    for (const TranscriptEntry& entry : transcript.entries) {
      if (const auto* text = std::get_if<TextEntry>(&entry)) {
        out.push_back("[" + ROLE_LABEL(text->role) + "]");
        out.push_back(text->text);
        out.emplace_back();
        continue;
      }
      if (const auto* call = std::get_if<CallEntry>(&entry)) {
        const CallDecision* decision = decisionByCall.get(call->callId);
        const std::string action = decision != nullptr ? decision->action : std::string("keep");
        const std::string renderedInput = action == "keep" ? call->inputJson : truncatedCallInput(call->inputJson, options.headChars);
        out.push_back("[tool call " + (call->callId.empty() ? std::string("(no call id)") : call->callId) + "] " +
                      (call->tool.empty() ? std::string("(unnamed tool)") : call->tool) + " — " +
                      (decision != nullptr ? decision->action : std::string("no decision (kept)")));
        out.push_back("input: " + renderedInput);
        out.emplace_back();
        continue;
      }
      const auto& result = std::get<ResultEntry>(entry);
      const CallDecision* decision = decisionByCall.get(result.callId);
      if (decision == nullptr) {
        out.push_back("[tool result " + (result.callId.empty() ? std::string("(no call id)") : result.callId) + " — no matching call in this transcript]");
        out.push_back(result.text);
        out.emplace_back();
        continue;
      }
      if (decision->action == "keep") {
        out.push_back("[tool result " + result.callId + " — kept verbatim]");
        out.push_back(result.text);
        out.emplace_back();
        continue;
      }
      const std::string chars = std::to_string(rt::length(result.text));
      const std::string label = decision->action == "drop_call" ? "[tool result " + result.callId + " — truncated (call dropped), original was " + chars + " chars]"
                                                                : "[tool result " + result.callId + " — truncated, original was " + chars + " chars]";
      out.push_back(label);
      out.push_back(truncatedResultText(result.text, result.isError, options.headChars));
      out.emplace_back();
    }
    const std::optional<types::CompactResult::Stats>& stats = options.stats;
    const std::optional<PromptExclusion>& excluded = transcript.exclusion;
    std::string statsLine = "calls=0 kept=0 results_dropped=0 calls_dropped=0 pinned=0 chars_before=0 chars_after=0 requests=0 stage=none";
    if (stats) {
      statsLine = join(
        {
          "calls=" + std::to_string(stats->resultsDropped + stats->callsDropped + stats->kept + stats->pinned),
          "kept=" + std::to_string(stats->kept),
          "results_dropped=" + std::to_string(stats->resultsDropped),
          "calls_dropped=" + std::to_string(stats->callsDropped),
          "pinned=" + std::to_string(stats->pinned),
          "chars_before=" + std::to_string(stats->charsBefore),
          "chars_after=" + std::to_string(stats->charsAfter),
          "requests=" + std::to_string(stats->requests),
          "stage=" + (stats->stateStage.empty() ? std::string("none") : stats->stateStage),
        },
        " ");
    }
    out.push_back("[fast-jev-compaction stats] " + join(
                                                     {
                                                       "items=" + std::to_string(transcript.entries.size()),
                                                       "excluded_prompt_chars=" + std::to_string(excluded ? excluded->chars : 0),
                                                       "excluded_prompt_reason=" + (excluded ? excluded->reason : std::string("none")),
                                                       "opaque_items=" + std::to_string(transcript.opaqueItems),
                                                       statsLine,
                                                     },
                                                     " "));
    return join(out, "\n");
  }

  std::optional<TurnMetadata> parseTurnMetadata(const std::optional<std::string>& header) {
    if (!header || header->empty()) return std::nullopt;
    json::value parsed;
    try {
      parsed = rt::jsonParse(*header);
    } catch (const rt::JsError&) {
      return std::nullopt;
    }
    if (const UnknownRecord* object = record(parsed)) return *object;
    return std::nullopt;
  }

  bool isResponsesCompaction(const std::optional<TurnMetadata>& metadata) {
    if (!metadata) return false;
    const json::value* kind = rt::get(*metadata, "request_kind");
    if (kind == nullptr || !kind->is_string() || kind->as_string() != "compaction") return false;
    const UnknownRecord* compaction = record(rt::get(*metadata, "compaction"));
    if (compaction == nullptr) return false;
    const json::value* implementation = rt::get(*compaction, "implementation");
    return implementation != nullptr && implementation->is_string() && implementation->as_string() == "responses";
  }
}
