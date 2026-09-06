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
  constructor(readonly path: string, readonly scope: string[]) {
    super(`write scope violation: ${path} is not writable (scope: ${scope.join(", ")})`);
    this.name = "WriteScopeViolationError";
  }
}

export interface FixtureWorkspaceOptions {
  fixtureDir: string;
  runId: string;
  /** Parent directory for disposable workspaces (must be writable, git-ignored). */
  tmpParent: string;
  task: TaskManifest;
}

const LINUX_SANDBOX_PATH = "/sandbox-bin:/usr/bin:/bin";
const MACOS_SANDBOX_PATH = "/usr/bin:/bin";
const LINUX_SANDBOX_BOOTSTRAP = 'mkdir -p "$HOME" "$TMPDIR" && exec /usr/bin/sh -c "$0"';
const MACOS_SANDBOX_BOOTSTRAP = 'mkdir -p "$HOME" "$TMPDIR" && exec /bin/sh -c "$0"';

/**
 * Environment variables deliberately made visible to a fixture shell.
 *
 * PWD is included because POSIX shells export it when they start. Some macOS
 * shells also add SHLVL and _; those are shell-generated values, rather than
 * inherited values, and contain no host configuration or secret.
 */
export const SAFE_SHELL_ENVIRONMENT_KEYS = [
  "GIT_CONFIG_GLOBAL",
  "GIT_CONFIG_NOSYSTEM",
  "GIT_CONFIG_SYSTEM",
  "HOME",
  "PATH",
  "PWD",
  "TMPDIR",
] as const;

type ExecOptions = {
  timeoutMs: number;
  capture: boolean;
  signal?: AbortSignal;
  env?: Record<string, string>;
};

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
    opts: ExecOptions,
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
      env: opts.env,
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
    const root = Deno.realPathSync(this.root);
    const path = Deno.build.os === "linux" ? LINUX_SANDBOX_PATH : MACOS_SANDBOX_PATH;
    const environment = sandboxEnvironment(root, path);

    if (Deno.build.os === "linux") {
      // Keep the command surface useful for benchmark tasks without making
      // the host root or the repository checkout readable. /usr/bin and the
      // library directories are system runtime paths; the workspace is the
      // only writable host bind. awk is commonly an /etc/alternatives
      // symlink, so expose its resolved binary through a private PATH entry
      // instead of exposing the alternatives directory.
      const awk = Deno.realPathSync("/usr/bin/awk");
      return await this.exec(
        [
          "sh",
          "-c",
          [
            'sandbox="$(command -v bwrap)" || { echo "shell execution sandbox requires bwrap" >&2; exit 126; }',
            'exec "$sandbox" --die-with-parent --unshare-user --unshare-pid --unshare-ipc --unshare-uts --unshare-cgroup --new-session \\',
            "  --tmpfs / \\",
            "  --dev /dev \\",
            "  --tmpfs /tmp \\",
            "  --dir /sandbox-bin \\",
            "  --dir /usr \\",
            "  --dir /usr/bin \\",
            "  --dir /usr/lib \\",
            "  --dir /usr/share \\",
            "  --dir /usr/share/git-core \\",
            "  --dir /lib \\",
            "  --dir /lib64 \\",
            "  --ro-bind /usr/bin /usr/bin \\",
            "  --ro-bind /lib /lib \\",
            "  --ro-bind /lib64 /lib64 \\",
            "  --ro-bind /usr/lib/git-core /usr/lib/git-core \\",
            "  --ro-bind /usr/share/git-core /usr/share/git-core \\",
            '  --ro-bind "$3" /sandbox-bin/awk \\',
            "  --symlink usr/bin /bin \\",
            '  --bind "$1" "$1" \\',
            '  --tmpfs "$1/.home" \\',
            '  --tmpfs "$1/.tmp" \\',
            '  --chdir "$1" \\',
            "  --clearenv \\",
            '  --setenv PATH "' + LINUX_SANDBOX_PATH + '" \\',
            '  --setenv HOME "$1/.home" \\',
            '  --setenv TMPDIR "$1/.tmp" \\',
            '  --setenv PWD "$1" \\',
            "  --setenv GIT_CONFIG_GLOBAL /dev/null \\",
            "  --setenv GIT_CONFIG_SYSTEM /dev/null \\",
            "  --setenv GIT_CONFIG_NOSYSTEM 1 \\",
            '/usr/bin/sh -c "$4" "$2"',
          ].join("\n"),
          "fixture-sandbox",
          root,
          command,
          awk,
          LINUX_SANDBOX_BOOTSTRAP,
        ],
        { timeoutMs, capture: true, signal, env: environment },
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
      "(allow file-read* (subpath " + sandboxString(root) + "))",
      '(allow file-read* (subpath "/bin"))',
      '(allow file-read* (subpath "/usr/bin"))',
      '(allow file-read* (subpath "/usr/lib"))',
      '(allow file-read* (subpath "/usr/libexec"))',
      '(allow file-read* (subpath "/usr/share/git-core"))',
      '(allow file-read* (subpath "/usr/libexec/git-core"))',
      '(allow file-read* (subpath "/System/Library"))',
      '(allow file-read* (subpath "/private/var/db/dyld"))',
      '(allow file-read* (literal "/dev/null"))',
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
        [
          "exec /usr/bin/env -i \\",
          '  "PATH=/usr/bin:/bin" \\',
          '  "HOME=$3/.home" \\',
          '  "TMPDIR=$3/.tmp" \\',
          '  "PWD=$3" \\',
          "  GIT_CONFIG_GLOBAL=/dev/null \\",
          "  GIT_CONFIG_SYSTEM=/dev/null \\",
          "  GIT_CONFIG_NOSYSTEM=1 \\",
          '  /usr/bin/sandbox-exec -p "$1" /bin/sh -c "$4" "$2"',
        ].join("\n"),
        "fixture-sandbox",
        profile,
        command,
        root,
        MACOS_SANDBOX_BOOTSTRAP,
      ],
      { timeoutMs, capture: true, signal, env: environment },
    );
  }
}

function sandboxEnvironment(root: string, path: string): Record<string, string> {
  return {
    GIT_CONFIG_GLOBAL: "/dev/null",
    GIT_CONFIG_NOSYSTEM: "1",
    GIT_CONFIG_SYSTEM: "/dev/null",
    HOME: root + "/.home",
    PATH: path,
    PWD: root,
    TMPDIR: root + "/.tmp",
  };
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
