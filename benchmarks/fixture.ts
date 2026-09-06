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

type FixtureEntryKind = "file" | "directory" | "symlink" | "other";

interface FixtureEntryState {
  readonly path: string;
  readonly kind: FixtureEntryKind;
  readonly mode: number | null;
  /** Modification time in milliseconds; incidental directory changes are filtered from child diffs. */
  readonly mtimeMs: number | null;
  readonly size: number;
  readonly content?: Uint8Array;
  readonly linkTarget?: string;
}

type FixtureWorkspaceSnapshot = Map<string, FixtureEntryState>;

function comparePaths(a: string, b: string): number {
  return a < b ? -1 : a > b ? 1 : 0;
}

function pathDepth(path: string): number {
  return path.split("/").length;
}

function isPathOrDescendant(path: string, parent: string): boolean {
  return path === parent || path.startsWith(`${parent}/`);
}

function statesEqual(a: FixtureEntryState | undefined, b: FixtureEntryState | undefined): boolean {
  if (a === undefined || b === undefined) return a === b;
  if (a.kind !== b.kind || a.mode !== b.mode || a.mtimeMs !== b.mtimeMs) return false;
  if (a.kind !== "directory" && a.size !== b.size) return false;
  if (a.kind === "file") {
    if (a.content === undefined || b.content === undefined) return a.content === b.content;
    if (a.content.length !== b.content.length) return false;
    return a.content.every((byte, index) => byte === b.content![index]);
  }
  if (a.kind === "symlink") return a.linkTarget === b.linkTarget;
  return true;
}

export class WriteScopeViolationError extends Error {
  readonly path: string;
  readonly paths: readonly string[];
  readonly scope: readonly string[];

