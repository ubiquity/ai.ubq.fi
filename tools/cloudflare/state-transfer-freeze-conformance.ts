// Actual production Worker/SQLite DO conformance. Synthetic providers and private
// test bindings only; this runner creates no deployment or acceptance receipt.
import assert from "node:assert/strict";
import { connect } from "node:net";
import { lstat, mkdir, readFile, stat, writeFile } from "node:fs/promises";
import { join, isAbsolute } from "node:path";
import { fileURLToPath } from "node:url";
import { testNetworkArgv } from "../../scripts/test-network.ts";
import { sha256 } from "./state-transfer.ts";
import { ARTIFACT_SHA, ARTIFACT_TREE, Fixture, ROOT, bytesHash, type Inputs, type Json } from "./fixtures/state-transfer-freeze-support.ts";
import { FixturePrerequisite, preparedPath } from "./fixtures/state-transfer-freeze-prepared.ts";
import { positiveNativeReceiver } from "./fixtures/state-transfer-freeze-native.ts";
import { frozenInverse } from "./fixtures/state-transfer-freeze-inverse.ts";
import {
  backgroundWriter,
  eofWithoutTerminal,
  heldRequest,
  nativeOrdering,
  orphanAndCancellation,
  signedUnprovedCallback,
} from "./fixtures/state-transfer-freeze-cases.ts";

export const caseMatrix = [
  { name: "held-stream", run: (fixture: Fixture) => heldRequest(fixture, true) },
  { name: "held-buffer", run: (fixture: Fixture) => heldRequest(fixture, false) },
  { name: "held-background", run: backgroundWriter },
  { name: "native-ordering", run: nativeOrdering },
  { name: "positive-native-receiver", run: positiveNativeReceiver },
  { name: "frozen-inverse", run: frozenInverse },
  { name: "orphan-restart", run: orphanAndCancellation },
  { name: "signed-unproved-callback", run: signedUnprovedCallback },
  { name: "eof", run: eofWithoutTerminal },
] as const;

const sourceFiles = [
  "tools/cloudflare/state-transfer-freeze-conformance.ts",
  "tools/cloudflare/fixtures/state-transfer-freeze-support.ts",
  "tools/cloudflare/fixtures/state-transfer-freeze-cases.ts",
  "tools/cloudflare/fixtures/state-transfer-freeze-driver.mjs",
  "tools/cloudflare/fixtures/state-transfer-freeze-prepared.ts",
  "tools/cloudflare/fixtures/state-transfer-freeze-native.ts",
  "tools/cloudflare/fixtures/state-transfer-freeze-inverse.ts",
  "tools/cloudflare/state-transfer.ts",
  "tools/cloudflare/state-transfer-control.ts",
  "scripts/test-network.ts",
  "rust/crates/gateway-authority/tests/freeze_native_fixture.rs",
];
const missing = [
  "Actual Rust WriterGroup task-abandonment Drop injection; HTTP EOF and client cancellation have separate cases",
  "Actual five-minute AwaitFrozen protocol timeout; the bounded harness exercises client cancellation separately",
  "Restore the native inverse into a NEW Bun KV and independently recapture exact raw keys, revisions, expiry and typed values",
];

const argumentsFrom = (args: readonly string[]): Inputs => {
  const pairs = args.filter((arg) => arg !== "--fixture-child");
  assert.equal(pairs.length, 8, "Pass exactly --worker-js, --worker-wasm, --frames and --run-directory absolute paths");
  const flags = new Map<string, string>();
  for (let index = 0; index < pairs.length; index += 2) {
    assert(["--worker-js", "--worker-wasm", "--frames", "--run-directory"].includes(pairs[index]));
    assert(!flags.has(pairs[index]));
    assert(isAbsolute(pairs[index + 1]));
    flags.set(pairs[index], pairs[index + 1]);
  }
  const required = (flag: string): string => {
    const value = flags.get(flag);
    assert(value);
    return value;
  };
  return { workerJs: required("--worker-js"), workerWasm: required("--worker-wasm"), framesPath: required("--frames"), directory: required("--run-directory") };
};

const deniedEgress = (): Promise<void> =>
  new Promise((resolve, reject) => {
    const socket = connect({ host: "1.1.1.1", port: 443 });
    const timer = setTimeout(() => {
      socket.destroy();
      reject(new Error("External TCP timeout does not prove OS denial"));
    }, 3000);
    socket.once("connect", () => {
      clearTimeout(timer);
      socket.destroy();
      reject(new Error("External TCP escaped the fixture boundary"));
    });
    socket.once("error", (error: NodeJS.ErrnoException) => {
      clearTimeout(timer);
      if (["EPERM", "EACCES", "ENETUNREACH", "EHOSTUNREACH"].includes(error.code ?? "")) resolve();
      else reject(error);
    });
  });

const provenance = async (inputs: Inputs): Promise<Json> => {
  const source = Object.fromEntries(await Promise.all(sourceFiles.map(async (path) => [path, await bytesHash(join(ROOT, path))])));
  const git = (args: string[]): string | null => {
    const result = Bun.spawnSync(["git", "-C", ROOT, ...args], { stdout: "pipe", stderr: "pipe" });
    return result.exitCode === 0 ? result.stdout.toString().trim() : null;
  };
  return {
    kind: "test_only_unaccepted_freeze_conformance",
    accepted: false,
    deployment_eligible: false,
    artifact: {
      git_sha: ARTIFACT_SHA,
      git_tree: ARTIFACT_TREE,
      worker_js: inputs.workerJs,
      worker_js_sha256: await bytesHash(inputs.workerJs),
      worker_wasm: inputs.workerWasm,
      worker_wasm_sha256: await bytesHash(inputs.workerWasm),
    },
    harness: {
      git_sha: git(["rev-parse", "HEAD"]),
      git_tree: git(["rev-parse", "HEAD^{tree}"]),
      dirty_paths: git(["status", "--porcelain"]),
      source_sha256: source,
    },
    frames_sha256: await bytesHash(inputs.framesPath),
    bun_version: Bun.version,
  };
};

