import assert from "node:assert/strict";
import { CODEX_AUTH_POOL_KV_KEY, CodexError, fetchCodexResponses, resetCodexAuthCacheForTest } from "../src/codex.ts";
import { setKvForTest } from "../src/kv.ts";
import {
  claimCodexRoutingProbe,
  CODEX_ACCOUNT_ROUTING_KV_KEY,
  CODEX_CAPACITY_ROUTING_MAX_AGE_MS,
  CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY,
  CODEX_HALF_OPEN_LEASE_MS,
  CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS,
  codexCredentialVersion,
  getCodexQuotaBlockFence,
  markCodexCredentialInvalid,
  markCodexQuotaBlocked,
  markCodexRecoveryProbeQuotaBlocked,
  markCodexSuccess,
  markCodexUpstreamTimeout,
  parseCodexAccountRoutingState,
  readCodex429,
  recheckCodexRoutingSlot,
  reconcileCodexQuotaAfterStaleVerifiedReset,
  reconcileCodexQuotaAfterVerifiedReset,
  reconcileCodexRoutingAccount,
  recordCodexCapacityRoutingObservations,
  resetCodexAccountRoutingForTest,
  selectCodexRoutingAccounts,
} from "../src/codex_account_routing.ts";
import { PROVIDER_CAPACITY_SNAPSHOT_KEY } from "../src/provider_capacity_contract.ts";
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

