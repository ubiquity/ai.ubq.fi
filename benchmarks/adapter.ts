/**
 * Benchmark adapter contract and the deterministic `reference` adapter.
 *
 * Adapters A/B/C/D (m03) implement {@link BenchmarkAdapter}. Each adapter is
 * handed a {@link AdapterRunContext}: the task manifest, a disposable
 * workspace, an event-recording sink, and a tool-call limit check. Adapters
 * record their own trajectory events; the runner derives metrics, runs
 * verification, evaluates oracles, and writes the result record.
 *
 * The built-in `reference` adapter executes the task's `scripted_trail`
 * against the tool layer below. It never calls an external model, so the
 * default benchmark commands are fully hermetic and reproducible.
 *
 * Tool surface (provisional, aligned with the canonical names from the plan):
 * filesystem.read/find/search, shell.exec, editor.apply_patch,
 * task.update_plan, browser.search/open/find. m04 owns the final canonical
 * schemas; the browser tools return a deterministic "unavailable" error here
 * until fake backends land with m04.
 */

import { FixtureWorkspace, globMatch, WriteScopeViolationError } from "./fixture.ts";
import { TaskManifest, TrailStep, TrajectoryEvent } from "./schemas.ts";

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

export class TrailMismatchError extends Error {
  constructor(readonly stepIndex: number, readonly step: TrailStep, readonly result: ToolResult) {
    super(
      `trail step ${stepIndex + 1} (${step.tool}): expected ${describeExpectation(step)}, got ok=${result.ok} ${
        result.error ?? ""
      }`.trim(),
    );
    this.name = "TrailMismatchError";
  }
}

export class ToolCallLimitExceededError extends Error {
  constructor(readonly maxToolCalls: number) {
    super(`tool call limit exceeded: max_tool_calls=${maxToolCalls}`);
    this.name = "ToolCallLimitExceededError";
  }
}

export class TaskTimeoutError extends Error {
  constructor(readonly timeoutMs: number) {
    super(`task timed out after ${timeoutMs}ms`);
    this.name = "TaskTimeoutError";
  }
}

function describeExpectation(step: TrailStep): string {
  const exp = step.expect;
  if (!exp) return "no assertion";
  const parts: string[] = [];
  const expectedOk = exp.ok ?? (exp.error_contains === undefined);
  parts.push(`ok=${expectedOk}`);
  for (const s of exp.output_contains ?? []) parts.push(`output contains ${JSON.stringify(s)}`);
  if (exp.error_contains !== undefined) parts.push(`error contains ${JSON.stringify(exp.error_contains)}`);
  return parts.join(", ");
}

// ---------------------------------------------------------------------------
// Adapter contract
// ---------------------------------------------------------------------------

export interface AdapterRunContext {
  runId: string;
  task: TaskManifest;
  workspace: FixtureWorkspace;
  /** Append a validated trajectory event. */
  record(event: TrajectoryEvent): void;
  /** Throw when the run exceeded the declared max_tool_calls. */
  checkToolLimit(): void;
  /** Aborts when the whole-run timeout fired. */
  signal: AbortSignal;
  time(): string;
}

export interface BenchmarkAdapter {
  /** Stable config id used in result records (A, B, C, D, reference, ...). */
  configId: string;
  name: string;
  description: string;
  /**
   * True when the adapter issues model calls. The runner refuses to run
   * such adapters until an approved external-inference gate exists; the
   * hermetic default (reference) never needs one.
   */
  requiresExternalInference: boolean;
  run(ctx: AdapterRunContext): Promise<void>;
}

// ---------------------------------------------------------------------------
// Tool layer
// ---------------------------------------------------------------------------

export interface ToolResult {
  ok: boolean;
  output?: string;
  error?: string;
  error_code?: string;
}

interface ToolSchema {
  required: string[];
  types: Record<string, "string" | "boolean" | "string[]">;
}

export const TOOL_SCHEMAS: Record<string, ToolSchema> = {
  "filesystem.read": { required: ["path"], types: { path: "string" } },
  "filesystem.find": { required: ["path"], types: { path: "string", pattern: "string" } },
  "filesystem.search": { required: ["path", "query"], types: { path: "string", query: "string" } },
  "shell.exec": { required: ["command"], types: { command: "string" } },
  "editor.apply_patch": { required: ["path"], types: { path: "string", old: "string", new: "string", add: "boolean" } },
  "task.update_plan": { required: ["plan"], types: { plan: "string[]" } },
  "browser.search": { required: ["query"], types: { query: "string" } },
  "browser.open": { required: ["url"], types: { url: "string" } },
  "browser.find": { required: ["query"], types: { query: "string" } },
};

export const CANONICAL_TOOL_NAMES = Object.keys(TOOL_SCHEMAS).sort();

