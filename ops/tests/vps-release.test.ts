import assert from "node:assert/strict";

const service = await Deno.readTextFile(new URL("../ai-ubq-fi.service", import.meta.url));
const launcher = await Deno.readTextFile(new URL("../../scripts/serve-vps.ts", import.meta.url));
const configuration = JSON.parse(await Deno.readTextFile(new URL("../../deno.json", import.meta.url)));
const productionRoot = "/home/codex/repos/ubiquity/ai.ubq.fi";
const shellQuote = (value: string): string => `'${value.replaceAll("'", "'\\''")}'`;

async function writeRelease(root: string, sha: string, label: string): Promise<string> {
  const release = `${root}/.data/releases/${sha}`;
  await Deno.mkdir(`${release}/scripts`, { recursive: true });
  await Deno.mkdir(`${release}/src`);
  await Deno.writeTextFile(
    `${release}/deno.json`,
    JSON.stringify({ ...configuration, imports: { "fixture-dependency": "./dependency.ts" } }),
  );
  await Deno.writeTextFile(`${release}/deno.lock`, '{"version":"5","specifiers":{}}\n');
  await Deno.writeTextFile(`${release}/dependency.ts`, `export const label = ${JSON.stringify(label)};\n`);
  await Deno.writeTextFile(
    `${release}/scripts/serve-vps.ts`,
    `globalThis.fixtureLauncher = ${JSON.stringify(label)};\n${launcher}`,
  );
  await Deno.writeTextFile(
    `${release}/src/config.ts`,
    `export const config = { isDeploy: false, adminTokens: new Set(["fixture-admin"]) };
export const runtimeGitSha = () => ${JSON.stringify(sha)};
`,
  );
  await Deno.writeTextFile(`${release}/src/kv.ts`, "export const initializeKv = (_kv) => {};\n");
  // Exercise the real launcher and KV path without opening a port or calling a provider.
  await Deno.writeTextFile(
    `${release}/serve.ts`,
    `import { label } from "fixture-dependency";
import { runtimeGitSha } from "./src/config.ts";
Deno.serve = () => {
  console.log(JSON.stringify({
    probe: "vps-release",
    dependency: label,
    launcher: globalThis.fixtureLauncher,
    sha: runtimeGitSha(),
    cwd: Deno.cwd(),
    env: Deno.env.get("VPS_FIXTURE_VALUE"),
  }));
  return { finished: Promise.resolve(), shutdown: async () => {} };
};
export default { fetch: () => new Response("fixture") };
`,
  );
  return release;
}

Deno.test("VPS rollback starts the selected launcher's configuration and keeps root data", async () => {
  const root = await Deno.realPath(await Deno.makeTempDir({ prefix: "vps-release-" }));
  try {
    const olderSha = "a".repeat(40);
    const newerSha = "b".repeat(40);
    const older = await writeRelease(root, olderSha, "older");
    const newer = await writeRelease(root, newerSha, "newer");
    await Deno.mkdir(`${root}/scripts`);
    await Deno.writeTextFile(
      `${root}/scripts/serve-vps.ts`,
      `globalThis.fixtureLauncher = "checkout";\n${launcher}`,
    );
    await Deno.writeTextFile(
      `${root}/deno.json`,
      JSON.stringify({ imports: { "fixture-dependency": "./dependency.ts" } }),
    );
    await Deno.writeTextFile(`${root}/deno.lock`, '{"version":"5","specifiers":{}}\n');
    await Deno.writeTextFile(`${root}/dependency.ts`, 'export const label = "checkout";\n');
    await Deno.writeTextFile(`${root}/.env`, "VPS_FIXTURE_VALUE=repository-root\n");
    const kv = await Deno.openKv(`${root}/.data/kv.sqlite3`);
    await kv.set(["persistent-fixture"], "keep-across-rollback");
    kv.close();

    const execStart = service.match(/^ExecStart=(.+)$/m)?.[1];
    assert.ok(execStart, "the systemd service must have an ExecStart command");
    const command = (execStart.match(/^\/bin\/sh -c '(.*)'$/)?.[1] ?? execStart)
      .replaceAll(productionRoot, root)
      .replace("/usr/local/bin/deno", shellQuote(Deno.execPath()));

    for (const [release, sha, label] of [[newer, newerSha, "newer"], [older, olderSha, "older"]]) {
      try {
        await Deno.remove(`${root}/.data/current`);
      } catch (error) {
        if (!(error instanceof Deno.errors.NotFound)) throw error;
      }
      await Deno.symlink(release, `${root}/.data/current`);
      const result = await new Deno.Command("/bin/sh", {
        args: ["-c", command],
        cwd: root,
        clearEnv: true,
        env: { DENO_TIMELINE: "production", DENO_DIR: `${root}/.data/deno` },
        stdout: "piped",
        stderr: "piped",
      }).output();
      const stdout = new TextDecoder().decode(result.stdout);
      assert.equal(result.success, true, new TextDecoder().decode(result.stderr));
      const line = stdout.split("\n").find((line) => line.startsWith('{"probe":"vps-release"'));
      assert.ok(line, stdout);
      assert.deepEqual(JSON.parse(line), {
        probe: "vps-release",
        dependency: label,
        launcher: label,
        sha,
        cwd: await Deno.realPath(root),
        env: "repository-root",
      });
      // Even a broken mutable configuration must not affect the next rollback.
      await Deno.writeTextFile(`${root}/deno.json`, "invalid mutable checkout config");
      await Deno.writeTextFile(`${root}/deno.lock`, "invalid mutable checkout lock");
    }
    const reopened = await Deno.openKv(`${root}/.data/kv.sqlite3`);
    try {
      assert.equal((await reopened.get(["persistent-fixture"])).value, "keep-across-rollback");
    } finally {
      reopened.close();
    }
  } finally {
    await Deno.remove(root, { recursive: true });
  }
});
