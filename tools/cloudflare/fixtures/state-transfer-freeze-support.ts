import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { canonical, main, mintIntent, sha256, type Transport } from "../state-transfer.ts";
import { activate, finalizeImport, mintControlIntent, type Binding, type FinalizePlan, type FrozenRef } from "../state-transfer-control.ts";

export const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../../..");
export const ARTIFACT_SHA = "bf9c949686d14a193564be507d4048c4b3709244";
export const ARTIFACT_TREE = "31f3fc58424ceaaecfbe0d32796ad3271e99b3a8";
export const ADMIN = "fixture-admin";
export type Json = Record<string, unknown>;
export type Permit = Readonly<{ id: string; serving_epoch: number; capability: string }>;
export type State = Readonly<{
  gate: Binding & { phase: string };
  counts: Record<"roots" | "native_operations" | "native_joins" | "import_frames" | "blocked", number>;
}>;
export const object = (value: unknown): Json => {
  assert(typeof value === "object" && value !== null && !Array.isArray(value), "Expected a JSON object");
  return value as Json;
};
export const bytesHash = async (path: string): Promise<string> => sha256(await readFile(path));
export const tuple = (...parts: string[]): string =>
  `tuple-v1:${Buffer.concat(
    parts.map((part) => {
      assert(!part.includes("\0"));
      return Buffer.concat([Buffer.from([2]), Buffer.from(part), Buffer.from([0])]);
    })
  ).toString("hex")}`;
export const jsonRecord = (revision: string, value: unknown): Json => ({ schema_version: 1, value_encoding: "json", revision, value, expires_at_ms: null });
export const permitAccess = (permit: Permit): Json => ({ kind: "permit", value: permit });
const reader = { kind: "operator_read" };
const randomHex = (): string => Buffer.from(crypto.getRandomValues(new Uint8Array(32))).toString("hex");
const boundedFetch: Transport["fetch"] = (input, init) => fetch(input, { ...init, signal: init.signal ?? AbortSignal.timeout(20_000) });

export type Inputs = Readonly<{ workerJs: string; workerWasm: string; framesPath: string; directory: string }>;
export type WriterFixture = Readonly<{ js: string; jsSha256: string; wasm: string; wasmSha256: string; sourceSha: string; sourceTree: string }>;
async function writerConfiguration(writer?: WriterFixture): Promise<Readonly<{ binding: string; service: string }>> {
  if (!writer) return { binding: "", service: "" };
  assert.equal(await bytesHash(writer.js), writer.jsSha256);
  assert.equal(await bytesHash(writer.wasm), writer.wasmSha256);
  const q = JSON.stringify;
  return {
    binding: ', (name = "WRITER_FIXTURE", service = "writer-fixture")',
    service: `,
 (name = "writer-fixture", worker = (
  modules = [(name = "writer-entrypoint.mjs", esModule = embed ${q(join(ROOT, "tools/cloudflare/fixtures/state-transfer-freeze-writers.mjs"))}),
   (name = "writer-build/index.js", esModule = embed ${q(writer.js)}), (name = "writer-build/index_bg.wasm", wasm = embed ${q(writer.wasm)})],
  compatibilityDate = "2026-10-18", compatibilityFlags = ["enable_request_signal","request_signal_passthrough"], globalOutbound = "internet",
  bindings = [(name = "AUTHORITY", durableObjectNamespace = (className = "GatewayAuthority", serviceName = "gateway")),
   (name = "UOS_MATRIX_SCENARIO", text = "freeze-writers")]))`,
  };
}
export type Pending<T> = { promise: Promise<T>; settled: boolean };
export const pending = <T>(promise: Promise<T>): Pending<T> => {
  const result = { promise, settled: false };
  void promise.then(
    () => {
      result.settled = true;
    },
    () => {
      result.settled = true;
    }
  );
  return result;
};

export class Fixture {
  origin = "";
  binding?: Binding;
  drain?: ReturnType<typeof mintControlIntent>;
  writer?: WriterFixture;
  private _child?: Bun.Subprocess;
  private _sequence = 0;
  readonly facts: Json[] = [];

  constructor(
    readonly inputs: Inputs,
    readonly label: string
  ) {}

  get directory(): string {
    return join(this.inputs.directory, this.label);
  }