  constructor(pathOrPaths: string | readonly string[], scope: readonly string[]) {
    const paths = [...new Set(typeof pathOrPaths === "string" ? [pathOrPaths] : pathOrPaths)].sort(comparePaths);
    const evidence = paths.length === 1 ? `${paths[0]} is not writable` : `paths ${paths.join(", ")} are not writable`;
    super(`write scope violation: ${evidence} (scope: ${scope.join(", ")})`);
    this.name = "WriteScopeViolationError";
    this.path = paths[0] ?? "";
    this.paths = paths;
    this.scope = [...scope];
  }
}

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
    const before = this.captureSnapshot();
    let result: { code: number; stdout: string; stderr: string; timedOut: boolean } | undefined;
    let commandError: unknown;
    try {
      result = await this.execShellSandbox(command, timeoutMs, signal);
    } catch (err) {
      commandError = err;
    }

    const after = this.captureSnapshot();
    const changedPaths = this.changedPaths(before, after);
    const unauthorizedPaths = changedPaths.filter((path) => !this.isAllowedWrite(path));
    if (unauthorizedPaths.length > 0) {
      let restorationError: unknown;
      try {
        this.restoreUnauthorizedChanges(before, after, unauthorizedPaths);
      } catch (err) {
        restorationError = err;
      }
      const violation = new WriteScopeViolationError(unauthorizedPaths, this.task.allowed_write_scope);
      if (restorationError !== undefined) {
        const detail = restorationError instanceof Error ? restorationError.message : String(restorationError);
        violation.message += `; restoration failed: ${detail}`;
      }
      throw violation;
    }
    if (commandError !== undefined) throw commandError;
    return result!;
  }

  private async execShellSandbox(
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

  private captureSnapshot(): FixtureWorkspaceSnapshot {
    const snapshot: FixtureWorkspaceSnapshot = new Map();
    const walk = (dir: string, prefix: string): void => {
      const entries = [...Deno.readDirSync(dir)].sort((a, b) => comparePaths(a.name, b.name));
      for (const entry of entries) {
        const path = prefix === "" ? entry.name : `${prefix}/${entry.name}`;
        const abs = `${this.root}/${path}`;
        const state = this.captureEntryState(abs, path);
        snapshot.set(path, state);
        if (state.kind === "directory") walk(abs, path);
      }
    };
    walk(this.root, "");
    return snapshot;
  }

  private captureEntryState(abs: string, path: string): FixtureEntryState {
    const info = Deno.lstatSync(abs);
    const kind: FixtureEntryKind = info.isFile
      ? "file"
      : info.isDirectory
      ? "directory"
      : info.isSymlink
      ? "symlink"
      : "other";
    const mode = info.mode === null ? null : info.mode & 0o7777;
    const mtimeMs = kind === "symlink" ? null : info.mtime?.getTime() ?? null;
    if (kind === "file") {
      let content: Uint8Array | undefined;
      try {
        content = Deno.readFileSync(abs);
      } catch {
        // The mode may have been changed to deny reads. The missing content is
        // intentionally treated as a changed state and restored from `before`.
      }
      return { path, kind, mode, mtimeMs, size: info.size, content };
    }
    if (kind === "symlink") return { path, kind, mode, mtimeMs, size: info.size, linkTarget: Deno.readLinkSync(abs) };
    return { path, kind, mode, mtimeMs, size: kind === "directory" ? 0 : info.size };
  }

  private changedPaths(before: FixtureWorkspaceSnapshot, after: FixtureWorkspaceSnapshot): string[] {
    const paths = new Set<string>([...before.keys(), ...after.keys()]);
    const changed = [...paths].filter((path) => !statesEqual(before.get(path), after.get(path)));
    return changed.filter((path) => {
      const previous = before.get(path);
      const current = after.get(path);
      if (
        previous?.kind === "directory" && current?.kind === "directory" &&
        previous.mode === current.mode && previous.mtimeMs !== current.mtimeMs
      ) {
        return !changed.some((child) => child !== path && isPathOrDescendant(child, path));
      }
      return true;
    }).sort(comparePaths);
  }

  private restoreUnauthorizedChanges(
    before: FixtureWorkspaceSnapshot,
    after: FixtureWorkspaceSnapshot,
    unauthorizedPaths: readonly string[],
  ): void {
    const forcedWholeTreePaths = new Set<string>();
    const forcedMetadataPaths = new Set<string>();
    for (const path of unauthorizedPaths) {
      const parts = path.split("/");
      parts.pop();
      for (let length = parts.length; length > 0; length--) {
        const ancestor = parts.slice(0, length).join("/");
        const previous = before.get(ancestor);
        const current = after.get(ancestor);
        if (previous?.kind === "directory" && (current === undefined || current.kind !== "directory")) {
          forcedWholeTreePaths.add(ancestor);
          break;
        }
        if (previous?.kind === "directory" && current?.kind === "directory") {
          forcedMetadataPaths.add(ancestor);
        }
      }
    }
    let wholeTreePaths = unauthorizedPaths.filter((path) => {
      const previous = before.get(path);
      const current = after.get(path);
      return previous === undefined || current === undefined || previous.kind !== current.kind;
    }).filter((path, index, paths) =>
      !paths.some((parent, parentIndex) => {
        return parentIndex !== index && parent !== path && isPathOrDescendant(path, parent);
      })
    );
    wholeTreePaths = [...new Set([...wholeTreePaths, ...forcedWholeTreePaths])];

    // Restore directory modes before recreating children so deleted children
    // can be restored even when an ancestor was chmodded by the command.
    const metadataPaths = [...new Set([...unauthorizedPaths, ...forcedMetadataPaths])].filter((path) => {
      const previous = before.get(path);
      const current = after.get(path);
      return previous !== undefined && current !== undefined && previous.kind === current.kind &&
        !wholeTreePaths.some((root) => isPathOrDescendant(path, root));
    }).sort((a, b) => {
      const depth = pathDepth(a) - pathDepth(b);
      return depth !== 0 ? depth : comparePaths(a, b);
    });
    for (const path of metadataPaths) this.restoreEntryState(before.get(path)!);

    const roots = wholeTreePaths.sort((a, b) => {
      const depth = pathDepth(a) - pathDepth(b);
      return depth !== 0 ? depth : comparePaths(a, b);
    });
    for (const path of roots) {
      if (roots.some((parent) => parent !== path && isPathOrDescendant(path, parent))) continue;
      this.restoreSubtree(before, path);
    }

    // A changed descendant can be covered by a whole-tree restoration above;
    // all remaining paths have the same type and need only their own state.
    for (const path of unauthorizedPaths) {
      if (roots.some((root) => root !== path && isPathOrDescendant(path, root))) continue;
      const previous = before.get(path);
      if (previous !== undefined && after.get(path) !== undefined && previous.kind === after.get(path)!.kind) {
        this.restoreEntryState(previous);
      }
    }
    // Child removal/recreation can update an ancestor mtime after the first
    // metadata pass, so restore those ancestor states once more at the end.
    for (const path of forcedMetadataPaths) this.restoreEntryState(before.get(path)!);
  }

  private restoreSubtree(before: FixtureWorkspaceSnapshot, path: string): void {
    const previous = before.get(path);
    this.removePath(path);
    if (previous === undefined) return;

    const states = [...before.values()]
      .filter((state) => isPathOrDescendant(state.path, path))
      .sort((a, b) => {
        const depth = pathDepth(a.path) - pathDepth(b.path);
        return depth !== 0 ? depth : comparePaths(a.path, b.path);
      });
    for (const state of states) this.createEntry(state);
    for (const state of states) this.restoreMetadata(state);
  }

  private restoreEntryState(previous: FixtureEntryState): void {
    const abs = this.assertPath(previous.path);
    const current = statIfExists(abs, true);
    if (current === null) {
      this.createEntry(previous);
      this.restoreMetadata(previous);
      return;
    }
    if (
      current.isDirectory !== (previous.kind === "directory") || current.isFile !== (previous.kind === "file") ||
      current.isSymlink !== (previous.kind === "symlink")
    ) {
      throw new Error(`cannot restore changed entry type without its subtree: ${previous.path}`);
    }
    if (previous.kind === "file") {
      if (previous.content !== undefined) {
        this.makeWritable(abs, current);
        Deno.writeFileSync(abs, previous.content);
      }
    } else if (previous.kind === "symlink" && previous.linkTarget !== undefined) {
      const currentTarget = Deno.readLinkSync(abs);
      if (currentTarget !== previous.linkTarget) {
        Deno.removeSync(abs);
        Deno.symlinkSync(previous.linkTarget, abs);
      }
    }
    this.restoreMetadata(previous);
  }

  private createEntry(state: FixtureEntryState): void {
    const abs = this.assertPath(state.path);
    this.ensureRestoreParent(state.path);
    const current = statIfExists(abs, true);
    if (current !== null) {
      if (state.kind === "directory" && current.isDirectory && !current.isSymlink) return;
      this.removePath(state.path);
    }
    switch (state.kind) {
      case "directory":
        Deno.mkdirSync(abs);
        break;
      case "file":
        Deno.writeFileSync(abs, state.content ?? new Uint8Array());
        break;
      case "symlink":
        Deno.symlinkSync(state.linkTarget ?? "", abs);
        break;
      case "other":
        throw new Error(`cannot restore unsupported filesystem entry: ${state.path}`);
    }
  }

  private restoreMetadata(state: FixtureEntryState): void {
    const abs = this.assertPath(state.path);
    const current = statIfExists(abs, true);
    if (current === null) return;
    if (state.mode !== null && state.kind !== "symlink") Deno.chmodSync(abs, state.mode);
    if (state.mtimeMs !== null && state.kind !== "symlink") {
      const atimeMs = current.atime?.getTime() ?? state.mtimeMs;
      Deno.utimeSync(abs, new Date(atimeMs), new Date(state.mtimeMs));
    }
  }

  private ensureRestoreParent(path: string): void {
    const parts = path.split("/");
    parts.pop();
    let current = this.root;
    for (const part of parts) {
      current += `/${part}`;
      const info = statIfExists(current, true);
      if (info === null) {
        Deno.mkdirSync(current);
      } else if (!info.isDirectory || info.isSymlink) {
        throw new Error(`cannot restore ${path}: parent is not a directory`);
      }
    }
  }

  private removePath(path: string): void {
    const abs = this.assertPath(path);
    const info = statIfExists(abs, true);
    if (info === null) return;
    if (info.isDirectory && !info.isSymlink) this.makeTreeWritable(abs);
    Deno.removeSync(abs, { recursive: info.isDirectory && !info.isSymlink });
  }

  private makeTreeWritable(abs: string): void {
    const info = statIfExists(abs, true);
    if (info === null || info.isSymlink || !info.isDirectory) return;
    try {
      Deno.chmodSync(abs, 0o700);
    } catch {
      // The removal below will report a deterministic restoration failure when
      // the platform does not permit changing directory permissions.
    }
    for (const entry of Deno.readDirSync(abs)) this.makeTreeWritable(`${abs}/${entry.name}`);
  }

  private makeWritable(abs: string, info: Deno.FileInfo): void {
    if (info.mode !== null) Deno.chmodSync(abs, info.mode | 0o600);
  }
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

function statIfExists(p: string, noFollow = false): Deno.FileInfo | null {
  try {
    return noFollow ? Deno.lstatSync(p) : Deno.statSync(p);
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
