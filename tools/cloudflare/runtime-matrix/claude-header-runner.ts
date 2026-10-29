/** Standalone actual-Rust header-meter fixture. Prepared artifacts only; never deploy. */
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import { createServer } from "node:net";
import { lstat, mkdtemp, readFile, realpath, writeFile } from "node:fs/promises";
import { dirname, isAbsolute, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { testNetworkArgv } from "../../../scripts/test-network.ts";
import { MATRIX_MODELS } from "../fixtures/provider-matrix.mjs";
import { assertSingleAdmission, createClient, object, post, responseObject, selectProviders, settled, stringKey, type MatrixContext } from "./contracts.ts";
import { progressiveBody } from "./direct.ts";
import { fixtureConfig } from "./workerd-config.ts";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../../..");
const SELF = fileURLToPath(import.meta.url);
const OAUTH_FIXTURE = "sk-ant-oat-fixture-header-meter";
const ATTEMPT = "claude_header_upstream_attempt";
const INPUTS = [
  "tools/cloudflare/runtime-matrix/claude-header-runner.ts",
  "tools/cloudflare/fixtures/claude-header-driver.mjs",
  "tools/cloudflare/fixtures/runtime-driver.mjs",
  "tools/cloudflare/fixtures/provider-matrix.mjs",
  "tools/cloudflare/runtime-matrix/workerd-config.ts",
  "tools/cloudflare/runtime-matrix/contracts.ts",
  "tools/cloudflare/runtime-matrix/direct.ts",
  "scripts/test-network.ts",
  "static/admin.html",
  "static/admin.js",
  "static/admin.css",
  "static/auth.js",
  "package.json",
  "bun.lock",
  "rust/Cargo.lock",
];
type Running = { origin: string; publicOrigin: string; stop: () => Promise<void> };
type Account = { account_id: string; account_cohort_id: string };
type Session = { current: Running; config: string; workspace: string; binary: string; cases: string[] };
type Provenance = { sha: string; tree: string; target: string; hashes: Record<string, string>; workerd: string };

const hash = (bytes: Uint8Array): string => createHash("sha256").update(bytes).digest("hex");
const git = (...args: string[]): string => {
  const result = Bun.spawnSync(["/usr/bin/git", ...args], {
    cwd: ROOT,
    env: { PATH: "/usr/bin:/bin", GIT_CONFIG_GLOBAL: "/dev/null", GIT_CONFIG_NOSYSTEM: "1" },
  });
  assert.equal(result.exitCode, 0, "Read-only source identity command failed.");
  return result.stdout.toString().trim();
};
async function hashes(paths: readonly string[]): Promise<Record<string, string>> {
  return Object.fromEntries(await Promise.all(paths.map(async (path) => [path, hash(await readFile(path))])));
}
function frozen(sha: string, tree: string): void {
  assert.equal(git("rev-parse", "HEAD"), sha);
  assert.equal(git("rev-parse", "HEAD^{tree}"), tree);
  assert.equal(git("status", "--porcelain", "--untracked-files=all"), "", "The committed runtime source must remain frozen.");
}
async function provenance(sha: string, pairPath: string): Promise<Provenance> {
  assert.match(sha, /^[a-f0-9]{40}$/u);
  const tree = git("rev-parse", "HEAD^{tree}");
  frozen(sha, tree);
  const pair = object(JSON.parse(await readFile(pairPath, "utf8")));
  assert.equal(pair.sha, sha);
  assert.equal(pair.tree, tree);
  assert.equal(await realpath(String(pair.root)), await realpath(ROOT));
  const target = await realpath(String(pair.target));
  const owner = object(JSON.parse(await readFile(join(target, ".uos-worktree-owner.json"), "utf8")));
  assert.equal(owner.version, 1);
  assert.equal(owner.sourceRoot, await realpath(join(ROOT, "rust")));
  const artifacts: string[] = [];
  for (const role of ["shipping", "fixture"]) {
    const entries = pair[role];
    assert(Array.isArray(entries) && entries.length === 2, "A complete same-source shipping/fixture pair is required.");
    const rows = entries.map(object);
    assert.deepEqual(
      rows.map((row) => String(row.file)).sort((left, right) => left.localeCompare(right)),
      ["index.js", "index_bg.wasm"]
    );
    for (const row of rows) {
      const packageName = role === "shipping" ? "gateway-worker" : "gateway-worker-fixture";
      const source = join(ROOT, "rust/crates", packageName, "build", String(row.file));
      assert.equal(await realpath(String(row.source)), await realpath(source));
      const stat = await lstat(source);
      assert(stat.isFile() && !stat.isSymbolicLink() && stat.size > 0);
      const bytes = await readFile(source);
      assert.equal(hash(bytes), row.sha256);
      assert.equal(hash(await readFile(String(row.preserved))), row.sha256);
      if (row.file === "index_bg.wasm") {
        assert(WebAssembly.validate(bytes));
        assert(bytes.includes(Buffer.from(`cloudflare-${sha}`)), "WASM must embed the actual full source identity.");
      }
      artifacts.push(source);
    }
  }
  const require = createRequire(import.meta.url);
  const binary = object(require("workerd")).default;
  assert.equal(typeof binary, "string");
  const pkg = object(JSON.parse(await readFile(join(ROOT, "node_modules/workerd/package.json"), "utf8")));
  assert.equal(pkg.version, "1.20261012.1");
  return {
    sha,
    tree,
    target,
    workerd: binary as string,
    hashes: await hashes([...INPUTS.map((path) => join(ROOT, path)), pairPath, binary as string, ...artifacts]),
  };
}

async function config(workspace: string): Promise<string> {
  const path = await fixtureConfig(ROOT, workspace, "claude");
  const original = await readFile(path, "utf8");
  const base = join(ROOT, "tools/cloudflare/fixtures/runtime-driver.mjs");
  const wrapper = join(ROOT, "tools/cloudflare/fixtures/claude-header-driver.mjs");
  const needle = `(name = "driver.mjs", esModule = embed ${JSON.stringify(base)})`;
  assert.equal(original.split(needle).length, 2, "The existing fixture module registration must match exactly.");
  const replacement = `(name = "driver.mjs", esModule = embed ${JSON.stringify(wrapper)}), (name = "runtime-driver.mjs", esModule = embed ${JSON.stringify(base)})`;
  await writeFile(path, original.replace(needle, replacement));
  return path;
}
async function reservePort(): Promise<number> {
  const server = createServer();
  await new Promise<void>((done, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", done);
  });
  const address = server.address();
  assert(address && typeof address !== "string");
  await new Promise<void>((done, reject) =>
    server.close((error) => {
      if (error) reject(error);
      else done();
    })
  );
  return address.port;
}
async function start(session: Omit<Session, "current">): Promise<Running> {
  const port = await reservePort();
  const publicPort = await reservePort();
  const prefix = join(session.workspace, `workerd-${Date.now()}`);
  const child = Bun.spawn(
    [
      session.binary,
      "serve",
      session.config,
      "--experimental",
      "--import-path",
      "/",
      "--socket-addr",
      `main=127.0.0.1:${port}`,
      "--socket-addr",
      `public=127.0.0.1:${publicPort}`,
    ],
    { cwd: session.workspace, stdout: Bun.file(`${prefix}.stdout`), stderr: Bun.file(`${prefix}.stderr`) }
  );
  const stop = async (): Promise<void> => {
    if (child.exitCode !== null) return;
    child.kill("SIGTERM");
    const escalation = setTimeout(() => {
      if (child.exitCode === null) child.kill("SIGKILL");
    }, 3000);
    try {
      await child.exited;
    } finally {
      clearTimeout(escalation);
    }
  };
  const running = { origin: `http://127.0.0.1:${port}`, publicOrigin: `http://127.0.0.1:${publicPort}`, stop };
  try {
    // The CLI has no ready IPC endpoint. These bounded startup retries stop as
    // soon as its real HTTP socket accepts; they do not reconcile product state.
    for (const delay of [0, 20, 40, 80, 160, 320, 640, 1280, 2560]) {
      if (delay) await Bun.sleep(delay);
      assert.equal(child.exitCode, null, "workerd exited before readiness.");
      try {
        if ((await fetch(`${running.publicOrigin}/health`)).ok) return running;
      } catch {
        /* socket not listening yet */
      }
    }
    throw new Error("The real workerd HTTP socket did not become ready.");
  } catch (error) {
    await stop();
    throw error;
  }
}
async function json(response: Response, status = 200): Promise<Record<string, unknown>> {
  assert.equal(response.status, status, "Unexpected fixture HTTP status; response secrets are not logged.");
  return object(await response.json());
}
function context(session: Session): MatrixContext {
  const url = (path: string): string => session.current.publicOrigin + path;
  const rpc = async (body: unknown): Promise<Record<string, unknown>> => {
    assert.equal(object(body).operation, "read", "This fixture has no generic row-write authority.");
    return json(
      await fetch(session.current.origin + "/store", { method: "POST", body: JSON.stringify(body), headers: { "content-type": "application/json" } })
    );
  };
  const read = async (key: string): Promise<Record<string, unknown> | null> => {
    const value = (await rpc({ operation: "read", key })).record;
    assert(value === null || (typeof value === "object" && !Array.isArray(value)));
    return value === null ? null : object(value);
  };
  return {
    url,
    rpc,
    read,
    cases: session.cases,
    post: (path, body, token = "fixture-access", signal) =>
      fetch(url(path), {
        method: "POST",
        body: JSON.stringify(body),
        signal,
        headers: { "content-type": "application/json", authorization: `Bearer ${token}`, connection: "close" },
      }),
    events: async () => {
      const response = await fetch(session.current.origin + "/control");
      assert.equal(response.status, 200);
      const value: unknown = await response.json();
      assert(Array.isArray(value));
      return value;
    },
    lifecycle: async (id) => {
      // The existing read-only store fixture exposes no settlement subscription.
      // Retain its bounded three-second visibility wait, at most 31 reads.
      for (let attempt = 0; attempt < 31; attempt += 1) {
        const row = await read(stringKey("uos_ai", "routing_runtime", "v1", "session", id));
        if (row && object(row.value).finished === true) return object(row.value);
        if (attempt < 30) await Bun.sleep(100);
      }
      throw new Error("The original Rust admission did not settle within the existing fixture bound.");
    },
  };
}
const adminHeaders = { authorization: "Bearer fixture-admin", connection: "close" };
async function capacity(ctx: MatrixContext): Promise<Record<string, unknown>> {
  // Setup discovery finishes before this helper; every cold/restarted/switched read gets its own baseline.
  const before = await attempts(ctx);
  const response = await fetch(ctx.url("/admin/providers/capacity"), { headers: adminHeaders });
  const snapshot = await json(response);
  assert.deepEqual(await attempts(ctx), before, "Every capacity read must add zero audited outbound attempts, including the first read.");
  assert.equal(object(snapshot.claude).quota_source, "response_headers", "Current Claude capacity must identify its header-only projection.");
  return snapshot;
}
async function attempts(ctx: MatrixContext): Promise<Record<string, unknown>[]> {
  return (await ctx.events()).map(object).filter((event) => event.kind === ATTEMPT);
}
async function assertNoOauth(ctx: MatrixContext): Promise<void> {
  assert.equal((await attempts(ctx)).filter((event) => event.category === "oauth_usage").length, 0, "Even refused OAuth usage attempts are forbidden.");
}
// This no-click group uses synthetic programmatic auth; Passport transport is not part of its scenario.
const EXPECTED_CLAUDE_OUTBOUND = new Map<string, string>([
  ["GET api.anthropic.com/v1/models", "models"],
  ["GET api.anthropic.com/v1/organizations/workspaces", "workspaces"],
  ["POST api.anthropic.com/v1/messages", "inference"],
]);
async function assertExpectedOutboundAttempts(ctx: MatrixContext): Promise<void> {
  for (const event of await attempts(ctx)) {
    const expected = EXPECTED_CLAUDE_OUTBOUND.get(`${event.method} ${event.hostname}${event.pathname}`);
    assert(expected, "Unexpected audited outbound destination/method, including a refused attempt.");
    assert.equal(event.category, expected, "The audited endpoint must have its actual scenario category.");
  }
  await assertNoOauth(ctx);
}
function windowOf(snapshot: Record<string, unknown>, name: string): Record<string, unknown> | null {
  const value = object(object(snapshot.claude).windows)[name];
  return value === undefined || value === null ? null : object(value);
}
function points(snapshot: Record<string, unknown>): Record<string, unknown>[] {
  const value = object(snapshot.claude_history).points;
  assert(Array.isArray(value));
  return value.map(object);
}
function passiveProjection(snapshot: Record<string, unknown>): unknown {
  const source = object(snapshot.claude);
  return {
    account: source.account_id,
    cohort: source.account_cohort_id,
    windows: source.windows,
    standard: source.standard,
    observed: source.source_observed_at_ms,
    usage: source.usage,
    snapshot: source.usage_snapshot,
    points: points(snapshot),
  };
}
function expectedReset(events: Record<string, unknown>[], name: string): number {
  const emitted = events.toReversed().find((event) => typeof object(event.headers)[name] === "string");
  assert(emitted, `The controlled upstream must actually send ${name}.`);
  const raw = object(emitted.headers)[name];
  assert(typeof raw === "string" && /^\d+$/u.test(raw), "Reset headers must be integer Unix seconds, never ISO snapshot dates.");
  const seconds = Number(raw);
  assert(Number.isSafeInteger(seconds) && seconds > 0 && Number.isSafeInteger(seconds * 1000));
  return seconds * 1000;
}
async function assertResetMapping(ctx: MatrixContext, snapshot: Record<string, unknown>): Promise<void> {
  const emitted = (await ctx.events()).map(object).filter((event) => event.kind === "claude_header_response");
  for (const [family, name] of [
    ["five_hour", "anthropic-ratelimit-unified-5h-reset"],
    ["seven_day", "anthropic-ratelimit-unified-7d-reset"],
  ]) {
    assert.equal(windowOf(snapshot, family)?.reset_at_ms, expectedReset(emitted, name), `${family} reset must map exactly from sent seconds to milliseconds.`);
  }
  const standard = object(snapshot.claude).standard;
  assert(Array.isArray(standard));
  const requests = standard.map(object).find((row) => row.family === "requests");
  assert(requests, "The controlled standard requests family must be present.");
  assert.equal(requests.limit, 100);
  assert.equal(requests.remaining, 70);
  assert.equal(requests.reset_at_ms, expectedReset(emitted, "anthropic-ratelimit-requests-reset"));
}
async function headerView(ctx: MatrixContext, snapshot: Record<string, unknown>, account: Account, five: number, seven: number): Promise<void> {
  const source = object(snapshot.claude);
  assert.equal(source.account_id, account.account_id);
  assert.equal(source.account_cohort_id, account.account_cohort_id);
  assert.equal(windowOf(snapshot, "five_hour")?.used_percent, five);
  assert.equal(windowOf(snapshot, "seven_day")?.used_percent, seven);
  assert.equal(source.usage, null);
  assert(source.usage_snapshot === undefined || source.usage_snapshot === null);
  assert(typeof source.source_observed_at_ms === "number" && source.source_observed_at_ms > 0);
  assert(points(snapshot).some((point) => point.account_id === account.account_id && point.source === "response_headers"));
  await assertResetMapping(ctx, snapshot);
}
async function account(ctx: MatrixContext, label: string): Promise<Account> {
  const configured = await responseObject(
    await ctx.post("/admin/providers/claude-code/auth", { credential: OAUTH_FIXTURE, create_new: true, activate: true, label }, "fixture-admin")
  );
  assert.equal(typeof configured.account_id, "string");
  const source = object((await capacity(ctx)).claude);
  assert.equal(source.account_id, configured.account_id);
  assert.equal(source.credential_kind, "oauth", "An API-key account would not exercise the retired OAuth collector.");
  assert.equal(typeof source.account_cohort_id, "string");
  return { account_id: source.account_id as string, account_cohort_id: source.account_cohort_id as string };
}
async function stored(ctx: MatrixContext, owner: Account): Promise<unknown> {
  return Promise.all([
    ctx.read(stringKey("uos_ai", "claude_code_quota", "v1", owner.account_id, owner.account_cohort_id)),
    ctx.read(stringKey("uos_ai", "claude_code_usage", "v1", "lease", owner.account_id, owner.account_cohort_id)),
  ]);
}
async function passiveReads(ctx: MatrixContext, owner: Account, before: Record<string, unknown>): Promise<void> {
  const calls = await attempts(ctx);
  const records = await stored(ctx, owner);
  for (let count = 0; count < 3; count += 1) assert.deepEqual(passiveProjection(await capacity(ctx)), passiveProjection(before));
  const manual = await responseObject(await ctx.post("/admin/providers/claude-code/quota/refresh", owner, "fixture-admin"));
  assert.equal(manual.refreshed, false);
  assert.equal(manual.reason, "response_headers");
  assert.equal(manual.usage, null);
  assert.equal(manual.observed_at_ms, object(before.claude).source_observed_at_ms);
  assert.deepEqual(await attempts(ctx), calls, "Passive/cached/manual reads must add zero outbound requests.");
  assert.deepEqual(await stored(ctx, owner), records, "Reads must not rewrite quota, create samples or renew a collector lease.");
  assert.deepEqual(passiveProjection(await capacity(ctx)), passiveProjection(before));
  await assertNoOauth(ctx);
}
async function infer(ctx: MatrixContext, mode: string, stream = false): Promise<void> {
  const response = await post(ctx, "/v1/responses", { model: MATRIX_MODELS.claude, input: `fixture:matrix:claude:${mode}`, stream });
  if (mode === "quota") {
    assert.notEqual(response.status, 200);
    await response.arrayBuffer();
  } else if (stream) await progressiveBody(response);
  else assert.equal((await responseObject(response)).status, "completed");
  assertSingleAdmission(await settled(ctx, response), "claude_code");
  await assertNoOauth(ctx);
}

async function normalCases(ctx: MatrixContext, owner: Account): Promise<void> {
  const unknown = await capacity(ctx);
  assert.equal(object(unknown.claude).source_observed_at_ms, null);
  assert.equal(windowOf(unknown, "five_hour")?.used_percent ?? null, null);
  await passiveReads(ctx, owner, unknown);
  ctx.cases.push("claude_header_passive_unknown_reads_make_zero_upstream_calls");
  await infer(ctx, "headers-json");
  const first = await capacity(ctx);
  await headerView(ctx, first, owner, 40, 20);
  ctx.cases.push("claude_header_all_reset_families_match_emitted_unix_seconds");
  await passiveReads(ctx, owner, first);
  ctx.cases.push("claude_header_ordinary_json_updates_quota_history_and_passive_cache");
  await infer(ctx, "headers-sse", true);
  const streamed = await capacity(ctx);
  await headerView(ctx, streamed, owner, 40, 20);
  ctx.cases.push("claude_header_ordinary_sse_retains_one_admission_and_settlement");
  await infer(ctx, "headers-partial");
  const partial = await capacity(ctx);
  await headerView(ctx, partial, owner, 60, 20);
  assert.equal(windowOf(partial, "seven_day")?.observed_at_ms, windowOf(streamed, "seven_day")?.observed_at_ms);
  assert.deepEqual(object(partial.claude).standard, object(streamed.claude).standard);
  await infer(ctx, "headers-none");
  const headerless = await capacity(ctx);
  await headerView(ctx, headerless, owner, 60, 20);
  assert.deepEqual(object(headerless.claude).windows, object(partial.claude).windows);
  assert.equal(object(headerless.claude).source_observed_at_ms, object(partial.claude).source_observed_at_ms);
  ctx.cases.push("claude_header_partial_and_headerless_preserve_family_observation_times");
  await infer(ctx, "quota");
  const refusal = await capacity(ctx);
  await headerView(ctx, refusal, owner, 60, 25);
  assert.equal(windowOf(refusal, "five_hour")?.observed_at_ms, windowOf(partial, "five_hour")?.observed_at_ms);
  await passiveReads(ctx, owner, refusal);
  ctx.cases.push("claude_header_ordinary_refusal_keeps_independent_family_evidence");
}
async function servedAssets(ctx: MatrixContext): Promise<void> {
  const response = await fetch(ctx.url("/admin"));
  assert.equal(response.status, 200);
  const html = await response.text();
  assert.equal(hash(Buffer.from(html)), hash(await readFile(join(ROOT, "static/admin.html"))), "The actual Rust asset route must serve the committed UI.");
  const begin = html.indexOf('id="admin-auth-gate"');
  const gate = html.slice(begin, html.indexOf('id="admin-loading-grid"', begin));
  assert(begin > 0);
  assert.equal((gate.match(/<button\b/gu) ?? []).length, 1);
  assert(gate.includes('id="admin-passport-login"') && gate.includes("data-passport-signin"));
  assert(html.includes('id="admin-auth-compat" hidden aria-hidden="true"'));
  const script = await fetch(ctx.url("/admin.js"));
  assert.equal(script.status, 200);
  const bytes = await script.text();
  assert.equal(hash(Buffer.from(bytes)), hash(await readFile(join(ROOT, "static/admin.js"))));
  assert(!/capacityClaudeFilter|selectedClaudeHistoryAccountId|renderClaudeHistorySelector/u.test(bytes));
  ctx.cases.push("claude_header_actual_rust_served_passport_assets_without_history_dropdown");
}
async function restartCase(session: Session, ctx: MatrixContext, owner: Account): Promise<void> {
  const before = await capacity(ctx);
  const records = await stored(ctx, owner);
  const calls = await attempts(ctx);
  await session.current.stop();
  session.current = await start(session);
  assert.deepEqual(passiveProjection(await capacity(ctx)), passiveProjection(before));
  assert.deepEqual(await stored(ctx, owner), records);
  assert.deepEqual(await attempts(ctx), calls, "Restarted passive reads cannot dispatch or refresh credentials.");
  await assertNoOauth(ctx);
  ctx.cases.push("claude_header_same_state_restart_preserves_evidence_without_oauth");
}
async function accountHistory(ctx: MatrixContext, clientId: string): Promise<Account> {
  const before = await capacity(ctx);
  const previous = object(before.claude).account_id;
  const owner = await account(ctx, "controlled header B");
  assert.notEqual(owner.account_id, previous);
  await responseObject(await ctx.post("/admin/providers/claude-code/access", { allowed_key_ids: [clientId] }, "fixture-admin"));
  const unknown = await capacity(ctx);
  assert.equal(object(unknown.claude).source_observed_at_ms, null);
  await infer(ctx, "headers-account-b");
  const switched = await capacity(ctx);
  await headerView(ctx, switched, owner, 10, 20);
  assert(
    points(switched).some((point) => point.account_id === previous),
    "Switching the active account must retain earlier history."
  );
  await passiveReads(ctx, owner, switched);
  ctx.cases.push("claude_header_active_account_changes_without_deleting_retained_history");
  return owner;
}
function browserChecks(owner: Account, initialOwner: Account): Record<string, unknown> {
  return {
    expectedActiveAccount: owner.account_id,
    expectedActiveCohort: owner.account_cohort_id,
    expectedAccountSwitch: { initial: initialOwner, current: owner },
    expectedFiveHourUsedPercent: 10,
    expectedFiveHourRemainingPercent: 90,
    browserAccepted: false,
    serverHoldOnly: true,
    automatedBrowserAssertionsExecuted: false,
    signedOutExpression: `(() => { const visible = e => !!e.getClientRects().length && getComputedStyle(e).visibility !== 'hidden'; const compatibility = document.querySelector('#admin-auth-compat'); return { passportButtons: [...document.querySelectorAll('[data-passport-signin]')].filter(visible).length, compatibilityHidden: compatibility?.hidden === true && compatibility.getAttribute('aria-hidden') === 'true', historyDropdownAbsent: !document.querySelector('[data-capacity-claude-filter], select[aria-label="Claude history account (view only)"]') }; })()`,
    requiredBrowserScenarios: [
      "rust_served_passport_and_active_account",
      "v2_indexeddb_unmarked_98_percent_fresh_capacity_failure",
      "marked_cached_projection_positive_control",
    ],
    required: [
      "A server hold or browser-ready file is not browser acceptance; retain separate executed DOM/network evidence.",
      "Use an isolated task browser with no real credentials/cookies; never click Passport or perform sign-in.",
      "On actual Rust-served assets after initialization, assert one visible Passport action and hidden compatibility nodes before/after panel expansion, on desktop/mobile.",
      "Use only existing synthetic programmatic admin auth and real Rust account activation: render initial account A, switch to B, refresh through existing UI lifecycle, and verify the active account/cohort label, header times and actual history without mixing A/B.",
      "Current B five-hour measurement is 10% used / 90% remaining; do not confuse consumed and remaining capacity.",
      "Assert absent [data-capacity-claude-filter] and the history-account ARIA select; retain the legitimate provider-account configuration selector.",
      "Take the existing controlled attempt baseline before browser passive actions and compare after: no OAuth, inference, discovery or refresh attempts, including refused attempts.",
      "V2 negative: seed the actual scoped IndexedDB format/key using existing admin-cache helpers with an unmarked legacy Claude98% window/time/standard/current-row contribution, alongside Codex cache and proven header history; fail only the fresh capacity read, then execute actual hydration/render.",
      "Assert legacy Claude measurement/time/standard/current-row series is absent or awaiting data after failed fetch, while Codex and proven header history remain; do not rewrite the old cached row as proven.",
      "Marked positive control: cache an actual marked Rust response and verify its values/timestamps/history still render with the fresh read failed; restore interception afterward.",
      "All assets and successful API payloads must remain actual Rust responses; no mocked successful capacity response or generic server row writer proves this gate.",
    ],
    remainingNativeOrBrowserProof: [
      "Exact first accepted-history count/cohort and raw durable-history revision/write stability need the defining existing typed read/native evidence; current view equality alone is not that proof.",
      "Out-of-order clocks and exact cohort/revision replacement remain the named native fixtures; no extra product clock/producer is introduced.",
    ],
  };
}
async function holdForBrowser(): Promise<void> {
  await new Promise<void>((done) => {
    const finish = (): void => {
      clearTimeout(timeout);
      process.off("SIGTERM", finish);
      process.off("SIGINT", finish);
      done();
    };
    const timeout = setTimeout(finish, 600_000);
    process.once("SIGTERM", finish);
    process.once("SIGINT", finish);
  });
}
async function execute(sha: string, pairPath: string, workspace: string, browser: boolean): Promise<void> {
  const proof = await provenance(sha, pairPath);
  const initial = { config: await config(workspace), workspace, binary: proof.workerd, cases: [] as string[] };
  const session: Session = { ...initial, current: await start(initial) };
  const ctx = context(session);
  const stop = (): void => {
    void session.current.stop();
  };
  process.once("SIGTERM", stop);
  process.once("SIGINT", stop);
  try {
    const response = await fetch(ctx.url("/health"));
    const health = await json(response);
    assert.equal(object(health.release).git_sha, sha);
    assert.equal(object(health.release).deployment_id, `cloudflare-${sha}`);
    assert.equal(response.headers.get("x-uos-git-sha"), sha);
    ctx.cases.push("claude_header_exact_compiled_rust_source_identity");
    await selectProviders(ctx, ["claude_code"]);
    const client = await createClient(ctx);
    ctx.clientToken = client.token;
    const owner = await account(ctx, "controlled header A");
    await responseObject(await ctx.post("/admin/providers/claude-code/access", { allowed_key_ids: [client.id] }, "fixture-admin"));
    await assertNoOauth(ctx);
    await normalCases(ctx, owner);
    await restartCase(session, ctx, owner);
    const current = await accountHistory(ctx, client.id);
    await servedAssets(ctx);
    await assertExpectedOutboundAttempts(ctx);
    const receipt = {
      status: "controlled_rust_component_only",
      sourceSha: sha,
      sourceTree: proof.tree,
      cases: ctx.cases,
      outboundAttempts: await attempts(ctx),
      responseHeaders: (await ctx.events()).map(object).filter((event) => event.kind === "claude_header_response"),
      inputHashes: proof.hashes,
      platform: process.platform,
      arch: process.arch,
      bun: Bun.version,
      workerd: "1.20261012.1",
      browser: browserChecks(current, owner),
      limitations: [
        "Rendered browser assertions remain a separate gate.",
        "Legacy direct-error/data injection, out-of-order clocks and revision replacement require the named native fixtures; no generic row writer is added.",
        "No deployment, real Passport/provider/credential operation or whole-port acceptance.",
      ],
    };
    await writeFile(join(workspace, "receipt.json"), JSON.stringify(receipt, null, 2) + "\n");
    if (browser) {
      await writeFile(
        join(workspace, "browser-ready.json"),
        JSON.stringify(
          {
            publicOrigin: session.current.publicOrigin,
            controlEventsUrl: session.current.origin + "/control",
            outboundAttemptsBeforeBrowser: await attempts(ctx),
            ...browserChecks(current, owner),
          },
          null,
          2
        ) + "\n"
      );
      console.log(JSON.stringify({ browserReady: join(workspace, "browser-ready.json"), publicOrigin: session.current.publicOrigin }));
      await holdForBrowser();
    }
    await assertExpectedOutboundAttempts(ctx);
    receipt.outboundAttempts = await attempts(ctx);
    await writeFile(join(workspace, "receipt.json"), JSON.stringify(receipt, null, 2) + "\n");
    frozen(sha, proof.tree);
    assert.deepEqual(await hashes(Object.keys(proof.hashes)), proof.hashes);
    console.log(JSON.stringify({ evidence: workspace, ...receipt }));
  } finally {
    process.off("SIGTERM", stop);
    process.off("SIGINT", stop);
    await session.current.stop();
  }
}
async function capture(stream: ReadableStream<Uint8Array>, path: string, forward = false): Promise<string> {
  const reader = stream.getReader();
  const chunks: Uint8Array[] = [];
  try {
    for (;;) {
      const part = await reader.read();
      if (part.done) break;
      chunks.push(part.value);
      if (forward) process.stdout.write(part.value);
    }
  } finally {
    reader.releaseLock();
  }
  const bytes = Buffer.concat(chunks);
  await writeFile(path, bytes);
  return bytes.toString();
}
async function outer(sha: string, pairPath: string, browser: boolean): Promise<void> {
  const proof = await provenance(sha, pairPath);
  const workspace = await mkdtemp(join(proof.target, "claude-header-runtime-"));
  const argv = testNetworkArgv([
    process.execPath,
    "--no-env-file",
    "--no-install",
    SELF,
    "--fixture-child",
    sha,
    pairPath,
    workspace,
    browser ? "browser" : "verify",
  ]);
  assert(process.env.HOME && isAbsolute(process.env.HOME), "An explicit inherited absolute HOME is required.");
  const env = { PATH: process.env.PATH ?? "/usr/bin:/bin", HOME: process.env.HOME, TMPDIR: workspace, BUN_RUNTIME_TRANSPILER_CACHE_PATH: "0" };
  const child = Bun.spawn(argv, { cwd: ROOT, env, stdout: "pipe", stderr: "pipe" });
  const stop = (): void => {
    if (child.exitCode === null) child.kill("SIGTERM");
  };
  process.once("SIGTERM", stop);
  process.once("SIGINT", stop);
  const timeout = setTimeout(stop, browser ? 720_000 : 120_000);
  if (browser) console.log(JSON.stringify({ workspace, browserReady: join(workspace, "browser-ready.json") }));
  try {
    const [exit, stdout] = await Promise.all([
      child.exited,
      capture(child.stdout, join(workspace, "stdout.log"), browser),
      capture(child.stderr, join(workspace, "stderr.log")),
    ]);
    assert.equal(exit, 0, `Actual Rust fixture failed; evidence: ${workspace}`);
    if (!browser) console.log(stdout.trim());
  } finally {
    clearTimeout(timeout);
    process.off("SIGTERM", stop);
    process.off("SIGINT", stop);
    stop();
  }
}
async function main(): Promise<void> {
  assert.equal(Bun.version, "1.4.0");
  assert(["darwin", "linux"].includes(process.platform), "A supported real OS fixture boundary is required.");
  const child = process.argv[2] === "--fixture-child";
  const offset = child ? 3 : 2;
  const sha = process.argv[offset];
  const pair = process.argv[offset + 1];
  assert(sha && pair, "Usage: claude-header-runner.ts FULL_SHA ARTIFACT_PAIR_JSON [--browser]");
  if (child) {
    const workspace = await realpath(process.argv[offset + 2] ?? "");
    const proof = await provenance(sha, resolve(pair));
    assert.equal(dirname(workspace), proof.target);
    await execute(sha, resolve(pair), workspace, process.argv[offset + 3] === "browser");
  } else {
    assert(process.argv.length === 4 || (process.argv.length === 5 && process.argv[4] === "--browser"));
    await outer(sha, resolve(pair), process.argv[4] === "--browser");
  }
}
if (import.meta.main) await main();
