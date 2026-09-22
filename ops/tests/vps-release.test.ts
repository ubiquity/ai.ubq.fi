import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";

const SERVICE_SOURCE = new URL("../ai-ubq-fi.service", import.meta.url);
const LAUNCHER_SOURCE = new URL("../../scripts/serve-vps.ts", import.meta.url);
const CONFIGURATION_SOURCE = new URL("../../deno.json", import.meta.url);
const PRODUCTION_ROOT = "/home/codex/repos/ubiquity/ai.ubq.fi";
const PRODUCTION_DENO = "/usr/local/bin/deno";
const CHECKOUT_MARKER = "checkout";
const RELEASE_A = { sha: "a".repeat(40), label: "release-a" } as const;
const RELEASE_B = { sha: "b".repeat(40), label: "release-b" } as const;
const ENV_NAME = "VPS_FIXTURE_VALUE";
const ENV_FILE = ".env";
const ENV_MARKER = "repository-root";
const KV_KEY: Deno.KvKey = ["vps-release-fixture"];
const KV_VALUE = "survives-rollback";
const TELEMETRY_ORDER = ["shutdownOptionalTelemetry", "kv.close"];
const FIXTURE_PARENT = fileURLToPath(new URL("../../.cleanup-evidence/vps-release-fixtures", import.meta.url));
const CHILD_TIMEOUT_MS = 30_000;

type FixtureRun = { code: number; stdout: string; stderr: string };
type Probe = {
  probe: string;
  dependency: string;
  launcher: string;
  sha: string;
  cwd: string;
  env: string | null;
};

const decoder = new TextDecoder();

const serviceSource = await Deno.readTextFile(SERVICE_SOURCE);
const launcherSource = await Deno.readTextFile(LAUNCHER_SOURCE);
const configuration = JSON.parse(await Deno.readTextFile(CONFIGURATION_SOURCE)) as Record<string, unknown>;

const shellQuote = (value: string): string => `'${value.replaceAll("'", "'\\''")}'`;

const fixtureKvSource = `const events = [];
const orderFile = new URL("../../../../.data/telemetry-order.json", import.meta.url);
export const recordEvent = (name) => {
  events.push(name);
};
export const initializeKv = (kv) => {
  const close = kv.close.bind(kv);
  kv.close = () => {
    recordEvent("kv.close");
    Deno.writeTextFileSync(orderFile, JSON.stringify(events));
    close();
  };
};
`;

const fixtureServeSource = `import { label } from "fixture-dependency";
import { recordEvent } from "./src/kv.ts";
import { runtimeGitSha } from "./src/config.ts";
Deno.serve = () => {
  console.log(JSON.stringify({
    probe: "vps-release",
    dependency: label,
    launcher: globalThis.fixtureLauncher,
    sha: runtimeGitSha(),
    cwd: Deno.cwd(),
    env: Deno.env.get(${JSON.stringify(ENV_NAME)}),
  }));
  return { finished: Promise.resolve(), shutdown: async () => {} };
};
export const shutdownOptionalTelemetry = async () => {
  recordEvent("shutdownOptionalTelemetry");
};
export default { fetch: () => new Response("fixture") };
`;

/**
 * The service fixture is driven by the real `ExecStart` rather than a mirror of
 * the launcher: the production root, the Deno binary, and systemd's `$$`
 * escaping are relocated into a disposable fixture root under
 * `.cleanup-evidence/vps-release-fixtures/`. Only that subtree is read or
 * written, every child is bounded and reaped, and no listener or provider is
 * ever contacted.
 */
const execStart = /^ExecStart=(.+)$/m.exec(serviceSource)?.[1];
assert.ok(execStart, "the systemd unit must define ExecStart");
const wrapped = /^\/bin\/sh -c '(.*)'$/.exec(execStart)?.[1] ?? execStart;
assert.notEqual(wrapped, execStart, "the VPS unit must select the release through /bin/sh -c");
assert.ok(wrapped.includes("$$"), "the unit must escape the release shell's $ as $$ for systemd");

