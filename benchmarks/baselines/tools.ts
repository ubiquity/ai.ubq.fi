/**
 * Baseline tool execution layer (m03).
 *
 * Adapters A/B/D drive a model (or a process bridge) that issues calls
 * against the same provisional canonical tool surface the m02 benchmark
 * defines in `benchmarks/adapter.ts`. Validation and the tool schemas are
 * imported from the shared module so the baseline and the scripted
 * `reference` adapter stay aligned; execution semantics mirror the reference
 * adapter's `executeTool` (m04 owns the final canonical implementation).
 *
 * The canonical tool set exposed by every live baseline:
 * filesystem.read/find/search, shell.exec, editor.apply_patch,
 * task.update_plan, browser.search/open/find (the browser tools are
 * deterministic "unavailable" errors until m04 ships fake backends).
 */

import { CANONICAL_TOOL_NAMES, TOOL_SCHEMAS, type ToolResult, validateToolArgs } from "../adapter.ts";
import { FixtureWorkspace, globMatch } from "../fixture.ts";

export type { ToolResult };

/** An OpenAI Chat Completions tool definition used by baseline requests. */
export interface CanonicalToolDefinition {
  name: string;
  description: string;
  parameters: Record<string, unknown>;
  /** Cerebras requires one strictness value per request (m01 probe fact). */
  strict: boolean;
}

export const TOOL_DESCRIPTIONS: Record<string, string> = {
  "filesystem.read": "Read a UTF-8 text file relative to the workspace root.",
  "filesystem.find": "List files under a workspace path matching a glob pattern.",
  "filesystem.search": "Search file contents under a workspace path for a query string.",
  "shell.exec": "Run a shell command in the workspace root and return its output.",
  "editor.apply_patch": "Create a file (add) or replace the single occurrence of `old` with `new`.",
  "task.update_plan": "Record the current plan as a list of non-empty strings.",
  "browser.search": "Search the web for a query (not wired until m04).",
  "browser.open": "Open a URL (not wired until m04).",
  "browser.find": "Find text in the currently open page (not wired until m04).",
};

const jsonSchemaType = (t: "string" | "boolean" | "string[]"): Record<string, unknown> => {
  if (t === "string") return { type: "string" };
  if (t === "boolean") return { type: "boolean" };
  return { type: "array", items: { type: "string" } };
};

/** Builds the canonical tool definitions as official Chat Completions tools. */
export function canonicalToolDefinitions(strict: boolean): CanonicalToolDefinition[] {
  return CANONICAL_TOOL_NAMES.map((name) => {
    const schema = TOOL_SCHEMAS[name];
    return {
      name,
      description: TOOL_DESCRIPTIONS[name] ?? `${name} tool`,
      parameters: {
        type: "object",
        properties: Object.fromEntries(
          Object.entries(schema.types).map(([key, t]) => [key, jsonSchemaType(t)]),
        ),
        required: schema.required,
        additionalProperties: false,
      },
      strict,
    };
  });
}

const SEARCH_LINE_LIMIT = 200;
const SHELL_TIMEOUT_MS = 20_000;
const OUTPUT_LIMIT = 8000;

function clip(s: string): string {
  return s.length > OUTPUT_LIMIT ? `${s.slice(0, OUTPUT_LIMIT)}…[truncated]` : s;
}

/** Executes one canonical tool in the disposable workspace (mirror of m02). */
export async function executeBaselineTool(
  workspace: FixtureWorkspace,
  tool: string,
  args: Record<string, unknown>,
  signal?: AbortSignal,
): Promise<ToolResult> {
  switch (tool) {
    case "filesystem.read": {
      const path = args.path as string;
      return { ok: true, output: clip(workspace.read(path)) };
    }
    case "filesystem.find": {
      const rel = args.path as string;
      const pattern = (args.pattern as string | undefined) ?? "**";
      const files = workspace.listFiles(rel).filter((p) => {
        const base = p.split("/").pop() ?? p;
        return globMatch(pattern, p) || (!pattern.includes("/") && globMatch(pattern, base));
      });
      return { ok: true, output: files.length === 0 ? "(no matches)" : files.join("\n") };
    }
    case "filesystem.search": {
      const rel = args.path as string;
      const query = (args.query as string).toLowerCase();
      const lines: string[] = [];
      for (const file of workspace.listFiles(rel)) {
        if (lines.length >= SEARCH_LINE_LIMIT) break;
        const content = workspace.read(file);
        content.split("\n").forEach((line, i) => {
          if (lines.length < SEARCH_LINE_LIMIT && line.toLowerCase().includes(query)) {
            lines.push(`${file}:${i + 1}:${line}`);
          }
        });
      }
      return { ok: true, output: lines.length === 0 ? "(no matches)" : lines.join("\n") };
    }
    case "shell.exec": {
      const command = args.command as string;
      const res = await workspace.execShell(command, SHELL_TIMEOUT_MS, signal);
      const output = [res.stdout, res.stderr].filter((s) => s.trim() !== "").join("\n");
      if (res.timedOut) {
        return { ok: false, error: `command timed out after ${SHELL_TIMEOUT_MS}ms`, error_code: "timeout" };
      }
      if (res.code !== 0) {
        return { ok: false, error: clip(output || `exit code ${res.code}`), error_code: "exec_failed" };
      }
      return { ok: true, output: clip(output) };
    }
    case "editor.apply_patch": {
      const path = args.path as string;
      try {
        const add = args.add === true;
        const old = (args.old as string | undefined) ?? "";
        const next = (args.new as string | undefined) ?? "";
        const out = workspace.applyPatch(path, old, next, add);
        return { ok: true, output: out.detail };
      } catch (err) {
        const error = err as Error;
        return {
          ok: false,
          error: error.message,
          error_code: "patch_failed",
        };
      }
    }
    case "task.update_plan": {
      const plan = args.plan as string[];
      if (!plan.every((p) => typeof p === "string" && p.length > 0)) {
        return { ok: false, error: "plan entries must be non-empty strings", error_code: "invalid_args" };
      }
      return { ok: true, output: `plan updated (${plan.length} items)` };
    }
    case "browser.search":
    case "browser.open":
    case "browser.find":
      return {
        ok: false,
        error: `${tool} is not wired in the baseline adapters; m04 supplies fake backends`,
        error_code: "unavailable",
      };
    default:
      return { ok: false, error: `unknown tool ${tool}`, error_code: "invalid_args" };
  }
}

/** Validates tool arguments through the shared schema (single source). */
export function validateCanonicalToolArgs(
  tool: string,
  args: Record<string, unknown>,
): { valid: boolean; reason?: string } {
  return validateToolArgs(tool, args);
}
