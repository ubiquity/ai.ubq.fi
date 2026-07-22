import assert from "node:assert/strict";
import { sha256Base64Url } from "../src/utils.ts";

if (typeof Deno.KvU64 !== "function") {
  (Deno as unknown as { KvU64: typeof Deno.KvU64 }).KvU64 = class {
    constructor(readonly value: bigint) {}
  } as typeof Deno.KvU64;
}

const encodeKey = (key: Deno.KvKey): string => JSON.stringify(key);

class CountingKv {
  readonly values = new Map<string, unknown>();
  reads = 0;
  readUnits = 0;
  writes = 0;
  sums = 0;
  retries = 0;

  resetCounts(): void {
    this.reads = 0;
    this.readUnits = 0;
    this.writes = 0;
    this.sums = 0;
    this.retries = 0;
  }

  get<T>(key: Deno.KvKey, _options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
    this.reads += 1;
    const value = this.values.get(encodeKey(key)) as T | undefined;
    const bytes = value === undefined
      ? 0
      : new TextEncoder().encode(JSON.stringify(value, (_, item) => typeof item === "bigint" ? item.toString() : item))
        .length;
    this.readUnits += Math.max(1, Math.ceil(bytes / 4096));
    return Promise.resolve({
      key,
      value: value ?? null,
      versionstamp: value === undefined ? null : "00000000000000000001",
    } as Deno.KvEntryMaybe<T>);
  }

  set(key: Deno.KvKey, value: unknown): Promise<Deno.KvCommitResult> {
    this.values.set(encodeKey(key), value);
    this.writes += 1;
    return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" });
  }

  delete(key: Deno.KvKey): Promise<void> {
    this.values.delete(encodeKey(key));
    this.writes += 1;
    return Promise.resolve();
  }

  list<T>(selector: Deno.KvListSelector): Deno.KvListIterator<T> {
    const prefix = "prefix" in selector ? selector.prefix : [];
    const entries = [...this.values].filter(([encoded]) => {
      const key = JSON.parse(encoded) as Deno.KvKey;
      return prefix.every((part, index) => part === key[index]);
    });
    return (async function* () {
      for (const [encoded, value] of entries) {
        const key = JSON.parse(encoded) as Deno.KvKey;
        yield { key, value, versionstamp: "00000000000000000001" } as Deno.KvEntry<T>;
      }
    })() as unknown as Deno.KvListIterator<T>;
  }

  atomic(): Deno.AtomicOperation {
    const mutations: Array<
      | { kind: "set"; key: Deno.KvKey; value: unknown }
      | { kind: "delete"; key: Deno.KvKey }
      | { kind: "sum"; key: Deno.KvKey; value: bigint }
    > = [];
    const operation = {
      check: () => operation,
      set: (key: Deno.KvKey, value: unknown) => {
        mutations.push({ kind: "set", key, value });
        return operation;
      },
      delete: (key: Deno.KvKey) => {
        mutations.push({ kind: "delete", key });
        return operation;
      },
      sum: (key: Deno.KvKey, value: bigint) => {
        mutations.push({ kind: "sum", key, value });
        return operation;
      },
      commit: () => {
        for (const mutation of mutations) {
          const encoded = encodeKey(mutation.key);
          if (mutation.kind === "delete") this.values.delete(encoded);
          else if (mutation.kind === "set") this.values.set(encoded, mutation.value);
          else {
            const current = this.values.get(encoded) as Deno.KvU64 | undefined;
            this.values.set(encoded, new Deno.KvU64((current?.value ?? 0n) + mutation.value));
            this.sums += 1;
          }
          this.writes += 1;
        }
        return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" });
      },
    };
    return operation as unknown as Deno.AtomicOperation;
  }
}

const kv = new CountingKv();
(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kv as unknown as Deno.Kv);

