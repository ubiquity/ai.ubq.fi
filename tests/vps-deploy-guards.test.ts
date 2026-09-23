import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";

const OPS_DEPLOY_SOURCE = fileURLToPath(new URL("../ops/deploy.ts", import.meta.url));
const OPS_RELEASE_RETENTION_SOURCE = fileURLToPath(new URL("../ops/release_retention.ts", import.meta.url));
const CANONICAL_ROOT_LITERAL = '"/home/codex/repos/ubiquity/ai.ubq.fi"';
const FIXTURE_PARENT = fileURLToPath(new URL("../.cleanup-evidence/vps-deploy-guards-fixtures", import.meta.url));
const CHILD_TIMEOUT_MS = 30_000;
const PREFLIGHT_TIMEOUT_MS = 15_000;
const FIXTURE_IDENTITY = { name: "vps-deploy-guards-fixture", email: "vps-deploy-guards-fixture@example.invalid" } as const;

const isPermissionDenied = (error: unknown): boolean =>
  error instanceof Deno.errors.PermissionDenied ||
  (error instanceof Error && error.name === "PermissionDenied") ||
  (error instanceof Error && /Requires (read|run|write|env) access/.test(error.message));

/**
 * These fixtures drive the real `ops/deploy.ts` (or a copy whose single
 * canonical-root constant is relocated into the fixture) inside disposable Git
 * repositories under `.cleanup-evidence/vps-deploy-guards-fixtures/`. They need
 * scoped filesystem and subprocess capabilities that the ordinary restricted
 * `deno task test` sandbox withholds, so the dedicated invocation is registered
 * in `scripts/verify.sh` and the Gateway CI validate job. Without those
 * capabilities the fixtures are reported ignored instead of silently passing.
 */
const fixtureHostPath = await (async (): Promise<string | undefined> => {
  try {
    const path = Deno.env.get("PATH");
    await Deno.readTextFile(OPS_DEPLOY_SOURCE);
    const git = await new Deno.Command("git", { args: ["--version"], stdout: "null", stderr: "null" }).output();
    assert.ok(git.success, "the dedicated fixture command requires a working git");
    const deno = await new Deno.Command(Deno.execPath(), { args: ["--version"], stdout: "null", stderr: "null" }).output();
    assert.ok(deno.success, "the dedicated fixture command requires a spawnable Deno");
    return path;
  } catch (error) {
    if (isPermissionDenied(error)) return undefined;
    throw error;
  }
})();
const fixtureIgnored = fixtureHostPath === undefined;

type FixtureRun = { code: number; stdout: string; stderr: string };
type Fixture = { root: string; script: string; env: Record<string, string>; dispose: () => Promise<void> };
type DeployOptions = { write?: string; env?: Record<string, string> };

const decode = (bytes: Uint8Array): string => new TextDecoder().decode(bytes);

/**
 * Every fixture Git call runs with a synthetic identity and no global, system,
 * template, hook, or signing configuration. The child environment is replaced
 * rather than extended, so fixture Git and the fixture deploy process never
 * observe host environment, credential, or `.env` state, and no remote is ever
 * configured.
 */
const fixtureEnvironment = (root: string, hostPath: string): Record<string, string> => ({
  PATH: hostPath,
  HOME: root,
  DENO_DIR: `${root}/.deno-cache`,
  NO_COLOR: "1",
  GIT_CONFIG_GLOBAL: "/dev/null",
  GIT_CONFIG_SYSTEM: "/dev/null",
  GIT_CONFIG_NOSYSTEM: "1",
  GIT_TERMINAL_PROMPT: "0",
  GIT_AUTHOR_NAME: FIXTURE_IDENTITY.name,
  GIT_AUTHOR_EMAIL: FIXTURE_IDENTITY.email,
  GIT_COMMITTER_NAME: FIXTURE_IDENTITY.name,
  GIT_COMMITTER_EMAIL: FIXTURE_IDENTITY.email,
});

