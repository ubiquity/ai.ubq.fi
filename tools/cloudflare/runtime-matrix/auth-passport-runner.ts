/** Auth-only compiled Worker runner. Prepared artifacts/tools only; no installers,
 * fake auth endpoint, direct row writer, live Passport call or provider dispatch. */
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import { mkdir, mkdtemp, readFile, realpath, writeFile } from "node:fs/promises";
import { dirname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { testNetworkArgv } from "../../../scripts/test-network.ts";
import { runAuthPassportCases, runAuthPassportDateCases, runAuthPassportAdditionalCases, type AuthPassportHarness } from "./auth-passport.ts";
import { observePassportDates } from "../fixtures/auth-passport-date-oracle.mjs";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../../..");
const inner = process.argv[2] === "--isolated-child";
const offset = inner ? 3 : 2;
const expectedSha = process.argv[offset];
const artifacts = process.argv[offset + 1];
assert(expectedSha && /^[a-f0-9]{40}$/u.test(expectedSha) && artifacts, "Pass the exact compiled SHA and prepared artifact directory.");
assert.equal(Bun.version, "1.4.0");
assert.equal(process.platform, "linux", "This portable runner requires the actual Linux namespace boundary.");
const target = await realpath(process.env.CARGO_TARGET_DIR ?? "");
const owner: unknown = JSON.parse(await readFile(join(target, ".uos-worktree-owner.json"), "utf8"));
assert(object(owner).sourceRoot === join(root, "rust"), "The actual source worktree must own this target.");
const application = await realpath(artifacts);
assert(application.startsWith(target + sep), "Prepared first-party Worker artifacts must stay under this worktree's overlay target.");
const require = createRequire(import.meta.url);
const workerd: unknown = require("workerd");
const binary = object(workerd).default;
assert(typeof binary === "string");
const tool: unknown = JSON.parse(await readFile(join(root, "node_modules/workerd/package.json"), "utf8"));
assert.equal(object(tool).version, "1.20261012.1");
const digest = (bytes: Uint8Array): string => createHash("sha256").update(bytes).digest("hex");
function object(value: unknown): Record<string, unknown> {
  assert(value !== null && typeof value === "object" && !Array.isArray(value));
  return value as Record<string, unknown>;
}
async function config(workspace: string): Promise<string> {
  await mkdir(join(workspace, "state"), { recursive: true });
  const quoted = JSON.stringify;
  const document = `using Workerd = import ${quoted(join(root, "node_modules/workerd/workerd.capnp"))};
const config :Workerd.Config = (
  services = [
    (name = "internet", network = (allow = [])),
    (name = "state", disk = (path = ${quoted(join(workspace, "state"))}, writable = true)),
    (name = "assets", disk = (path = ${quoted(join(root, "static"))})),
    (name = "gateway", worker = (
      modules = [(name = "index.js", esModule = embed ${quoted(join(application, "index.js"))}), (name = "index_bg.wasm", wasm = embed ${quoted(join(application, "index_bg.wasm"))})],
      compatibilityDate = "2026-10-18", compatibilityFlags = ["enable_request_signal", "request_signal_passthrough"],
      globalOutbound = "driver",
      bindings = [(name = "AUTHORITY", durableObjectNamespace = (className = "GatewayAuthority")), (name = "ASSETS", service = "assets"),
        (name = "UOS_AI_PASSPORT_CLIENT_ID", text = "ai"), (name = "UOS_AI_PASSPORT_AUDIENCE", text = "ai"),
        (name = "UOS_AI_PASSPORT_ORIGIN", text = "https://ai.ubq.fi"), (name = "UOS_AI_PASSPORT_REDIRECT_URI", text = "https://ai.ubq.fi/auth/callback")],
      durableObjectNamespaces = [(className = "GatewayAuthority", uniqueKey = "uos-auth-passport-common-v2", enableSql = true)],
      durableObjectStorage = (localDisk = "state")
    )),
    (name = "driver", worker = (
      modules = [(name = "auth-passport-driver.mjs", esModule = embed ${quoted(join(root, "tools/cloudflare/fixtures/auth-passport-driver.mjs"))}),
        (name = "auth-passport-upstream.mjs", esModule = embed ${quoted(join(root, "tools/cloudflare/fixtures/auth-passport-upstream.mjs"))})],
      compatibilityDate = "2026-10-18", compatibilityFlags = ["enable_request_signal", "request_signal_passthrough"], globalOutbound = "internet",
      bindings = [(name = "AUTHORITY", durableObjectNamespace = (className = "GatewayAuthority", serviceName = "gateway")), (name = "CONTROL", durableObjectNamespace = (className = "AuthPassportControl"))],
      durableObjectNamespaces = [(className = "AuthPassportControl", uniqueKey = "uos-auth-passport-control-v2", enableSql = true)],
      durableObjectStorage = (localDisk = "state")
    ))
  ],
  sockets = [(name = "control", http = (), service = "driver"), (name = "public", http = (), service = "gateway")]
);`;
  const path = join(workspace, "auth-passport.capnp");
  await writeFile(path, document);
  return path;
}
async function reserve(): Promise<number> {
  const server = Bun.serve({ hostname: "127.0.0.1", port: 0, fetch: () => new Response(null, { status: 503 }) });
  const port = server.port;
  assert(typeof port === "number");
  await server.stop(true);
  return port;
}
type Running = Readonly<{ origin: string; controlOrigin: string; stop(): Promise<void> }>;
async function start(workspace: string, path: string): Promise<Running> {
  const port = await reserve();
  const controlPort = await reserve();
  const id = Date.now();
  const child = Bun.spawn([binary as string, "serve", path, "--experimental", "--import-path", "/", "--socket-addr", `public=127.0.0.1:${port}`, "--socket-addr", `control=127.0.0.1:${controlPort}`], {
    cwd: workspace, env: { PATH: process.env.PATH ?? "/usr/bin:/bin", TMPDIR: workspace },
    stdout: Bun.file(join(workspace, `workerd-${id}.stdout.log`)), stderr: Bun.file(join(workspace, `workerd-${id}.stderr.log`)),
  });
  const running = {
    origin: `http://127.0.0.1:${port}`, controlOrigin: `http://127.0.0.1:${controlPort}`,
    async stop(): Promise<void> {
      if (child.exitCode !== null) return;
      child.kill("SIGTERM");
      const timer = setTimeout(() => { if (child.exitCode === null) child.kill("SIGKILL"); }, 3000);
      try { await child.exited; } finally { clearTimeout(timer); }
    },
  };
  try {
    const until = Date.now() + 10_000;
    while (Date.now() < until) {
      assert.equal(child.exitCode, null, "The actual workerd exited during startup.");
      let response: Response | undefined;
      try { response = await fetch(`${running.origin}/health`); } catch { /* bounded process startup */ }
      if (response?.status === 200) {
        const body = object(await response.json());
        assert.equal(object(body.release).git_sha, expectedSha);
        assert.equal(response.headers.get("x-uos-git-sha"), expectedSha);
        return running;
      }
      await response?.arrayBuffer();
      await Bun.sleep(20);
    }
    throw new Error("Compiled auth Worker did not become ready.");
  } catch (error) { await running.stop(); throw error; }
}
async function execute(workspace: string): Promise<void> {
  const source = Bun.spawnSync(["git", "rev-parse", "HEAD", "HEAD^{tree}"], { cwd: root });
  assert.equal(source.exitCode, 0);
  const [head, tree] = source.stdout.toString().trim().split("\n");
  assert.equal(head, expectedSha);
  const status = Bun.spawnSync(["git", "status", "--porcelain"], { cwd: root });
  assert.equal(status.exitCode, 0); assert.equal(status.stdout.toString(), "", "The compiled-source worktree must be clean.");
  const path = await config(workspace);
  let running = await start(workspace, path);
  const cases: string[] = [];
  const control = async (body: unknown): Promise<void> => {
    const response = await fetch(`${running.controlOrigin}/auth-passport/control`, { method: "POST", body: JSON.stringify(body), headers: { "content-type": "application/json" } });
    assert.equal(response.status, 204); await response.arrayBuffer();
  };
  const context: AuthPassportHarness = {
    cases,
    request(path, init = {}) {
      const headers = new Headers(init.headers);
      // Connect only to the namespace loopback; preserve Caddy's fixed production Host semantics.
      headers.set("host", "ai.ubq.fi"); headers.set("x-forwarded-proto", "https");
      return fetch(`${running.origin}${path}`, { ...init, headers, redirect: "manual" });
    },
    mode: (value) => control({ operation: "mode", value }),
    dateCase: (value) => control(value === null ? { operation: "created_at_reset" } : { operation: "created_at", value }),
    async readSource(key) {
      const response = await fetch(`${running.controlOrigin}/auth-passport/read`, { method: "POST", body: JSON.stringify({ key }), headers: { "content-type": "application/json" } });
      assert.equal(response.status, 200);
      const record = object(await response.json()).record;
      if (record === null) return null;
      const row = object(record);
      assert(typeof row.revision === "string" && (row.expires_at_ms === null || typeof row.expires_at_ms === "number") && typeof row.value_encoding === "string");
      return { revision: row.revision, expires_at_ms: row.expires_at_ms, value: row.value, value_encoding: row.value_encoding };
    },
    async restart() { await running.stop(); running = await start(workspace, path); },
    holdIntrospection: () => control({ operation: "hold" }), waitForIntrospection: () => control({ operation: "wait_entered" }), releaseIntrospection: () => control({ operation: "release" }),
    holdRevoke: () => control({ operation: "hold_revoke" }), waitForRevoke: () => control({ operation: "wait_revoke" }), releaseRevoke: () => control({ operation: "release_revoke" }),
  };
  const oracle = observePassportDates();
  try {
    await runAuthPassportCases(context);
    await runAuthPassportAdditionalCases(context);
    await runAuthPassportDateCases(context, oracle);
    const expected = ["auth_passport_pkce_real_rs256_durable_common_authority_and_repeated_cookies", "auth_passport_existing_authority_survives_worker_restart", "auth_passport_central_role_demotion_is_immediate", "auth_passport_503_preserves_authority_and_recovers_without_login", "auth_passport_authoritative_401_deletes_even_with_rejected_body_cancellation", "auth_passport_logout_wins_over_inflight_introspection_without_republication", "auth_passport_atomic_replacement_and_local_first_best_effort_logout", "auth_passport_transient_exchange_failures_preserve_previous_authority", "auth_passport_utf16_subject_handle_and_profile_v2_record", "auth_passport_date_parse_source_oracle_and_exact_created_at"];
    assert.deepEqual(cases, expected);
    const receipt = { status: "controlled_component_only", source: { head, tree }, platform: process.platform, arch: process.arch, bun: Bun.version, workerd: "1.20261012.1", cases, dateOracle: oracle, binarySha256: digest(await readFile(binary as string)), indexSha256: digest(await readFile(join(application, "index.js"))), wasmSha256: digest(await readFile(join(application, "index_bg.wasm"))), limitation: "No Mac/native/browser/client acceptance, deployment or live Passport effect is established." };
    await writeFile(join(workspace, "receipt.json"), JSON.stringify(receipt, null, 2) + "\n");
    console.log(JSON.stringify({ evidence: workspace, ...receipt }));
  } finally { await running.stop(); }
}
if (inner) {
  const workspace = process.argv[offset + 2]; assert(workspace);
  await execute(workspace);
} else {
  const workspace = await mkdtemp(join(target, "auth-passport-conformance-"));
  const command = testNetworkArgv([process.execPath, "--no-env-file", "--no-install", fileURLToPath(import.meta.url), "--isolated-child", expectedSha, application, workspace]);
  const child = Bun.spawn(command, { cwd: root, env: { PATH: process.env.PATH ?? "/usr/bin:/bin", TMPDIR: workspace, CARGO_TARGET_DIR: target, BUN_RUNTIME_TRANSPILER_CACHE_PATH: "0" }, stdout: "pipe", stderr: "pipe" });
  const stop = (): void => { if (child.exitCode === null) child.kill("SIGTERM"); };
  process.once("SIGINT", stop); process.once("SIGTERM", stop);
  const timeout = setTimeout(stop, 90_000);
  try {
    const [exit, stdout, stderr] = await Promise.all([child.exited, new Response(child.stdout).text(), new Response(child.stderr).text()]);
    await writeFile(join(workspace, "stdout.log"), stdout); await writeFile(join(workspace, "stderr.log"), stderr);
    assert.equal(exit, 0, `Actual compiled auth Worker conformance failed; evidence: ${workspace}`);
    console.log(stdout.trim());
  } finally { clearTimeout(timeout); process.off("SIGINT", stop); process.off("SIGTERM", stop); stop(); }
}
