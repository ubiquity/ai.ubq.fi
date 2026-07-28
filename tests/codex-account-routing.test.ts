import assert from "node:assert/strict";
import { setKvForTest } from "../src/kv.ts";
import {
  claimCodexRoutingProbe,
  CODEX_ACCOUNT_ROUTING_KV_KEY,
  codexCredentialVersion,
  getCodexQuotaBlockFence,
  markCodexCredentialInvalid,
  markCodexQuotaBlocked,
  markCodexSuccess,
  parseCodexAccountRoutingState,
  readCodex429,
  recheckCodexRoutingSlot,
  reconcileCodexQuotaAfterVerifiedReset,
  reconcileCodexRoutingAccount,
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
        let last = 0;
        for (const write of writes) this.values.set(key(write.key), write.value);
        for (const write of writes) {
          const encoded = key(write.key);
          last = (this.versions.get(encoded) ?? 0) + 1;
          this.versions.set(encoded, last);
        }
        return Promise.resolve({ ok: true, versionstamp: String(last).padStart(20, "0") } as const);
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

Deno.test("v2 routing ignores the v1 key and rejects v1 payloads", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const credentialVersion = await codexCredentialVersion(singlePool.accounts[0]!);
    const v1State = {
      v: 1,
      updated_at_ms: now,
      slots: [{
        credential_version: credentialVersion,
        quota_blocked_until_ms: now + 60_000,
        quota_block_source: "cooldown",
        invalid_credential_version: credentialVersion,
        primary_used_percent: null,
        secondary_used_percent: null,
        observed_reset_at_ms: null,
        generation: 1,
        probe_lease: null,
      }],
    };
    await kv.set(["uos_ai", "codex_account_routing", "v1"], v1State);

    assert.equal(parseCodexAccountRoutingState(v1State), null);
    const selected = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(selected.kind, "eligible");
    assert.equal(kv.values.has(key(CODEX_ACCOUNT_ROUTING_KV_KEY)), false);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("ordinary and incomplete 429 variants never persist a quota block", async () => {
  const now = 1_700_000_000_000;
  const futureResetSeconds = Math.floor(now / 1_000) + 120;
  const cases = [
    {
      name: "bare",
      response: () => new Response(null, { status: 429 }),
      usageLimitReached: false,
      retryAtMs: null,
    },
    {
      name: "generic with valid Retry-After",
      response: () =>
        new Response(JSON.stringify({ error: { type: "rate_limit_error" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": "60" },
        }),
      usageLimitReached: false,
      retryAtMs: now + 60_000,
    },
    {
      name: "legacy reset fields only",
      response: () =>
        new Response(
          JSON.stringify({ error: { type: "usage_limit_reached", resets_at: futureResetSeconds } }),
          {
            status: 429,
            headers: {
              "Content-Type": "application/json",
              "x-codex-primary-reset-at": String(futureResetSeconds),
            },
          },
        ),
      usageLimitReached: true,
      retryAtMs: null,
    },
    {
      name: "usage limit without Retry-After",
      response: () =>
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json" },
        }),
      usageLimitReached: true,
      retryAtMs: null,
    },
    {
      name: "usage limit with invalid decimal Retry-After",
      response: () =>
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": "1.5" },
        }),
      usageLimitReached: true,
      retryAtMs: null,
    },
    {
      name: "truncated usage limit with valid Retry-After",
      response: () =>
        new Response(
          JSON.stringify({ error: { type: "usage_limit_reached", detail: "x".repeat(70 * 1_024) } }),
          {
            status: 429,
            headers: { "Content-Type": "application/json", "Retry-After": "60" },
          },
        ),
      usageLimitReached: false,
      retryAtMs: now + 60_000,
    },
  ] as const;

  for (const testCase of cases) {
    const kv = new RoutingKv();
    setKvForTest(kv as unknown as Deno.Kv);
    resetCodexAccountRoutingForTest();
    try {
      const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
      assert.equal(initial.kind, "eligible", testCase.name);
      if (initial.kind !== "eligible") continue;

      const classified = await markCodexQuotaBlocked(initial.accounts[0]!, testCase.response(), now);
      assert.equal(classified.response.status, 429, testCase.name);
      assert.equal(classified.usageLimitReached, testCase.usageLimitReached, testCase.name);
      assert.equal(classified.retryAtMs, testCase.retryAtMs, testCase.name);
      assert.equal(kv.values.has(key(CODEX_ACCOUNT_ROUTING_KV_KEY)), false, testCase.name);

      resetCodexAccountRoutingForTest();
      const selected = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1);
      assert.equal(selected.kind, "eligible", testCase.name);
    } finally {
      setKvForTest(null);
      resetCodexAccountRoutingForTest();
    }
  }
});

