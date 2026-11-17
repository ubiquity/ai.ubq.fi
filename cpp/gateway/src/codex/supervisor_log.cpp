#include "uos/codex/supervisor_log.hpp"

#include "uos/rt/date.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <span>

namespace uos::codex::supervisor_log {
  namespace {
    constexpr std::int64_t MAX_TAIL_BYTES = 256 * 1024;
    constexpr std::size_t MAX_EVENTS = 200;

    using JsonRecord = json::object;

    bool isRecord(const json::value* value) { return rt::isRecord(value); }

    bool isUnknownArray(const json::value* value) { return value != nullptr && value->is_array(); }

    std::optional<std::string> asString(const json::value* value) {
      if (value == nullptr || !value->is_string()) return std::nullopt;
      return std::string(value->get_string());
    }

    std::optional<std::string> textOrNull(const std::optional<std::string>& text) {
      if (!text) return std::nullopt;
      std::string trimmed = rt::trim(*text);
      if (rt::length(trimmed) > 0) return trimmed;
      return std::nullopt;
    }

    std::optional<std::string> textOrNull(const json::value* value) { return textOrNull(asString(value)); }

    /**
     * Legacy assistant messages have no channel/phase; only visible phases are
     * accepted. `final_answer` is the app-server phase the follow panel renders as
     * "Assistant · final", so the brief must treat it as visible too.
     */
    constexpr std::array<std::string_view, 3> VISIBLE_MESSAGE_PHASES = {"commentary", "final", "final_answer"};

    /** Rejects private analysis/reasoning channels and phases; absent values are legacy visible. */
    bool isVisibleMessage(const JsonRecord& payload) {
      for (const char* key : {"channel", "phase"}) {
        const std::optional<std::string> text = textOrNull(rt::get(payload, key));
        if (!text) continue;
        const std::string value = rt::toLowerCase(*text);
        bool visible = false;
        for (const std::string_view phase : VISIBLE_MESSAGE_PHASES) visible = visible || value == phase;
        if (!visible) return false;
      }
      return true;
    }

