/**
 * Deterministic fake tool backends (plan m04).
 *
 * These fakes are the offline stand-ins for the real infrastructure routing:
 * no network, no browser daemon, no live shell, no filesystem access outside
 * the injected maps. They are the guarantee that the canonical tool surface
 * (schema validation, envelopes, path/write boundaries, command exit/
 * stdout/stderr semantics) is testable deterministically.
 *
 * - {@link FakeWorkspaceBackend}: an in-memory file tree plus a scripted
 *   {@link FakeShell} and write-scope enforcement.
 * - {@link FakeBrowserBackend}: a page map, a search index and current-page
 *   state (the state a real browser daemon would own).
 * - {@link FakePlanBackend}: an in-memory ordered plan.
 *
 * {@link createFakeToolBackends} assembles a complete {@link ToolBackends}
 * with a small built-in site map so harnesses get deterministic behavior out
 * of the box (the benchmark reference adapter relies on this).
 */

import {
  type BrowserBackend,
  type BrowserLineMatch,
  type BrowserPage,
  type BrowserSearchResult,
  globMatch,
  normalizeWorkspacePath,
  type PlanBackend,
  type ShellExecResult,
  type ToolBackends,
  type WorkspaceBackend,
} from "./backend.ts";
import { ToolExecutionError } from "./result.ts";

// ---------------------------------------------------------------------------
// Fake shell
// ---------------------------------------------------------------------------

/** One scripted shell response. `command` matches exactly or by RegExp. */
export interface FakeShellEntry {
  command: string | RegExp;
  exit_code?: number;
  stdout?: string;
  stderr?: string;
  /** Simulate a time-out instead of returning an exit code. */
  timed_out?: boolean;
}

/** Deterministic scripted shell: first matching entry wins, unknown falls back. */
export class FakeShell {
  #fallback: Readonly<Omit<FakeShellEntry, "command">>;

  constructor(
    readonly entries: readonly FakeShellEntry[] = [],
    fallback?: Readonly<Omit<FakeShellEntry, "command">>,
  ) {
    this.#fallback = fallback ?? { exit_code: 127 };
  }

  exec(command: string): ShellExecResult {
    for (const entry of this.entries) {
      const matched = typeof entry.command === "string" ? entry.command === command : entry.command.test(command);
      if (!matched) continue;
      if (entry.timed_out === true) return { exit_code: -1, stdout: "", stderr: "", timed_out: true };
      return {
        exit_code: entry.exit_code ?? 0,
        stdout: entry.stdout ?? "",
        stderr: entry.stderr ?? "",
        timed_out: false,
      };
    }
    const fallback = this.#fallback;
    return {
      exit_code: fallback.exit_code ?? 127,
      stdout: fallback.stdout ?? "",
      stderr: fallback.stderr ?? `fake shell: no scripted response for ${JSON.stringify(command)}`,
      timed_out: false,
    };
  }
}

// ---------------------------------------------------------------------------
// Fake workspace
// ---------------------------------------------------------------------------

export interface FakeWorkspaceOptions {
  /** Label used in error messages; default `fake-workspace`. */
  label?: string;
  /** Initial file tree, keyed by workspace-relative path. */
  files?: Readonly<Record<string, string>>;
  /** Allowed write scope globs (`!` negates); default `["**"]`. */
  writeScope?: readonly string[];
  /** Scripted shell; default rejects every command with exit 127. */
  shell?: FakeShell;
}

/** In-memory workspace backend with scripted shell and write-scope checks. */
export class FakeWorkspaceBackend implements WorkspaceBackend {
  readonly label: string;
  readonly #files: Map<string, string>;
  readonly #writeScope: readonly string[];
  readonly #shell: FakeShell;

  constructor(opts: FakeWorkspaceOptions = {}) {
    this.label = opts.label ?? "fake-workspace";
    this.#files = new Map(Object.entries(opts.files ?? {}));
    this.#writeScope = opts.writeScope ?? ["**"];
    this.#shell = opts.shell ?? new FakeShell();
  }

