// Port of lib/jev_compaction/types.ts: the JSON form of the library's objects.
#include "uos/lib/jev_compaction/types.hpp"

#include "uos/rt/json.hpp"

namespace uos::lib::jev_compaction::types {
  json::value toJson(const HistoryToolCall& call) {
    json::object out;
    rt::objectDefine(out, "id", json::value(call.id));
    rt::objectDefine(out, "tool", json::value(call.tool));
    rt::objectDefine(out, "input", json::value(call.input));
    rt::objectDefine(out, "result", json::value(call.result));
    return out;
  }

  json::value toJson(const HistoryEntry& entry) {
    json::object out;
    rt::objectDefine(out, "i", rt::makeNumber(entry.i));
    rt::objectDefine(out, "role", json::value(entry.role));
    rt::objectDefine(out, "text", json::value(entry.text));
    if (entry.tool_calls) {
      json::array calls;
      if (const auto* structured = std::get_if<std::vector<HistoryToolCall>>(&*entry.tool_calls)) {
        for (const HistoryToolCall& call : *structured) calls.push_back(toJson(call));
      } else {
        for (const std::string& line : std::get<std::vector<std::string>>(*entry.tool_calls)) calls.push_back(json::value(line));
      }
      rt::objectDefine(out, "tool_calls", json::value(std::move(calls)));
    }
    return out;
  }

  json::value toJson(const CompactionState& state) {
    json::array history;
    for (const HistoryEntry& entry : state.history) history.push_back(toJson(entry));
    json::object out;
    rt::objectDefine(out, "context", json::value(state.context));
    rt::objectDefine(out, "goal", json::value(state.goal));
    rt::objectDefine(out, "history", json::value(std::move(history)));
    return out;
  }

  json::value toJson(const NoulQuestion& question) {
    json::object out;
    rt::objectDefine(out, "type", json::value(question.type));
    rt::objectDefine(out, "instructions", json::value(question.instructions));
    return out;
  }

  json::value toJson(const JevQuestions& questions) {
    json::object out;
    for (const auto& [name, question] : questions) rt::objectDefine(out, name, toJson(question));
    return out;
  }
}