  async stop(): Promise<void> {
    const child = this._child;
    if (!child) return;
    this._child = undefined;
    if (child.exitCode === null) child.kill("SIGTERM");
    const timer = setTimeout(() => {
      if (child.exitCode === null) child.kill("SIGKILL");
    }, 3000);
    try {
      await child.exited;
    } finally {
      clearTimeout(timer);
    }
  }

  async restart(intentDigest: string, ingress = true): Promise<void> {
    await this.stop();
    await mkdir(join(this.directory, "state"), { recursive: true, mode: 0o700 });
    const require = createRequire(import.meta.url);
    const runtime = object(require("workerd"));
    assert.equal(typeof runtime.default, "string");
    const workerd = String(runtime.default);
    const packagePath = require.resolve("workerd/package.json");
    assert.equal(object(JSON.parse(await readFile(packagePath, "utf8"))).version, "1.20261012.1");
    assert.equal(await bytesHash(workerd), "fabfd6c40a3bb74a98aac54999b456bdb3b7aa7ede4cbbb33898e1f8836a2564", "Trusted Linux ARM64 workerd hash differs");
    const version = Bun.spawnSync([workerd, "--version"], { stdout: "pipe", stderr: "pipe" });
    assert.equal(version.exitCode, 0);
    assert.equal(version.stdout.toString().trim(), "workerd 2026-10-12");
    const q = JSON.stringify;
    const writer = await writerConfiguration(this.writer);
    const access = ingress ? ', (name = "UOS_AI_TOKEN", text = "fixture-access")' : "";
    const native = JSON.stringify({
      origin: "https://fixture-native.invalid",
      identity: {
        key_id: "freeze-key",
        owner_id: "freeze-owner",
        owner_epoch: "freeze-epoch",
        key_base64url: Buffer.alloc(32, 31).toString("base64url"),
        capabilities: ["supervisor_read", "owner_renewal", "access_publish"],
      },
      renewal_owners: [{ owner_id: "freeze-owner", codex_home: "/fixture/native-owner" }],
    });
    const text = `using Workerd = import ${q(join(dirname(packagePath), "workerd.capnp"))};
const config :Workerd.Config = ( services = [
 (name = "internet", network = (allow = [])),
 (name = "state", disk = (path = ${q(join(this.directory, "state"))}, writable = true)),
 (name = "gateway", worker = (
  modules = [(name = "index.js", esModule = embed ${q(this.inputs.workerJs)}), (name = "index_bg.wasm", wasm = embed ${q(this.inputs.workerWasm)})],
  compatibilityDate = "2026-10-18", compatibilityFlags = ["enable_request_signal","request_signal_passthrough"], globalOutbound = "driver",
  bindings = [(name = "AUTHORITY", durableObjectNamespace = (className = "GatewayAuthority")), (name = "DENO_DEPLOY_TOKEN", text = ${q(ADMIN)}),
   (name = "UOS_STATE_TRANSFER_INTENT_SHA256", text = ${q(intentDigest)}), (name = "DEEPSEEK_API_KEY", text = "fixture-provider-secret"),
   (name = "LITHOSAI_API_KEY", text = "fixture-provider-secret"), (name = "UOS_NATIVE_CARRIER", text = ${q(native)})${access}],
  durableObjectNamespaces = [(className = "GatewayAuthority", uniqueKey = "uos-freeze-production-v1", enableSql = true)], durableObjectStorage = (localDisk = "state"))),
 (name = "driver", worker = (
  modules = [(name = "driver.mjs", esModule = embed ${q(join(ROOT, "tools/cloudflare/fixtures/state-transfer-freeze-driver.mjs"))})],
  compatibilityDate = "2026-10-18", compatibilityFlags = ["enable_request_signal","request_signal_passthrough"], globalOutbound = "internet",
  bindings = [(name = "GATEWAY", service = "gateway"), (name = "AUTHORITY", durableObjectNamespace = (className = "GatewayAuthority", serviceName = "gateway")),
   (name = "CONTROL", durableObjectNamespace = (className = "FreezeFixtureControl"))${writer.binding}],
  durableObjectNamespaces = [(className = "FreezeFixtureControl", uniqueKey = "uos-freeze-fixture-control-v1", enableSql = true)], durableObjectStorage = (localDisk = "state")))${writer.service}
 ], sockets = [(name = "main", http = (), service = "driver")]);`;
    const stem = `workerd-${++this._sequence}`;
    const config = join(this.directory, `${stem}.capnp`);
    await writeFile(config, text, { flag: "wx", mode: 0o600 });
    const reserve = Bun.serve({ hostname: "127.0.0.1", port: 0, fetch: () => new Response(null, { status: 503 }) });
    const port = reserve.port;
    assert(typeof port === "number");
    await reserve.stop(true);
    const argv = [workerd, "serve", config, "--experimental", "--import-path", "/", "--socket-addr", `main=127.0.0.1:${port}`];
    const child = Bun.spawn(argv, {
      cwd: this.directory,
      env: { PATH: "/usr/bin:/bin", TMPDIR: this.directory },
      stdout: Bun.file(join(this.directory, `${stem}.stdout.log`)),
      stderr: Bun.file(join(this.directory, `${stem}.stderr.log`)),
    });
    this._child = child;
    this.origin = `http://127.0.0.1:${port}`;
    this.facts.push({ operation: "start", argv, config_sha256: sha256(text), workerd_sha256: await bytesHash(workerd), writer_fixture: this.writer ?? null });
    const deadline = Date.now() + 15_000;
    while (Date.now() < deadline) {
      assert.equal(child.exitCode, null, `workerd exited: ${this.directory}/${stem}.stderr.log`);
      try {
        const response = await this.public("/health");
        assert.equal(response.status, 200);
        assert.equal(response.headers.get("x-uos-git-sha"), ARTIFACT_SHA);
        assert.equal(object(object(await response.json()).release).git_sha, ARTIFACT_SHA);
        return;
      } catch (error) {
        if (!(error instanceof TypeError)) throw error;
        await Bun.sleep(30);
      }
    }
    throw new Error(`workerd startup exceeded deadline: ${this.directory}`);
  }

