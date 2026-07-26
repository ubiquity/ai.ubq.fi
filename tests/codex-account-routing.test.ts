import assert from "node:assert/strict";
import { setKvForTest } from "../src/kv.ts";
import {
  markCodexCredentialInvalid,
  markCodexQuotaBlocked,
  markCodexSuccess,
  readCodex429,
  recheckCodexRoutingSlot,
  resetCodexAccountRoutingForTest,
  selectCodexRoutingAccounts,
} from "../src/codex_account_routing.ts";
import type { CodexAuthPoolState } from "../src/types.ts";

const key = (value: Deno.KvKey): string => JSON.stringify(value);

class RoutingKv {
  values = new Map<string, unknown>();
  versions = new Map<string, number>();

  get<T>(kvKey: Deno.KvKey): Promise<Deno.KvEntryMaybe<T>> {
    const encoded = key(kvKey);
    const value = this.values.get(encoded) as T | undefined;
    const version = this.versions.get(encoded);
    return Promise.resolve({
      key: kvKey,
      value: value ?? null,
      versionstamp: version === undefined ? null : String(version).padStart(20, "0"),
    } as Deno.KvEntryMaybe<T>);
  }

  set(kvKey: Deno.KvKey, value: unknown): Promise<Deno.KvCommitResult> {
    const encoded = key(kvKey);
    const version = (this.versions.get(encoded) ?? 0) + 1;
    this.values.set(encoded, value);
    this.versions.set(encoded, version);
    return Promise.resolve({ ok: true, versionstamp: String(version).padStart(20, "0") });
  }

  atomic(): Deno.AtomicOperation {
    const writes: Array<{ key: Deno.KvKey; value: unknown }> = [];
    const checks: Array<{ key: Deno.KvKey; versionstamp: string | null }> = [];
    const chain = {
      check: (...entries: Array<{ key: Deno.KvKey; versionstamp: string | null }>) => {
        checks.push(...entries);
        return chain;
      },
      set: (kvKey: Deno.KvKey, value: unknown) => {
        writes.push({ key: kvKey, value });
        return chain;
      },
      commit: () => {
        for (const entry of checks) {
          const current = this.versions.get(key(entry.key));
          const versionstamp = current === undefined ? null : String(current).padStart(20, "0");
          if (versionstamp !== entry.versionstamp) return Promise.resolve({ ok: false } as const);
        }
        for (const write of writes) this.values.set(key(write.key), write.value);
        for (const write of writes) {
          const encoded = key(write.key);
          this.versions.set(encoded, (this.versions.get(encoded) ?? 0) + 1);
        }
        return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" } as const);
      },
    };
    return chain as unknown as Deno.AtomicOperation;
  }
}

const pool: CodexAuthPoolState = {
  accounts: [
    { access_token: "access-one", refresh_token: "refresh-one", account_id: "one", updated_at_ms: 1 },
    { access_token: "access-two", refresh_token: "refresh-two", account_id: "two", updated_at_ms: 1 },
  ],
  updated_at_ms: 1,
};

const singlePool: CodexAuthPoolState = {
  accounts: [pool.accounts[0]!],
  updated_at_ms: pool.updated_at_ms,
};

