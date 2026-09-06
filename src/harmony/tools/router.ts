/**
 * Canonical tool router (plan m04).
 *
 * {@link runTool} is the single dispatch point for the compact tool surface:
 * it validates arguments, enforces path and write boundaries, dispatches to
 * the injected backends, formats deterministic model-facing text, and returns
 * one machine-readable {@link ToolResult} envelope. Tool-level failures never
 * throw — they are envelopes — so harnesses can record and classify them.
 *
 * Formatting rules (stable, asserted by focused tests):
 * - filesystem.read returns the file content (clipped).
 * - filesystem.find returns workspace-relative paths, one per line;
 *   `(no matches)` when nothing matches; patterns match the full path or the
 *   basename when the pattern has no `/`.
 * - filesystem.search returns `path:line:content` lines (case-insensitive
 *   substring, at most 200 lines); `(no matches)` when nothing matches.
 * - browser.search returns `- title` / `  url` / `  snippet` blocks;
 *   `(no results)` when nothing matches.
 * - browser.open returns `title`, the URL, then the page content.
 * - browser.find returns `line:content` lines; `(no matches)` when none.
 * - shell.exec merges stdout and stderr for `output` and always exposes
 *   `exit_code`/`stdout`/`stderr`; non-zero exits are `exec_failed` failures,
 *   timeouts are `timeout` failures.
 * - editor.apply_patch returns the patch detail (`created <path>` or
 *   `patched <path>`).
 * - task.update_plan returns `plan updated (<n> items)`.
 */

import { globMatch, normalizeWorkspacePath, type ToolBackends, type WorkspaceBackend } from "./backend.ts";
import {
  clipToolText,
  SEARCH_LINE_LIMIT,
  SHELL_DEFAULT_TIMEOUT_MS,
  TOOL_OUTPUT_LIMIT,
  ToolExecutionError,
  toolFailure,
  type ToolResult,
} from "./result.ts";
import { validateToolArguments } from "./schemas.ts";

/** Per-dispatch options; all bounds have stable defaults. */
export interface RunToolOptions {
  /** Whole-run abort signal (also aborts in-flight shell commands). */
  signal?: AbortSignal;
  /** Shell command time bound; default 20s. */
  shellTimeoutMs?: number;
  /** Output limit per envelope field; default 8000 chars. */
  outputLimit?: number;
  /** Search/find line limit; default 200 lines. */
  searchLineLimit?: number;
}

const pathError = (rel: string): ToolResult => toolFailure("path_escape", `path escapes workspace root: ${rel}`);

const requiredPath = (args: Readonly<Record<string, unknown>>): string | null =>
  normalizeWorkspacePath(String(args.path));

const readFile = (
  backend: WorkspaceBackend,
  args: Readonly<Record<string, unknown>>,
  outputLimit: number,
): ToolResult => {
  const rel = requiredPath(args);
  if (rel === null) return pathError(String(args.path));
  return { ok: true, output: clipToolText(backend.read(rel), outputLimit) };
};

const findFiles = (backend: WorkspaceBackend, args: Readonly<Record<string, unknown>>): ToolResult => {
  const rel = requiredPath(args);
  if (rel === null) return pathError(String(args.path));
  const pattern = String(args.pattern ?? "**");
  const files = backend.listFiles(rel).filter((path) => {
    const base = path.split("/").pop() ?? path;
    return globMatch(pattern, path) || (!pattern.includes("/") && globMatch(pattern, base));
  });
  return { ok: true, output: files.length === 0 ? "(no matches)" : files.join("\n") };
};

const searchFiles = (
  backend: WorkspaceBackend,
  args: Readonly<Record<string, unknown>>,
  lineLimit: number,
): ToolResult => {
  const rel = requiredPath(args);
  if (rel === null) return pathError(String(args.path));
  const query = String(args.query).toLowerCase();
  const lines: string[] = [];
  for (const file of backend.listFiles(rel)) {
    if (lines.length >= lineLimit) break;
    const content = backend.read(file);
    for (const [index, line] of content.split("\n").entries()) {
      if (lines.length >= lineLimit) break;
      if (line.toLowerCase().includes(query)) lines.push(`${file}:${index + 1}:${line}`);
    }
  }
  return { ok: true, output: lines.length === 0 ? "(no matches)" : lines.join("\n") };
};

const execShell = async (
  backend: WorkspaceBackend,
  args: Readonly<Record<string, unknown>>,
  opts: Readonly<RunToolOptions>,
): Promise<ToolResult> => {
  const command = String(args.command);
  const timeoutMs = opts.shellTimeoutMs ?? SHELL_DEFAULT_TIMEOUT_MS;
  const outputLimit = opts.outputLimit ?? TOOL_OUTPUT_LIMIT;
  const result = await backend.execShell(command, { timeoutMs, signal: opts.signal });
  if (result.timed_out) return toolFailure("timeout", `command timed out after ${timeoutMs}ms`);
  const stdout = clipToolText(result.stdout, outputLimit);
  const stderr = clipToolText(result.stderr, outputLimit);
  // Model-facing output: trimmed non-empty lines joined (raw streams stay in
  // stdout/stderr fields).
  const merged = [result.stdout, result.stderr]
    .map((text) => text.trimEnd())
    .filter((text) => text !== "")
    .join("\n");
  if (result.error_code !== undefined) {
    return {
      ok: false,
      error: clipToolText(merged || result.error_code, outputLimit),
      error_code: result.error_code,
      exit_code: result.exit_code,
      stdout,
      stderr,
    };
  }
  if (result.exit_code !== 0) {
    return {
      ok: false,
      error: clipToolText(merged || `exit code ${result.exit_code}`, outputLimit),
      error_code: "exec_failed",
      exit_code: result.exit_code,
      stdout,
      stderr,
    };
  }
  return { ok: true, output: clipToolText(merged, outputLimit), exit_code: result.exit_code, stdout, stderr };
};

