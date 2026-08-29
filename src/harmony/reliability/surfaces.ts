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

import type { ToolDefinition } from "../types.ts";
import { toolDefinitions } from "../tools/schemas.ts";
import { estimateJsonTokens } from "./context.ts";

/** Experimental m05 tool definitions (never routed; measurement only). */
export const BROAD_EXPERIMENTAL_TOOLS: readonly ToolDefinition[] = [
  {
    name: "filesystem.write",
    description: "Create or replace a file with the given content (experimental, not routed by the canonical harness).",
    parameters: {
      type: "object",
      properties: {
        path: { type: "string", minLength: 1 },
        content: { type: "string" },
      },
      required: ["path"],
      additionalProperties: false,
    },
    strict: false,
  },
  {
    name: "git.status",
    description: "Show the working-tree status of the repository (experimental, not routed).",
    parameters: {
      type: "object",
      properties: {},
      required: [],
      additionalProperties: false,
    },
    strict: false,
  },
  {
    name: "git.commit",
    description: "Commit staged files with a message (experimental, not routed).",
    parameters: {
      type: "object",
      properties: {
        message: { type: "string", minLength: 1 },
      },
      required: ["message"],
      additionalProperties: false,
    },
    strict: false,
  },
  {
    name: "task.complete",
    description:
      "Mark the current task complete (experimental, not routed; final answers are accepted by the harness instead).",
    parameters: {
      type: "object",
      properties: {
        note: { type: "string" },
      },
      required: [],
      additionalProperties: false,
    },
    strict: false,
  },
];

export const BROAD_EXPERIMENTAL_NAMES: readonly string[] = BROAD_EXPERIMENTAL_TOOLS.map((t) => t.name);

export type ToolSurfaceId = "compact" | "broad";

export interface ToolSurface {
  id: ToolSurfaceId;
  definitions: readonly ToolDefinition[];
}

/** The canonical (m04) compact surface. */
export const compactToolSurface = (): ToolSurface => ({
  id: "compact",
  definitions: toolDefinitions(),
});

/** The experimental broad surface (canonical + experimental definitions). */
export const broadToolSurface = (): ToolSurface => ({
  id: "broad",
  definitions: [...toolDefinitions(), ...BROAD_EXPERIMENTAL_TOOLS],
});

/** Deterministic estimated token cost of a surface's tool definitions. */
export const surfaceTokenCost = (surface: ToolSurface): number =>
  estimateJsonTokens(surface.definitions.map((tool) => ({
    name: tool.name,
    description: tool.description,
    parameters: tool.parameters,
    strict: tool.strict ?? false,
  })));

/** Deterministic one-line surface summary. */
export const describeSurface = (surface: ToolSurface): string =>
  `${surface.id}: ${surface.definitions.length} definitions, ${surfaceTokenCost(surface)} estimated tokens`;