Deno.test("valid delta Retry-After durably blocks a fully parsed usage limit", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const payload = JSON.stringify({ error: { type: "usage_limit_reached" } });
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;

    const classified = await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(payload, {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "60" },
      }),
      now,
    );
    assert.equal(classified.usageLimitReached, true);
    assert.equal(classified.retryAtMs, now + 60_000);
    assert.equal(await classified.response.text(), payload);

    const state = kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)) as {
      v: number;
      slots: Array<{ quota_blocked_until_ms: number | null; quota_block_source: string | null }>;
    };
    assert.equal(state.v, 2);
    assert.equal(state.slots[0]?.quota_blocked_until_ms, now + 60_000);
    assert.equal(state.slots[0]?.quota_block_source, "header_retry_after");

    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1);
    assert.equal(selected.kind, "quota_blocked");
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("relative Retry-After blocks ordinary routing but cannot mint a banked-reset fence", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const account = initial.accounts[0]!;
    const classified = await markCodexQuotaBlocked(
      account,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "60" },
      }),
      now,
    );

    assert.equal(classified.retryAtMs, now + 60_000);
    assert.equal(classified.resetDeadlineIsStable, false);
    assert.equal(await getCodexQuotaBlockFence(account, now + 60_000), null);
    const blocked = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1);
    assert.equal(blocked.kind, "quota_blocked");
    if (blocked.kind !== "quota_blocked") return;
    assert.deepEqual(blocked.blockedAccounts, []);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("valid HTTP-date Retry-After durably blocks a fully parsed usage limit", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const retryAtMs = now + 120_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;

    const classified = await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(retryAtMs).toUTCString() },
      }),
      now,
    );
    assert.equal(classified.usageLimitReached, true);
    assert.equal(classified.retryAtMs, retryAtMs);

    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1);
    assert.equal(selected.kind, "quota_blocked");
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

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
    const original = new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
      status: 429,
      headers: { "Content-Type": "application/json", "Retry-After": "60" },
    });
    const replayable = await markCodexQuotaBlocked(first, original, now);
    assert.equal(replayable.response.status, 429);
    assert.match(await replayable.response.text(), /usage_limit_reached/);

    const afterOne = await selectCodexRoutingAccounts(pool, pool.accounts, now + 1);
    assert.equal(afterOne.kind, "eligible");
    if (afterOne.kind !== "eligible") return;
    assert.deepEqual(afterOne.accounts.map((account) => account.auth.account_id), ["two"]);

    await markCodexQuotaBlocked(
      afterOne.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "60" },
      }),
      now + 1,
    );
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

Deno.test("429 classification reads fragmented JSON and preserves the response", async () => {
  const now = 1_700_000_000_000;
  const payload = JSON.stringify({ error: { type: "usage_limit_reached" } });
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
    now,
  );
  assert.equal(parsed.usageLimitReached, true);
  assert.equal(parsed.retryAtMs, now + 1_000);
  assert.equal(await parsed.response.text(), payload);
});

Deno.test("429 classification rejects malformed UTF-8 before recognizing a usage-limit body", async () => {
  const now = 1_700_000_000_000;
  const encoder = new TextEncoder();
  const prefix = encoder.encode('{"error":{"type":"usage_limit_reached","message":"');
  const suffix = encoder.encode('"}}');
  const bytes = new Uint8Array(prefix.length + 1 + suffix.length);
  bytes.set(prefix);
  bytes[prefix.length] = 0x80;
  bytes.set(suffix, prefix.length + 1);
  const parsed = await readCodex429(
    new Response(bytes, {
      status: 429,
      headers: { "Content-Type": "application/json", "Retry-After": new Date(now + 60_000).toUTCString() },
    }),
    now,
  );

  assert.equal(parsed.usageLimitReached, false);
  assert.equal(parsed.retryAtMs, now + 60_000);
  assert.deepEqual(Array.from(new Uint8Array(await parsed.response.arrayBuffer())), Array.from(bytes));
});

