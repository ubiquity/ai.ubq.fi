import { test } from "bun:test";
import assert from "node:assert/strict";
import { chmod, lstat, mkdir, mkdtemp, readFile, rm, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { GEMINI_QUOTA_FILE, projectAntigravityStatusLine, readGeminiQuotaSnapshot } from "../src/admin/gemini-quota-snapshot.ts";
import { config } from "../src/config.ts";
import handler from "../src/handler/index.ts";
import { resetKvForTest, setKvForTest } from "../src/kv.ts";

const ROOT = fileURLToPath(new URL("../", import.meta.url));
const ROUTE = "/admin/providers/gemini/quota";
const ADMIN = "gemini-fixture-admin";
const CLIENT = "gemini-fixture-client";
const EVENT = {
  product: "antigravity",
  email: "fixture@example.test",
  plan_tier: "Pro",
  quota: { weekly: { remaining_fraction: 0.25, reset_in_seconds: 7200 } },
};

const withFixture = async (run: (root: string, directory: string) => Promise<void>) => {
  const root = await mkdtemp(join(tmpdir(), "gemini-quota-"));
  const directory = join(root, ".data");
  try {
    await mkdir(directory, { mode: 0o700 });
    await run(root, directory);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
};

test("the status line projects only subscription data and validates each bucket independently", () => {
  const observation = projectAntigravityStatusLine({
    ...EVENT,
    cwd: "/private/workspace",
    context_window: { total_input_tokens: 37_000 },
    transcript_path: "/private/transcript.jsonl",
    model: { id: "unrelated" },
    quota: {
      weekly: { remaining_fraction: 0.25, reset_time: "2026-11-18T08:00:00+01:00", reset_in_seconds: 30, other: "ignored" },
      balance: { remaining_amount: 0, reset_in_seconds: 45, disabled: false },
      disabled: { remaining_fraction: 2, remaining_amount: -1, reset_time: "2026-02-30T00:00:00Z", disabled: true },
      ambiguous: { remaining_fraction: 0, remaining_amount: 0 },
      missing: null,
      "fixture@example.test": { remaining_fraction: 1 },
    },
  });
  assert.deepEqual(observation, {
    status: "observed",
    plan_tier: "Pro",
    quota: {
      weekly: { remaining_fraction: 0.25, reset_time: "2026-11-18T07:00:00.000Z" },
      balance: { remaining_amount: 0, reset_in_seconds: 45, disabled: false },
      disabled: { disabled: true },
      ambiguous: {},
    },
  });
  assert.deepEqual(projectAntigravityStatusLine({ ...EVENT, product: "other" }), { status: "invalid_event" });
  assert.deepEqual(projectAntigravityStatusLine({ ...EVENT, agent_state: "authenticating" }), { status: "signed_out" });
  assert.deepEqual(projectAntigravityStatusLine({ product: "antigravity", email: EVENT.email }), { status: "observed", plan_tier: null, quota: {} });
});

test("the documented hook runs silently outside the checkout and saves only change or freshness updates", async () => {
  await withFixture(async (root, directory) => {
    const docs = await readFile(join(ROOT, "ops/README.md"), "utf8");
    const sample = /```json\n(\{\n {2}"statusLine"[\s\S]*?\n\})\n```/.exec(docs);
    assert.ok(sample);
    const { statusLine } = JSON.parse(sample[1]) as { statusLine: { command: string; stack_with_default: boolean } };
    assert.equal(statusLine.stack_with_default, true);
    const command = statusLine.command.replaceAll("/path/to/bun", process.execPath).replaceAll("/path/to/gateway", root);
    const outside = join(root, "session");
    await mkdir(outside);
    await Promise.all(["ops", "scripts", "src"].map((name) => symlink(join(ROOT, name), join(root, name), "dir")));

    const send = async (input: string) => {
      const child = Bun.spawn(["/bin/sh", "-c", command], { cwd: outside, stdin: new Blob([input]), stdout: "pipe", stderr: "pipe" });
      const [exit, stdout, stderr] = await Promise.all([child.exited, new Response(child.stdout).text(), new Response(child.stderr).text()]);
      assert.deepEqual({ exit, stdout, stderr }, { exit: 0, stdout: "", stderr: "" });
    };

    await send(JSON.stringify(EVENT));
    const destination = join(directory, GEMINI_QUOTA_FILE);
    const first = await readGeminiQuotaSnapshot(directory);
    assert.equal(first.status, "observed");
    assert.deepEqual(first.quota, EVENT.quota);
    assert.equal((await lstat(destination)).mode & 0o777, 0o600);
    assert.doesNotMatch(await readFile(destination, "utf8"), /fixture@example\.test|email/);

    // Only the countdown and unrelated session data change. Overlapping writes share the same lock.
    const withinInterval = { ...first, observed_at_ms: Date.now() - 2000, quota: { weekly: { ...EVENT.quota.weekly, reset_in_seconds: 7202 } } };
    await writeFile(destination, JSON.stringify(withinInterval));
    const initialInode = (await lstat(destination)).ino;
    await Promise.all([send(JSON.stringify({ ...EVENT, context_window: { total_input_tokens: 7 } })), send(JSON.stringify(EVENT))]);
    assert.deepEqual(await readGeminiQuotaSnapshot(directory), withinInterval);
    assert.equal((await lstat(destination)).ino, initialInode);

    const previousAt = Date.now() - 31_000;
    await writeFile(destination, JSON.stringify({ ...first, observed_at_ms: previousAt }));
    await send(JSON.stringify(EVENT));
    const refreshed = await readGeminiQuotaSnapshot(directory);
    assert.ok("observed_at_ms" in refreshed && refreshed.observed_at_ms > previousAt);

    await send(JSON.stringify({ ...EVENT, email: "" }));
    assert.equal((await readGeminiQuotaSnapshot(directory)).status, "signed_out");
    assert.doesNotMatch(await readFile(destination, "utf8"), /weekly|plan_tier/);
    await send(JSON.stringify(EVENT));
    await send(JSON.stringify({ ...EVENT, conversation_title: "a".repeat(256 * 1024) }));
    assert.equal((await readGeminiQuotaSnapshot(directory)).status, "invalid_event");
  });
});

test("the normal admin entrypoint serves the local snapshot only to admins and fails closed on corruption", async () => {
  await withFixture(async (root, directory) => {
    const previousCwd = process.cwd();
    const admins = config.adminTokens as Set<string>;
    const clients = config.authTokens as Set<string>;
    const request = (path: string, token?: string, method = "GET") =>
      handler(new Request(`https://ai.ubq.fi${path}`, { method, headers: token ? { Authorization: `Bearer ${token}` } : {} }));
    const destination = join(directory, GEMINI_QUOTA_FILE);
    const snapshot = { version: 1, observed_at_ms: Date.now(), ...projectAntigravityStatusLine(EVENT) };
    try {
      process.chdir(root);
      setKvForTest(null);
      admins.add(ADMIN);
      clients.add(CLIENT);

      const html = await request("/admin");
      assert.equal(html.status, 200);
      const body = await html.text();
      assert.match(body, /id="gemini-quota-card"/);
      const script = /type="module" src="(\/admin\.js\?v=[^"]+)"/.exec(body);
      assert.ok(script);
      const asset = await request(script[1]);
      assert.equal(asset.status, 200);
      assert.match(await asset.text(), /\/admin\/providers\/gemini\/quota/);
      assert.deepEqual(await (await request(ROUTE, ADMIN)).json(), { status: "missing" });

      await writeFile(destination, JSON.stringify(snapshot), { mode: 0o600 });
      assert.equal((await request(ROUTE)).status, 401);
      assert.equal((await request(ROUTE, CLIENT)).status, 401);
      assert.equal((await request(ROUTE, ADMIN, "POST")).status, 404);
      assert.doesNotMatch(await (await request("/health")).text(), /gemini|fixture@example\.test/i);

      const result = await request(`${ROUTE}?path=/unrelated`, ADMIN);
      assert.equal(result.status, 200);
      assert.equal(result.headers.get("Cache-Control"), "no-store");
      const { age_ms, ...data } = (await result.json()) as Record<string, unknown>;
      assert.equal(typeof age_ms, "number");
      assert.deepEqual(data, snapshot);

      await chmod(destination, 0o644);
      assert.deepEqual(await (await request(ROUTE, ADMIN)).json(), { status: "unavailable" });
      await chmod(destination, 0o600);
      await writeFile(destination, JSON.stringify({ ...snapshot, email: EVENT.email }));
      assert.deepEqual(await (await request(ROUTE, ADMIN)).json(), { status: "unavailable" });
      await rm(destination);
      await symlink(join(ROOT, "ops/README.md"), destination);
      assert.deepEqual(await (await request(ROUTE, ADMIN)).json(), { status: "unavailable" });
    } finally {
      admins.delete(ADMIN);
      clients.delete(CLIENT);
      resetKvForTest();
      process.chdir(previousCwd);
    }
  });
});