Deno.test("quota circuits isolate Spark, GPT-OSS, and standard model pools", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const sparkDeadline = now + 7 * 24 * 60 * 60_000;
    const standardDeadline = now + 60 * 60_000;
    const exhausted = (deadline: number) =>
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(deadline).toUTCString() },
      });

    const spark = await selectCodexRoutingAccounts(
      singlePool,
      singlePool.accounts,
      now,
      "gpt-5.3-codex-spark",
    );
    assert.equal(spark.kind, "eligible");
    if (spark.kind !== "eligible") return;
    await markCodexQuotaBlocked(spark.accounts[0]!, exhausted(sparkDeadline), now);

    assert.equal(
      (await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1, "gpt-5.3-codex-spark")).kind,
      "quota_blocked",
    );
    const luna = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1, "gpt-5.6-luna");
    assert.equal(luna.kind, "eligible");
    if (luna.kind !== "eligible") return;
    await markCodexQuotaBlocked(luna.accounts[0]!, exhausted(standardDeadline), now + 1);
    assert.equal(typeof await getCodexQuotaBlockFence(spark.accounts[0]!, sparkDeadline), "number");
    assert.equal(typeof await getCodexQuotaBlockFence(luna.accounts[0]!, standardDeadline), "number");

    assert.equal(
      (await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 2, "gpt-5.6-terra")).kind,
      "quota_blocked",
    );
    assert.equal(
      (await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 2, "gpt-oss-120b")).kind,
      "eligible",
    );
    const afterStandardReset = await selectCodexRoutingAccounts(
      singlePool,
      singlePool.accounts,
      standardDeadline + 1,
      "gpt-5.6-luna",
    );
    assert.equal(afterStandardReset.kind, "eligible");
    assert.equal(
      (await selectCodexRoutingAccounts(
        singlePool,
        singlePool.accounts,
        standardDeadline + 1,
        "gpt-5.3-codex-spark",
      )).kind,
      "quota_blocked",
    );

    await recordCodexCapacityRoutingObservations([{
      slot: 0,
      account_id: "one",
      state: "available",
      source_observed_at_ms: now + 2,
      snapshot_at_ms: now + 2,
      windows: {
        primary: { limit_window_seconds: 604_800, used_percent: 50, reset_at_ms: standardDeadline },
        secondary: null,
      },
      additional_rate_limits: [{
        limit_name: "GPT-5.3-Codex-Spark",
        metered_feature: "codex_bengalfox",
        windows: {
          primary: { limit_window_seconds: 604_800, used_percent: 100, reset_at_ms: sparkDeadline },
          secondary: null,
        },
      }],
    }], now + 2);
    assert.equal(
      (await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 3, "gpt-5.6-luna")).kind,
      "eligible",
    );
    assert.equal(
      (await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 3, "gpt-5.3-codex-spark")).kind,
      "quota_blocked",
    );
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("an unrelated class 429 is recorded while another class owns a probe lease", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const sparkDeadline = now + 60_000;
    const standardDeadline = sparkDeadline + 60_000;
    const usageLimitResponse = (deadline: number) =>
      new Response(
        JSON.stringify({ error: { type: "usage_limit_reached", resets_at: Math.floor(deadline / 1_000) } }),
        {
          status: 429,
          headers: { "Content-Type": "application/json" },
        },
      );
    const spark = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now, "gpt-5.3-codex-spark");
    assert.equal(spark.kind, "eligible");
    if (spark.kind !== "eligible") return;
    await markCodexQuotaBlocked(spark.accounts[0]!, usageLimitResponse(sparkDeadline), now);

    const expiredSpark = await selectCodexRoutingAccounts(
      singlePool,
      singlePool.accounts,
      sparkDeadline + 1,
      "gpt-5.3-codex-spark",
    );
    assert.equal(expiredSpark.kind, "eligible");
    if (expiredSpark.kind !== "eligible") return;
    const claimed = await claimCodexRoutingProbe(singlePool, expiredSpark.accounts[0]!, sparkDeadline + 1);
    assert.ok(claimed);
    if (!claimed) return;

    const standard = await selectCodexRoutingAccounts(
      singlePool,
      singlePool.accounts,
      sparkDeadline + 2,
      "gpt-5.6-luna",
    );
    assert.equal(standard.kind, "eligible");
    if (standard.kind !== "eligible") return;
    await markCodexQuotaBlocked(standard.accounts[0]!, usageLimitResponse(standardDeadline), sparkDeadline + 2);

    const state = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(state?.slots[0]?.quota_blocks_by_class?.standard?.blocked_until_ms, standardDeadline);
    assert.equal(state?.slots[0]?.probe_lease?.quota_class, "spark");
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("legacy named class blocks remain enforced until migrated", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const deadline = now + 60_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now, "gpt-5.6-luna");
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    await kv.set(CODEX_ACCOUNT_ROUTING_KV_KEY, {
      v: 2,
      updated_at_ms: now,
      banked_reset_legacy_identity_unresolved: false,
      slots: [{
        account_id_hash: initial.accounts[0]!.accountIdHash,
        credential_version: initial.accounts[0]!.credentialVersion,
        quota_blocked_until_ms: deadline,
        quota_block_source: "header_retry_after",
        quota_blocked_classes: ["standard"],
        invalid_credential_version: null,
        primary_used_percent: null,
        secondary_used_percent: null,
        quota_signal_observed_at_ms: now,
        capacity_observed_at_ms: null,
        upstream_timeout_blocked_until_ms: null,
        observed_reset_at_ms: null,
        observed_reset_at_is_stable: false,
        banked_reset_generation_ambiguous: false,
        banked_reset_recovery_probe_pending: false,
        generation: 1,
        probe_lease: null,
      }],
    });
    resetCodexAccountRoutingForTest();
    assert.equal(
      (await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1, "gpt-5.6-luna")).kind,
      "quota_blocked",
    );
    assert.equal(
      (await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1, "gpt-5.3-codex-spark")).kind,
      "eligible",
    );
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("response-header timeouts fence one account and fail closed when every account is blocked", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(pool, pool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;

    await markCodexUpstreamTimeout(initial.accounts[0]!, now);
    const blocked = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(
      blocked?.slots[0]?.upstream_timeout_blocked_until_ms,
      now + CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS,
    );

    resetCodexAccountRoutingForTest();
    const sibling = await selectCodexRoutingAccounts(pool, pool.accounts, now + 1);
    assert.equal(sibling.kind, "eligible");
    if (sibling.kind !== "eligible") return;
    assert.deepEqual(sibling.accounts.map((account) => account.slot), [1]);
    assert.deepEqual(sibling.skippedSlots, [1]);

    await markCodexUpstreamTimeout(sibling.accounts[0]!, now + 1);
    resetCodexAccountRoutingForTest();
    const unavailable = await selectCodexRoutingAccounts(pool, pool.accounts, now + 2);
    assert.equal(unavailable.kind, "upstream_blocked");
    if (unavailable.kind !== "upstream_blocked") return;
    assert.equal(unavailable.retryAtMs, now + CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS);

    resetCodexAccountRoutingForTest();
    const halfOpen = await selectCodexRoutingAccounts(
      pool,
      pool.accounts,
      now + CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS + 1,
    );
    assert.equal(halfOpen.kind, "eligible");
    if (halfOpen.kind !== "eligible") return;
    assert.equal(halfOpen.accounts[0]?.probeRequired, true);
    const claimed = await claimCodexRoutingProbe(
      pool,
      halfOpen.accounts[0]!,
      now + CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS + 1,
    );
    assert.ok(claimed);
    await markCodexSuccess(claimed!);
    const recovered = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(recovered?.slots[0]?.upstream_timeout_blocked_until_ms, null);
    assert.equal(recovered?.slots[0]?.probe_lease, null);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("an expired quota timestamp does not misclassify a held timeout probe", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const account = initial.accounts[0]!;
    await markCodexUpstreamTimeout(account, now - CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1);
    const state = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.ok(state);
    if (!state) return;
    const probeExpiresAtMs = now + 1_000;
    await kv.set(CODEX_ACCOUNT_ROUTING_KV_KEY, {
      ...state,
      updated_at_ms: now,
      slots: state.slots.map((slot, index) =>
        index === account.slot
          ? {
            ...slot,
            quota_blocked_until_ms: now - 1,
            quota_block_source: "header_retry_after",
            upstream_timeout_blocked_until_ms: now - 1,
            probe_lease: {
              token: "held-timeout-probe",
              expires_at_ms: probeExpiresAtMs,
              generation: slot.generation,
              circuit: "upstream_timeout",
            },
          }
          : slot
      ),
    });
    resetCodexAccountRoutingForTest();

    const selected = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(selected.kind, "upstream_blocked");
    if (selected.kind !== "upstream_blocked") return;
    assert.equal(selected.retryAtMs, probeExpiresAtMs);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("a held quota probe does not misclassify a stale timeout as upstream blocked", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const account = initial.accounts[0]!;
    await markCodexUpstreamTimeout(account, now - CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1);
    const state = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.ok(state);
    if (!state) return;
    const probeExpiresAtMs = now + 1_000;
    await kv.set(CODEX_ACCOUNT_ROUTING_KV_KEY, {
      ...state,
      updated_at_ms: now,
      slots: state.slots.map((slot, index) =>
        index === account.slot
          ? {
            ...slot,
            quota_blocked_until_ms: now - 1,
            quota_block_source: "header_retry_after",
            upstream_timeout_blocked_until_ms: now - 1,
            probe_lease: {
              token: "held-quota-probe",
              expires_at_ms: probeExpiresAtMs,
              generation: slot.generation,
              circuit: "quota",
            },
          }
          : slot
      ),
    });
    resetCodexAccountRoutingForTest();

    const selected = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(selected.kind, "quota_blocked");
    if (selected.kind !== "quota_blocked") return;
    assert.equal(selected.retryAtMs, probeExpiresAtMs);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("timeout blocks take precedence over quota blocks in a mixed unavailable pool", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(pool, pool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    await markCodexQuotaBlocked(
      initial.accounts.find((account) => account.auth.account_id === "one")!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "60" },
      }),
      now,
    );
    await markCodexUpstreamTimeout(
      initial.accounts.find((account) => account.auth.account_id === "two")!,
      now,
    );

    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(pool, pool.accounts, now + 1);
    assert.equal(selected.kind, "upstream_blocked");
    if (selected.kind === "upstream_blocked") {
      assert.equal(selected.retryAtMs, now + 60_000);
    }
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("fetchCodexResponses uses the sibling account on the request after a timeout", async () => {
  const kv = new RoutingKv();
  const originalFetch = globalThis.fetch;
  const now = Date.now();
  const authPool: CodexAuthPoolState = {
    accounts: pool.accounts.map((account) => ({ ...account, updated_at_ms: now })),
    updated_at_ms: now,
  };
  const calls: string[] = [];
  const timeoutController = new AbortController();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAuthCacheForTest();
  await kv.set(CODEX_AUTH_POOL_KV_KEY, authPool);
  try {
    globalThis.fetch = (input, init): Promise<Response> => {
      const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
      if (!url.endsWith("/responses")) throw new Error(`Unexpected Codex URL: ${url}`);
      const accountId = new Headers(init?.headers).get("ChatGPT-Account-ID");
      calls.push(accountId ?? "");
      if (calls.length === 1) {
        timeoutController.abort(new DOMException("Codex fixture timeout", "TimeoutError"));
        return Promise.reject(timeoutController.signal.reason);
      }
      return Promise.resolve(new Response(null, { status: 200 }));
    };

    await assert.rejects(
      fetchCodexResponses({ model: "gpt-5-routing", input: "timeout" }, { signal: timeoutController.signal }),
      (error: unknown) => error instanceof CodexError && error.code === "gateway_timeout" && error.status === 504,
    );
    resetCodexAccountRoutingForTest();

    const response = await fetchCodexResponses({ model: "gpt-5-routing", input: "next request" });
    assert.equal(response.status, 200);
    assert.deepEqual(calls, ["one", "two"]);
    await response.arrayBuffer();
  } finally {
    globalThis.fetch = originalFetch;
    setKvForTest(null);
    resetCodexAuthCacheForTest();
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
      name: "usage limit with string body reset",
      response: () =>
        new Response(
          JSON.stringify({ error: { type: "usage_limit_reached", resets_at: String(futureResetSeconds) } }),
          {
            status: 429,
            headers: { "Content-Type": "application/json" },
          },
        ),
      usageLimitReached: true,
      retryAtMs: null,
    },
    {
      name: "usage limit with fractional body reset",
      response: () =>
        new Response(JSON.stringify({ error: { type: "usage_limit_reached", resets_at: futureResetSeconds + 0.5 } }), {
          status: 429,
          headers: { "Content-Type": "application/json" },
        }),
      usageLimitReached: true,
      retryAtMs: null,
    },
    {
      name: "usage limit with expired body reset",
      response: () =>
        new Response(
          JSON.stringify({ error: { type: "usage_limit_reached", resets_at: Math.floor(now / 1_000) - 1 } }),
          {
            status: 429,
            headers: { "Content-Type": "application/json" },
          },
        ),
      usageLimitReached: true,
      retryAtMs: null,
    },
    {
      name: "usage limit with overflowing body reset",
      response: () =>
        new Response(JSON.stringify({ error: { type: "usage_limit_reached", resets_at: Number.MAX_SAFE_INTEGER } }), {
          status: 429,
          headers: { "Content-Type": "application/json" },
        }),
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

Deno.test("exact future body resets_at durably identifies the Codex quota window", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const resetAtSeconds = Math.floor(now / 1_000) + 120;
    const resetAtMs = resetAtSeconds * 1_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;

    const classified = await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(
        JSON.stringify({
          error: {
            type: "usage_limit_reached",
            resets_at: resetAtSeconds,
            resets_in_seconds: 120,
          },
        }),
        {
          status: 429,
          headers: {
            "Content-Type": "application/json",
            "x-codex-primary-reset-at": String(resetAtSeconds),
          },
        },
      ),
      now,
    );
    assert.equal(classified.usageLimitReached, true);
    assert.equal(classified.retryAtMs, resetAtMs);
    assert.equal(classified.quotaBlockSource, "body_resets_at");
    assert.equal(classified.resetDeadlineIsStable, true);
    assert.equal(classified.resetDeadlineConflict, false);

    const state = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(state?.slots[0]?.quota_blocked_until_ms, resetAtMs);
    assert.equal(state?.slots[0]?.quota_block_source, "body_resets_at");
    assert.equal(state?.slots[0]?.observed_reset_at_ms, resetAtMs);
    assert.equal(state?.slots[0]?.observed_reset_at_is_stable, true);
    assert.equal(state?.slots[0]?.banked_reset_generation_ambiguous, false);

    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1);
    assert.equal(selected.kind, "quota_blocked");
    if (selected.kind !== "quota_blocked") return;
    assert.deepEqual(
      selected.blockedAccounts.map(({ quotaResetAtMs, routingGeneration }) => ({
        quotaResetAtMs,
        routingGeneration,
      })),
      [{ quotaResetAtMs: resetAtMs, routingGeneration: state!.slots[0]!.generation }],
    );
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("an ordinary ambiguous deadline does not use the bounded recovery-probe lease", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const firstResetAtMs = now + 60_000;
    const conflictingResetAtMs = now + 120_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;

    await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(
        JSON.stringify({
          error: { type: "usage_limit_reached", resets_at: Math.floor(firstResetAtMs / 1_000) },
        }),
        {
          status: 429,
          headers: {
            "Content-Type": "application/json",
            "Retry-After": new Date(conflictingResetAtMs).toUTCString(),
          },
        },
      ),
      now,
    );
    const conflicted = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(conflicted?.slots[0]?.banked_reset_generation_ambiguous, true);
    assert.equal(conflicted?.slots[0]?.banked_reset_recovery_probe_pending, false);

    const halfOpen = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, conflictingResetAtMs + 1);
    assert.equal(halfOpen.kind, "eligible");
    if (halfOpen.kind !== "eligible") return;
    const claimed = await claimCodexRoutingProbe(singlePool, halfOpen.accounts[0]!, conflictingResetAtMs + 1);
    assert.ok(claimed);
    if (!claimed) return;
    const longResetAtMs = conflictingResetAtMs + 120_000;
    await markCodexQuotaBlocked(
      claimed,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(longResetAtMs).toUTCString() },
      }),
      conflictingResetAtMs + 1,
    );
    const afterProbe = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(afterProbe?.slots[0]?.quota_blocked_until_ms, longResetAtMs);
    assert.equal(afterProbe?.slots[0]?.banked_reset_recovery_probe_pending, false);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("a failed verified-reset probe uses a bounded retry instead of the old quota deadline", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const resetAtMs = now + 60_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now, "gpt-5.3-codex-spark");
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;

    await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(resetAtMs).toUTCString() },
      }),
      now,
    );
    const beforeReset = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    const routingGeneration = beforeReset?.slots[0]?.generation;
    assert.equal(typeof routingGeneration, "number");

    const recovery = await reconcileCodexQuotaAfterVerifiedReset(initial.accounts[0]!, {
      quotaResetAtMs: resetAtMs,
      routingGeneration: routingGeneration as number,
    });
    assert.ok(recovery);
    assert.equal(recovery?.probeCircuit, "quota");
    assert.equal(
      parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)))?.slots[0]?.probe_lease
        ?.quota_class,
      "spark",
    );

    const failedProbe = await markCodexRecoveryProbeQuotaBlocked(
      recovery!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(resetAtMs).toUTCString() },
      }),
      now,
    );
    assert.equal(failedProbe.retryAtMs, resetAtMs);
    const afterProbe = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(afterProbe?.slots[0]?.quota_blocked_until_ms, now + CODEX_HALF_OPEN_LEASE_MS);
    assert.equal(afterProbe?.slots[0]?.banked_reset_generation_ambiguous, true);
    assert.equal(afterProbe?.slots[0]?.banked_reset_recovery_probe_pending, true);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("a non-quota verified-reset probe clears recovery-pending evidence", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const resetAtMs = now + 60_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now, "gpt-5.3-codex-spark");
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;

    await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(resetAtMs).toUTCString() },
      }),
      now,
    );
    const beforeReset = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    const routingGeneration = beforeReset?.slots[0]?.generation;
    assert.equal(typeof routingGeneration, "number");

    const recovery = await reconcileCodexQuotaAfterVerifiedReset(initial.accounts[0]!, {
      quotaResetAtMs: resetAtMs,
      routingGeneration: routingGeneration as number,
    });
    assert.ok(recovery);

    const released = await markCodexRecoveryProbeQuotaBlocked(
      recovery!,
      new Response(JSON.stringify({ error: { type: "rate_limit_error" } }), {
        status: 429,
        headers: { "Content-Type": "application/json" },
      }),
      now,
    );
    assert.equal(released.usageLimitReached, false);
    const afterProbe = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(afterProbe?.slots[0]?.quota_blocked_until_ms, null);
    assert.equal(afterProbe?.slots[0]?.banked_reset_recovery_probe_pending, false);
    assert.equal(afterProbe?.slots[0]?.probe_lease, null);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("a stale verified reset opens a fenced probe without spending another reset", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const resetAtMs = now + 60_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;

    await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(resetAtMs).toUTCString() },
      }),
      now,
    );
    const beforeStaleRecovery = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.ok(beforeStaleRecovery);
    const staleGeneration = beforeStaleRecovery!.slots[0]!.generation + 1;
    await kv.set(CODEX_ACCOUNT_ROUTING_KV_KEY, {
      ...beforeStaleRecovery,
      slots: [{
        ...beforeStaleRecovery!.slots[0]!,
        generation: staleGeneration,
        banked_reset_generation_ambiguous: true,
      }],
    });
    resetCodexAccountRoutingForTest();

    const recovery = await reconcileCodexQuotaAfterStaleVerifiedReset(initial.accounts[0]!, {
      quotaResetAtMs: resetAtMs,
      routingGeneration: staleGeneration,
    });
    assert.ok(recovery);
    assert.equal(recovery?.probeGeneration, staleGeneration + 1);
    const afterRecovery = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(afterRecovery?.slots[0]?.quota_blocked_until_ms, null);
    assert.equal(afterRecovery?.slots[0]?.banked_reset_generation_ambiguous, true);
    assert.ok(afterRecovery?.slots[0]?.probe_lease);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("legacy neutral v2 state permits its first canonical body resets_at fence", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const resetAtSeconds = Math.floor(now / 1_000) + 120;
    const resetAtMs = resetAtSeconds * 1_000;
    const credentialVersion = await codexCredentialVersion(singlePool.accounts[0]!);
    await kv.set(CODEX_ACCOUNT_ROUTING_KV_KEY, {
      v: 2,
      updated_at_ms: now,
      slots: [{
        credential_version: credentialVersion,
        quota_blocked_until_ms: null,
        quota_block_source: null,
        invalid_credential_version: null,
        primary_used_percent: null,
        secondary_used_percent: null,
        observed_reset_at_ms: null,
        generation: 0,
        probe_lease: null,
      }],
    });

    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(
        JSON.stringify({ error: { type: "usage_limit_reached", resets_at: resetAtSeconds } }),
        { status: 429, headers: { "Content-Type": "application/json" } },
      ),
      now,
    );

    const state = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(state?.slots[0]?.generation, 1);
    assert.equal(state?.slots[0]?.quota_block_source, "body_resets_at");
    assert.equal(state?.slots[0]?.observed_reset_at_ms, resetAtMs);
    assert.equal(state?.slots[0]?.observed_reset_at_is_stable, true);
    assert.equal(state?.slots[0]?.banked_reset_generation_ambiguous, false);
    assert.equal(await getCodexQuotaBlockFence(initial.accounts[0]!, resetAtMs), 1);

    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1);
    assert.equal(selected.kind, "quota_blocked");
    if (selected.kind !== "quota_blocked") return;
    assert.deepEqual(
      selected.blockedAccounts.map(({ quotaResetAtMs, routingGeneration }) => ({
        quotaResetAtMs,
        routingGeneration,
      })),
      [{ quotaResetAtMs: resetAtMs, routingGeneration: 1 }],
    );
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("only the exact contaminated first body fence repairs legacy ambiguity", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const resetAtMs = now + 120_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const account = initial.accounts[0]!;
    const contaminatedSlot = {
      account_id_hash: account.accountIdHash,
      credential_version: account.credentialVersion,
      quota_blocked_until_ms: resetAtMs,
      quota_block_source: "body_resets_at",
      invalid_credential_version: null,
      primary_used_percent: 100,
      secondary_used_percent: 0,
      observed_reset_at_ms: resetAtMs,
      observed_reset_at_is_stable: true,
      banked_reset_generation_ambiguous: true,
      generation: 1,
      probe_lease: null,
    };
    const state = (slot: Record<string, unknown>, legacyIdentityUnresolved = false) => ({
      v: 2,
      updated_at_ms: now,
      banked_reset_legacy_identity_unresolved: legacyIdentityUnresolved,
      slots: [slot],
    });

    const repaired = parseCodexAccountRoutingState(state(contaminatedSlot));
    assert.equal(repaired?.slots[0]?.banked_reset_generation_ambiguous, false);
    await kv.set(CODEX_ACCOUNT_ROUTING_KV_KEY, state(contaminatedSlot));
    assert.equal(await getCodexQuotaBlockFence(account, resetAtMs), 1);

    const missingInvalidCredential: Record<string, unknown> = { ...contaminatedSlot };
    delete missingInvalidCredential.invalid_credential_version;
    const nearMisses = [
      { name: "later generation", slot: { ...contaminatedSlot, generation: 2 } },
      { name: "header source", slot: { ...contaminatedSlot, quota_block_source: "header_retry_after" } },
      {
        name: "deadline mismatch",
        slot: { ...contaminatedSlot, quota_blocked_until_ms: resetAtMs + 1 },
      },
      {
        name: "unstable observation",
        slot: { ...contaminatedSlot, observed_reset_at_is_stable: false },
      },
      {
        name: "active probe",
        slot: {
          ...contaminatedSlot,
          probe_lease: { token: "probe", expires_at_ms: now + 30_000, generation: 1 },
        },
      },
      {
        name: "invalid credential",
        slot: { ...contaminatedSlot, invalid_credential_version: account.credentialVersion },
      },
      { name: "missing invalid credential field", slot: missingInvalidCredential },
      { name: "malformed invalid credential field", slot: { ...contaminatedSlot, invalid_credential_version: 1 } },
      { name: "missing account identity", slot: { ...contaminatedSlot, account_id_hash: null } },
    ];
    for (const testCase of nearMisses) {
      const parsed = parseCodexAccountRoutingState(state(testCase.slot));
      assert.equal(parsed?.slots[0]?.banked_reset_generation_ambiguous, true, testCase.name);
    }
    const globallyUnresolved = parseCodexAccountRoutingState(state(contaminatedSlot, true));
    assert.equal(globallyUnresolved?.slots[0]?.banked_reset_generation_ambiguous, true);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("conflicting absolute body and header deadlines permanently fail closed", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const bodyResetAtSeconds = Math.floor(now / 1_000) + 60;
    const headerResetAtMs = now + 120_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;

    const classified = await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(
        JSON.stringify({ error: { type: "usage_limit_reached", resets_at: bodyResetAtSeconds } }),
        {
          status: 429,
          headers: {
            "Content-Type": "application/json",
            "Retry-After": new Date(headerResetAtMs).toUTCString(),
          },
        },
      ),
      now,
    );
    assert.equal(classified.retryAtMs, headerResetAtMs);
    assert.equal(classified.quotaBlockSource, "body_resets_at");
    assert.equal(classified.resetDeadlineIsStable, false);
    assert.equal(classified.resetDeadlineConflict, true);

    const state = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(state?.slots[0]?.banked_reset_generation_ambiguous, true);
    assert.equal(await getCodexQuotaBlockFence(initial.accounts[0]!, headerResetAtMs), null);

    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1);
    assert.equal(selected.kind, "quota_blocked");
    if (selected.kind === "quota_blocked") assert.deepEqual(selected.blockedAccounts, []);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("body resets_at preserves longer relative routing delays without authorizing redemption", async () => {
  const now = 1_700_000_000_000;
  const bodyResetAtSeconds = Math.floor(now / 1_000) + 60;
  for (
    const testCase of [
      { name: "shorter", retryAfter: "30", expectedRetryAtMs: now + 60_000, stable: true, conflict: false },
      { name: "equal", retryAfter: "60", expectedRetryAtMs: now + 60_000, stable: true, conflict: false },
      { name: "longer", retryAfter: "120", expectedRetryAtMs: now + 120_000, stable: false, conflict: true },
    ] as const
  ) {
    const classified = await readCodex429(
      new Response(
        JSON.stringify({ error: { type: "usage_limit_reached", resets_at: bodyResetAtSeconds } }),
        {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": testCase.retryAfter },
        },
      ),
      now,
    );
    assert.equal(classified.retryAtMs, testCase.expectedRetryAtMs, testCase.name);
    assert.equal(classified.quotaBlockSource, "body_resets_at", testCase.name);
    assert.equal(classified.resetDeadlineIsStable, testCase.stable, testCase.name);
    assert.equal(classified.resetDeadlineConflict, testCase.conflict, testCase.name);
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

Deno.test("eligible routing exposes only stable blocked siblings to the banked-reset cohort", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const retryAtMs = now + 120_000;
    const initial = await selectCodexRoutingAccounts(pool, pool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;

    const classified = await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: {
          "Content-Type": "application/json",
          "Retry-After": new Date(retryAtMs).toUTCString(),
        },
      }),
      now,
    );
    assert.equal(classified.resetDeadlineIsStable, true);

    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(pool, pool.accounts, now + 1);
    assert.equal(selected.kind, "eligible");
    if (selected.kind !== "eligible") return;
    assert.deepEqual(selected.accounts.map((account) => account.slot), [1]);
    assert.equal(selected.accounts[0]?.probeRequired, false);
    assert.equal(selected.blockedAccounts.length, 1);
    assert.equal(selected.blockedAccounts[0]?.slot, 0);
    assert.equal(selected.blockedAccounts[0]?.quotaResetAtMs, retryAtMs);
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
    assert.deepEqual(afterOne.blockedAccounts, []);

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
    if (halfOpen.kind === "eligible") {
      assert.equal(halfOpen.accounts.length, 1);
      assert.equal(halfOpen.accounts[0]?.probeRequired, true);
      assert.deepEqual(halfOpen.blockedAccounts, []);
    }
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

Deno.test("same-account credential rotation retains an active upstream timeout circuit", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    await markCodexUpstreamTimeout(initial.accounts[0]!, now);

    const rotated: CodexAuthPoolState = {
      accounts: [{ ...singlePool.accounts[0]!, access_token: "rotated-access", updated_at_ms: now + 1 }],
      updated_at_ms: now + 1,
    };
    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(rotated, rotated.accounts, now + 1);
    assert.equal(selected.kind, "upstream_blocked");
    if (selected.kind === "upstream_blocked") {
      assert.equal(selected.retryAtMs, now + CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS);
    }
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("credential refresh transfers an owned upstream-timeout probe", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    await markCodexUpstreamTimeout(initial.accounts[0]!, now - CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1);
    const halfOpen = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(halfOpen.kind, "eligible");
    if (halfOpen.kind !== "eligible") return;
    const probe = await claimCodexRoutingProbe(singlePool, halfOpen.accounts[0]!, now);
    assert.ok(probe);
    if (!probe) return;

    const rotated = {
      ...singlePool.accounts[0]!,
      access_token: "rotated-access",
      updated_at_ms: now + 1,
    };
    const reconciled = await reconcileCodexRoutingAccount(probe, rotated);
    assert.equal(reconciled.probeCircuit, "upstream_timeout");
    assert.equal(reconciled.probeToken, probe.probeToken);
    assert.notEqual(reconciled.probeGeneration, probe.probeGeneration);
    const transferred = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(transferred?.slots[0]?.probe_lease?.circuit, "upstream_timeout");
    assert.equal(transferred?.slots[0]?.probe_lease?.generation, reconciled.probeGeneration);

    await markCodexSuccess(reconciled);
    const recovered = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(recovered?.slots[0]?.upstream_timeout_blocked_until_ms, null);
    assert.equal(recovered?.slots[0]?.probe_lease, null);
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
    assert.equal(released.kind, "quota_blocked");
    if (released.kind === "quota_blocked") {
      assert.ok(released.retryAtMs !== null && released.retryAtMs > now + 1);
    }

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

Deno.test("fresh Spark capacity reconciles a blocked account and sends it first", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(pool, pool.accounts, now, "gpt-5.3-codex-spark");
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const accountTwo = initial.accounts.find((account) => account.auth.account_id === "two");
    assert.ok(accountTwo);
    const blockedUntil = now + 60_000;
    await markCodexQuotaBlocked(
      accountTwo,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(blockedUntil).toUTCString() },
      }),
      now,
    );

    await recordCodexCapacityRoutingObservations([{
      slot: 1,
      account_id: "two",
      state: "available",
      source_observed_at_ms: now + 1,
      snapshot_at_ms: now + 1,
      windows: {
        primary: { limit_window_seconds: 604_800, used_percent: 100, reset_at_ms: now + 604_800_000 },
        secondary: null,
      },
      additional_rate_limits: [{
        limit_name: "GPT-5.3-Codex-Spark",
        metered_feature: "codex_bengalfox",
        windows: {
          primary: { limit_window_seconds: 18_000, used_percent: 50, reset_at_ms: now + 18_000_000 },
          secondary: null,
        },
      }],
    }], now + 1);

    const selected = await selectCodexRoutingAccounts(pool, pool.accounts, now + 2, "gpt-5.3-codex-spark");
    assert.equal(selected.kind, "eligible");
    if (selected.kind !== "eligible") return;
    assert.equal(selected.accounts[0]?.auth.account_id, "two");
    assert.equal(selected.accounts[0]?.quotaHeadroom, 50);
    assert.equal(selected.blockedAccounts.length, 0);

    const state = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(state?.slots[1]?.quota_blocked_until_ms, null);
    assert.equal(state?.slots[1]?.primary_used_percent, 100);
    assert.equal(state?.slots[1]?.capacity_observed_at_ms, now + 1);
    const storedObservation = JSON.stringify(kv.values.get(key(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY)));
    assert.equal(storedObservation.includes("account_id"), true);
    assert.equal(storedObservation.includes('"two"'), false);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("a newer inference 429 remains authoritative over an older positive capacity sample", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(pool, pool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    await recordCodexCapacityRoutingObservations([{
      slot: 1,
      account_id: "two",
      state: "available",
      source_observed_at_ms: now + 1,
      snapshot_at_ms: now + 1,
      windows: {
        primary: { limit_window_seconds: 10_800, used_percent: 50, reset_at_ms: now + 10_800_000 },
        secondary: { limit_window_seconds: 86_400, used_percent: 50, reset_at_ms: now + 86_400_000 },
      },
      additional_rate_limits: [],
    }], now + 1);
    const positive = await selectCodexRoutingAccounts(pool, pool.accounts, now + 2);
    assert.equal(positive.kind, "eligible");
    if (positive.kind !== "eligible") return;
    const accountTwo = positive.accounts.find((account) => account.auth.account_id === "two");
    assert.ok(accountTwo);
    const blockedUntil = now + 60_000;
    await markCodexQuotaBlocked(
      accountTwo,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(blockedUntil).toUTCString() },
      }),
      now + 2,
    );

    const selected = await selectCodexRoutingAccounts(pool, pool.accounts, now + 3);
    assert.equal(selected.kind, "eligible");
    if (selected.kind !== "eligible") return;
    assert.equal(selected.accounts.some((account) => account.auth.account_id === "two"), false);
    assert.equal(selected.blockedAccounts.some((account) => account.auth.account_id === "two"), true);
    const state = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(state?.slots[1]?.quota_blocked_until_ms, blockedUntil);
    assert.equal(state?.slots[1]?.quota_signal_observed_at_ms, now + 2);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("stale positive capacity cannot reopen a quota circuit", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const deadline = now + 60_000;
    await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(deadline).toUTCString() },
      }),
      now,
    );
    await recordCodexCapacityRoutingObservations([{
      slot: 0,
      account_id: "one",
      state: "available",
      source_observed_at_ms: now - CODEX_CAPACITY_ROUTING_MAX_AGE_MS - 1,
      snapshot_at_ms: now - CODEX_CAPACITY_ROUTING_MAX_AGE_MS - 1,
      windows: {
        primary: { limit_window_seconds: 10_800, used_percent: 50, reset_at_ms: now + 10_800_000 },
        secondary: null,
      },
      additional_rate_limits: [],
    }], now);
    const selected = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now + 1);
    assert.equal(selected.kind, "quota_blocked");
    const state = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(state?.slots[0]?.quota_blocked_until_ms, deadline);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("the persisted analytics snapshot reopens the matching account for its model", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(pool, pool.accounts, now, "gpt-5.3-codex-spark");
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const accountTwo = initial.accounts.find((account) => account.auth.account_id === "two");
    assert.ok(accountTwo);
    const deadline = now + 60_000;
    await markCodexQuotaBlocked(
      accountTwo,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(deadline).toUTCString() },
      }),
      now,
    );
    await kv.set(PROVIDER_CAPACITY_SNAPSHOT_KEY, {
      snapshot_at_ms: now + 1,
      sources: [{
        source: "codex",
        slot: 2,
        state: "available",
        source_observed_at_ms: now + 1,
        snapshot_at_ms: now + 1,
        windows: {
          primary: { limit_window_seconds: 604_800, used_percent: 100, reset_at_ms: now + 604_800_000 },
          secondary: null,
        },
        additional_rate_limits: [{
          limit_name: "GPT-5.3-Codex-Spark",
          metered_feature: "codex_bengalfox",
          windows: {
            primary: { limit_window_seconds: 18_000, used_percent: 50, reset_at_ms: now + 18_000_000 },
            secondary: null,
          },
        }],
      }],
    });
    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(pool, pool.accounts, now + 2, "gpt-5.3-codex-spark");
    assert.equal(selected.kind, "eligible");
    if (selected.kind !== "eligible") return;
    assert.equal(selected.accounts[0]?.auth.account_id, "two");
    assert.equal(selected.accounts[0]?.quotaHeadroom, 50);
    const state = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(state?.slots[1]?.quota_blocked_until_ms, null);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("a legacy slot-only dashboard snapshot cannot attach to a replacement account", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const replacement = {
      access_token: "replacement-access",
      refresh_token: "replacement-refresh",
      account_id: "replacement",
      updated_at_ms: now + 1,
    };
    const replacementPool: CodexAuthPoolState = { accounts: [replacement], updated_at_ms: now + 1 };
    const initial = await selectCodexRoutingAccounts(replacementPool, replacementPool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const deadline = now + 60_000;
    await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(deadline).toUTCString() },
      }),
      now + 1,
    );
    await kv.set(PROVIDER_CAPACITY_SNAPSHOT_KEY, {
      snapshot_at_ms: now,
      sources: [{
        source: "codex",
        slot: 1,
        state: "available",
        source_observed_at_ms: now,
        snapshot_at_ms: now,
        windows: {
          primary: { limit_window_seconds: 10_800, used_percent: 50, reset_at_ms: now + 10_800_000 },
          secondary: null,
        },
        additional_rate_limits: [],
      }],
    });
    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(replacementPool, replacementPool.accounts, now + 2);
    assert.equal(selected.kind, "quota_blocked");
    const state = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(state?.slots[0]?.quota_blocked_until_ms, deadline);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("capacity reconciliation uses the requested model instead of any additional headroom", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(pool, pool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const accountTwo = initial.accounts.find((account) => account.auth.account_id === "two");
    assert.ok(accountTwo);
    const blockedUntil = now + 60_000;
    await markCodexQuotaBlocked(
      accountTwo,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(blockedUntil).toUTCString() },
      }),
      now,
    );

    await recordCodexCapacityRoutingObservations([{
      slot: 1,
      account_id: "two",
      state: "available",
      source_observed_at_ms: now + 1,
      snapshot_at_ms: now + 1,
      windows: {
        primary: { limit_window_seconds: 10_800, used_percent: 100, reset_at_ms: now + 10_800_000 },
        secondary: null,
      },
      additional_rate_limits: [{
        limit_name: "GPT-5.3-Codex-Spark",
        metered_feature: "codex_bengalfox",
        windows: {
          primary: { limit_window_seconds: 18_000, used_percent: 50, reset_at_ms: now + 18_000_000 },
          secondary: null,
        },
      }],
    }], now + 1);

    const nonSpark = await selectCodexRoutingAccounts(pool, pool.accounts, now + 2, "gpt-5.2-codex");
    assert.equal(nonSpark.kind, "eligible");
    if (nonSpark.kind !== "eligible") return;
    assert.equal(nonSpark.accounts.some((account) => account.auth.account_id === "two"), false);
    assert.equal(nonSpark.blockedAccounts.some((account) => account.auth.account_id === "two"), true);

    const spark = await selectCodexRoutingAccounts(pool, pool.accounts, now + 2, "gpt-5.3-codex-spark");
    assert.equal(spark.kind, "eligible");
    if (spark.kind !== "eligible") return;
    assert.equal(spark.accounts[0]?.auth.account_id, "two");
    assert.equal(spark.accounts[0]?.quotaHeadroom, 50);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("capacity reconciliation preserves reset ambiguity and an active recovery probe", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const now = 1_700_000_000_000;
    const initial = await selectCodexRoutingAccounts(singlePool, singlePool.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const account = initial.accounts[0]!;
    const resetAtMs = now + 60_000;
    await markCodexQuotaBlocked(
      account,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(resetAtMs).toUTCString() },
      }),
      now,
    );
    const routingGeneration = await getCodexQuotaBlockFence(account, resetAtMs);
    assert.equal(typeof routingGeneration, "number");
    if (routingGeneration === null) return;
    const recoveryProbe = await reconcileCodexQuotaAfterVerifiedReset(account, {
      quotaResetAtMs: resetAtMs,
      routingGeneration,
    });
    assert.ok(recoveryProbe);
    if (!recoveryProbe) return;
    const before = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(before?.slots[0]?.banked_reset_generation_ambiguous, true);
    assert.equal(before?.slots[0]?.banked_reset_recovery_probe_pending, true);
    assert.equal(before?.slots[0]?.probe_lease?.token, recoveryProbe.probeToken);

    await recordCodexCapacityRoutingObservations([{
      slot: 0,
      account_id: "one",
      state: "available",
      source_observed_at_ms: now + 1,
      snapshot_at_ms: now + 1,
      windows: {
        primary: { limit_window_seconds: 10_800, used_percent: 50, reset_at_ms: now + 10_800_000 },
        secondary: null,
      },
      additional_rate_limits: [],
    }], now + 1);

    const after = parseCodexAccountRoutingState(kv.values.get(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    assert.equal(after?.slots[0]?.banked_reset_generation_ambiguous, true);
    assert.equal(after?.slots[0]?.banked_reset_recovery_probe_pending, true);
    assert.equal(after?.slots[0]?.observed_reset_at_ms, resetAtMs);
    assert.equal(after?.slots[0]?.probe_lease?.token, recoveryProbe.probeToken);
    assert.equal(after?.slots[0]?.generation, before?.slots[0]?.generation);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("capacity observations expire old account identities from durable routing state", async () => {
  const kv = new RoutingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAccountRoutingForTest();
  try {
    const firstAtMs = 1_700_000_000_000;
    const secondAtMs = firstAtMs + CODEX_CAPACITY_ROUTING_MAX_AGE_MS + 1;
    const observation = (accountId: string, snapshotAtMs: number, usedPercent: number) => ({
      slot: 0,
      account_id: accountId,
      state: "available" as const,
      source_observed_at_ms: snapshotAtMs,
      snapshot_at_ms: snapshotAtMs,
      windows: {
        primary: { limit_window_seconds: 10_800, used_percent: usedPercent, reset_at_ms: snapshotAtMs + 10_800_000 },
        secondary: null,
      },
      additional_rate_limits: [],
    });

    await recordCodexCapacityRoutingObservations([observation("replaced-account", firstAtMs, 20)], firstAtMs);
    await recordCodexCapacityRoutingObservations([observation("current-account", secondAtMs, 40)], secondAtMs);

    const stored = kv.values.get(key(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY)) as {
      observations?: readonly { snapshot_at_ms?: number; windows?: { primary?: { used_percent?: number } } }[];
    } | undefined;
    assert.equal(stored?.observations?.length, 1);
    assert.equal(stored?.observations?.[0]?.snapshot_at_ms, secondAtMs);
    assert.equal(stored?.observations?.[0]?.windows?.primary?.used_percent, 40);
  } finally {
    setKvForTest(null);
    resetCodexAccountRoutingForTest();
  }
});