    std::string messageText(const JsonRecord& payload) {
      const json::value* content = rt::get(payload, "content");
      if (!isUnknownArray(content)) return "";
      std::vector<std::string> parts;
      for (const json::value& part : content->get_array()) {
        if (!isRecord(&part)) continue;
        const std::optional<std::string> type = asString(rt::get(part, "type"));
        if (type != "output_text" && type != "input_text" && type != "text") continue;
        const std::optional<std::string> text = asString(rt::get(part, "text"));
        if (rt::truthy(text)) parts.push_back(*text);
      }
      std::string joined;
      for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) joined += "\n";
        rt::append(joined, parts[index]);
      }
      return joined;
    }

    std::optional<SupervisorLogEvent> messageEventOf(const JsonRecord& payload, double atMs) {
      if (!isVisibleMessage(payload)) return std::nullopt;
      const std::optional<std::string> role = asString(rt::get(payload, "role"));
      if (role != "user" && role != "assistant") return std::nullopt;
      std::string text = messageText(payload);
      if (text.empty()) return std::nullopt;
      return SupervisorLogEvent{atMs, *role, std::move(text)};
    }

    std::optional<SupervisorLogEvent> eventOf(const JsonRecord& record) {
      const json::value* type = rt::get(record, "type");
      if (type == nullptr || !type->is_string() || type->get_string() != "response_item") return std::nullopt;
      const json::value* payloadValue = rt::get(record, "payload");
      if (!isRecord(payloadValue)) return std::nullopt;
      const JsonRecord& payload = payloadValue->get_object();
      const double atMs = rt::Date::parse(asString(rt::get(record, "timestamp")).value_or(""));
      if (!rt::Number::isFinite(atMs)) return std::nullopt;
      const std::optional<std::string> payloadType = asString(rt::get(payload, "type"));
      if (payloadType == "message") return messageEventOf(payload, atMs);
      if (payloadType == "custom_tool_call" || payloadType == "function_call") {
        const std::string name = textOrNull(rt::get(payload, "name")).value_or(*payloadType);
        const std::string status = textOrNull(rt::get(payload, "status")).value_or("recorded");
        return SupervisorLogEvent{atMs, "tool", rt::concat("tool: " + name + " ", status)};
      }
      if (payloadType == "custom_tool_call_output" || payloadType == "function_call_output") {
        const std::optional<std::string> output = textOrNull(rt::get(payload, "output"));
        if (!output) return std::nullopt;
        return SupervisorLogEvent{atMs, "tool", "tool output: " + *output};
      }
      return std::nullopt;
    }

    std::optional<SupervisorLogEvent> parseLine(std::string_view line) {
      const std::string trimmed = rt::trim(line);
      if (trimmed.empty()) return std::nullopt;
      json::value parsed;
      try {
        parsed = rt::jsonParse(trimmed);
      } catch (const rt::JsError&) {
        return std::nullopt;
      }
      return isRecord(&parsed) ? eventOf(parsed.get_object()) : std::nullopt;
    }
  }

  std::optional<std::string> resolveSupervisorRolloutPath(std::string_view codexHome, std::string_view threadId, const json::value* rolloutPath) {
    const std::optional<std::string> path = asString(rolloutPath);
    if (!path) return std::nullopt;
    return resolveSupervisorRolloutPath(codexHome, threadId, std::string_view(*path));
  }

  std::optional<std::string> resolveSupervisorRolloutPath(std::string_view codexHome, std::string_view threadId, std::string_view rolloutPath) {
    const std::optional<std::string> path = textOrNull(std::optional<std::string>(std::string(rolloutPath)));
    if (!path || !rt::startsWith(*path, "/") || rt::includes(*path, "..")) return std::nullopt;
    std::string home(codexHome);
    while (rt::endsWith(home, "/")) home = rt::slice(home, 0, -1);
    const bool inSessions = rt::startsWith(*path, rt::concat(home, "/sessions/"));
    const bool inArchived = rt::startsWith(*path, rt::concat(home, "/archived_sessions/"));
    if (!inSessions && !inArchived) return std::nullopt;
    const std::string name = rt::slice(*path, rt::lastIndexOf(*path, "/") + 1);
    return rt::includes(name, threadId) ? path : std::nullopt;
  }

  std::vector<SupervisorLogEvent> parseSupervisorRolloutTail(std::string_view text) {
    std::vector<SupervisorLogEvent> events;
    for (const std::string& line : rt::split(text, "\n")) {
      std::optional<SupervisorLogEvent> event = parseLine(line);
      if (!event) continue;
      events.push_back(std::move(*event));
      if (events.size() > MAX_EVENTS) events.erase(events.begin());
    }
    return events;
  }

  rt::Task<std::vector<SupervisorLogEvent>> readSupervisorRolloutTail(ReadSupervisorRolloutTailInput input) {
    if (!rt::truthy(input.codexHome)) co_return std::vector<SupervisorLogEvent>{};
    const json::value* rolloutPath = input.rolloutPath ? &*input.rolloutPath : nullptr;
    const std::optional<std::string> lexical = resolveSupervisorRolloutPath(*input.codexHome, input.threadId, rolloutPath);
    if (!lexical) co_return std::vector<SupervisorLogEvent>{};
    std::vector<SupervisorLogEvent> events;
    try {
      const std::string resolved = rt::fs::promises::realpath(*lexical);
      co_await rt::microtask();
      if (!resolveSupervisorRolloutPath(*input.codexHome, input.threadId, std::string_view(resolved))) co_return std::vector<SupervisorLogEvent>{};
      rt::fs::FileHandle file = rt::fs::promises::open(resolved, "r");
      co_await rt::microtask();
      rt::Reason failure;
      try {
        const std::int64_t size = file.stat().size;
        co_await rt::microtask();
        const std::int64_t start = std::max<std::int64_t>(0, size - MAX_TAIL_BYTES);
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size - start));
        std::size_t read = 0;
        while (read < buffer.size()) {
          const std::size_t bytesRead = file.read(buffer, read, buffer.size() - read, start + static_cast<std::int64_t>(read)).bytesRead;
          co_await rt::microtask();
          if (bytesRead == 0) break;
          read += bytesRead;
        }
        const std::string text = rt::decodeUtf8(std::span<const std::uint8_t>(buffer.data(), read));
        // "\n" is ASCII, so its byte offset in the decoded UTF-8 text marks the same cut as the UTF-16 index.
        const std::size_t newline = text.find('\n');
        const std::string_view complete = start > 0 && newline != std::string::npos ? std::string_view(text).substr(newline + 1) : std::string_view(text);
        events = parseSupervisorRolloutTail(complete);
      } catch (...) {
        failure = std::current_exception();
      }
      file.close();
      co_await rt::microtask();
      if (failure) std::rethrow_exception(failure);
    } catch (...) {
      events.clear();
    }
    co_return events;
  }
}
