#include "uos/harmony/reliability/surfaces.hpp"

#include "uos/harmony/reliability/context.hpp"
#include "uos/harmony/tools/schemas.hpp"

#include <utility>

namespace uos::harmony::reliability::surfaces {
  ToolSurface compactToolSurface() { return ToolSurface{.id = "compact", .definitions = tools::schemas::toolDefinitions()}; }

  ToolSurface broadToolSurface() {
    std::vector<types::ToolDefinition> definitions = tools::schemas::toolDefinitions();
    definitions.insert(definitions.end(), BROAD_EXPERIMENTAL_TOOLS.begin(), BROAD_EXPERIMENTAL_TOOLS.end());
    return ToolSurface{.id = "broad", .definitions = std::move(definitions)};
  }

  double surfaceTokenCost(const ToolSurface& surface) {
    json::array tools;
    for (const types::ToolDefinition& tool : surface.definitions) {
      json::object entry;
      entry["name"] = tool.name;
      entry["description"] = tool.description;
      entry["parameters"] = tool.parameters;
      entry["strict"] = tool.strict.value_or(false);
      tools.push_back(std::move(entry));
    }
    return context::estimateJsonTokens(tools);
  }
}
