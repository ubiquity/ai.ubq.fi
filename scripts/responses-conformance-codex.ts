import assert from "node:assert/strict";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { isAbsolute, join } from "node:path";
import { fileURLToPath } from "node:url";
import { runCodexConformance } from "../tests/helpers/responses-conformance-client.ts";
import { initializeTestNativeKv, verifiedTestNativeKvBinding, type TestNativeKvBinding } from "./test-native-kv.ts";
import { testNetworkArgv } from "./test-network.ts";

type Options = Readonly<{
  executable: string;
  expectedSha256: string;
  expectedVersion: string;
  captureDirectory: string;
  nativeBinding: TestNativeKvBinding;
}>;

const parseOptions = (args: readonly string[]): Options => {
  const names = ["codex-executable", "codex-sha256", "codex-version", "capture-directory", "native-kv-binding"];
  const values = new Map<string, string>();
  for (const argument of args) {
    const match = /^--([^=]+)=(.+)$/.exec(argument);
    if (!match || !names.includes(match[1]) || values.has(match[1])) throw new Error("Pass each supported --name=value option exactly once");
    values.set(match[1], match[2]);
  }
  const required = (name: string): string => {
    const value = values.get(name);
    if (!value) throw new Error("Missing --" + name);
    return value;
  };
  const executable = required("codex-executable");
  const captureDirectory = required("capture-directory");
  assert(isAbsolute(executable) && isAbsolute(captureDirectory), "Executable and fresh capture directory must be absolute");
  const expectedSha256 = required("codex-sha256");
  const expectedVersion = required("codex-version");
  assert(/^[a-f0-9]{64}$/.test(expectedSha256), "An independently pinned Codex SHA256 is required");
  assert(/^\d+\.\d+\.\d+$/.test(expectedVersion), "An exact Codex CLI version is required");
  return { executable, expectedSha256, expectedVersion, captureDirectory, nativeBinding: verifiedTestNativeKvBinding(required("native-kv-binding")) };
};

async function replay(options: Options): Promise<void> {
  initializeTestNativeKv(options.nativeBinding);
  const { startResponsesConformanceFixture } = await import("../tests/helpers/responses-conformance-fixture.ts");
  const fixture = await startResponsesConformanceFixture({ holdToolDelta: false, captureDirectory: join(options.captureDirectory, "gateway") });
  try {
    const native = await runCodexConformance({
      executable: options.executable,
      expectedSha256: options.expectedSha256,
      expectedVersion: options.expectedVersion,
      endpoint: fixture.endpoint,
      token: fixture.token,
      captureDirectory: join(options.captureDirectory, "native"),
      onTextDelta: (event) => {
        fixture.acknowledge(2, event);
      },
    });
    if (fixture.terminals.length < 2) await fixture.journal.wait((entry) => entry.kind === "request_terminal" && fixture.terminals.length === 2);
    assert.equal(fixture.calls.length, 2, "One tool turn and one coherent continuation must dispatch exactly twice");
    assert.equal(fixture.terminals.length, 2);
    await fixture.save(native);
  } finally {
    await fixture.stop();
  }
  const reopened = fixture.verifyReopenedLedger();
  assert.equal(reopened.outcome, "verified");
  console.log(
    JSON.stringify({
      kind: "responses_conformance_codex",
      status: "passed",
      captureDirectory: options.captureDirectory,
      dispatches: fixture.calls.length,
      durableReopen: reopened.outcome,
    })
  );
}

async function isolated(options: Options, args: readonly string[]): Promise<void> {
  // mkdir without recursive reuse preserves every earlier transcript and database.
  await mkdir(options.captureDirectory, { mode: 0o700 });
  const command = testNetworkArgv([process.execPath, "--no-env-file", "--no-install", fileURLToPath(import.meta.url), "--fixture-child", ...args]);
  const env: Record<string, string> = { PATH: process.env.PATH ?? "/usr/bin:/bin", TMPDIR: options.captureDirectory, BUN_RUNTIME_TRANSPILER_CACHE_PATH: "0" };
  if (process.env.HOME) env.HOME = process.env.HOME;
  const child = Bun.spawn(command, { env, stdin: "ignore", stdout: "pipe", stderr: "pipe" });
  const stop = (): void => {
    if (child.exitCode === null) child.kill("SIGTERM");
  };
  process.once("SIGINT", stop);
  process.once("SIGTERM", stop);
  const timer = setTimeout(stop, 60_000);
  try {
    const [code, stdout, stderr] = await Promise.all([child.exited, new Response(child.stdout).text(), new Response(child.stderr).text()]);
    await Promise.all([writeFile(join(options.captureDirectory, "stdout.log"), stdout), writeFile(join(options.captureDirectory, "stderr.log"), stderr)]);
    assert.equal(code, 0, "Synthetic native replay failed; inspect its capture directory");
    const raw: unknown = JSON.parse(await readFile(join(options.captureDirectory, "gateway/raw.json"), "utf8"));
    assert(raw && typeof raw === "object" && "storage" in raw, "Durable source fixture evidence is required");
    console.log(stdout.trim());
  } finally {
    clearTimeout(timer);
    process.off("SIGINT", stop);
    process.off("SIGTERM", stop);
    if (child.exitCode === null) {
      child.kill("SIGKILL");
      await child.exited;
    }
  }
}

if (import.meta.main) {
  try {
    assert.equal(Bun.version, "1.4.0", "Use the prepared pinned Bun runtime");
    const args = process.argv.slice(2);
    const child = args[0] === "--fixture-child";
    if (child) args.shift();
    const options = parseOptions(args);
    if (child) await replay(options);
    else await isolated(options, args);
  } catch (error) {
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 1;
  }
}
