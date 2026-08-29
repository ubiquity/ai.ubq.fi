/**
 * Canonical tool result envelope (plan m04).
 *
 * Every tool routed by `router.ts` returns one {@link ToolResult} — a
 * machine-readable envelope with a closed set of {@link ToolErrorCode}s, so
 * harnesses (m05 reliability detection, benchmark adapters) can classify
 * failures without parsing prose. The envelope is a structural superset of the
 * benchmark trajectory's `tool_result` shape: `ok`, `output`, `error`,
 * `error_code`, plus shell-specific `exit_code`/`stdout`/`stderr`.
 *
 * Limits keep every result bounded regardless of backend content.
 */

/** Closed set of machine-readable tool error codes. */
export type ToolErrorCode =
  | "invalid_args" // schema or tool-level argument rejection
  | "path_escape" // path resolved outside the workspace root
  | "write_scope" // write outside the task's allowed write scope
  | "not_found" // file, directory or page does not exist
  | "exec_failed" // shell command exited with a non-zero code
  | "timeout" // shell command exceeded its time bound
  | "patch_failed" // patch could not be applied (missing/ambiguous text, exists)
  | "unavailable" // no backend is injected for this tool
  | "internal"; // unexpected backend failure

/** One canonical tool result. */
export interface ToolResult {
  ok: boolean;
  /** Model-facing text on success (clipped to the output limit). */
  output?: string;
  /** Model-facing error text on failure (clipped to the output limit). */
  error?: string;
  /** Machine-readable error code; present exactly when `ok` is false. */
  error_code?: ToolErrorCode;
  /** shell.exec: the command's exit code (absent on timeout). */
  exit_code?: number;
  /** shell.exec: captured standard output (clipped). */
  stdout?: string;
  /** shell.exec: captured standard error (clipped). */
  stderr?: string;
}

/** Maximum characters of any single result output/error field. */
export const TOOL_OUTPUT_LIMIT = 8_000;
/** Maximum lines returned by a single filesystem/browser search or find. */
export const SEARCH_LINE_LIMIT = 200;
/** Default shell command time bound in milliseconds. */
export const SHELL_DEFAULT_TIMEOUT_MS = 20_000;

/** Clips long tool text with a stable, indexable suffix. */
export const clipToolText = (text: string, limit: number = TOOL_OUTPUT_LIMIT): string =>
  text.length > limit ? `${text.slice(0, limit)}…[truncated]` : text;

/** Builds a failure envelope. */
export const toolFailure = (error_code: ToolErrorCode, error: string): ToolResult => ({
  ok: false,
  error,
  error_code,
});

/** Backend failure carrying a canonical, machine-readable code. */
export class ToolExecutionError extends Error {
  readonly code: ToolErrorCode;

  constructor(code: ToolErrorCode, message: string) {
    super(message);
    this.name = "ToolExecutionError";
    this.code = code;
  }
}