async function isolated(inputs: Inputs): Promise<number> {
  await deniedEgress();
  const receipts: Json[] = [];
  let failed = false;
  for (const item of caseMatrix) {
    const fixture = new Fixture(inputs, item.name);
    const started = Date.now();
    try {
      await item.run(fixture);
      receipts.push({ case: item.name, passed: true, elapsed_ms: Date.now() - started, facts: fixture.facts });
    } catch (error) {
      const blocked = error instanceof FixturePrerequisite;
      failed = !blocked;
      receipts.push({
        case: item.name,
        passed: false,
        blocked,
        elapsed_ms: Date.now() - started,
        facts: fixture.facts,
        error: error instanceof Error ? error.stack : String(error),
      });
    } finally {
      await fixture.stop();
      await writeFile(join(inputs.directory, "case-progress.json"), JSON.stringify(receipts, null, 2) + "\n", { mode: 0o600 });
    }
    // A real source failure invalidates the candidate; do not spend more runtime
    // or hide it among later passes. The artifact stays unchanged for diagnosis.
    if (failed) break;
  }
  const receipt = {
    ...(await provenance(inputs)),
    os_egress_denied: true,
    cases: receipts,
    all_implemented_cases_passed: !failed && receipts.length === caseMatrix.length && receipts.every((receipt) => receipt.passed === true),
    acceptance_complete: false,
    missing_acceptance: [...missing, ...receipts.filter((receipt) => receipt.blocked === true).map((receipt) => receipt.error)],
  };
  await writeFile(join(inputs.directory, "receipt.json"), JSON.stringify(receipt, null, 2) + "\n", { flag: "wx", mode: 0o600 });
  console.log(JSON.stringify(receipt));
  // Exit2 deliberately preserves the distinction between implemented checks
  // passing and the owner's complete freeze acceptance matrix being proved.
  return failed ? 1 : 2;
}

async function outer(inputs: Inputs): Promise<number> {
  await assert.rejects(stat(inputs.directory), /ENOENT/u, "Use a NEW task-local run directory");
  await mkdir(inputs.directory, { recursive: true, mode: 0o700 });
  const frames = await readFile(inputs.framesPath);
  const privateFrames = join(inputs.directory, "frames.private.ndjson");
  await writeFile(privateFrames, frames, { flag: "wx", mode: 0o600 });
  try {
    const metadata = await lstat(preparedPath(inputs.framesPath));
    assert(metadata.isFile() && !metadata.isSymbolicLink() && (metadata.mode & 0o077) === 0, "Prepared tools input must be a private regular file");
    const prepared = await readFile(preparedPath(inputs.framesPath));
    await writeFile(preparedPath(privateFrames), prepared, { flag: "wx", mode: 0o600 });
  } catch (error) {
    if (!(error instanceof Error && "code" in error && error.code === "ENOENT")) throw error;
  }
  assert.equal(await bytesHash(privateFrames), sha256(frames));
  const argv = testNetworkArgv([
    process.execPath,
    "--no-env-file",
    "--no-install",
    fileURLToPath(import.meta.url),
    "--worker-js",
    inputs.workerJs,
    "--worker-wasm",
    inputs.workerWasm,
    "--frames",
    privateFrames,
    "--run-directory",
    inputs.directory,
    "--fixture-child",
  ]);
  await writeFile(
    join(inputs.directory, "invocation.json"),
    JSON.stringify({ argv, provenance: await provenance({ ...inputs, framesPath: privateFrames }) }, null, 2) + "\n",
    { flag: "wx", mode: 0o600 }
  );
  const child = Bun.spawn(argv, {
    cwd: ROOT,
    env: { PATH: process.env.PATH ?? "/usr/bin:/bin", TMPDIR: inputs.directory, BUN_RUNTIME_TRANSPILER_CACHE_PATH: "0" },
    stdin: "ignore",
    stdout: Bun.file(join(inputs.directory, "runner.stdout.log")),
    stderr: Bun.file(join(inputs.directory, "runner.stderr.log")),
  });
  const stop = (): void => {
    if (child.exitCode === null) child.kill("SIGTERM");
  };
  process.once("SIGINT", stop);
  process.once("SIGTERM", stop);
  const deadline = setTimeout(stop, 240_000);
  try {
    const code = await child.exited;
    console.log(JSON.stringify({ evidence: inputs.directory, code, acceptance_complete: false }));
    return code;
  } finally {
    clearTimeout(deadline);
    process.off("SIGINT", stop);
    process.off("SIGTERM", stop);
    stop();
  }
}

if (import.meta.main) {
  assert.equal(Bun.version, "1.4.0");
  const inputs = argumentsFrom(Bun.argv.slice(2));
  (Bun.argv.includes("--fixture-child") ? isolated(inputs) : outer(inputs)).then(
    (code) => {
      process.exitCode = code;
    },
    (error: unknown) => {
      console.error(error instanceof Error ? error.stack : String(error));
      process.exitCode = 1;
    }
  );
}