const replayCommand = (root: string): string => {
  const denoBinary = shellQuote(Deno.execPath());
  const command = wrapped
    .replaceAll("$$", "$")
    .replaceAll(PRODUCTION_ROOT, () => root)
    .replace(PRODUCTION_DENO, () => denoBinary);
  assert.ok(command.includes(root), "the replayed ExecStart must use the fixture root");
  assert.ok(command.includes(`exec ${denoBinary} run`), "the replayed ExecStart must exec the running Deno binary");
  assert.equal(command.split(denoBinary).length - 1, 1, "the replayed ExecStart must name the Deno binary exactly once");
  return command;
};

const writeRelease = async (root: string, sha: string, label: string): Promise<void> => {
  const release = `${root}/.data/releases/${sha}`;
  await Deno.mkdir(`${release}/scripts`, { recursive: true });
  await Deno.mkdir(`${release}/src`, { recursive: true });
  // The release's own config, lockfile, import map, and launcher are the only
  // ones a release may execute; the mutable checkout copies stay untouched.
  await Deno.writeTextFile(`${release}/deno.json`, JSON.stringify({ ...configuration, imports: { "fixture-dependency": "./dependency.ts" } }));
  await Deno.writeTextFile(`${release}/deno.lock`, '{"version":"5","specifiers":{}}\n');
  await Deno.writeTextFile(`${release}/dependency.ts`, `export const label = ${JSON.stringify(label)};\n`);
  await Deno.writeTextFile(`${release}/scripts/serve-vps.ts`, `globalThis.fixtureLauncher = ${JSON.stringify(label)};\n${launcherSource}`);
  await Deno.writeTextFile(
    `${release}/src/config.ts`,
    `export const config = { isDeploy: false, adminTokens: new Set(["fixture-admin"]) };\nexport const runtimeGitSha = () => ${JSON.stringify(sha)};\n`
  );
  await Deno.writeTextFile(`${release}/src/kv.ts`, fixtureKvSource);
  await Deno.writeTextFile(`${release}/serve.ts`, fixtureServeSource);
};

const prepareFixtureRoot = async (): Promise<string> => {
  await Deno.mkdir(FIXTURE_PARENT, { recursive: true });
  const root = await Deno.realPath(await Deno.makeTempDir({ dir: FIXTURE_PARENT, prefix: "case-" }));
  await Deno.mkdir(`${root}/.data/releases`, { recursive: true });
  await Deno.mkdir(`${root}/scripts`, { recursive: true });
  // The mutable checkout is a decoy: its launcher and dependency are labelled
  // `checkout`, so any fallback to it fails the per-run markers.
  await Deno.writeTextFile(`${root}/deno.json`, JSON.stringify({ imports: { "fixture-dependency": "./dependency.ts" } }));
  await Deno.writeTextFile(`${root}/deno.lock`, '{"version":"5","specifiers":{}}\n');
  await Deno.writeTextFile(`${root}/dependency.ts`, `export const label = ${JSON.stringify(CHECKOUT_MARKER)};\n`);
  await Deno.writeTextFile(`${root}/scripts/serve-vps.ts`, `globalThis.fixtureLauncher = ${JSON.stringify(CHECKOUT_MARKER)};\n${launcherSource}`);
  await Deno.writeTextFile(`${root}/${ENV_FILE}`, `${ENV_NAME}=${ENV_MARKER}\n`);
  const kv = await Deno.openKv(`${root}/.data/kv.sqlite3`);
  try {
    await kv.set(KV_KEY, KV_VALUE);
  } finally {
    kv.close();
  }
  return root;
};