const runFixtureGit = async (root: string, env: Record<string, string>, args: string[]): Promise<string> => {
  const result = await new Deno.Command("git", { args, cwd: root, env, clearEnv: true, stdout: "piped", stderr: "piped" }).output();
  if (!result.success) throw new Error(`fixture git ${args.join(" ")} failed: ${decode(result.stderr).trim()}`);
  return decode(result.stdout).trim();
};

/**
 * Writes the disposable script copy used by the relocated fixtures. The copy is
 * byte-identical to `ops/deploy.ts` except for the canonical-root constant, and
 * that is proven rather than assumed: the literal must occur exactly once, the
 * fixture path must appear exactly once in the copy, and restoring the original
 * literal must reproduce the source byte for byte, so no guard or lock logic can
 * have been altered by the relocation.
 */
const relocateDeployScript = async (root: string): Promise<string> => {
  const source = await Deno.readTextFile(OPS_DEPLOY_SOURCE);
  assert.equal(source.split(CANONICAL_ROOT_LITERAL).length - 1, 1, "ops/deploy.ts must contain exactly one canonical-root literal");
  const replacement = JSON.stringify(root);
  const relocated = source.replace(CANONICAL_ROOT_LITERAL, replacement);
  assert.equal(relocated.split(replacement).length - 1, 1, "the relocated canonical root must appear exactly once");
  assert.equal(relocated.replace(replacement, CANONICAL_ROOT_LITERAL), source, "relocation must change only the canonical-root constant");
  const script = `${root}/deploy.fixture.ts`;
  await Deno.writeTextFile(script, relocated);
  return script;
};

/**
 * The relocated copy imports `./release_retention.ts` exactly as `ops/deploy.ts`
 * does, and the deploy child may read only the fixture root, so the real module
 * must sit beside it. It is copied from the repository source rather than
 * stubbed, so the fixtures keep exercising the production import chain, and it
 * needs no relocation of its own: every path it uses is relative to the working
 * directory.
 */
const relocateRetentionModule = async (root: string): Promise<void> => {
  const source = await Deno.readTextFile(OPS_RELEASE_RETENTION_SOURCE);
  await Deno.writeTextFile(`${root}/release_retention.ts`, source);
};

