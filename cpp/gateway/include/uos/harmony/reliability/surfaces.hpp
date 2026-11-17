#pragma once
// Port of src/harmony/reliability/surfaces.ts.
/**
 * Tool-surface comparison definitions (plan m05).
 *
 * m05 measures the context cost of the model-facing tool surface and the
 * failure shape of calls to tools the router cannot serve:
 *
 * - **compact** — the canonical m04 surface (nine tools, the only version
 *   the harness may expose to the model);
 * - **broad** — the canonical nine plus EXPERIMENTAL definitions
 *   (`filesystem.write`, `git.status`, `git.commit`, `task.complete`).
 *   These exist only for the cost/evidence comparison.  They are NOT routed
 *   by the m04 router, so a model that calls one receives the deterministic
 *   `invalid_args` feedback — which is precisely the evidence that broad
 *   surfaces invite calls the canonical harness cannot serve.
 *
 * The broad surface is never registered in the canonical tool registry and is
 * never exposed by the canonical harness defaults.
 */
#include "uos/harmony/types.hpp"
#include "uos/rt/prelude.hpp"

#include <string>
#include <vector>

namespace uos::harmony::reliability::surfaces {
  /** Experimental m05 tool definitions (never routed; measurement only). */
  inline const std::vector<types::ToolDefinition> BROAD_EXPERIMENTAL_TOOLS{
    types::ToolDefinition{
      .name = "filesystem.write",
      .description = "Create or replace a file with the given content (experimental, not routed by the canonical harness).",
      .parameters =
        json::object{
          {"type", "object"},
          {"properties", json::object{{"path", json::object{{"type", "string"}, {"minLength", 1}}}, {"content", json::object{{"type", "string"}}}}},
          {"required", json::array{"path"}},
          {"additionalProperties", false},
        },
      .strict = false,
    },
    types::ToolDefinition{
      .name = "git.status",
      .description = "Show the working-tree status of the repository (experimental, not routed).",
      .parameters =
        json::object{
          {"type", "object"},
          {"properties", json::object{}},
          {"required", json::array{}},
          {"additionalProperties", false},
        },
      .strict = false,
    },
    types::ToolDefinition{
      .name = "git.commit",
      .description = "Commit staged files with a message (experimental, not routed).",
      .parameters =
        json::object{
          {"type", "object"},
          {"properties", json::object{{"message", json::object{{"type", "string"}, {"minLength", 1}}}}},
          {"required", json::array{"message"}},
          {"additionalProperties", false},
        },
      .strict = false,
    },
    types::ToolDefinition{
      .name = "task.complete",
      .description = "Mark the current task complete (experimental, not routed; final answers are accepted by the harness instead).",
      .parameters =
        json::object{
          {"type", "object"},
          {"properties", json::object{{"note", json::object{{"type", "string"}}}}},
          {"required", json::array{}},
          {"additionalProperties", false},
        },
      .strict = false,
    },
  };

  // "compact" | "broad"
  using ToolSurfaceId = std::string;

  struct ToolSurface {
    ToolSurfaceId id;
    std::vector<types::ToolDefinition> definitions;
    friend bool operator==(const ToolSurface&, const ToolSurface&) = default;
  };

  /** The canonical (m04) compact surface. */
  ToolSurface compactToolSurface();

  /** The experimental broad surface (canonical + experimental definitions). */
  ToolSurface broadToolSurface();

  /** Deterministic estimated token cost of a surface's tool definitions. */
  double surfaceTokenCost(const ToolSurface& surface);
}