Deno.test("quota circuits skip blocked slots and synthesize direct-fallback eligibility", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = Date.now();
    const initial = await selectCodexRoutingAccounts(pool, pool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const first = initial.accounts[0]!;
    const resetSeconds = Math.floor(now / 1000) + 60;
    const original = new Response(JSON.stringify({ error: { type: "usage_limit_reached", resets_at: resetSeconds } }), {
      status: 429,
      headers: { "Content-Type": "application/json", "x-codex-primary-reset-at": "2" },
    });
    const replayable = await markCodexQuotaBlocked(first, original, now);
    assert.equal(replayable.status, 429);
    assert.match(await replayable.text(), /usage_limit_reached/);

    const afterOne = await selectCodexRoutingAccounts(pool, pool.accounts, now + 1);
    assert.equal(afterOne.kind, "eligible");
    if (afterOne.kind !== "eligible") return;
    assert.deepEqual(afterOne.accounts.map((account) => account.auth.account_id), ["two"]);

    await markCodexQuotaBlocked(afterOne.accounts[0]!, new Response("limited", { status: 429 }), now + 1);
    const allBlocked = await selectCodexRoutingAccounts(pool, pool.accounts, now + 2);
    assert.equal(allBlocked.kind, "quota_blocked");

    assert.equal(await recheckCodexRoutingSlot(1), true);
    const halfOpen = await selectCodexRoutingAccounts(pool, pool.accounts, Date.now());
    assert.equal(halfOpen.kind, "eligible");
    if (halfOpen.kind === "eligible") assert.equal(halfOpen.accounts.length, 1);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("429 classification reads fragmented JSON before falling back to headers", async () => {
  const resetSeconds = Math.floor(Date.now() / 1000) + 90;
  const payload = JSON.stringify({ error: { type: "usage_limit_reached", resets_at: resetSeconds } });
  const source = new ReadableStream<Uint8Array>({
    start(controller) {
      const bytes = new TextEncoder().encode(payload);
      controller.enqueue(bytes.slice(0, 17));
      controller.enqueue(bytes.slice(17));
      controller.close();
    },
  });
  const parsed = await readCodex429(
    new Response(source, {
      status: 429,
      headers: { "Content-Type": "application/json", "Retry-After": "1" },
    }),
  );
  assert.equal(parsed.resetsAtMs, resetSeconds * 1_000);
  assert.equal(await parsed.response.text(), payload);
});

Deno.test("expired circuits grant one fenced probe and reject stale completion", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = Date.now();
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(
        JSON.stringify({ error: { type: "usage_limit_reached", resets_at: Math.floor(now / 1_000) + 60 } }),
        {
          status: 429,
          headers: { "Content-Type": "application/json" },
        },
      ),
      now,
    );

    const expiry = now + 61_000;
    resetCodexAccountRoutingForTest();
    const attempts = await Promise.all(
      Array.from({ length: 50 }, () => selectCodexRoutingAccounts(singlePool, singlePool.accounts, expiry)),
    );
    const probes = attempts.flatMap((selection) => selection.kind === "eligible" ? selection.accounts : []);
    assert.equal(probes.length, 1);
    const firstProbe = probes[0]!;

    resetCodexAccountRoutingForTest();
    const second = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, expiry + 30_001);
    assert.equal(second.kind, "eligible");
    if (second.kind !== "eligible") return;
    const secondProbe = second.accounts[0]!;
    assert.notEqual(firstProbe.probeToken, secondProbe.probeToken);

    await markCodexSuccess(firstProbe);
    resetCodexAccountRoutingForTest();
    const afterStaleSuccess = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, expiry + 30_002);
    assert.equal(afterStaleSuccess.kind, "quota_blocked");

    await markCodexSuccess(secondProbe);
    resetCodexAccountRoutingForTest();
    const recovered = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, expiry + 30_003);
    assert.equal(recovered.kind, "eligible");
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("an expired circuit receives one half-open probe before a healthy sibling", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = Date.now();
    const initial = await selectCodexRoutingAccounts(pool, pool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const blocked = initial.accounts.find((account) => account.auth.account_id === "one")!;
    await markCodexQuotaBlocked(
      blocked,
      new Response(
        JSON.stringify({ error: { type: "usage_limit_reached", resets_at: Math.floor(now / 1_000) + 60 } }),
        {
          status: 429,
          headers: { "Content-Type": "application/json" },
        },
      ),
      now,
    );

    const selection = await selectCodexRoutingAccounts(pool, pool.accounts, now + 61_000);
    assert.equal(selection.kind, "eligible");
    if (selection.kind !== "eligible") return;
    assert.equal(selection.accounts.length, 2);
    assert.equal(selection.accounts[0]?.auth.account_id, "one");
    assert.notEqual(selection.accounts[0]?.probeToken, null);
    assert.equal(selection.accounts[1]?.auth.account_id, "two");
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("credential rotation clears only the matching invalid circuit state", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    await markCodexCredentialInvalid(initial.accounts[0]!);
    const invalid = await selectCodexRoutingAccounts(singlePool, singlePool.accounts);
    assert.equal(invalid.kind, "credentials_invalid");

    const rotated: CodexAuthPoolState = {
      accounts: [{ ...singlePool.accounts[0]!, access_token: "rotated-access", updated_at_ms: Date.now() }],
      updated_at_ms: Date.now(),
    };
    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(rotated, rotated.accounts);
    assert.equal(selected.kind, "eligible");
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});