Deno.test("429 classification rejects duplicate error keys before JSON last-key resolution", async () => {
  const now = 1_700_000_000_000;
  const payload = '{"error":{"type":"rate_limit_error","type":"usage_limit_reached"}}';
  const parsed = await readCodex429(
    new Response(payload, {
      status: 429,
      headers: { "Content-Type": "application/json", "Retry-After": new Date(now + 60_000).toUTCString() },
    }),
    now,
  );

  assert.equal(parsed.usageLimitReached, false);
  assert.equal(await parsed.response.text(), payload);
});

Deno.test("429 classification returns a valid error when capture is truncated", async () => {
  const oversized = JSON.stringify({ error: { type: "usage_limit_reached", detail: "x".repeat(70 * 1_024) } });
  const parsed = await readCodex429(
    new Response(oversized, { status: 429, headers: { "Content-Type": "application/json" } }),
  );
  assert.equal(parsed.usageLimitReached, false);
  assert.equal(parsed.retryAtMs, null);
  assert.deepEqual(await parsed.response.json(), {
    error: {
      message: "Codex returned an oversized or incomplete rate-limit response.",
      type: "rate_limit_error",
      code: "codex_rate_limit_response_truncated",
      param: null,
    },
  });
});

Deno.test("ordinary 429 clears only the current fenced probe from an expired circuit", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "1" },
      }),
      now,
    );

    resetCodexAccountRoutingForTest();
    const firstSelection = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1_001);
    assert.equal(firstSelection.kind, "eligible");
    if (firstSelection.kind !== "eligible") return;
    const staleProbe = await claimCodexRoutingProbe(singlePool, firstSelection.accounts[0]!, now + 1_001);
    assert.ok(staleProbe);

    resetCodexAccountRoutingForTest();
    const secondSelection = await selectCodexRoutingAccounts(
      singlePool,
      singlePool.accounts,
      now + 1_001 + 30_001,
    );
    assert.equal(secondSelection.kind, "eligible");
    if (secondSelection.kind !== "eligible") return;
    const currentProbe = await claimCodexRoutingProbe(
      singlePool,
      secondSelection.accounts[0]!,
      now + 1_001 + 30_001,
    );
    assert.ok(currentProbe);
    assert.notEqual(staleProbe.probeToken, currentProbe.probeToken);

    await markCodexQuotaBlocked(
      staleProbe,
      new Response(JSON.stringify({ error: { type: "rate_limit_error" } }), {
        status: 429,
        headers: { "Content-Type": "application/json" },
      }),
      now + 1_001 + 30_002,
    );
    resetCodexAccountRoutingForTest();
    const afterStale = await selectCodexRoutingAccounts(
      singlePool,
      singlePool.accounts,
      now + 1_001 + 30_002,
    );
    assert.equal(afterStale.kind, "quota_blocked");

    await markCodexQuotaBlocked(
      currentProbe,
      new Response(JSON.stringify({ error: { type: "rate_limit_error" } }), {
        status: 429,
        headers: { "Content-Type": "application/json" },
      }),
      now + 1_001 + 30_003,
    );
    resetCodexAccountRoutingForTest();
    const released = await selectCodexRoutingAccounts(
      singlePool,
      singlePool.accounts,
      now + 1_001 + 30_003,
    );
    assert.equal(released.kind, "eligible");
    if (released.kind === "eligible") assert.equal(released.accounts[0]?.probeToken, null);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("a stale ordinary usage-limit 429 cannot overwrite a foreign half-open lease", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const staleSelection = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(staleSelection.kind, "eligible");
    if (staleSelection.kind !== "eligible") return;
    const staleAccount = staleSelection.accounts[0]!;
    assert.equal(staleAccount.probeGeneration, null);

    resetCodexAccountRoutingForTest();
    const blockingSelection = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(blockingSelection.kind, "eligible");
    if (blockingSelection.kind !== "eligible") return;
    await markCodexQuotaBlocked(
      blockingSelection.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "1" },
      }),
      now,
    );

    const probeAt = now + 1_001;
    resetCodexAccountRoutingForTest();
    const probeSelection = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, probeAt);
    assert.equal(probeSelection.kind, "eligible");
    if (probeSelection.kind !== "eligible") return;
    const currentProbe = await claimCodexRoutingProbe(singlePool, probeSelection.accounts[0]!, probeAt);
    assert.ok(currentProbe);
    const beforeStale429 = parseCodexAccountRoutingState(
      kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)),
    );
    assert.equal(beforeStale429?.slots[0]?.probe_lease?.token, currentProbe.probeToken);

    resetCodexAccountRoutingForTest();
    const stale429At = probeAt + 1;
    await markCodexQuotaBlocked(
      staleAccount,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "1" },
      }),
      stale429At,
    );
    const afterStale429 = parseCodexAccountRoutingState(
      kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)),
    );
    assert.deepEqual(afterStale429?.slots[0], beforeStale429?.slots[0]);

    resetCodexAccountRoutingForTest();
    const secondSelection = await selectCodexRoutingAccounts(
      singlePool,
      singlePool.accounts,
      stale429At + 1_001,
    );
    const secondProbe = secondSelection.kind === "eligible"
      ? await claimCodexRoutingProbe(singlePool, secondSelection.accounts[0]!, stale429At + 1_001)
      : null;
    assert.equal(secondProbe, null);
    assert.equal(secondSelection.kind, "quota_blocked");
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
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
        JSON.stringify({ error: { type: "usage_limit_reached" } }),
        {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": "60" },
        },
      ),
      now,
    );

    const expiry = now + 61_000;
    resetCodexAccountRoutingForTest();
    const probes = (
      await Promise.all(
        Array.from({ length: 50 }, async () => {
          const selection = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, expiry);
          if (selection.kind !== "eligible") return null;
          return await claimCodexRoutingProbe(singlePool, selection.accounts[0]!, expiry);
        }),
      )
    ).filter((probe) => probe !== null);
    assert.equal(probes.length, 1);
    const firstProbe = probes[0]!;

    resetCodexAccountRoutingForTest();
    const second = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, expiry + 30_001);
    assert.equal(second.kind, "eligible");
    if (second.kind !== "eligible") return;
    const secondProbe = await claimCodexRoutingProbe(singlePool, second.accounts[0]!, expiry + 30_001);
    assert.ok(secondProbe);
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
        JSON.stringify({ error: { type: "usage_limit_reached" } }),
        {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": "60" },
        },
      ),
      now,
    );

    const selection = await selectCodexRoutingAccounts(pool, pool.accounts, now + 61_000);
    assert.equal(selection.kind, "eligible");
    if (selection.kind !== "eligible") return;
    assert.equal(selection.accounts.length, 2);
    assert.equal(selection.accounts[0]?.auth.account_id, "one");
    assert.equal(selection.accounts[0]?.probeRequired, true);
    const probe = await claimCodexRoutingProbe(pool, selection.accounts[0]!, now + 61_000);
    assert.ok(probe);
    assert.notEqual(probe.probeToken, null);
    assert.equal(selection.accounts[1]?.auth.account_id, "two");
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("an expired second-account circuit never jumps the healthy first account", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = Date.now();
    const initial = await selectCodexRoutingAccounts(pool, pool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const blocked = initial.accounts.find((account) => account.auth.account_id === "two")!;
    await markCodexQuotaBlocked(
      blocked,
      new Response(
        JSON.stringify({ error: { type: "usage_limit_reached" } }),
        {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": "60" },
        },
      ),
      now,
    );

    const selection = await selectCodexRoutingAccounts(pool, pool.accounts, now + 61_000);
    assert.equal(selection.kind, "eligible");
    if (selection.kind !== "eligible") return;
    assert.deepEqual(selection.accounts.map((account) => account.auth.account_id), ["one", "two"]);
    assert.equal(selection.accounts[0]?.probeToken, null);
    assert.equal(selection.accounts[0]?.probeRequired, false);
    assert.equal(selection.accounts[1]?.probeRequired, true);
    assert.equal(selection.accounts[1]?.probeToken, null);
    const stateBeforeAttempt = parseCodexAccountRoutingState(
      kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)),
    );
    assert.equal(stateBeforeAttempt?.slots[1]?.probe_lease, null);

    const claimed = await claimCodexRoutingProbe(pool, selection.accounts[1]!, now + 61_000);
    assert.ok(claimed);
    assert.notEqual(claimed.probeToken, null);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("unchanged auth reconciliation preserves a single-account half-open success fence", async () => {
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
        JSON.stringify({ error: { type: "usage_limit_reached" } }),
        {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": "60" },
        },
      ),
      now,
    );

    const selection = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 61_000);
    assert.equal(selection.kind, "eligible");
    if (selection.kind !== "eligible") return;
    const probe = await claimCodexRoutingProbe(singlePool, selection.accounts[0]!, now + 61_000);
    assert.ok(probe);
    assert.notEqual(probe.probeGeneration, null);
    assert.notEqual(probe.probeToken, null);

    const reconciled = await reconcileCodexRoutingAccount(probe, probe.auth);
    assert.equal(reconciled.probeGeneration, probe.probeGeneration);
    assert.equal(reconciled.probeToken, probe.probeToken);
    await markCodexSuccess(reconciled);

    resetCodexAccountRoutingForTest();
    const recovered = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 61_001);
    assert.equal(recovered.kind, "eligible");
    if (recovered.kind === "eligible") {
      assert.equal(recovered.accounts[0]?.probeGeneration, null);
      assert.equal(recovered.accounts[0]?.probeToken, null);
    }
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

