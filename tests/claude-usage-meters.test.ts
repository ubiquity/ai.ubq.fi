import { test } from "bun:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync } from "node:fs";
import { resolve } from "node:path";
import { openKv, type Kv } from "#kv";
import { parseClaudeCodeUsageSnapshot, readClaudeCodeUsageSnapshot, CLAUDE_USAGE_LEGACY_WINDOWS } from "../src/provider/claude-code-usage-schema.ts";
import { parseClaudeCodeUsagePayload } from "../src/provider/claude-code-usage.ts";
import {
  claudeCodeExhaustionFromResponse,
  mergeClaudeCodeQuotaState,
  loadClaudeCodeQuotaState,
  observeClaudeCodeQuota,
  recordClaudeCodeQuotaOutcome,
} from "../src/provider/claude-code-quota.ts";
import { storeClaudeCodeStoredCredential } from "../src/provider/claude-code-credentials.ts";
import { resolveClaudeCodeAccount } from "../src/provider/claude-code-accounts.ts";
import { claudeCapacitySource } from "../src/provider/capacity.ts";
import { readClaudeCapacityHistory } from "../src/provider/claude-capacity-history.ts";
import { listClaudeUsageRollups } from "../src/provider/claude-capacity-rollups.ts";

const AT = 1_800_000_000_000;
const row = (patch: Record<string, unknown> = {}) => ({
  kind: "future_weekly",
  group: "weekly",
  percent: 121.5,
  resets_at: "2027-01-20T00:00:00Z",
  severity: "vendor_future",
  is_active: true,
  scope: { model: { display_name: "Future model", vendor: 7 }, surface: { display_name: "OAuth apps" } },
  ...patch,
});
const snapshot = (payload: unknown, at = AT) => {
  const value = parseClaudeCodeUsageSnapshot(payload, at);
  assert(value);
  return value;
};

test("independent nullable legacy usage fields retain their exact source shape", () => {
  for (const key of CLAUDE_USAGE_LEGACY_WINDOWS) {
    const input = { [key]: { utilization: null, resets_at: null } };
    const value = snapshot(input);
    assert.deepEqual(value.payload, input);
    assert.equal(value.meters.length, 1);
    assert.equal(value.meters[0]?.used_percent, null);
    assert.equal(value.meters[0]?.reset_at_ms, null);
    assert.deepEqual(snapshot({ [key]: null }).meters, []);
  }
  assert.equal(parseClaudeCodeUsageSnapshot({}, AT), null);
  assert.equal(parseClaudeCodeUsageSnapshot({ seven_day: { resets_at: null } }, AT), null);
  assert.equal(parseClaudeCodeUsageSnapshot({ limits: [row({ percent: "1" })] }, AT), null);
  assert.equal(parseClaudeCodeUsageSnapshot({ limits: [row({ scope: { model: {} } })] }, AT), null);
  assert.equal(snapshot({ five_hour: { utilization: 125, resets_at: "unparseable-provider-text" } }).meters[0]?.used_percent, 125);
  assert.equal(snapshot({ five_hour: { utilization: 125, resets_at: "unparseable-provider-text" } }).meters[0]?.reset_at_ms, null);
});

test("dynamic meters preserve server order, scope, severity, headline and finite over-100 values", () => {
  const payload = {
    limits: [row(), row({ kind: "future_session", group: "session", percent: 2, is_active: false }), row()],
    seven_day: { utilization: 10, resets_at: null },
    vendor: { retained: true },
  };
  const value = snapshot(payload);
  assert.deepEqual(value.payload, payload);
  assert.deepEqual(
    value.meters.map((meter) => meter.kind),
    ["future_weekly", "future_session", "future_weekly"]
  );
  assert.equal(new Set(value.meters.map((meter) => meter.id)).size, 3);
  assert.equal(value.meters[0]?.used_percent, 121.5);
  assert.equal(value.meters[0]?.severity, "vendor_future");
  assert.equal(value.meters[0]?.is_active, true);
  assert.equal(value.meters[1]?.is_active, false);
  assert.match(value.meters[0]?.label ?? "", /Future model.*OAuth apps/);
  assert.deepEqual(readClaudeCodeUsageSnapshot(value), value);
  assert.deepEqual(snapshot({ ...payload, limits: [] }).meters, []);
  assert.equal(snapshot({ ...payload, limits: null }).meters[0]?.kind, "seven_day");
  assert.equal(snapshot({ seven_day: payload.seven_day }).meters[0]?.kind, "seven_day");
  assert.deepEqual(snapshot({ extra_usage: { is_enabled: false, monthly_limit: null, used_credits: 12, utilization: null, currency: "USD" } }).meters, []);
});

