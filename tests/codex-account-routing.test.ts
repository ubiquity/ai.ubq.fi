import assert from "node:assert/strict";
import { setKvForTest } from "../src/kv.ts";
import {
  claimCodexRoutingProbe,
  CODEX_ACCOUNT_ROUTING_KV_KEY,
  codexCredentialVersion,
  markCodexCredentialInvalid,
  markCodexQuotaBlocked,
  markCodexSuccess,
  parseCodexAccountRoutingState,
  readCodex429,
  recheckCodexRoutingSlot,
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