Deno.test("a stale refresh reconciliation cannot overwrite a newer credential version", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const accountA = initial.accounts[0]!;
    const authB = { ...accountA.auth, access_token: "refresh-b", updated_at_ms: 2 };
    const authC = { ...accountA.auth, access_token: "rotation-c", updated_at_ms: 3 };
    const credentialC = await codexCredentialVersion(authC);
    const stateC = {
      v: 2 as const,
      updated_at_ms: Date.now(),
      slots: [{
        credential_version: credentialC,
        quota_blocked_until_ms: null,
        quota_block_source: null,
        invalid_credential_version: credentialC,
        primary_used_percent: null,
        secondary_used_percent: null,
        observed_reset_at_ms: null,
        generation: 7,
        probe_lease: null,
      }],
    };
    await kv.set(CODEX_ACCOUNT_ROUTING_KV_KEY, stateC);

    const reconciled = await reconcileCodexRoutingAccount(accountA, authB);
    assert.equal(reconciled.credentialVersion, await codexCredentialVersion(authB));
    const durable = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(durable?.slots[0]?.credential_version, credentialC);
    assert.equal(durable?.slots[0]?.invalid_credential_version, credentialC);
    assert.equal(durable?.slots[0]?.generation, 7);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("a revised active stable Retry-After fails closed rather than minting a second reset identity", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const account = initial.accounts[0]!;
    const firstResetAtMs = now + 60_000;
    await markCodexQuotaBlocked(
      account,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(firstResetAtMs).toUTCString() },
      }),
      now,
    );
    const firstFence = await getCodexQuotaBlockFence(account, firstResetAtMs);
    assert.equal(typeof firstFence, "number");
    if (firstFence === null) return;

    // A changed stable HTTP-date is not a provider-proven new quota
    // generation. The old record remains lookup-only, but neither deadline
    // may authorize a new claim or clear the current circuit.
    const latestResetAtMs = now + 120_000;
    await markCodexQuotaBlocked(
      account,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(latestResetAtMs).toUTCString() },
      }),
      now + 1,
    );
    assert.equal(await getCodexQuotaBlockFence(account, firstResetAtMs), null);
    assert.equal(await getCodexQuotaBlockFence(account, latestResetAtMs), null);
    const revised = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(revised?.slots[0]?.quota_blocked_until_ms, latestResetAtMs);
    assert.equal(revised?.slots[0]?.observed_reset_at_ms, firstResetAtMs);
    assert.equal(revised?.slots[0]?.banked_reset_generation_ambiguous, true);
    const blocked = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 2);
    assert.equal(blocked.kind, "quota_blocked");
    if (blocked.kind !== "quota_blocked") return;
    assert.deepEqual(
      blocked.blockedAccounts.map((candidate) => ({
        quotaResetAtMs: candidate.quotaResetAtMs,
        routingGeneration: candidate.routingGeneration,
      })),
      [{ quotaResetAtMs: firstResetAtMs, routingGeneration: revised!.slots[0]!.generation }],
    );
    assert.equal(
      await reconcileCodexQuotaAfterVerifiedReset(account, {
        quotaResetAtMs: firstResetAtMs,
        routingGeneration: firstFence,
      }),
      null,
    );
    assert.equal(
      await reconcileCodexQuotaAfterVerifiedReset(account, {
        quotaResetAtMs: firstResetAtMs,
        routingGeneration: revised!.slots[0]!.generation,
      }),
      null,
    );
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("a post-reset recovery probe fences delayed 429s and clears ambiguity only after success", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    // This request remains in flight while the first qualifying 429 is
    // reconciled, so its response must not create a new quota identity.
    const account = initial.accounts[0]!;
    const firstResetAtMs = now + 60_000;
    await markCodexQuotaBlocked(
      account,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(firstResetAtMs).toUTCString() },
      }),
      now,
    );
    const firstFence = await getCodexQuotaBlockFence(account, firstResetAtMs);
    assert.equal(typeof firstFence, "number");
    if (firstFence === null) return;

    const recoveryProbe = await reconcileCodexQuotaAfterVerifiedReset(account, {
      quotaResetAtMs: firstResetAtMs,
      routingGeneration: firstFence,
    });
    assert.ok(recoveryProbe);
    if (!recoveryProbe) return;
    assert.equal(recoveryProbe.probeGeneration, firstFence + 1);
    assert.ok(recoveryProbe.probeToken);
    const released = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1);
    assert.equal(released.kind, "eligible");

    const revisedResetAtMs = now + 120_000;
    await markCodexQuotaBlocked(
      account,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(revisedResetAtMs).toUTCString() },
      }),
      now + 1,
    );

    assert.equal(await getCodexQuotaBlockFence(account, firstResetAtMs), null);
    assert.equal(await getCodexQuotaBlockFence(account, revisedResetAtMs), null);
    const delayed = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(delayed?.slots[0]?.quota_blocked_until_ms, null);
    assert.equal(delayed?.slots[0]?.observed_reset_at_ms, firstResetAtMs);
    assert.equal(delayed?.slots[0]?.observed_reset_at_is_stable, true);
    assert.equal(delayed?.slots[0]?.banked_reset_generation_ambiguous, true);
    assert.equal(delayed?.slots[0]?.probe_lease?.token, recoveryProbe.probeToken);

    await markCodexSuccess(recoveryProbe);
    const recovered = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(recovered?.slots[0]?.observed_reset_at_ms, null);
    assert.equal(recovered?.slots[0]?.banked_reset_generation_ambiguous, false);

    const postSuccess = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 2);
    assert.equal(postSuccess.kind, "eligible");
    if (postSuccess.kind !== "eligible") return;
    const nextResetAtMs = now + 180_000;
    await markCodexQuotaBlocked(
      postSuccess.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(nextResetAtMs).toUTCString() },
      }),
      now + 2,
    );
    assert.equal(typeof await getCodexQuotaBlockFence(postSuccess.accounts[0]!, nextResetAtMs), "number");
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("a relative Retry-After after a stable deadline cannot mint a later reset identity", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const account = initial.accounts[0]!;
    const usageLimit = (retryAfter: string) =>
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": retryAfter },
      });

    const firstDeadline = now + 120_000;
    await markCodexQuotaBlocked(account, usageLimit(new Date(firstDeadline).toUTCString()), now);
    const firstFence = await getCodexQuotaBlockFence(account, firstDeadline);
    assert.equal(typeof firstFence, "number");
    if (firstFence === null) return;

    // A delta timeout can extend ordinary routing's block, but it cannot
    // revise a stable reset identity into a second redemption key.
    const relativeDeadline = now + 180_001;
    await markCodexQuotaBlocked(account, usageLimit("180"), now + 1);
    assert.equal(await getCodexQuotaBlockFence(account, firstDeadline), null);
    assert.equal(await getCodexQuotaBlockFence(account, relativeDeadline), null);

    // A later canonical date remains fenced too: D1 -> relative -> D2 must
    // stay lookup-only rather than restore a new key based on D2.
    const laterDeadline = now + 240_000;
    await markCodexQuotaBlocked(account, usageLimit(new Date(laterDeadline).toUTCString()), now + 2);
    assert.equal(await getCodexQuotaBlockFence(account, laterDeadline), null);
    const durable = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(durable?.slots[0]?.quota_blocked_until_ms, laterDeadline);
    assert.equal(durable?.slots[0]?.observed_reset_at_ms, firstDeadline);
    assert.equal(durable?.slots[0]?.observed_reset_at_is_stable, true);
    assert.equal(durable?.slots[0]?.banked_reset_generation_ambiguous, true);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("only a successful recovery probe clears reset-generation ambiguity", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = Math.floor(Date.now() / 1_000) * 1_000;
    const usageLimit = (retryAfter: string) =>
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": retryAfter },
      });
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const account = initial.accounts[0]!;
    const firstDeadline = now + 60_000;
    const revisedDeadline = now + 120_000;
    await markCodexQuotaBlocked(account, usageLimit(new Date(firstDeadline).toUTCString()), now);
    await markCodexQuotaBlocked(account, usageLimit(new Date(revisedDeadline).toUTCString()), now + 1);

    // An administrative recheck opens a normal probe but cannot clear the
    // ambiguous generation on its own.
    assert.equal(await recheckCodexRoutingSlot(1), true);
    const afterRecheck = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, Date.now());
    assert.equal(afterRecheck.kind, "eligible");
    if (afterRecheck.kind !== "eligible") return;
    assert.equal(afterRecheck.accounts[0]?.probeRequired, true);
    const firstProbe = await claimCodexRoutingProbe(singlePool, afterRecheck.accounts[0]!, Date.now());
    assert.ok(firstProbe);
    if (!firstProbe) return;

    const thirdNow = Math.floor(Date.now() / 1_000) * 1_000;
    const laterDeadline = thirdNow + 180_000;
    await markCodexQuotaBlocked(firstProbe, usageLimit(new Date(laterDeadline).toUTCString()), thirdNow);
    assert.equal(await getCodexQuotaBlockFence(firstProbe, laterDeadline), null);
    const stillAmbiguous = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(stillAmbiguous?.slots[0]?.observed_reset_at_ms, firstDeadline);
    assert.equal(stillAmbiguous?.slots[0]?.banked_reset_generation_ambiguous, true);

    // Only an actual successful half-open probe resets the provisional
    // identity. The next stable observation can then establish a fresh fence.
    assert.equal(await recheckCodexRoutingSlot(1), true);
    const afterSecondRecheck = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, Date.now());
    assert.equal(afterSecondRecheck.kind, "eligible");
    if (afterSecondRecheck.kind !== "eligible") return;
    const successfulProbe = await claimCodexRoutingProbe(
      singlePool,
      afterSecondRecheck.accounts[0]!,
      Date.now(),
    );
    assert.ok(successfulProbe);
    if (!successfulProbe) return;
    await markCodexSuccess(successfulProbe);
    const recovered = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(recovered?.slots[0]?.observed_reset_at_ms, null);
    assert.equal(recovered?.slots[0]?.banked_reset_generation_ambiguous, false);

    const postSuccess = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, Date.now());
    assert.equal(postSuccess.kind, "eligible");
    if (postSuccess.kind !== "eligible") return;
    const freshNow = Math.floor(Date.now() / 1_000) * 1_000;
    const freshDeadline = freshNow + 240_000;
    await markCodexQuotaBlocked(
      postSuccess.accounts[0]!,
      usageLimit(new Date(freshDeadline).toUTCString()),
      freshNow,
    );
    assert.equal(typeof await getCodexQuotaBlockFence(postSuccess.accounts[0]!, freshDeadline), "number");
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("same-account credential rotation and pool reordering retain reset ambiguity", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const usageLimit = (retryAfter: string) =>
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": retryAfter },
      });
    const initial = await selectCodexRoutingAccounts(pool, [pool.accounts[0]!], now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const account = initial.accounts[0]!;
    const firstDeadline = now + 60_000;
    const revisedDeadline = now + 120_000;
    await markCodexQuotaBlocked(account, usageLimit(new Date(firstDeadline).toUTCString()), now);
    await markCodexQuotaBlocked(account, usageLimit(new Date(revisedDeadline).toUTCString()), now + 1);

    const rotatedAccount = { ...pool.accounts[0]!, access_token: "rotated-access", updated_at_ms: now + 2 };
    const reorderedPool: CodexAuthPoolState = {
      accounts: [pool.accounts[1]!, rotatedAccount],
      updated_at_ms: now + 2,
    };
    // Simulate a fresh isolate that sees the durable state only after the pool
    // order and credential have both changed.
    resetCodexAccountRoutingForTest();
    const rotated = await selectCodexRoutingAccounts(reorderedPool, reorderedPool.accounts, now + 2);
    assert.equal(rotated.kind, "eligible");
    if (rotated.kind !== "eligible") return;
    // The refreshed account becomes normally routable, and its sibling remains
    // routable after the pool order changes. The old stable identity is still
    // retained only as a banked-reset fence.
    assert.deepEqual(rotated.accounts.map((candidate) => candidate.auth.account_id), ["two", "one"]);
    assert.equal(rotated.accounts.every((candidate) => !candidate.probeRequired), true);
    const rotatedRouting = rotated.accounts.find((candidate) => candidate.auth.account_id === "one");
    assert.ok(rotatedRouting);
    if (!rotatedRouting) return;
    const laterDeadline = revisedDeadline + 60_000;
    await markCodexQuotaBlocked(
      rotatedRouting,
      usageLimit(new Date(laterDeadline).toUTCString()),
      now + 2,
    );
    assert.equal(await getCodexQuotaBlockFence(rotatedRouting, laterDeadline), null);
    const durable = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(durable?.slots[1]?.observed_reset_at_ms, firstDeadline);
    assert.equal(durable?.slots[1]?.banked_reset_generation_ambiguous, true);
    assert.equal(durable?.slots[1]?.credential_version, await codexCredentialVersion(rotatedAccount));
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("an unmappable legacy stable identity cannot migrate to a replacement account", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const oldCredentialVersion = await codexCredentialVersion(singlePool.accounts[0]!);
    const legacyDeadline = now + 60_000;
    await kv.set(CODEX_ACCOUNT_ROUTING_KV_KEY, {
      v: 2,
      updated_at_ms: now,
      slots: [{
        credential_version: oldCredentialVersion,
        quota_blocked_until_ms: legacyDeadline,
        quota_block_source: "header_retry_after",
        invalid_credential_version: null,
        primary_used_percent: null,
        secondary_used_percent: null,
        observed_reset_at_ms: legacyDeadline,
        observed_reset_at_is_stable: true,
        banked_reset_generation_ambiguous: true,
        generation: 1,
        probe_lease: null,
      }],
    });
    const replacement = {
      access_token: "replacement-access",
      refresh_token: "replacement-refresh",
      account_id: "replacement",
      updated_at_ms: now + 1,
    };
    const replacementPool: CodexAuthPoolState = { accounts: [replacement], updated_at_ms: now + 1 };

    // An unknown legacy association must not make the replacement account
    // inherit the old circuit, but it must still stop a new reset key.
    const selected = await selectCodexRoutingAccounts(replacementPool, replacementPool.accounts, now + 1);
    assert.equal(selected.kind, "eligible");
    if (selected.kind !== "eligible") return;
    // Exercise the warm-cache normalization path too: the global legacy guard
    // is monotonic and may not disappear after the old slot is neutralized.
    const warmSelected = await selectCodexRoutingAccounts(replacementPool, replacementPool.accounts, now + 2);
    assert.equal(warmSelected.kind, "eligible");
    if (warmSelected.kind !== "eligible") return;
    const replacementRouting = warmSelected.accounts[0]!;
    const replacementDeadline = now + 120_000;
    await markCodexQuotaBlocked(
      replacementRouting,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(replacementDeadline).toUTCString() },
      }),
      now + 1,
    );
    assert.equal(await getCodexQuotaBlockFence(replacementRouting, replacementDeadline), null);
    const durable = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(durable?.banked_reset_legacy_identity_unresolved, true);
    assert.equal(durable?.slots[0]?.observed_reset_at_ms, replacementDeadline);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("an administrative recheck fences a stable reset identity until a successful probe", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const account = initial.accounts[0]!;
    const deadline = now + 60_000;
    await markCodexQuotaBlocked(
      account,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(deadline).toUTCString() },
      }),
      now,
    );
    assert.equal(typeof await getCodexQuotaBlockFence(account, deadline), "number");

    assert.equal(await recheckCodexRoutingSlot(1), true);
    assert.equal(await getCodexQuotaBlockFence(account, deadline), null);
    const durable = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(durable?.slots[0]?.observed_reset_at_ms, deadline);
    assert.equal(durable?.slots[0]?.banked_reset_generation_ambiguous, true);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});
