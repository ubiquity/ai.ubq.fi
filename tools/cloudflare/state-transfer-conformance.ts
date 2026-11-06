// Fixed synthetic Worker-level acceptance of the state-transfer routes on actual workerd.
// It drives the built gateway Worker and its real SQLite Durable Object through the operator
// CLI: closed route, refused credentials, import, exact replay, restart, export and verify.
// Synthetic frames only (state-transfer-fixture-frames); no credential, provider or network.
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { mkdir, readFile, stat, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { INTENT_HEADER, IMPORT_PATH, main, mintIntent, sha256, verifyReceipt } from "./state-transfer.ts";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const ADMIN = "fixture-admin";
type ObjectValue = Record<string, unknown>;
const isObject = (value: unknown): value is ObjectValue => typeof value === "object" && value !== null && !Array.isArray(value);

const config = (workerJs: string, workerWasm: string, state: string, intentSha256: string | undefined): string => {
  const quote = JSON.stringify;
  const intent = intentSha256 === undefined ? "" : `, (name = "UOS_STATE_TRANSFER_INTENT_SHA256", text = ${quote(intentSha256)})`;
  return `using Workerd = import ${quote(join(ROOT, "node_modules/workerd/workerd.capnp"))};
const config :Workerd.Config = ( services = [
(name = "internet", network = (allow = [])), (name = "state", disk = (path = ${quote(state)}, writable = true)),
(name = "gateway", worker = (modules = [(name = "index.js", esModule = embed ${quote(workerJs)}),(name = "index_bg.wasm", wasm = embed ${quote(workerWasm)})], compatibilityDate = "2026-10-18", compatibilityFlags = ["enable_request_signal","request_signal_passthrough"], bindings = [(name = "AUTHORITY", durableObjectNamespace = (className = "GatewayAuthority")), (name = "DENO_DEPLOY_TOKEN", text = ${quote(ADMIN)})${intent}], durableObjectNamespaces = [(className = "GatewayAuthority", uniqueKey = "uos-rust-state-transfer-conformance-v1", enableSql = true)], durableObjectStorage = (localDisk = "state")))], sockets = [(name = "main", http = (), service = "gateway")]);`;
};

type Running = Readonly<{ origin: string; stop: () => Promise<void> }>;
const start = async (directory: string, name: string, text: string, port: number): Promise<Running> => {
  const workerd: unknown = createRequire(import.meta.url)("workerd");
  assert(isObject(workerd) && typeof workerd.default === "string", "The prepared native workerd binary is required.");
  const path = join(directory, `${name}.capnp`);
  await writeFile(path, text, { mode: 0o600, flag: "wx" });
  const child = Bun.spawn([workerd.default, "serve", path, "--experimental", "--import-path", "/", "--socket-addr", `main=127.0.0.1:${port}`], {
    cwd: directory,
    stdout: Bun.file(join(directory, `${name}.stdout.log`)),
    stderr: Bun.file(join(directory, `${name}.stderr.log`)),
  });
  const stop = async (): Promise<void> => {
    child.kill();
    await child.exited;
  };
  const origin = `http://127.0.0.1:${port}`;
  const deadline = Date.now() + 20_000;
  while (Date.now() < deadline) {
    assert.equal(child.exitCode, null, "workerd exited during startup");
    try {
      if ((await fetch(`${origin}/health`)).status === 200) return { origin, stop };
    } catch {
      // Not listening yet.
    }
    await Bun.sleep(50);
  }
  await stop();
  throw new Error("workerd did not become ready");
};

const probe = async (origin: string, headers: Record<string, string>): Promise<Readonly<{ status: number; code: unknown }>> => {
  const response = await fetch(`${origin}${IMPORT_PATH}`, { headers });
  const body: unknown = await response.json().catch(() => null);
  const code = isObject(body) && isObject(body.error) ? body.error.code : undefined;
  return { status: response.status, code };
};

const flag = (name: string): string => {
  const at = Bun.argv.indexOf(`--${name}`);
  const value = at > 0 ? Bun.argv[at + 1] : "";
  assert(value.startsWith("/"), `--${name} <absolute path> is required`);
  return value;
};

const run = async (): Promise<ObjectValue> => {
  const workerJs = flag("worker-js");
  const workerWasm = flag("worker-wasm");
  const directory = flag("run-directory");
  await assert.rejects(stat(directory), /ENOENT/, "The run directory must be NEW");
  await mkdir(join(directory, "state"), { recursive: true, mode: 0o700 });
  const frames = await readFile(flag("frames"));
  const begin: unknown = JSON.parse(frames.toString("utf8").split("\n")[0]);
  assert(isObject(begin) && isObject(begin.data) && isObject(begin.data.manifest));
  const captureId = begin.data.manifest.capture_id;
  assert(typeof captureId === "string" && typeof begin.data.records === "number");
  const framesPath = join(directory, "frames.private.ndjson");
  await writeFile(framesPath, frames, { mode: 0o600, flag: "wx" });
  const frameCount = frames.toString("utf8").trimEnd().split("\n").length;
  const stageReceipt = join(directory, "frames.receipt.json");
  const stage = {
    kind: "m20_stage_frames",
    frames: frameCount,
    frame_file_bytes: frames.length,
    owner_frame: false,
    summary: { capture_id: captureId, records: begin.data.records },
  };
  await writeFile(stageReceipt, JSON.stringify(stage), { mode: 0o600, flag: "wx" });
  const state = join(directory, "state");
  const common = (origin: string): string[] => ["--origin", origin];
  // One origin and one namespace identify the same target across every restart.
  const listener = Bun.serve({ hostname: "127.0.0.1", port: 0, fetch: () => new Response(null, { status: 503 }) });
  const port = listener.port;
  assert(typeof port === "number");
  await listener.stop(true);

  // Closed: no intent digest configured means the route does not exist for anyone.
  let worker = await start(directory, "closed", config(workerJs, workerWasm, state, undefined), port);
  const closed = await probe(worker.origin, { authorization: `Bearer ${ADMIN}`, [INTENT_HEADER]: "x" });
  await worker.stop();
  assert.deepEqual(closed, { status: 404, code: "state_transfer_closed" });

  const importIntent = mintIntent({ kind: "import", captureId });
  worker = await start(directory, "import", config(workerJs, workerWasm, state, importIntent.sha256), port);
  const unauthenticated = await probe(worker.origin, { [INTENT_HEADER]: importIntent.token });
  const noIntent = await probe(worker.origin, { authorization: `Bearer ${ADMIN}` });
  const environment = { UOS_STATE_TRANSFER_ADMIN_TOKEN: ADMIN, UOS_STATE_TRANSFER_INTENT: importIntent.token };
  const importArgs = ["--frames", framesPath, "--frames-sha256", sha256(frames), "--frames-receipt", stageReceipt];
  const imported = await main(["import", ...common(worker.origin), ...importArgs, "--receipt", join(directory, "import.json")], environment);
  // A second complete run is an exact replay: the sealed target accepts nothing new.
  const replay = await main(["import", ...common(worker.origin), ...importArgs, "--receipt", join(directory, "import-replay.json")], environment);
  await worker.stop();
  assert.equal(unauthenticated.status, 401);
  assert.deepEqual(noIntent, { status: 403, code: "state_transfer_intent_refused" });
  assert.ok(verifyReceipt(importIntent.token, imported));
  assert.equal(replay.applied, 0);

  // Restart on the same Durable Object state with a separate export intent.
  const exportIntent = mintIntent({ kind: "export" });
  worker = await start(directory, "export", config(workerJs, workerWasm, state, exportIntent.sha256), port);
  const wrongScope = await probe(worker.origin, { authorization: `Bearer ${ADMIN}`, [INTENT_HEADER]: exportIntent.token });
  const exported = await main(
    ["export", ...common(worker.origin), "--archive", join(directory, "archive.private.ndjson"), "--receipt", join(directory, "export.json")],
    { UOS_STATE_TRANSFER_ADMIN_TOKEN: ADMIN, UOS_STATE_TRANSFER_INTENT: exportIntent.token }
  );
  await worker.stop();
  assert.equal(wrongScope.status, 403);
  const verified = await main(
    [
      "verify",
      ...common(worker.origin),
      ...importArgs,
      "--archive",
      join(directory, "archive.private.ndjson"),
      "--import-receipt",
      join(directory, "import.json"),
      "--export-receipt",
      join(directory, "export.json"),
    ],
    {},
    fetch,
    JSON.stringify({ v: 1, import_intent: importIntent.token, export_intent: exportIntent.token })
  );
  return {
    v: 1,
    kind: "state_transfer_conformance",
    capture_id: captureId,
    frames: frameCount,
    import_applied: imported.applied,
    replay_replayed: replay.replayed,
    export_records: exported.records,
    archive_sha256: exported.archive_sha256,
    verified: verified.matched,
    closed_route: closed,
    activation_authorized: false,
  };
};

if (import.meta.main) {
  run().then(
    (receipt) => {
      console.log(JSON.stringify(receipt, null, 2));
    },
    (error: unknown) => {
      console.error(error instanceof Error ? error.stack : String(error));
      process.exitCode = 1;
    }
  );
}
