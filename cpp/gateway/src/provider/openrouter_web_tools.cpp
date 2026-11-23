#include "uos/provider/openrouter_web_tools.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace uos::provider::openrouter_web_tools {
  namespace {
    constexpr std::array<std::string_view, 2> DEFAULT_WEB_TOOLS = {"openrouter:web_search", "openrouter:web_fetch"};

    bool existingWebTool(const json::value& tool, std::string_view type) {
      if (!utils::isRecord(tool)) return false;
      const std::optional<std::string_view> toolType = rt::getString(rt::get(tool, "type"));
      if (!toolType) return false;
      const std::string nativeType = rt::slice(type, rt::length("openrouter:"));
      return *toolType == type || *toolType == nativeType || rt::startsWith(*toolType, nativeType + "_");
    }
  }

  json::object withOpenRouterOpenAiWebTools(const json::object& body, std::string_view model) {
    static const rt::Regex OPENAI_MODEL("^~?openai\\/");
    if (!OPENAI_MODEL.test(model)) return body;
    const std::vector<std::string> parts = rt::split(model, ":");
    if (std::find(parts.begin(), parts.end(), "batch") != parts.end()) return body;
    const json::value* toolChoice = rt::get(body, "tool_choice");
    if (toolChoice != nullptr && !(toolChoice->is_string() && toolChoice->get_string() == "auto")) return body;
    if (rt::get(body, "functions") != nullptr || rt::get(body, "function_call") != nullptr) return body;
    const json::value* bodyTools = rt::get(body, "tools");
    if (bodyTools != nullptr && (!bodyTools->is_array() || bodyTools->get_array().empty())) return body;

    const json::array tools = bodyTools != nullptr && bodyTools->is_array() ? bodyTools->get_array() : json::array{};
    std::vector<std::string_view> missing;
    for (const std::string_view type : DEFAULT_WEB_TOOLS) {
      const bool present = std::any_of(tools.begin(), tools.end(), [type](const json::value& tool) { return existingWebTool(tool, type); });
      if (!present) missing.push_back(type);
    }
    if (missing.empty()) return body;
    json::array combined = tools;
    for (const std::string_view type : missing) combined.push_back(json::object{{"type", json::string(type)}});
    json::object result = rt::objectSpread(body);
    rt::objectDefine(result, "tools", json::value(std::move(combined)));
    return result;
  }
}