  read(rel: string): string {
    const path = this.#resolve(rel);
    const content = this.#files.get(path);
    if (content === undefined) throw new ToolExecutionError("not_found", `file not found: ${path}`);
    return content;
  }

  listFiles(rel: string): string[] {
    const path = this.#resolve(rel);
    const prefix = path === "" ? "" : `${path}/`;
    return [...this.#files.keys()].filter((file) => file.startsWith(prefix)).sort();
  }

  isAllowedWrite(rel: string): boolean {
    const path = normalizeWorkspacePath(rel);
    if (path === null) return false;
    let allowed = false;
    for (const pattern of this.#writeScope) {
      if (globMatch(pattern.replace(/^!/, ""), path)) allowed = !pattern.startsWith("!");
    }
    return allowed;
  }

  describeWriteScope(): string {
    return this.#writeScope.join(", ");
  }

  write(rel: string, content: string): void {
    const path = this.#resolve(rel);
    if (!this.isAllowedWrite(path)) {
      throw new ToolExecutionError(
        "write_scope",
        `write scope violation: ${path} is not writable (scope: ${this.describeWriteScope()})`,
      );
    }
    this.#files.set(path, content);
  }

  applyPatch(
    rel: string,
    patch: Readonly<{ old: string; new: string; add: boolean }>,
  ): Readonly<{ applied: true; detail: string }> {
    const path = this.#resolve(rel);
    if (!this.isAllowedWrite(path)) {
      throw new ToolExecutionError(
        "write_scope",
        `write scope violation: ${path} is not writable (scope: ${this.describeWriteScope()})`,
      );
    }
    if (patch.add) {
      if (this.#files.has(path)) {
        throw new ToolExecutionError("patch_failed", `patch add refused: ${path} already exists`);
      }
      this.#files.set(path, patch.new);
      return { applied: true, detail: `created ${path}` };
    }
    const content = this.#files.get(path);
    if (content === undefined) {
      throw new ToolExecutionError("patch_failed", `patch failed: ${path} does not exist`);
    }
    if (patch.old === "") {
      // Empty `old` prepends `new` at the start of the file (an empty string
      // would otherwise "occur" at every offset and always be ambiguous).
      this.#files.set(path, patch.new + content);
      return { applied: true, detail: `patched ${path}` };
    }
    const first = content.indexOf(patch.old);
    if (first === -1) {
      throw new ToolExecutionError("patch_failed", `patch failed: ${path} does not contain the expected old text`);
    }
    if (content.indexOf(patch.old, first + 1) !== -1) {
      throw new ToolExecutionError("patch_failed", `patch failed: old text occurs more than once in ${path}`);
    }
    this.#files.set(path, content.slice(0, first) + patch.new + content.slice(first + patch.old.length));
    return { applied: true, detail: `patched ${path}` };
  }

  execShell(
    command: string,
    _opts: Readonly<{ timeoutMs: number; signal?: AbortSignal }>,
  ): Promise<ShellExecResult> {
    return Promise.resolve(this.#shell.exec(command));
  }

  #resolve(rel: string): string {
    if (rel === "") return ""; // normalized workspace root
    const path = normalizeWorkspacePath(rel);
    if (path === null) throw new ToolExecutionError("path_escape", `path escapes workspace root: ${rel}`);
    return path;
  }
}

// ---------------------------------------------------------------------------
// Fake browser
// ---------------------------------------------------------------------------

export interface FakeBrowserSearchEntry {
  /** Exact query, matched case-insensitively. */
  query: string;
  results: readonly BrowserSearchResult[];
}

export interface FakeBrowserOptions {
  /** Pages reachable through browser.open, keyed by URL. */
  pages?: Readonly<Record<string, BrowserPage>>;
  /** Search index entries, matched case-insensitively by exact query. */
  search?: readonly FakeBrowserSearchEntry[];
  /** Results returned for any query without an index entry. */
  searchFallback?: readonly BrowserSearchResult[];
}

/** Deterministic offline browser: page map, search index, current-page state. */
export class FakeBrowserBackend implements BrowserBackend {
  readonly #pages: Map<string, BrowserPage>;
  readonly #searchIndex: Map<string, readonly BrowserSearchResult[]>;
  readonly #searchFallback: readonly BrowserSearchResult[];
  #currentUrl: string | null = null;

