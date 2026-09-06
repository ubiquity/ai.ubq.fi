/**
 * Disposable fixture workspaces.
 *
 * Every run executes against a private copy of a checked-in fixture snapshot
 * under the git-ignored runs root (see DEFAULT_RUNS_ROOT). The adapter's
 * writes are confined to that copy; write-scope globs declared in the task
 * manifest are enforced at the tool layer, and paths are always resolved
 * inside the workspace root. The canonical repository checkout is never
 * touched by benchmark runs.
 */

import { TaskManifest } from "./schemas.ts";

/** Thrown when the declared fixture_revision does not match the snapshot. */
export class FixtureRevisionMismatchError extends Error {
  constructor(readonly taskId: string, readonly expected: string, readonly actual: string) {
    super(
      `fixture revision mismatch for ${taskId}: manifest declares ${expected}, snapshot is ${actual}` +
        ` (regenerate fixtures or update the manifest)`,
    );
    this.name = "FixtureRevisionMismatchError";
  }
}

async function sha256Hex(data: Uint8Array): Promise<string> {
  const buf = await globalThis.crypto.subtle.digest("SHA-256", data as unknown as BufferSource);
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

/** Content-addressed revision of a fixture snapshot directory. */
export async function computeFixtureRevision(fixtureDir: string): Promise<string> {
  const files: { rel: string; abspath: string }[] = [];
  const walk = (dir: string, rel: string) => {
    const entries = [...Deno.readDirSync(dir)].filter((e) => e.isFile || e.isDirectory).sort((a, b) =>
      a.name.localeCompare(b.name)
    );
    for (const entry of entries) {
      const childRel = rel === "" ? entry.name : `${rel}/${entry.name}`;
      if (entry.isDirectory) walk(`${dir}/${entry.name}`, childRel);
      else files.push({ rel: childRel, abspath: `${dir}/${entry.name}` });
    }
  };
  walk(fixtureDir, "");
  const encoder = new TextEncoder();
  const parts: Uint8Array[] = [encoder.encode("fixture-v1\0")];
  for (const f of files) {
    parts.push(encoder.encode(f.rel + "\0"));
    parts.push(Deno.readFileSync(f.abspath));
  }
  const total = parts.reduce((n, p) => n + p.length, 0);
  const buf = new Uint8Array(total);
  let off = 0;
  for (const p of parts) {
    buf.set(p, off);
    off += p.length;
  }
  return `sha256:${await sha256Hex(buf)}`;
}

/** Glob matcher: `*` (segment), `**` (across segments), `?` (single char). */
export function globMatch(pattern: string, path: string): boolean {
  const rx = patternToRegExp(pattern);
  return rx.test(path);
}

function patternToRegExp(pattern: string): RegExp {
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
}

/** True when the joined path lexically stays inside the root. */
export function pathInside(root: string, rel: string): boolean {
  const rootAbs = normalizeLexically(root.replace(/\/+$/, ""));
  const joined = normalizeLexically(`${rootAbs}/${rel}`);
  return joined === rootAbs || joined.startsWith(rootAbs + "/");
}

function normalizeLexically(p: string): string {
  const parts = p.split("/");
  const out: string[] = [];
  for (const part of parts) {
    if (part === "" || part === ".") continue;
    if (part === "..") {
      out.pop();
      continue;
    }
    out.push(part);
  }
  return (p.startsWith("/") ? "/" : "") + out.join("/");
}

export class WriteScopeViolationError extends Error {
  readonly paths: string[];
  readonly path: string;

  constructor(pathOrPaths: string | readonly string[], readonly scope: string[]) {
    const paths = typeof pathOrPaths === "string" ? [pathOrPaths] : [...pathOrPaths];
    paths.sort(comparePaths);
    const normalizedPaths = [...new Set(paths)];
    const evidence = normalizedPaths.join(", ");
    const verb = normalizedPaths.length === 1 ? "is" : "are";
    super(`write scope violation: ${evidence} ${verb} not writable (scope: ${scope.join(", ")})`);
    this.paths = normalizedPaths;
    this.path = normalizedPaths[0] ?? "";
    this.name = "WriteScopeViolationError";
  }
}

type FixtureEntryKind = "file" | "directory" | "symlink" | "other";

interface FixtureEntryState {
  kind: FixtureEntryKind;
  mode: number | null;
  content?: Uint8Array;
  target?: string;
}

type FixtureTreeState = Map<string, FixtureEntryState>;

export interface FixtureWorkspaceOptions {
  fixtureDir: string;
  runId: string;
  /** Parent directory for disposable workspaces (must be writable, git-ignored). */
  tmpParent: string;
  task: TaskManifest;
}

export class FixtureWorkspace {
  readonly root: string;
  readonly fixtureDir: string;
  readonly task: TaskManifest;
  private prepared = false;

  constructor(opts: FixtureWorkspaceOptions) {
    this.fixtureDir = opts.fixtureDir;
    this.task = opts.task;
    this.root = `${opts.tmpParent}/${opts.runId}`;
  }

  /** True when the path matches the task's allowed_write_scope. */
  isAllowedWrite(rel: string): boolean {
    let allowed = false;
    for (const pattern of this.task.allowed_write_scope) {
      if (globMatch(pattern.replace(/^!/, ""), rel)) allowed = !pattern.startsWith("!");
    }
    return allowed;
  }

  private assertRoot(): void {
    if (!this.prepared) throw new Error("fixture workspace not prepared");
  }

  private assertPath(rel: string): string {
    if (rel === "" || rel.startsWith("/") || rel.split("/").includes("..")) {
      throw new Error(`path escapes workspace root: ${rel}`);
    }
    const abs = `${this.root}/${rel}`;
    if (!pathInside(this.root, rel)) throw new Error(`path escapes workspace root: ${rel}`);
    return abs;
  }

  /** Assemble the disposable working tree and optional git history. */
  async prepare(): Promise<void> {
    if (this.prepared) throw new Error("prepare called twice");
    await Deno.mkdir(this.root, { recursive: true });

    const git = this.task.git;
    if (git?.history && git.history.length > 0) {
      // Initialize a repository rooted at the disposable workspace so git
      // never walks up into the enclosing repository checkout.
      await gitCommand(this.root, ["init", "-q"]);
      for (const snapshot of git.history) {
        const snapDir = `${this.fixtureDir}/${snapshot}`;
        for (const entry of Deno.readDirSync(this.root)) {
          if (entry.name === ".git") continue; // keep the disposable repository
          await Deno.remove(`${this.root}/${entry.name}`, { recursive: true });
        }
        await copyTree(snapDir, this.root);
        await gitCommand(this.root, ["add", "-A"]);
        await gitCommand(this.root, [
          "-c",
          "user.email=benchmark@invalid.invalid",
          "-c",
          "user.name=benchmark",
          "commit",
          "-qm",
          snapshot.split("/").pop() ?? snapshot,
        ]);
      }
    } else {
      await copyTree(this.fixtureDir, this.root);
      if (git?.init) {
        await gitCommand(this.root, ["init", "-q"]);
        await gitCommand(this.root, ["add", "-A"]);
        await gitCommand(this.root, [
          "-c",
          "user.email=benchmark@invalid.invalid",
          "-c",
          "user.name=benchmark",
          "commit",
          "-qm",
          "base",
        ]);
      }
    }
    this.prepared = true;
  }

  /** Delete the disposable working tree. */
  async remove(): Promise<void> {
    await Deno.remove(this.root, { recursive: true });
  }

  read(rel: string): string {
    this.assertRoot();
    return Deno.readTextFileSync(this.assertPath(rel));
  }

  /** Write a file relative to the workspace root; enforces write scope. */
  write(rel: string, content: string): void {
    this.assertRoot();
    if (!this.isAllowedWrite(rel)) throw new WriteScopeViolationError(rel, this.task.allowed_write_scope);
    const abs = this.assertPath(rel);
    Deno.mkdirSync(abs.slice(0, abs.lastIndexOf("/")), { recursive: true });
    Deno.writeTextFileSync(abs, content);
  }

  /**
   * Minimal deterministic patch: replace the first (and only) occurrence of
   * `old` with `new`, or create the file when `add` is true.
   */
  applyPatch(rel: string, old: string, next: string, add: boolean): { applied: boolean; detail: string } {
    this.assertRoot();
    if (!this.isAllowedWrite(rel)) throw new WriteScopeViolationError(rel, this.task.allowed_write_scope);
    const abs = this.assertPath(rel);
    if (add) {
      if (existsSync(abs)) throw new Error(`patch add refused: ${rel} already exists`);
      Deno.mkdirSync(abs.slice(0, abs.lastIndexOf("/")), { recursive: true });
      Deno.writeTextFileSync(abs, next);
      return { applied: true, detail: `created ${rel}` };
    }
    const absStat = statIfExists(abs);
    if (absStat === null || !absStat.isFile) throw new Error(`patch failed: ${rel} does not exist`);
    const content = Deno.readTextFileSync(abs);
    const first = content.indexOf(old);
    if (first === -1) throw new Error(`patch failed: ${rel} does not contain the expected old text`);
    if (content.indexOf(old, first + 1) !== -1) {
      throw new Error(`patch failed: old text occurs more than once in ${rel}`);
    }
    const patched = content.slice(0, first) + next + content.slice(first + old.length);
    Deno.writeTextFileSync(abs, patched);
    return { applied: true, detail: `patched ${rel}` };
  }

  /** Relative paths of files under the workspace root (sorted). */
  listFiles(rel = ""): string[] {
    this.assertRoot();
    const out: string[] = [];
    const start = rel === "" ? this.root : this.assertPath(rel);
    const walk = (dir: string, prefix: string) => {
      const entries = [...Deno.readDirSync(dir)].filter((e) => e.isFile || e.isDirectory).sort((a, b) =>
        a.name.localeCompare(b.name)
      );
      for (const entry of entries) {
        const child = prefix === "" ? entry.name : `${prefix}/${entry.name}`;
        if (entry.isDirectory) walk(`${dir}/${entry.name}`, child);
        else out.push(child);
      }
    };
    walk(start, rel);
    return out.sort();
  }

  /** Run a command in the workspace; used by oracles and verification. */
  async exec(
    cmd: string[],
    opts: { timeoutMs: number; capture: boolean; signal?: AbortSignal },
  ): Promise<{ code: number; stdout: string; stderr: string; timedOut: boolean }> {
    const signal = opts.signal === undefined
      ? AbortSignal.timeout(opts.timeoutMs)
      : AbortSignal.any([AbortSignal.timeout(opts.timeoutMs), opts.signal]);
    const proc = new Deno.Command(cmd[0], {
      args: cmd.slice(1),
      cwd: this.root,
      stdout: opts.capture ? "piped" : "null",
      stderr: opts.capture ? "piped" : "null",
      signal,
    });
    try {
      const out = await proc.output();
      // When the signal fired, Deno may surface the child's kill exit code
      // instead of an AbortError; classify by signal state rather than code.
      if (signal.aborted) {
        return { code: -1, stdout: "", stderr: "", timedOut: true };
      }
      return {
        code: out.code,
        stdout: new TextDecoder().decode(out.stdout),
        stderr: new TextDecoder().decode(out.stderr),
        timedOut: false,
      };
    } catch (err) {
      if ((err as Error).name === "AbortError" || (err as { cause?: { name?: string } }).cause?.name === "AbortError") {
        return { code: -1, stdout: "", stderr: "", timedOut: true };
      }
      throw err;
    }
  }

  /** Run a shell command in the workspace. */
  async execShell(
    command: string,
    timeoutMs: number,
    signal?: AbortSignal,
  ): Promise<{ code: number; stdout: string; stderr: string; timedOut: boolean }> {
    this.assertRoot();
    const before = captureFixtureTree(this.root);
    let result: { code: number; stdout: string; stderr: string; timedOut: boolean } | undefined;
    let commandError: unknown;
    let commandThrew = false;
    try {
      result = await this.execShellUnscoped(command, timeoutMs, signal);
    } catch (err) {
      commandThrew = true;
      commandError = err;
    }

    const after = captureFixtureTree(this.root);
    const changed = changedFixturePaths(before, after);
    const violations = changed.filter((path) => !this.isAllowedWrite(path));

    if (violations.length > 0) {
      restoreFixtureTree(this.root, before, after, violations);
      throw new WriteScopeViolationError(violations, this.task.allowed_write_scope);
    }
    if (commandThrew) throw commandError;
    return result!;
  }

  /** Execute the shell inside the disposable-root sandbox without task-scope checks. */
  private async execShellUnscoped(
    command: string,
    timeoutMs: number,
    signal?: AbortSignal,
  ): Promise<{ code: number; stdout: string; stderr: string; timedOut: boolean }> {
    if (Deno.build.os === "linux") {
      const root = Deno.realPathSync(this.root);
      return await this.exec(
        [
          "sh",
          "-c",
          [
            'sandbox="$(command -v bwrap)" || { echo "shell execution sandbox requires bwrap" >&2; exit 126; }',
            'export HOME="$1" GIT_CONFIG_GLOBAL=/dev/null',
            'exec "$sandbox" --die-with-parent --unshare-all --new-session --ro-bind / / --dev /dev --bind "$1" "$1" --chdir "$1" sh -c "$2"',
          ].join("\n"),
          "fixture-sandbox",
          root,
          command,
        ],
        { timeoutMs, capture: true, signal },
      );
    }
    if (Deno.build.os !== "darwin") {
      return {
        code: 126,
        stdout: "",
        stderr: `shell execution sandbox is unavailable on ${Deno.build.os}`,
        timedOut: false,
      };
    }

    // A working directory is not a security boundary: an arbitrary shell can
    // use absolute paths, `..`, or symlinks to mutate the host checkout. Seatbelt
    // resolves filesystem objects before applying the subpath rule, so all
    // writes remain inside this disposable fixture even through a symlink.
    const profile = [
      "(version 1)",
      "(deny default)",
      '(import "system.sb")',
      "(allow file-read*)",
      "(allow process-exec)",
      "(allow process-fork)",
      '(allow file-write* (literal "/dev/null"))',
      `(allow file-write* (subpath ${sandboxString(Deno.realPathSync(this.root))}))`,
      "(deny network*)",
    ].join("\n");
    return await this.exec(
      [
        "sh",
        "-c",
        'export HOME="$3" GIT_CONFIG_GLOBAL=/dev/null; exec /usr/bin/sandbox-exec -p "$1" sh -c "$2"',
        "fixture-sandbox",
        profile,
        command,
        Deno.realPathSync(this.root),
      ],
      { timeoutMs, capture: true, signal },
    );
  }
}

function comparePaths(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

function captureFixtureTree(root: string): FixtureTreeState {
  const state: FixtureTreeState = new Map();
  const walk = (dir: string, prefix: string): void => {
    const entries = [...Deno.readDirSync(dir)].sort((left, right) => comparePaths(left.name, right.name));
    for (const entry of entries) {
      const rel = prefix === "" ? entry.name : `${prefix}/${entry.name}`;
      const abs = `${dir}/${entry.name}`;
      const info = Deno.lstatSync(abs);
      const entryState = captureFixtureEntry(abs, info);
      state.set(rel, entryState);
      if (entryState.kind === "directory") walk(abs, rel);
    }
  };
  walk(root, "");
  return state;
}

function captureFixtureEntry(path: string, info: Deno.FileInfo): FixtureEntryState {
  const mode = info.mode === null ? null : info.mode & 0o7777;
  if (info.isDirectory) return { kind: "directory", mode };
  if (info.isFile) return { kind: "file", mode, content: Deno.readFileSync(path) };
  if (info.isSymlink) return { kind: "symlink", mode, target: Deno.readLinkSync(path) };
  return { kind: "other", mode };
}

function changedFixturePaths(before: FixtureTreeState, after: FixtureTreeState): string[] {
  const paths = new Set<string>([...before.keys(), ...after.keys()]);
  return [...paths].filter((path) => !sameFixtureEntry(before.get(path), after.get(path))).sort(comparePaths);
}

function sameFixtureEntry(left: FixtureEntryState | undefined, right: FixtureEntryState | undefined): boolean {
  if (left === undefined || right === undefined) return left === right;
  if (left.kind !== right.kind || left.mode !== right.mode) return false;
  if (left.kind === "file") return sameBytes(left.content!, right.content!);
  if (left.kind === "symlink") return left.target === right.target;
  return true;
}

function sameBytes(left: Uint8Array, right: Uint8Array): boolean {
  if (left.length !== right.length) return false;
  for (let i = 0; i < left.length; i++) {
    if (left[i] !== right[i]) return false;
  }
  return true;
}

function restoreFixtureTree(
  root: string,
  before: FixtureTreeState,
  after: FixtureTreeState,
  violations: readonly string[],
): void {
  const target = new Map(after);
  for (const path of violations) {
    const original = before.get(path);
    if (original === undefined) target.delete(path);
    else target.set(path, original);
  }
  for (const path of violations) {
    const parts = path.split("/");
    for (let i = 1; i < parts.length; i++) {
      const ancestorPath = parts.slice(0, i).join("/");
      const original = before.get(ancestorPath);
      const desired = target.get(ancestorPath);
      if (original !== undefined && (desired === undefined || desired.kind !== original.kind)) {
        target.set(ancestorPath, original);
      }
    }
  }
  pruneImpossibleFixtureDescendants(target);

  const current = captureFixtureTree(root);
  makeFixtureTreeWritable(root, current);
  const removals = [...current.keys()]
    .filter((path) => {
      const desired = target.get(path);
      return desired === undefined || desired.kind !== current.get(path)!.kind;
    })
    .sort((left, right) => pathDepth(right) - pathDepth(left) || comparePaths(right, left));
  for (const path of removals) removeFixtureEntry(`${root}/${path}`);

  const paths = [...target.keys()].sort((left, right) =>
    pathDepth(left) - pathDepth(right) || comparePaths(left, right)
  );
  for (const path of paths) {
    const desired = target.get(path)!;
    const abs = `${root}/${path}`;
    const existing = fixtureEntryIfExists(abs);
    if (existing === null) {
      ensureFixtureParent(root, path);
      createFixtureEntry(abs, desired);
      continue;
    }
    if (existing.kind !== desired.kind) {
      removeFixtureEntry(abs);
      ensureFixtureParent(root, path);
      createFixtureEntry(abs, desired);
      continue;
    }
    updateFixtureEntry(abs, existing, desired);
  }
  for (const path of paths) {
    const desired = target.get(path)!;
    if (desired.kind !== "symlink") {
      applyFixtureMode(`${root}/${path}`, desired);
    }
  }
}

function pruneImpossibleFixtureDescendants(target: FixtureTreeState): void {
  const paths = [...target.keys()].sort((left, right) =>
    pathDepth(left) - pathDepth(right) || comparePaths(left, right)
  );
  for (const path of paths) {
    const parts = path.split("/");
    for (let i = 1; i < parts.length; i++) {
      const ancestor = target.get(parts.slice(0, i).join("/"));
      if (ancestor === undefined || ancestor.kind !== "directory") {
        target.delete(path);
        break;
      }
    }
  }
}

function pathDepth(path: string): number {
  return path.split("/").length;
}

function fixtureEntryIfExists(path: string): FixtureEntryState | null {
  try {
    return captureFixtureEntry(path, Deno.lstatSync(path));
  } catch (err) {
    if (err instanceof Deno.errors.NotFound) return null;
    throw err;
  }
}

function ensureFixtureParent(root: string, path: string): void {
  const slash = path.lastIndexOf("/");
  if (slash === -1) return;
  Deno.mkdirSync(`${root}/${path.slice(0, slash)}`, { recursive: true });
}

function makeFixtureTreeWritable(root: string, current: FixtureTreeState): void {
  for (const [path, entry] of current) {
    if (entry.kind === "directory" && entry.mode !== null) {
      Deno.chmodSync(`${root}/${path}`, entry.mode | 0o700);
    }
  }
}

function createFixtureEntry(path: string, entry: FixtureEntryState): void {
  switch (entry.kind) {
    case "directory":
      Deno.mkdirSync(path, { recursive: true });
      break;
    case "file":
      Deno.writeFileSync(path, entry.content!);
      applyFixtureMode(path, entry);
      break;
    case "symlink":
      Deno.symlinkSync(entry.target!, path);
      break;
    case "other":
      throw new Error(`cannot restore unsupported fixture entry: ${path}`);
  }
}

function updateFixtureEntry(path: string, current: FixtureEntryState, desired: FixtureEntryState): void {
  if (desired.kind === "file" && !sameBytes(current.content!, desired.content!)) {
    if (current.mode !== null && (current.mode & 0o222) === 0) Deno.chmodSync(path, current.mode | 0o600);
    Deno.writeFileSync(path, desired.content!);
  }
  if (desired.kind === "symlink" && current.target !== desired.target) {
    removeFixtureEntry(path);
    Deno.symlinkSync(desired.target!, path);
  }
}

function applyFixtureMode(path: string, entry: FixtureEntryState): void {
  if (entry.mode !== null) Deno.chmodSync(path, entry.mode);
}

function removeFixtureEntry(path: string): void {
  const info = fixtureEntryIfExists(path);
  if (info === null) return;
  Deno.removeSync(path, { recursive: info.kind === "directory" });
}

function sandboxString(value: string): string {
  return `"${value.replaceAll("\\", "\\\\").replaceAll('"', '\\"')}"`;
}

function existsSync(p: string): boolean {
  try {
    Deno.statSync(p);
    return true;
  } catch {
    return false;
  }
}

function statIfExists(p: string): Deno.FileInfo | null {
  try {
    return Deno.statSync(p);
  } catch {
    return null;
  }
}

async function copyTree(src: string, dst: string): Promise<void> {
  await Deno.mkdir(dst, { recursive: true });
  for (const entry of Deno.readDirSync(src)) {
    const s = `${src}/${entry.name}`;
    const d = `${dst}/${entry.name}`;
    if (entry.isDirectory) {
      await copyTree(s, d);
    } else if (entry.isFile) {
      await Deno.copyFile(s, d);
    }
  }
}

async function gitCommand(cwd: string, args: string[]): Promise<void> {
  const proc = new Deno.Command("git", { args, cwd, stdout: "piped", stderr: "piped" });
  const out = await proc.output();
  if (out.code !== 0) {
    throw new Error(
      `git ${args.join(" ")} failed (exit ${out.code}): ${new TextDecoder().decode(out.stderr).trim()}`,
    );
  }
}
