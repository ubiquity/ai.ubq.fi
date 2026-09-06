/**
 * Canonical tool backend contracts and workspace boundaries (plan m04).
 *
 * The router (`router.ts`) owns tool semantics; actual I/O lives behind these
 * injected interfaces so harnesses stay deterministic and testable offline:
 *
 * - {@link WorkspaceBackend}: the disposable workspace (read/list/write/patch/
 *   shell) with write-scope enforcement (m02 task manifests).
 * - {@link BrowserBackend}: browser search/open/find (a real browser daemon is
 *   replaceable; the deterministic offline fake lives in `fakes.ts`).
 * - {@link PlanBackend}: in-memory task plan state (a per-run scratch store in
 *   real harnesses).
 *
 * Path boundaries are enforced here: {@link normalizeWorkspacePath} rejects
 * absolute paths and `..` traversal, and backends resolve every path below
 * their workspace root. Backends throw `ToolExecutionError` (from
 * `result.ts`) with the canonical error codes; the router converts them to
 * {@link ToolResult} envelopes and never lets backend failures escape as
 * exceptions.
 */

/** Result of one shell execution. */
export interface ShellExecResult {
  exit_code: number;
  stdout: string;
  stderr: string;
  timed_out: boolean;
  /** Present when the command was rolled back for violating task write scope. */
  error_code?: "write_scope";
}

/** Workspace-relative file operations and shell execution. */
export interface WorkspaceBackend {
  /** Stable label for error messages, e.g. `fixture-workspace` or `fake-workspace`. */
  readonly label: string;
  /** Read a file. Throws ToolExecutionError `not_found` when it does not exist. */
  read(rel: string): string;
  /** Sorted workspace-relative paths of the files under a directory. */
  listFiles(rel: string): string[];
  /** True when the path is inside the workspace and matches the write scope. */
  isAllowedWrite(rel: string): boolean;
  /** Human-readable write scope description, e.g. `**, !protected/**`. */
  describeWriteScope(): string;
  /** Write a file. Throws ToolExecutionError `write_scope` when out of scope. */
  write(rel: string, content: string): void;
  /**
   * Apply a patch (replace the unique occurrence of `old` with `new`, or
   * create the file when `add` is true). Throws ToolExecutionError
   * `write_scope` or `patch_failed`.
   */
  applyPatch(
    rel: string,
    patch: Readonly<{ old: string; new: string; add: boolean }>,
  ): Readonly<{ applied: true; detail: string }>;
  /** Run a command with `sh -c` inside the workspace. */
  execShell(command: string, opts: Readonly<{ timeoutMs: number; signal?: AbortSignal }>): Promise<ShellExecResult>;
}

/** One deterministic web search result. */
export interface BrowserSearchResult {
  title: string;
  url: string;
  snippet: string;
}

/** One opened page. */
export interface BrowserPage {
  url: string;
  title: string;
  content: string;
}

/** One text match inside the current page. */
export interface BrowserLineMatch {
  line: number;
  text: string;
}

/** Browser state and navigation; backends own the current-page state. */
export interface BrowserBackend {
  /** Search the web. Returns an empty list when nothing matches. */
  search(query: string): readonly BrowserSearchResult[];
  /** Open a page (making it the current page). Throws `not_found` when unknown. */
  open(url: string): BrowserPage;
  /** Search the current page. Throws `invalid_args` when no page is open. */
  findOnCurrentPage(query: string): Readonly<{ url: string; matches: readonly BrowserLineMatch[] }>;
}

/** Task plan state. */
export interface PlanBackend {
  /** Replace the plan; returns the number of stored items. */
  update(plan: readonly string[]): Readonly<{ items: number }>;
}

/** The injected dependencies for one canonical tool dispatch. */
export interface ToolBackends {
  readonly workspace: WorkspaceBackend;
  /** Absent when the harness runs without browser support. */
  readonly browser?: BrowserBackend;
  /** Absent when the harness runs without plan tracking. */
  readonly plan?: PlanBackend;
}

/**
 * Normalizes a workspace-relative path and rejects traversal.
 *
 * Returns the normalized relative path ("" for the workspace root, e.g. "."),
 * or null when the path is empty, absolute, or contains a `..` segment.
 */
export const normalizeWorkspacePath = (rel: string): string | null => {
  if (typeof rel !== "string" || rel.length === 0 || rel.startsWith("/")) return null;
  const parts: string[] = [];
  for (const segment of rel.split("/")) {
    if (segment === "" || segment === ".") continue;
    if (segment === "..") return null;
    parts.push(segment);
  }
  return parts.join("/");
};

/** True when the path could traverse outside the workspace root. */
export const workspacePathEscapes = (rel: string): boolean => normalizeWorkspacePath(rel) === null;

const patternToRegExp = (pattern: string): RegExp => {
  let re = "^";
  let i = 0;
  while (i < pattern.length) {
    const c = pattern[i];
    if (c === "*") {
      if (pattern[i + 1] === "*") {
        // `**` matches across slashes; collapse `**/` or trailing `**`.
        if (pattern[i + 2] === "/") {
          re += "(?:.*/)?";
          i += 3;
        } else {
          re += ".*";
          i += 2;
        }
      } else {
        re += "[^/]*";
        i += 1;
      }
    } else if (c === "?") {
      re += "[^/]";
      i += 1;
    } else if (c === "/") {
      re += "/";
      i += 1;
    } else {
      re += c.replace(/[.+^${}()|[\]\\]/g, "\\$&");
      i += 1;
    }
  }
  re += "$";
  return new RegExp(re);
};

/**
 * Glob matcher with the benchmark fixture semantics: `*` (one segment),
 * `**` (across segments), `?` (single character).
 */
export const globMatch = (pattern: string, path: string): boolean => patternToRegExp(pattern).test(path);