test("scoped usage at exhaustion cannot become an unscoped quota guard", () => {
  const observation = parseClaudeCodeUsagePayload({ limits: [row({ percent: 100 })] }, AT);
  assert(observation);
  assert.deepEqual(observation.windows, {});
  assert.equal(claudeCodeExhaustionFromResponse(429, observation), null);
  const state = mergeClaudeCodeQuotaState(null, observation, null, AT);
  assert.equal(state.blocked_until_ms, null);
  assert.equal(state.block_window, null);
  assert.equal(state.cooldown_until_ms, null);
  assert.equal(state.pool_proofs, undefined);
  const partial = parseClaudeCodeUsagePayload({ seven_day_sonnet: { utilization: null, resets_at: null } }, AT + 1);
  assert(partial);
  const replaced = mergeClaudeCodeQuotaState(state, partial, null, AT + 1);
  assert.deepEqual(replaced.usage_snapshot?.payload, partial.usage_snapshot?.payload);
  assert.equal(replaced.usage_snapshot?.meters.length, 1);
  assert.deepEqual(mergeClaudeCodeQuotaState(replaced, observation, null, AT + 2).usage_snapshot, replaced.usage_snapshot);
});

test("direct snapshots, current readback, history and scoped rollups survive native reopen without header freshness", async () => {
  const root = resolve(".cleanup-evidence/claude-usage-native");
  mkdirSync(root, { recursive: true });
  const directory = mkdtempSync(`${root}/case-`);
  const path = `${directory}/kv.sqlite3`;
  let kv: Kv = await openKv(path);
  try {
    const at = Date.now();
    await storeClaudeCodeStoredCredential(kv, { secret: "sk-ant-oat-fixture-usage-native" });
    const binding = await resolveClaudeCodeAccount(kv, () => null);
    assert(binding);
    const payload = { limits: [row(), row({ kind: "surface_only", scope: { surface: { display_name: "Future surface" } }, percent: 8 })] };
    const observation = parseClaudeCodeUsagePayload(payload, at);
    assert(observation);
    await recordClaudeCodeQuotaOutcome(kv, {
      binding,
      observation,
      response_status: 200,
      nowMs: at,
      usage: { source: "oauth_usage", state: "ok", status: 200, message: null, attempted_at_ms: at, observed_at_ms: at },
    });
    kv.close();
    kv = await openKv(path);
    assert.deepEqual((await loadClaudeCodeQuotaState(kv, binding))?.usage_snapshot?.payload, payload);
    const history = await readClaudeCapacityHistory(kv, at + 1);
    assert.equal(history.scan, "ok");
    assert.deepEqual(history.points[0]?.usage_snapshot?.payload, payload);
    const rollups = await listClaudeUsageRollups(kv, at - 3_600_000, at + 1);
    assert.equal(rollups.scan, "ok");
    assert.equal(rollups.points.length, 2);
    assert.equal(rollups.points[0]?.samples, 1);
    await recordClaudeCodeQuotaOutcome(kv, {
      binding,
      observation: null,
      nowMs: at + 10,
      usage: { source: "oauth_usage", state: "unreachable", status: null, message: "offline", attempted_at_ms: at + 10, observed_at_ms: null },
    });
    await recordClaudeCodeQuotaOutcome(kv, {
      binding,
      observation: observeClaudeCodeQuota(new Headers({ "anthropic-ratelimit-unified-5h-utilization": "0.1" }), at + 20),
      nowMs: at + 20,
    });
    const source = await claudeCapacitySource(kv, at + 20, () => null);
    assert.equal(source.usage_snapshot, undefined);
    assert.equal(source.usage, null);
    assert.equal(source.source_observed_at_ms, null);
    assert.equal(source.windows.five_hour, null);
    assert.equal(source.state, "available");
    const empty = parseClaudeCodeUsagePayload({ limits: [] }, at + 30);
    assert(empty);
    await recordClaudeCodeQuotaOutcome(kv, { binding, observation: empty, nowMs: at + 30 });
    assert.deepEqual((await loadClaudeCodeQuotaState(kv, binding))?.usage_snapshot?.meters, []);
    assert.deepEqual((await readClaudeCapacityHistory(kv, at + 31)).points.at(-1)?.usage_snapshot?.meters, []);
  } finally {
    kv.close();
    rmSync(directory, { recursive: true, force: true });
  }
});