/** Validate tool arguments against the provisional schema. */
export function validateToolArgs(tool: string, args: Record<string, unknown>): { valid: boolean; reason?: string } {
  const schema = TOOL_SCHEMAS[tool];
  if (!schema) return { valid: false, reason: `unknown tool ${tool}` };
  for (const key of Object.keys(args)) {
    if (!(key in schema.types)) return { valid: false, reason: `unexpected argument ${JSON.stringify(key)}` };
  }
  for (const key of schema.required) {
    if (!(key in args) || args[key] === undefined) {
      return { valid: false, reason: `missing required argument ${JSON.stringify(key)}` };
    }
    const expected = schema.types[key];
    const actual = args[key];
    const ok = expected === "string"
      ? typeof actual === "string"
      : expected === "boolean"
      ? typeof actual === "boolean"
      : Array.isArray(actual) && actual.every((x) => typeof x === "string");
    if (!ok) return { valid: false, reason: `argument ${JSON.stringify(key)} must be ${expected}` };
  }
  return { valid: true };
}

const SEARCH_LINE_LIMIT = 200;
const SHELL_TIMEOUT_MS = 20_000;
const OUTPUT_LIMIT = 8000;

function clip(s: string): string {
  return s.length > OUTPUT_LIMIT ? `${s.slice(0, OUTPUT_LIMIT)}…[truncated]` : s;
}

async function executeTool(
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
        if (err instanceof WriteScopeViolationError) {
          return { ok: false, error: err.message, error_code: "write_scope" };
        }
        return { ok: false, error: (err as Error).message, error_code: "patch_failed" };
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
        error: `${tool} is not wired in the reference adapter; m04 supplies fake backends`,
        error_code: "unavailable",
      };
    default:
      return { ok: false, error: `unknown tool ${tool}`, error_code: "invalid_args" };
  }
}

// ---------------------------------------------------------------------------
// Reference (scripted) adapter
// ---------------------------------------------------------------------------

export const referenceAdapter: BenchmarkAdapter = {
  configId: "reference",
  name: "reference",
  description: "Deterministic scripted-trail executor; records trajectories and never calls an external model.",
  requiresExternalInference: false,
  async run(ctx: AdapterRunContext): Promise<void> {
    const { task } = ctx;
    if (!task.scripted_trail) {
      throw new TrailMismatchError(-1, { tool: "(none)", args: {} } as TrailStep, {
        ok: false,
        error: `task ${task.id} declares no scripted_trail; the reference adapter can only replay recorded trails`,
        error_code: "invalid_args",
      });
    }
    let seq = 0;
    for (let i = 0; i < task.scripted_trail.length; i++) {
      ctx.checkToolLimit();
      if (ctx.signal.aborted) throw new TaskTimeoutError(task.timeout_ms);
      const step = task.scripted_trail[i];
      const id = `t${++seq}`;
      const validated = validateToolArgs(step.tool, step.args);
      ctx.record({
        type: "tool_call",
        at: ctx.time(),
        id,
        tool: step.tool,
        arguments: step.args,
        valid: validated.valid,
        invalid_reason: validated.valid ? undefined : validated.reason,
        is_wrong_tool: step.wrong === true ? true : undefined,
        is_repeated: step.repeat === true ? true : undefined,
      });
      const started = Date.now();
      let result: ToolResult;
      if (!validated.valid) {
        result = { ok: false, error: `invalid arguments: ${validated.reason}`, error_code: "invalid_args" };
      } else if (step.inject) {
        result = { ok: false, error: step.inject.error, error_code: step.inject.error_code ?? "injected_failure" };
      } else if (step.wrong) {
        result = { ok: false, error: "wrong tool for this task step", error_code: "wrong_tool" };
      } else {
        try {
          result = await executeTool(ctx.workspace, step.tool, step.args, ctx.signal);
        } catch (err) {
          result = { ok: false, error: (err as Error).message, error_code: "exec_failed" };
        }
      }
      ctx.record({
        type: "tool_result",
        at: ctx.time(),
        id,
        ok: result.ok,
        output: result.output,
        error: result.error,
        error_code: result.error_code,
        duration_ms: Date.now() - started,
      });
      if (step.expect) {
        const expectedOk = step.expect.ok ?? (step.expect.error_contains === undefined);
        const okMatches = result.ok === expectedOk;
        const outputOk = (step.expect.output_contains ?? []).every((s) => (result.output ?? "").includes(s));
        const errorOk = step.expect.error_contains === undefined
          ? true
          : !result.ok && (result.error ?? "").includes(step.expect.error_contains);
        if (!(okMatches && outputOk && errorOk)) throw new TrailMismatchError(i, step, result);
      }
    }
  },
};

/** Registry consumed by the runner. m03 registers adapters A/B/D here. */
export function defaultAdapters(): BenchmarkAdapter[] {
  return [referenceAdapter];
}