const createFixture = async (options: { branch: string; tracking: "match" | "mismatch" | "missing"; relocate: boolean }): Promise<Fixture> => {
  const hostPath = fixtureHostPath;
  if (hostPath === undefined) throw new Error("fixture capabilities are unavailable");
  await Deno.mkdir(FIXTURE_PARENT, { recursive: true });
  const root = await Deno.realPath(await Deno.makeTempDir({ dir: FIXTURE_PARENT, prefix: "case-" }));
  const env = fixtureEnvironment(root, hostPath);
  await Deno.mkdir(`${root}/empty-hooks`, { recursive: true });
  await runFixtureGit(root, env, ["-c", `init.templateDir=${root}/empty-hooks`, "init", "-q"]);
  await runFixtureGit(root, env, ["symbolic-ref", "HEAD", "refs/heads/development"]);
  await runFixtureGit(root, env, ["config", "core.hooksPath", `${root}/empty-hooks`]);
  await runFixtureGit(root, env, ["config", "commit.gpgsign", "false"]);
  await runFixtureGit(root, env, ["config", "tag.gpgsign", "false"]);
  await runFixtureGit(root, env, ["config", "user.name", FIXTURE_IDENTITY.name]);
  await runFixtureGit(root, env, ["config", "user.email", FIXTURE_IDENTITY.email]);
  const script = options.relocate ? await relocateDeployScript(root) : OPS_DEPLOY_SOURCE;
  if (options.relocate) await relocateRetentionModule(root);
  await Deno.writeTextFile(`${root}/release.txt`, "fixture release\n");
  await runFixtureGit(root, env, ["add", "--", "release.txt"]);
  // The relocated script and the module it imports are committed with the
  // release so the fixture checkout stays tracked-clean for the guard's
  // `git status --porcelain --untracked-files=no` check.
  if (options.relocate) await runFixtureGit(root, env, ["add", "--", "deploy.fixture.ts", "release_retention.ts"]);
  await runFixtureGit(root, env, ["commit", "-q", "-m", "fixture release"]);
  const releaseSha = await runFixtureGit(root, env, ["rev-parse", "HEAD"]);
  if (options.tracking === "mismatch") {
    await Deno.writeTextFile(`${root}/next.txt`, "newer local change\n");
    await runFixtureGit(root, env, ["add", "--", "next.txt"]);
    await runFixtureGit(root, env, ["commit", "-q", "-m", "newer local change"]);
    await runFixtureGit(root, env, ["update-ref", "refs/remotes/origin/development", releaseSha]);
  } else if (options.tracking === "match") {
    await runFixtureGit(root, env, ["update-ref", "refs/remotes/origin/development", releaseSha]);
  }
  if (options.branch !== "development") {
    await runFixtureGit(root, env, ["update-ref", `refs/heads/${options.branch}`, releaseSha]);
    await runFixtureGit(root, env, ["symbolic-ref", "HEAD", `refs/heads/${options.branch}`]);
  }
  assert.equal(await runFixtureGit(root, env, ["branch", "--show-current"]), options.branch, "fixture branch setup failed");
  return {
    root,
    script,
    env,
    dispose: async () => {
      await Deno.remove(root, { recursive: true }).catch(() => {});
      await Deno.remove(FIXTURE_PARENT).catch(() => {});
    },
  };
};

/**
 * The deploy child gets only `git` execution plus read access to its disposable
 * fixture. The lock-wait case additionally gets write access to that fixture's
 * `.data` directory alone. It can never write outside the fixture, can never run
 * sudo, gh, or tar, and has no network permission even if a guard were bypassed.
 */
const launchDeployFixture = (fixture: Fixture, options: DeployOptions = {}): Deno.ChildProcess =>
  new Deno.Command(Deno.execPath(), {
    args: [
      "run",
      "--no-config",
      "--no-prompt",
      "--allow-run=git",
      `--allow-read=${fixture.root}`,
      ...(options.write === undefined ? [] : [`--allow-write=${options.write}`]),
      fixture.script,
    ],
    cwd: fixture.root,
    env: options.env ?? fixture.env,
    clearEnv: true,
    stdout: "piped",
    stderr: "piped",
  }).spawn();

const killChild = (child: Deno.ChildProcess): void => {
  try {
    child.kill("SIGKILL");
  } catch {
    /* The child already exited. */
  }
};

/**
 * Bounded settlement: the child is killed and reaped if it does not settle, so a
 * failed assertion cannot leave a fixture process behind.
 */
const settleChild = async (child: Deno.ChildProcess, timeoutMs: number): Promise<FixtureRun> => {
  const output = child.output();
  const timeout = Promise.withResolvers<never>();
  const timer = setTimeout(() => {
    killChild(child);
    timeout.reject(new Error(`the deploy fixture child did not settle within ${timeoutMs}ms`));
  }, timeoutMs);
  try {
    const result = await Promise.race([output, timeout.promise]);
    return { code: result.code, stdout: decode(result.stdout), stderr: decode(result.stderr) };
  } catch (error) {
    await output.catch(() => {});
    throw error;
  } finally {
    clearTimeout(timer);
  }
};

const runDeployFixture = (fixture: Fixture, options: DeployOptions = {}): Promise<FixtureRun> =>
  settleChild(launchDeployFixture(fixture, options), CHILD_TIMEOUT_MS);