const selectRelease = async (root: string, sha: string): Promise<void> => {
  const relativeCurrent = ".data/current";
  const current = `${root}/${relativeCurrent}`;
  try {
    await Deno.remove(current);
  } catch (error) {
    if (!(error instanceof Deno.errors.NotFound)) throw error;
  }
  // Deno.symlink() requires unscoped read and write grants, so the link is made
  // by the already-permitted shell: `-n` never traverses a symlinked
  // destination and `-f` replaces any stale link left behind.
  const target = `releases/${sha}`;
  const link = new Deno.Command("/bin/sh", {
    args: ["-c", `ln -sfn ${shellQuote(target)} ${shellQuote(relativeCurrent)}`],
    cwd: root,
    clearEnv: true,
    env: { PATH: "/usr/bin:/bin" },
    stdout: "piped",
    stderr: "piped",
  }).spawn();
  const result = await settleChild(link, CHILD_TIMEOUT_MS);
  assert.equal(result.code, 0, `selecting release ${sha} failed: ${result.stderr}`);
};

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
    timeout.reject(new Error(`the VPS release fixture child did not settle within ${timeoutMs}ms`));
  }, timeoutMs);
  try {
    const result = await Promise.race([output, timeout.promise]);
    return { code: result.code, stdout: decoder.decode(result.stdout), stderr: decoder.decode(result.stderr) };
  } catch (error) {
    await output.catch(() => {});
    throw error;
  } finally {
    clearTimeout(timer);
  }
};

const runRelease = async (root: string, command: string, sha: string, label: string): Promise<Probe> => {
  await selectRelease(root, sha);
  const child = new Deno.Command("/bin/sh", {
    args: ["-c", command],
    cwd: root,
    clearEnv: true,
    env: {
      DENO_DIR: `${root}/.data/deno`,
      DENO_TIMELINE: "production",
      DENO_NO_UPDATE_CHECK: "1",
      NO_COLOR: "1",
    },
    stdout: "piped",
    stderr: "piped",
  }).spawn();
  const run = await settleChild(child, CHILD_TIMEOUT_MS);
  assert.equal(run.code, 0, `release ${label} failed: ${run.stderr}`);
  const line = run.stdout.split("\n").find((candidate) => candidate.includes('"probe":"vps-release"'));
  assert.ok(line, `release ${label} did not report its startup probe: ${run.stdout}`);
  const probe = JSON.parse(line) as Probe;
  assert.equal(probe.dependency, label, "the release's own import map must supply its dependency");
  assert.equal(probe.launcher, label, "the selected release must supply the launcher");
  assert.notEqual(probe.launcher, CHECKOUT_MARKER, "the mutable checkout launcher must never run");
  assert.equal(probe.sha, sha, "the release must report its own Git identity");
  assert.equal(probe.cwd, root, "the service must keep the repository root as its working directory");
  assert.equal(probe.env, ENV_MARKER, "the repository-root .env must supply runtime configuration");
  const order = JSON.parse(await Deno.readTextFile(`${root}/.data/telemetry-order.json`)) as string[];
  assert.deepEqual(order, TELEMETRY_ORDER, "optional telemetry must drain before the KV handle closes");
  return probe;
};

Deno.test("VPS releases execute their own launcher, config, and lockfile across rollback", async () => {
  const root = await prepareFixtureRoot();
  try {
    const command = replayCommand(root);
    await writeRelease(root, RELEASE_A.sha, RELEASE_A.label);
    await writeRelease(root, RELEASE_B.sha, RELEASE_B.label);

    const firstA = await runRelease(root, command, RELEASE_A.sha, RELEASE_A.label);
    await runRelease(root, command, RELEASE_B.sha, RELEASE_B.label);
    const secondA = await runRelease(root, command, RELEASE_A.sha, RELEASE_A.label);
    assert.deepEqual(secondA, firstA, "re-selecting release A must reproduce its markers");

    // Corrupt the mutable checkout config and lock: a release that re-reads them
    // fails under --frozen, while the selected release's own copies still win.
    await Deno.writeTextFile(`${root}/deno.json`, "invalid mutable checkout configuration");
    await Deno.writeTextFile(`${root}/deno.lock`, "invalid mutable checkout lockfile");
    const rollback = await runRelease(root, command, RELEASE_A.sha, RELEASE_A.label);
    assert.deepEqual(rollback, firstA, "rollback must reproduce release A despite a broken checkout");

    const reopened = await Deno.openKv(`${root}/.data/kv.sqlite3`);
    try {
      assert.equal((await reopened.get(KV_KEY)).value, KV_VALUE, "runtime KV data must survive a rollback");
    } finally {
      reopened.close();
    }
  } finally {
    await Deno.remove(root, { recursive: true }).catch(() => {});
    await Deno.remove(FIXTURE_PARENT).catch(() => {});
  }
});