const { default: handler } = await import("../src/handler.ts");
const {
  apiKeyPolicyFromHashRecord,
  apiKeyUsageV2Key,
  authenticateApiKeyToken,
  invalidateApiKeyPolicy,
  resetApiKeyPolicyCacheForTest,
} = await import("../src/api_key_policy.ts");
const {
  loadRuntimeConfig,
  RUNTIME_CONFIG_CACHE_TTL_MS,
  RUNTIME_CONFIG_V2_KEY,
  resetRuntimeConfigCacheForTest,
} = await import("../src/runtime_config.ts");
const { resetCodexRateLimitCacheForTest } = await import("../src/codex_rate_limit.ts");
const { resetCodexAuthCacheForTest } = await import("../src/codex.ts");

const MODEL = "gpt-5-kv-budget";
const now = Date.now();
const runtime = {
  version: 2,
  default_model: MODEL,
  default_reasoning_effort: "medium",
  codex_models: {
    source: "chatgpt_codex",
    client_version: "0.150.0",
    updated_at_ms: now,
    models: [{
      slug: MODEL,
      default_reasoning_level: "medium",
      supported_reasoning_levels: ["none", "low", "medium", "high"],
    }],
  },
  updated_at_ms: now,
};

const seedKey = async (token: string, id: string, limit: number) => {
  const hash = await sha256Base64Url(token);
  const record = {
    id,
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: limit,
    usage_requests: 0,
    usage_reset_at_ms: now + 60_000,
    window_ms: 60_000,
    paid_fallback_enabled: false,
    paid_fallback_limit_microcredits: 0,
    paid_fallback_spent_microcredits: 0,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
  };
  kv.values.set(encodeKey(["ubq_ai", "api_keys", "hash", hash]), record);
  return { hash, record };
};

const request = (token: string): Request =>
  new Request("https://ai.ubq.fi/v1/responses", {
    method: "POST",
    headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
    body: JSON.stringify({ input: "ping" }),
  });

const sse = (): Response =>
  new Response(
    `data: ${
      JSON.stringify({
        type: "response.completed",
        response: { model: MODEL, output: [], usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 } },
      })
    }\n\n`,
    { status: 200, headers: { "Content-Type": "text/event-stream" } },
  );

