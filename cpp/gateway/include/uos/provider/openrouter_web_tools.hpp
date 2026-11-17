#pragma once
// Port of src/provider/openrouter-web-tools.ts.
#include "uos/rt/prelude.hpp"

#include <string_view>

namespace uos::provider::openrouter_web_tools {
  /** Supply OpenRouter-operated web tools without overriding explicit client tool controls. */
  // Returns `body` unchanged, or a copy with the missing web tools appended to `tools`.
  json::object withOpenRouterOpenAiWebTools(const json::object& body, std::string_view model);
}