const applyPatch = (backend: WorkspaceBackend, args: Readonly<Record<string, unknown>>): ToolResult => {
  const rel = requiredPath(args);
  if (rel === null) return pathError(String(args.path));
  if (!backend.isAllowedWrite(rel)) {
    return toolFailure(
      "write_scope",
      `write scope violation: ${rel} is not writable (scope: ${backend.describeWriteScope()})`,
    );
  }
  const add = args.add === true;
  const old = typeof args.old === "string" ? args.old : "";
  const next = typeof args.new === "string" ? args.new : "";
  const applied = backend.applyPatch(rel, { old, new: next, add });
  return { ok: true, output: applied.detail };
};

const updatePlan = (backends: ToolBackends, args: Readonly<Record<string, unknown>>): ToolResult => {
  const planBackend = backends.plan;
  if (planBackend === undefined) {
    return toolFailure("unavailable", "task.update_plan is not available in this configuration");
  }
  const stored = planBackend.update(args.plan as string[]);
  return { ok: true, output: `plan updated (${stored.items} items)` };
};

const browserSearch = (
  backends: ToolBackends,
  args: Readonly<Record<string, unknown>>,
  outputLimit: number,
): ToolResult => {
  const browser = backends.browser;
  if (browser === undefined) {
    return toolFailure("unavailable", "browser.search is not available in this configuration");
  }
  const results = browser.search(String(args.query));
  if (results.length === 0) return { ok: true, output: "(no results)" };
  const text = results.map((result) => `- ${result.title}\n  ${result.url}\n  ${result.snippet}`).join("\n");
  return { ok: true, output: clipToolText(text, outputLimit) };
};

const browserOpen = (
  backends: ToolBackends,
  args: Readonly<Record<string, unknown>>,
  outputLimit: number,
): ToolResult => {
  const browser = backends.browser;
  if (browser === undefined) {
    return toolFailure("unavailable", "browser.open is not available in this configuration");
  }
  const page = browser.open(String(args.url));
  const text = `${page.title}\n${page.url}\n\n${page.content}`;
  return { ok: true, output: clipToolText(text, outputLimit) };
};

const browserFind = (backends: ToolBackends, args: Readonly<Record<string, unknown>>): ToolResult => {
  const browser = backends.browser;
  if (browser === undefined) {
    return toolFailure("unavailable", "browser.find is not available in this configuration");
  }
  const { matches } = browser.findOnCurrentPage(String(args.query));
  if (matches.length === 0) return { ok: true, output: "(no matches)" };
  return { ok: true, output: matches.map((match) => `${match.line}:${match.text}`).join("\n") };
};

/**
 * Routes one canonical tool call to its backend and returns a machine-readable
 * result envelope. Never throws for tool-level failures; unexpected backend
 * exceptions become `internal` envelopes.
 */
export const runTool = async (
  backends: ToolBackends,
  tool: string,
  args: unknown,
  opts: RunToolOptions = {},
): Promise<ToolResult> => {
  const outputLimit = opts.outputLimit ?? TOOL_OUTPUT_LIMIT;
  const validated = validateToolArguments(tool, args);
  if (!validated.valid) return toolFailure("invalid_args", `invalid arguments: ${validated.reason}`);

  try {
    switch (tool) {
      case "filesystem.read":
        return readFile(backends.workspace, validated.arguments, outputLimit);
      case "filesystem.find":
        return findFiles(backends.workspace, validated.arguments);
      case "filesystem.search":
        return searchFiles(backends.workspace, validated.arguments, opts.searchLineLimit ?? SEARCH_LINE_LIMIT);
      case "shell.exec":
        return await execShell(backends.workspace, validated.arguments, opts);
      case "editor.apply_patch":
        return applyPatch(backends.workspace, validated.arguments);
      case "task.update_plan":
        return updatePlan(backends, validated.arguments);
      case "browser.search":
        return browserSearch(backends, validated.arguments, outputLimit);
      case "browser.open":
        return browserOpen(backends, validated.arguments, outputLimit);
      case "browser.find":
        return browserFind(backends, validated.arguments);
      default:
        return toolFailure("invalid_args", `unknown tool ${JSON.stringify(tool)}`);
    }
  } catch (err) {
    if (err instanceof ToolExecutionError) return toolFailure(err.code, err.message);
    const message = err instanceof Error && err.message.length > 0 ? err.message : String(err);
    return toolFailure("internal", `internal error: ${message}`);
  }
};