Deno.test("KV budget: warm unlimited inference performs zero KV operations", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexRateLimitCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(["uos_ai", "runtime_config", "v2"]), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), {
    access_token: "access",
    refresh_token: "refresh",
    account_id: "acct",
    updated_at_ms: Date.now(),
  });
  const token = `u_${"1".repeat(64)}`;
  await seedKey(token, "unlimited", -1);
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => Promise.resolve(sse());
  try {
    kv.resetCounts();
    assert.equal((await handler(request(token))).status, 200);
    assert.ok(kv.reads <= 4, `cold inference used ${kv.reads} reads`);
    assert.ok(kv.readUnits <= 4, `cold inference used ${kv.readUnits} 4KiB read units`);

    kv.resetCounts();
    assert.equal((await handler(request(token))).status, 200);
    assert.deepEqual({ reads: kv.reads, writes: kv.writes }, { reads: 0, writes: 0 });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("KV budget: warm bounded inference reads once and atomically sums once", async () => {
  const token = `u_${"2".repeat(64)}`;
  const { hash, record } = await seedKey(token, "bounded", 100);
  resetApiKeyPolicyCacheForTest();
  const policy = apiKeyPolicyFromHashRecord(hash, record, now);
  assert.ok(policy);
  kv.values.set(encodeKey(apiKeyUsageV2Key(policy)), new Deno.KvU64(0n));
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => Promise.resolve(sse());
  try {
    assert.equal((await handler(request(token))).status, 200);
    kv.resetCounts();
    assert.equal((await handler(request(token))).status, 200);
    assert.deepEqual({ reads: kv.reads, writes: kv.writes, sums: kv.sums, retries: kv.retries }, {
      reads: 1,
      writes: 1,
      sums: 1,
      retries: 0,
    });

    kv.resetCounts();
    await Promise.all(Array.from({ length: 20 }, () => handler(request(token))));
    assert.equal(kv.retries, 0);
    assert.equal(kv.sums, 20);
    assert.equal((kv.values.get(encodeKey(apiKeyUsageV2Key(policy))) as Deno.KvU64).value, 22n);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("KV budget: changing only a bounded limit preserves the active v2 counter", async () => {
  const token = `u_${"4".repeat(64)}`;
  const { hash, record } = await seedKey(token, "limit-change", 100);
  const original = apiKeyPolicyFromHashRecord(hash, record, now);
  const lowered = apiKeyPolicyFromHashRecord(hash, { ...record, usage_limit_requests: 10 }, now);
  assert.ok(original && lowered);
  assert.deepEqual(apiKeyUsageV2Key(lowered), apiKeyUsageV2Key(original));
  kv.values.set(encodeKey(apiKeyUsageV2Key(original)), new Deno.KvU64(12n));
  kv.values.set(encodeKey(["ubq_ai", "api_keys", "hash", hash]), { ...record, usage_limit_requests: 10 });
  resetApiKeyPolicyCacheForTest();
  const decision = await authenticateApiKeyToken(token, { kv: kv as unknown as Deno.Kv, nowMs: now });
  assert.equal(decision.ok, false);
  if (!decision.ok) assert.equal(decision.response.status, 429);
});

Deno.test("KV budget: runtime configuration revalidates after the bounded isolate TTL", async () => {
  resetRuntimeConfigCacheForTest();
  const first = structuredClone(runtime);
  const second = {
    ...runtime,
    default_model: "gpt-5-kv-budget-next",
    codex_models: {
      ...runtime.codex_models,
      models: [{
        slug: "gpt-5-kv-budget-next",
        default_reasoning_level: "high",
        supported_reasoning_levels: ["none", "high"],
      }],
    },
    updated_at_ms: now + 1,
  };
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), first);
  kv.resetCounts();
  assert.equal((await loadRuntimeConfig(kv as unknown as Deno.Kv, now))?.default_model, MODEL);
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), second);
  assert.equal(
    (await loadRuntimeConfig(kv as unknown as Deno.Kv, now + RUNTIME_CONFIG_CACHE_TTL_MS - 1))?.default_model,
    MODEL,
  );
  const refreshed = await Promise.all(
    Array.from(
      { length: 20 },
      () => loadRuntimeConfig(kv as unknown as Deno.Kv, now + RUNTIME_CONFIG_CACHE_TTL_MS + 1),
    ),
  );
  assert.ok(refreshed.every((config) => config?.default_model === second.default_model));
  assert.equal(kv.reads, 2);
});

Deno.test("KV budget: malformed tokens are rejected without KV and policy expiry refreshes revocation", async () => {
  kv.resetCounts();
  assert.equal((await handler(request("malformed"))).status, 401);
  assert.deepEqual({ reads: kv.reads, writes: kv.writes }, { reads: 0, writes: 0 });

  const token = `u_${"3".repeat(64)}`;
  const { hash, record } = await seedKey(token, "revoked", -1);
  resetApiKeyPolicyCacheForTest();
  assert.equal((await authenticateApiKeyToken(token, { kv: kv as unknown as Deno.Kv, nowMs: now })).ok, true);
  kv.values.set(encodeKey(["ubq_ai", "api_keys", "hash", hash]), { ...record, revoked_at_ms: now + 1 });
  assert.equal(
    (await authenticateApiKeyToken(token, { kv: kv as unknown as Deno.Kv, nowMs: now + 29_999 })).ok,
    true,
  );
  assert.equal(
    (await authenticateApiKeyToken(token, { kv: kv as unknown as Deno.Kv, nowMs: now + 30_001 })).ok,
    false,
  );
  invalidateApiKeyPolicy("revoked");
});