  constructor(opts: FakeBrowserOptions = {}) {
    this.#pages = new Map(Object.entries(opts.pages ?? {}));
    this.#searchIndex = new Map((opts.search ?? []).map((entry) => [entry.query.toLowerCase(), entry.results]));
    this.#searchFallback = opts.searchFallback ?? [];
  }

  search(query: string): readonly BrowserSearchResult[] {
    return this.#searchIndex.get(query.toLowerCase()) ?? this.#searchFallback;
  }

  open(url: string): BrowserPage {
    const page = this.#pages.get(url);
    if (page === undefined) throw new ToolExecutionError("not_found", `no page at ${url}`);
    this.#currentUrl = url;
    return page;
  }

  findOnCurrentPage(query: string): Readonly<{ url: string; matches: readonly BrowserLineMatch[] }> {
    if (this.#currentUrl === null) {
      throw new ToolExecutionError("invalid_args", "no page open; call browser.open first");
    }
    const page = this.#pages.get(this.#currentUrl);
    if (page === undefined) throw new ToolExecutionError("not_found", `no page at ${this.#currentUrl}`);
    const needle = query.toLowerCase();
    const matches: BrowserLineMatch[] = [];
    for (const [index, line] of page.content.split("\n").entries()) {
      if (line.toLowerCase().includes(needle)) matches.push({ line: index + 1, text: line });
    }
    return { url: this.#currentUrl, matches };
  }
}

// ---------------------------------------------------------------------------
// Fake plan
// ---------------------------------------------------------------------------

/** In-memory ordered task plan state. */
export class FakePlanBackend implements PlanBackend {
  #plan: string[] = [];

  update(plan: readonly string[]): Readonly<{ items: number }> {
    this.#plan = [...plan];
    return { items: this.#plan.length };
  }

  current(): readonly string[] {
    return [...this.#plan];
  }
}

// ---------------------------------------------------------------------------
// Defaults and assembly
// ---------------------------------------------------------------------------

/** Built-in deterministic site map used by `createFakeToolBackends`. */
export const FAKE_BROWSER_PAGES: Readonly<Record<string, BrowserPage>> = {
  "https://example.com/index.html": {
    url: "https://example.com/index.html",
    title: "Example",
    content: "Welcome to the example site.\nFind content on this page.\n",
  },
};

/** Built-in deterministic search index. */
export const FAKE_BROWSER_SEARCH: readonly FakeBrowserSearchEntry[] = [
  {
    query: "example",
    results: [
      {
        title: "Example",
        url: "https://example.com/index.html",
        snippet: "Welcome to the example site.",
      },
    ],
  },
];

export interface FakeToolBackendsOptions {
  files?: Readonly<Record<string, string>>;
  writeScope?: readonly string[];
  shell?: FakeShell;
  browserPages?: Readonly<Record<string, BrowserPage>>;
  browserSearch?: readonly FakeBrowserSearchEntry[];
  browserSearchFallback?: readonly BrowserSearchResult[];
}

/** Assembles a complete deterministic ToolBackends with default fake data. */
export const createFakeToolBackends = (opts: FakeToolBackendsOptions = {}): ToolBackends => ({
  workspace: new FakeWorkspaceBackend({
    files: opts.files,
    writeScope: opts.writeScope,
    shell: opts.shell,
  }),
  browser: new FakeBrowserBackend({
    pages: opts.browserPages ?? FAKE_BROWSER_PAGES,
    search: opts.browserSearch ?? FAKE_BROWSER_SEARCH,
    searchFallback: opts.browserSearchFallback,
  }),
  plan: new FakePlanBackend(),
});
