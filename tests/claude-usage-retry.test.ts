import { test } from "bun:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync } from "node:fs";
import { resolve } from "node:path";
import { openKv, type Kv } from "#kv";
import { setKvForTest } from "../src/kv.ts";
import { resolveClaudeCodeAccount } from "../src/provider/claude-code-accounts.ts";
import { resetClaudeCodeCredentialCacheForTest, storeClaudeCodeStoredCredential } from "../src/provider/claude-code-credentials.ts";
import { claudeCodeQuotaKey, loadClaudeCodeQuotaState, recordClaudeCodeQuotaOutcome } from "../src/provider/claude-code-quota.ts";
import { refreshClaudeCodeUsage } from "../src/provider/claude-code-usage-refresh.ts";

const usagePayload = { five_hour: { utilization: 42, resets_at: null }, seven_day: { utilization: 12, resets_at: null } };
const nullEnv = () => null;
test("legacy usage Retry-After survives native reopen without changing inference state", async () => {
  const root = resolve(".cleanup-evidence/claude-usage-retry");
  mkdirSync(root, { recursive: true });
  const directory = mkdtempSync(`${root}/case-`);
  const path = `${directory}/kv.sqlite3`;
  let kv: Kv = await openKv(path);
  resetClaudeCodeCredentialCacheForTest();
  setKvForTest(kv);
  try {
    const now = Date.now();
    await storeClaudeCodeStoredCredential(kv, { secret: "sk-ant-oat-fixture-usage-retry" });
    const account = await resolveClaudeCodeAccount(kv, nullEnv);
    assert(account);
    await refreshClaudeCodeUsage(kv, account, { now: () => now - 31_000, fetcher: () => Promise.resolve(Response.json(usagePayload)) });
    await recordClaudeCodeQuotaOutcome(kv, {
      binding: account,
      observation: null,
      transient: { retry_after_ms: 300_000 },
      nowMs: now - 31_000,
    });
    const previous = await loadClaudeCodeQuotaState(kv, account);
    assert(previous?.usage_snapshot);
    let calls = 0;
    const limited = () => {
      calls += 1;
      return Promise.resolve(new Response("Too many usage reads", { status: 429, headers: { "Retry-After": "120" } }));
    };
    const first = await refreshClaudeCodeUsage(kv, account, { now: () => now, fetcher: limited });
    assert.equal(first.kind, "completed");
    kv.close();
    kv = await openKv(path);
    setKvForTest(kv);

    assert.equal((await refreshClaudeCodeUsage(kv, account, { now: () => now + 31_000, fetcher: limited })).kind, "fresh");
    assert.equal(calls, 1, "the archived direct helper retains its durable retry deadline");
    const retained = await loadClaudeCodeQuotaState(kv, account);
    assert.deepEqual(retained?.usage_snapshot, previous.usage_snapshot);
    assert.deepEqual(retained.windows, previous.windows);
    assert.equal(retained.cooldown_until_ms, previous.cooldown_until_ms);
    assert.equal(retained.blocked_until_ms, previous.blocked_until_ms);
    assert.deepEqual(retained.pool_proofs, previous.pool_proofs);

    const retry = await refreshClaudeCodeUsage(kv, account, {
      now: () => now + 120_000,
      fetcher: () => {
        calls += 1;
        return Promise.resolve(Response.json(usagePayload));
      },
    });
    assert.equal(retry.kind, "completed");
    assert.equal(calls, 2);
    const recovered = await loadClaudeCodeQuotaState(kv, account);
    assert.equal(recovered?.usage?.state, "ok");
    assert.equal(Object.hasOwn(recovered.usage, "retry_at_ms"), false, "success clears the previous usage retry deadline");
    assert.equal(recovered.usage_snapshot?.observed_at_ms, now + 120_000);
    assert.equal(recovered.cooldown_until_ms, previous.cooldown_until_ms);
  } finally {
    setKvForTest(null);
    kv.close();
    resetClaudeCodeCredentialCacheForTest();
    rmSync(directory, { recursive: true, force: true });
  }
});

test("a usage 429 without Retry-After retains the existing thirty-second attempt freshness", async () => {
  const kv = await openKv(":memory:");
  setKvForTest(kv);
  try {
    await storeClaudeCodeStoredCredential(kv, { secret: "sk-ant-oat-fixture-usage-retry-missing" });
    const account = await resolveClaudeCodeAccount(kv, nullEnv);
    assert(account);
    const now = Date.now();
    let calls = 0;
    const fetcher = () => {
      calls += 1;
      return Promise.resolve(new Response("Too many usage reads", { status: 429 }));
    };
    assert.equal((await refreshClaudeCodeUsage(kv, account, { now: () => now, fetcher })).kind, "completed");
    assert.equal((await refreshClaudeCodeUsage(kv, account, { now: () => now + 29_000, fetcher })).kind, "fresh");
    assert.equal(calls, 1);
    assert.equal((await refreshClaudeCodeUsage(kv, account, { now: () => now + 31_000, fetcher })).kind, "completed");
    assert.equal(calls, 2);
  } finally {
    setKvForTest(null);
    kv.close();
    resetClaudeCodeCredentialCacheForTest();
  }
});

test("malformed and expired persisted usage retry deadlines cannot prevent collection", async () => {
  const kv = await openKv(":memory:");
  try {
    await storeClaudeCodeStoredCredential(kv, { secret: "sk-ant-oat-fixture-usage-retry-invalid" });
    const account = await resolveClaudeCodeAccount(kv, nullEnv);
    assert(account);
    const now = Date.now();
    await refreshClaudeCodeUsage(kv, account, {
      now: () => now,
      fetcher: () => Promise.resolve(new Response("Too many usage reads", { status: 429 })),
    });
    const previous = await loadClaudeCodeQuotaState(kv, account);
    assert(previous?.usage);
    for (const deadline of [undefined, null, "forever", NaN, Infinity, -1, now - 1, now + 1, Number.MAX_SAFE_INTEGER + 1]) {
      await kv.set(claudeCodeQuotaKey(account), { ...previous, usage: { ...previous.usage, retry_at_ms: deadline } });
      let calls = 0;
      await refreshClaudeCodeUsage(kv, account, {
        now: () => now + 31_000,
        fetcher: () => {
          calls += 1;
          return Promise.resolve(Response.json(usagePayload));
        },
      });
      assert.equal(calls, 1, `invalid or expired deadline ${String(deadline)} cannot suppress a stale attempt`);
    }
  } finally {
    kv.close();
    resetClaudeCodeCredentialCacheForTest();
  }
});