  public(path: string, init?: RequestInit): Promise<Response> {
    return boundedFetch(`${this.origin}/public${path}`, init ?? {});
  }
  transport(intent: string): Transport {
    return { origin: this.origin, adminToken: ADMIN, intent, fetch: boundedFetch };
  }
  async rpc(request: Json, access: Json = reader, objectName = "gateway-authority-v1"): Promise<Response> {
    return boundedFetch(`${this.origin}/rpc?object=${encodeURIComponent(objectName)}`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ access, request }),
    });
  }
  async exchange(request: Json, access: Json = reader): Promise<Json> {
    const response = await this.rpc(request, access);
    const text = await response.text();
    assert.equal(response.status, 200, text);
    return object(JSON.parse(text));
  }
  async coordinate(request: Json): Promise<Json> {
    return object((await this.exchange({ operation: "coordination", request })).reply);
  }
  async status(): Promise<State> {
    const state = object((await this.coordinate({ operation: "status" })).status);
    const gate = object(state.gate);
    const identity = object(gate.identity);
    const counts = object(state.counts);
    assert.equal(identity.source_sha, ARTIFACT_SHA);
    assert.equal(identity.source_tree, ARTIFACT_TREE);
    assert.match(String(identity.object_id), /^[a-f0-9]{64}$/u);
    assert(Number.isSafeInteger(gate.serving_epoch));
    for (const value of Object.values(counts)) assert(Number.isSafeInteger(value) && Number(value) >= 0);
    return state as State;
  }
  async acquire(kind: Json, parent: Permit | null = null): Promise<Permit> {
    const id = randomHex();
    const capability = randomHex();
    const result = await this.coordinate({ operation: "acquire", request: { id, capability_sha256: sha256(capability), kind, parent } });
    assert.equal(result.operation, "acquired");
    return { id, capability, serving_epoch: Number(result.serving_epoch) };
  }
  root(name: string): Promise<Permit> {
    return this.acquire({ kind: "root", binding: { request_id: name } });
  }
  async finish(permit: Permit, blocker: string | null = null): Promise<Json> {
    return this.coordinate({ operation: "finish", permit, blocker });
  }
  async commit(permit: Permit, key: string, record: Json): Promise<Response> {
    return this.rpc({ operation: "commit", checks: [], mutations: [{ operation: "put", key, record }] }, permitAccess(permit));
  }
  async inventory(): Promise<unknown[]> {
    const result = await this.exchange({
      operation: "query",
      selector: { kind: "prefix", prefix: "tuple-v1:", start: null, end: null },
      reverse: false,
      limit: 1000,
      cursor: null,
      now_ms: 0,
    });
    assert.equal(result.next_cursor, null, "Fixture inventory exceeds its explicit bound");
    assert(Array.isArray(result.entries));
    return result.entries;
  }
  async control(path: string): Promise<Response> {
    return boundedFetch(`${this.origin}/control${path}`, {});
  }
  async event(kind: string, name: string): Promise<Json> {
    const response = await this.control(`/wait?kind=${encodeURIComponent(kind)}&name=${encodeURIComponent(name)}`);
    assert.equal(response.status, 200);
    return object(await response.json());
  }
  async running(): Promise<void> {
    const frames = await readFile(this.inputs.framesPath);
    const begin = object(object(JSON.parse(frames.toString().split("\n")[0])).data);
    const captureId = String(object(begin.manifest).capture_id);
    const intent = mintIntent({ kind: "import", captureId });
    await this.restart(intent.sha256);
    for (const [path, authorization] of [
      ["/v1/models", "bad-token"],
      ["/admin/codex/auth", ADMIN],
      ["/uos/native/renewal-authority", ""],
    ]) {
      const response = await this.public(path, { method: "POST", headers: { authorization: `Bearer ${authorization}` }, body: "{}" });
      assert.equal(response.status, 503, "Provisioning must close before auth/native side effects");
      await response.arrayBuffer();
    }
    assert.equal((await this.inventory()).length, 0, "Pre-auth requests created canonical rows");
    const receiptPath = join(this.directory, "frames.receipt.json");
    await writeFile(
      receiptPath,
      JSON.stringify({
        kind: "m20_stage_frames",
        frames: frames.toString().trimEnd().split("\n").length,
        frame_file_bytes: frames.length,
        owner_frame: false,
        summary: { capture_id: captureId, records: begin.records },
      }),
      { flag: "wx", mode: 0o600 }
    );
    await main(
      [
        "import",
        "--origin",
        this.origin,
        "--frames",
        this.inputs.framesPath,
        "--frames-sha256",
        sha256(frames),
        "--frames-receipt",
        receiptPath,
        "--receipt",
        join(this.directory, "import.json"),
      ],
      { UOS_STATE_TRANSFER_ADMIN_TOKEN: ADMIN, UOS_STATE_TRANSFER_INTENT: intent.token }
    );
    const state = await this.status();
    const plan: FinalizePlan = {
      binding: { identity: state.gate.identity, serving_epoch: state.gate.serving_epoch },
      capture_id: captureId,
      staged_records: Number(begin.records),
      final_chain_digest: String(begin.digest),
      manifest_revision: `capture:${captureId}`,
      expected_owner: { kind: "absent" },
    };
    const finalization = mintControlIntent({ purpose: "finalize_import", data: plan });
    await this.restart(finalization.sha256);
    const frozen = await finalizeImport(this.transport(finalization.token), plan);
    const activation = mintControlIntent({ purpose: "activate", data: frozen });
    await this.restart(activation.sha256);
    await activate(this.transport(activation.token), frozen);
    this.binding = { identity: frozen.identity, serving_epoch: frozen.serving_epoch + 1 };
    this.drain = mintControlIntent({ purpose: "drain", data: this.binding });
    await this.restart(this.drain.sha256);
    this.facts.push({ operation: "provisioned", binding: this.binding, canonical_records: (await this.inventory()).length });
  }
  current(): Readonly<{ binding: Binding; transport: Transport }> {
    assert(this.binding && this.drain);
    return { binding: this.binding, transport: this.transport(this.drain.token) };
  }
  async exportFrozen(reference: FrozenRef): Promise<Readonly<{ path: string; sha256: string; records: number }>> {
    const before = await this.inventory();
    const intent = mintIntent({ kind: "export" });
    await this.restart(intent.sha256);
    const { exportPages } = await import("../state-transfer.ts");
    const exported = await exportPages(this.transport(intent.token));
    const rows = Buffer.from(exported.archive)
      .toString("utf8")
      .trimEnd()
      .split("\n")
      .flatMap((line) => {
        const page: unknown = JSON.parse(line);
        assert(Array.isArray(page));
        return page;
      });
    assert.equal(canonical(rows), canonical(before), "Latest frozen export differs from exact canonical records");
    assert.equal(canonical(exported.frozen_ref), canonical(reference));
    const path = join(this.directory, "latest-frozen.private.ndjson");
    await writeFile(path, exported.archive, { flag: "wx", mode: 0o600 });
    this.facts.push({ operation: "latest_frozen_export", records: rows.length, archive_sha256: exported.sha256, frozen_ref: reference });
    return { path, sha256: exported.sha256, records: rows.length };
  }
}