const pathExists = async (path: string): Promise<boolean> => {
  try {
    await Deno.lstat(path);
    return true;
  } catch (error) {
    if (error instanceof Deno.errors.NotFound) return false;
    throw error;
  }
};

const readDirNames = async (path: string): Promise<string[]> => {
  const names: string[] = [];
  for await (const entry of Deno.readDir(path)) names.push(entry.name);
  return names.sort((a, b) => a.localeCompare(b));
};

const waitForPath = async (path: string, timeoutMs: number, label: string): Promise<void> => {
  const deadline = performance.now() + timeoutMs;
  while (performance.now() < deadline) {
    if (await pathExists(path)) return;
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  throw new Error(`timed out waiting for ${label}`);
};

Deno.test({
  name: "VPS deployment rejects a non-canonical root before any Git subprocess runs",
  ignore: fixtureIgnored,
  fn: async () => {
    const fixture = await createFixture({ branch: "development", tracking: "match", relocate: false });
    try {
      // A marker-writing Git shim proves the root boundary runs first: if the
      // script reached Git in this arbitrary directory, the shim would record
      // it even though the root check fails later.
      const shimDir = `${fixture.root}/bin`;
      const marker = `${fixture.root}/git-ran.marker`;
      await Deno.mkdir(shimDir, { recursive: true });
      await Deno.writeTextFile(`${shimDir}/git`, `#!/bin/sh\nprintf ran > ${JSON.stringify(marker)}\necho "fixture git shim must not run" >&2\nexit 70\n`);
      await Deno.chmod(`${shimDir}/git`, 0o700);
      const run = await runDeployFixture(fixture, { env: { ...fixture.env, PATH: `${shimDir}:${fixture.env.PATH}` } });
      assert.notStrictEqual(run.code, 0, "a non-canonical root must fail closed");
      assert.match(run.stderr, /Run from the canonical VPS repository root/);
      assert.equal(await pathExists(marker), false, "the canonical-root check must precede every Git subprocess");
      assert.doesNotMatch(run.stderr, /fixture git shim must not run/);
      assert.equal(run.stdout.trim(), "", "no deployment step may report before the root boundary passes");
    } finally {
      await fixture.dispose();
    }
  },
});

Deno.test({
  name: "VPS deployment guards reject a relocated fixture that is not on the development branch",
  ignore: fixtureIgnored,
  fn: async () => {
    const fixture = await createFixture({ branch: "feature/not-development", tracking: "match", relocate: true });
    try {
      const run = await runDeployFixture(fixture);
      assert.notStrictEqual(run.code, 0, "a non-development branch must fail closed");
      assert.match(run.stderr, /allowed only from the development branch/);
      assert.doesNotMatch(run.stderr, /Run from the canonical VPS repository root/);
      assert.equal(await pathExists(`${fixture.root}/.data`), false, "the branch guard must precede .data creation");
      assert.equal(run.stdout.trim(), "");
    } finally {
      await fixture.dispose();
    }
  },
});

Deno.test({
  name: "VPS deployment guards reject a relocated HEAD that differs from origin/development",
  ignore: fixtureIgnored,
  fn: async () => {
    const fixture = await createFixture({ branch: "development", tracking: "mismatch", relocate: true });
    try {
      const run = await runDeployFixture(fixture);
      assert.notStrictEqual(run.code, 0, "a stale tracking ref must fail closed");
      assert.match(run.stderr, /exactly match origin\/development/);
      assert.doesNotMatch(run.stderr, /Run from the canonical VPS repository root/);
      assert.equal(await pathExists(`${fixture.root}/.data`), false, "the tracking-ref guard must precede .data creation");
      assert.equal(run.stdout.trim(), "");
    } finally {
      await fixture.dispose();
    }
  },
});

Deno.test({
  name: "VPS deployment guards fail closed when the relocated fixture lacks the origin/development tracking ref",
  ignore: fixtureIgnored,
  fn: async () => {
    const fixture = await createFixture({ branch: "development", tracking: "missing", relocate: true });
    try {
      const run = await runDeployFixture(fixture);
      assert.notStrictEqual(run.code, 0, "a missing tracking ref must fail closed");
      assert.match(run.stderr, /origin\/development tracking ref is missing/);
      assert.doesNotMatch(run.stderr, /Run from the canonical VPS repository root/);
      assert.equal(await pathExists(`${fixture.root}/.data`), false, "the missing-ref guard must precede .data creation");
      assert.equal(run.stdout.trim(), "");
    } finally {
      await fixture.dispose();
    }
  },
});

Deno.test({
  name: "VPS deployment guards accept a relocated matching preflight and stop at the .data write boundary",
  ignore: fixtureIgnored,
  fn: async () => {
    const fixture = await createFixture({ branch: "development", tracking: "match", relocate: true });
    try {
      const run = await runDeployFixture(fixture);
      // Positive control: the matching checkout clears every guard and is denied
      // only by the next boundary, the unpermitted `.data` write.
      assert.notStrictEqual(run.code, 0, "the unpermitted .data write must fail closed");
      assert.match(run.stderr, /Requires write access/);
      assert.doesNotMatch(run.stderr, /development branch|exactly match origin\/development|tracking ref is missing|canonical VPS repository root/);
      assert.equal(await pathExists(`${fixture.root}/.data`), false, "the denied write must not create .data state");
      assert.equal(run.stdout.trim(), "");
    } finally {
      await fixture.dispose();
    }
  },
});

Deno.test({
  name: "VPS deployment revalidates under the deployment lock and rejects a candidate that changed while queued",
  ignore: fixtureIgnored,
  fn: async () => {
    const fixture = await createFixture({ branch: "development", tracking: "match", relocate: true });
    let lock: Deno.FsFile | undefined;
    let child: Deno.ChildProcess | undefined;
    let lockReleased = false;
    try {
      const dataDir = `${fixture.root}/.data`;
      await Deno.mkdir(dataDir, { recursive: true });
      lock = await Deno.open(`${dataDir}/deploy.lock`, { create: true, write: true, mode: 0o600 });
      await lock.lock(true);
      child = launchDeployFixture(fixture, { write: dataDir });
      const pending = child;
      // The pre-lock preflight created `releases`, so the child has passed the
      // first validation and can now only be waiting on the held lock.
      await waitForPath(`${dataDir}/releases`, PREFLIGHT_TIMEOUT_MS, "the pre-lock checkout preflight");
      await new Promise((resolve) => setTimeout(resolve, 250));
      // Move HEAD while the deployment is queued; the tracking ref stays behind,
      // so only the post-lock revalidation can reject this candidate.
      await Deno.writeTextFile(`${fixture.root}/queued.txt`, "queued local change\n");
      await runFixtureGit(fixture.root, fixture.env, ["add", "--", "queued.txt"]);
      await runFixtureGit(fixture.root, fixture.env, ["commit", "-q", "-m", "queued local change"]);
      lock.close();
      lockReleased = true;
      // From here settlement owns the process and reaps it on timeout.
      child = undefined;
      const run = await settleChild(pending, CHILD_TIMEOUT_MS);
      assert.notStrictEqual(run.code, 0, "the post-lock revalidation must fail closed");
      assert.match(run.stderr, /exactly match origin\/development/);
      assert.deepEqual(await readDirNames(`${dataDir}/releases`), [], "no release staging or archive may exist before the post-lock preflight passes");
      assert.equal(await pathExists(`${dataDir}/current`), false, "no release may be selected before the post-lock preflight passes");
      assert.equal(run.stdout.trim(), "", "no deployment step may report before the post-lock preflight passes");
    } finally {
      if (child !== undefined) {
        killChild(child);
        await child.output().catch(() => {});
      }
      if (!lockReleased) lock?.close();
      await fixture.dispose();
    }
  },
});
