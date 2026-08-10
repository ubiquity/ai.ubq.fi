import assert from "node:assert/strict";

const encodeKey = (key: Deno.KvKey): string => JSON.stringify(key);

class ExperimentKv {
  readonly values = new Map<string, unknown>();
  private readonly revisions = new Map<string, number>();
  private nextRevision = 1;

  clear(): void {
    this.values.clear();
    this.revisions.clear();
    this.nextRevision = 1;
  }

  put(key: Deno.KvKey, value: unknown): void {
    this.write(key, value);
  }

  private versionstamp(key: Deno.KvKey): string | null {
    const revision = this.revisions.get(encodeKey(key));
    return revision === undefined ? null : String(revision).padStart(20, "0");
  }

  private write(key: Deno.KvKey, value: unknown): void {
    const encoded = encodeKey(key);
    this.values.set(encoded, value);
    this.revisions.set(encoded, this.nextRevision++);
  }

  private remove(key: Deno.KvKey): void {
    const encoded = encodeKey(key);
    this.values.delete(encoded);
    this.revisions.delete(encoded);
    this.nextRevision += 1;
  }

  get<T>(key: Deno.KvKey, options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
    assert.equal(options?.consistency, "strong");
    return Promise.resolve({
      key,
      value: (this.values.get(encodeKey(key)) ?? null) as T | null,
      versionstamp: this.versionstamp(key),
    } as Deno.KvEntryMaybe<T>);
  }

  set(key: Deno.KvKey, value: unknown): Promise<Deno.KvCommitResult> {
    this.write(key, value);
    return Promise.resolve({ ok: true, versionstamp: this.versionstamp(key)! });
  }

  delete(key: Deno.KvKey): Promise<void> {
    this.remove(key);
    return Promise.resolve();
  }

  atomic(): Deno.AtomicOperation {
    const checks: Array<{ key: Deno.KvKey; versionstamp: string | null }> = [];
    const writes: Array<{ type: "set" | "delete"; key: Deno.KvKey; value?: unknown }> = [];
    const chain = {
      check: (...entries: Array<{ key: Deno.KvKey; versionstamp: string | null }>) => {
        checks.push(...entries);
        return chain;
      },
      set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
        writes.push({ type: "set", key, value });
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        writes.push({ type: "delete", key });
        return chain;
      },
      commit: () => {
        if (checks.some((entry) => this.versionstamp(entry.key) !== entry.versionstamp)) {
          return Promise.resolve({ ok: false, versionstamp: null } as const);
        }
        for (const write of writes) {
          if (write.type === "set") this.write(write.key, write.value);
          else this.remove(write.key);
        }
        return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" } as const);
      },
    };
    return chain as unknown as Deno.AtomicOperation;
  }
}

const kv = new ExperimentKv();
(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kv as unknown as Deno.Kv);

const { setKvForTest } = await import("../src/kv.ts");
const { resetCodexAuthCacheForTest } = await import("../src/codex.ts");
const { resetRuntimeConfigCacheForTest } = await import("../src/runtime_config.ts");
const { handleAdminCodexCacheScopeExperiment } = await import("../src/admin.ts");
const {
  PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX,
  PromptCacheScopeExperimentBusyError,
  PromptCacheScopeExperimentFailedError,
  runPromptCacheScopeExperiment,
} = await import("../src/prompt_cache_scope_experiment.ts");

const AUTH_KEY: Deno.KvKey = ["ubq_ai", "codex_auth"];
const RUNTIME_KEY: Deno.KvKey = ["uos_ai", "runtime_config", "v2"];
const MODEL = "gpt-5.6-cache-scope-fixture";

const makeAuth = (label: string) => ({
  access_token: `access-${label}`,
  refresh_token: `refresh-${label}`,
  account_id: `account-${label}`,
  updated_at_ms: Date.now(),
});

const seed = (): void => {
  kv.clear();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAuthCacheForTest();
  resetRuntimeConfigCacheForTest();
  kv.put(
    AUTH_KEY,
    {
      accounts: [makeAuth("one"), makeAuth("two")],
      updated_at_ms: Date.now(),
    },
  );
  kv.put(
    RUNTIME_KEY,
    {
      version: 2,
      default_model: MODEL,
      default_reasoning_effort: "none",
      codex_models: {
        source: "chatgpt_codex",
        updated_at_ms: Date.now(),
        models: [{ slug: MODEL, supported_reasoning_levels: ["none"] }],
      },
      updated_at_ms: Date.now(),
    },
  );
};

const sseCompleted = (cachedTokens: number, cacheWriteTokens: number | undefined): Response => {
  const usage = {
    input_tokens: 3_000,
    output_tokens: 1,
    total_tokens: 3_001,
    input_tokens_details: {
      cached_tokens: cachedTokens,
      ...(cacheWriteTokens === undefined ? {} : { cache_write_tokens: cacheWriteTokens }),
    },
  };
  const event = `data: ${JSON.stringify({ type: "response.completed", response: { usage } })}\n\n`;
  const stream = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(new TextEncoder().encode(event));
      controller.close();
    },
  });
  return new Response(stream, { status: 200, headers: { "Content-Type": "text/event-stream" } });
};

Deno.test("cache scope endpoint rejects every request field before dispatch", async () => {
  const response = await handleAdminCodexCacheScopeExperiment(
    new Request("https://ai.ubq.fi/admin/providers/codex/cache-scope-experiment", {
      method: "POST",
      body: JSON.stringify({ slot: 1, cache_affinity: "forbidden" }),
    }),
  );
  assert.equal(response.status, 400);
});

Deno.test("cache scope experiment uses serial exact-slot evidence and persists only sanitized numeric results", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  const requests: Array<{ account: string | null; conversation: string | null; body: string }> = [];
  let responseIndex = 0;
  let refreshes = 0;
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url === "https://auth.openai.com/oauth/token") {
      refreshes += 1;
      return Promise.resolve(
        new Response(
          JSON.stringify({
            access_token: `access-refreshed-${refreshes}`,
            refresh_token: `refresh-refreshed-${refreshes}`,
          }),
          {
            status: 200,
            headers: { "Content-Type": "application/json" },
          },
        ),
      );
    }
    requests.push({
      account: request.headers.get("chatgpt-account-id"),
      conversation: request.headers.get("conversation_id"),
      body: init?.body as string,
    });
    const step = responseIndex++ % 10;
    const cachedTokens = [0, 2_560, 0, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560][step]!;
    const cacheWriteTokens = [2_560, 0, 2_560, 0, 0, 0, 0, 0, 0, 0][step]!;
    return Promise.resolve(sseCompleted(cachedTokens, cacheWriteTokens));
  };

  try {
    const result = await runPromptCacheScopeExperiment();
    assert.equal(result.classification, "account_scoped");
    assert.equal(result.cycles.length, 3);
    assert.equal(requests.length, 30);
    assert.equal(refreshes, 3);

    for (let cycle = 0; cycle < 3; cycle += 1) {
      const rows = requests.slice(cycle * 10, (cycle + 1) * 10);
      assert.deepEqual(rows.map((row) => row.account), [
        "account-one",
        "account-one",
        "account-two",
        "account-two",
        "account-one",
        "account-one",
        "account-one",
        "account-one",
        "account-one",
        "account-one",
      ]);
      assert.equal(rows.every((row) => row.body === rows[0]?.body), true, "the body must stay fixed within a cycle");
      const body = JSON.parse(rows[0]?.body ?? "{}") as Record<string, unknown>;
      assert.equal(
        body.stream,
        true,
        "the runner consumes upstream SSE itself while returning a buffered admin result",
      );
      assert.deepEqual(body.prompt_cache_options, { mode: "explicit", ttl: "30m" });
      assert.equal(rows[0]?.conversation, rows[6]?.conversation);
      assert.notEqual(rows[0]?.conversation, rows[7]?.conversation);
      assert.equal(rows[0]?.conversation, rows[9]?.conversation);
    }

    const persisted = kv.values.get(
      encodeKey([...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "chatgpt_codex", MODEL]),
    ) as {
      cycles?: Array<{
        samples?: Array<{
          raw_usage?: Record<string, unknown>;
          normalized_usage?: Record<string, unknown>;
        }>;
      }>;
    };
    const serialized = JSON.stringify(persisted);
    assert.match(serialized, /account_scoped/);
    assert.match(serialized, /"failure_code":null/);
    const firstSample = persisted.cycles?.[0]?.samples?.[0];
    assert.deepEqual(firstSample?.raw_usage, {
      input_tokens: 3_000,
      cached_tokens: 0,
      cache_write_tokens: 2_560,
      output_tokens: 1,
      total_tokens: 3_001,
    });
    assert.deepEqual(firstSample?.normalized_usage, firstSample?.raw_usage);
    assert.equal(
      Object.values(firstSample?.raw_usage ?? {}).every((value) => typeof value === "number"),
      true,
      "stored raw usage must contain bounded numeric fields only",
    );
    assert.equal(
      Object.values(firstSample?.normalized_usage ?? {}).every((value) => typeof value === "number"),
      true,
      "stored normalized usage must contain bounded numeric fields only",
    );
    assert.equal(serialized.includes("account-one"), false);
    assert.equal(serialized.includes("uos-cache-scope-v1"), false);
    assert.equal(serialized.includes("cache-scope-cycle"), false);
    assert.equal(serialized.includes("conversation"), false);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("cache scope experiment persists a safe failure code when cache-write telemetry is unavailable", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  let inferenceCalls = 0;
  globalThis.fetch = (input) => {
    const url = input instanceof Request ? input.url : input instanceof URL ? input.toString() : input;
    if (url === "https://auth.openai.com/oauth/token") {
      throw new Error("refresh must not run after invalid warm telemetry");
    }
    inferenceCalls += 1;
    return Promise.resolve(sseCompleted(0, undefined));
  };

  try {
    await assert.rejects(
      () => runPromptCacheScopeExperiment(),
      (error: unknown) =>
        error instanceof PromptCacheScopeExperimentFailedError && error.failureCode === "execution_failed",
    );
    assert.equal(inferenceCalls, 1);
    const persisted = kv.values.get(
      encodeKey([...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "chatgpt_codex", MODEL]),
    ) as { classification?: string; failure_code?: string };
    assert.equal(persisted.classification, "inconclusive");
    assert.equal(persisted.failure_code, "execution_failed");
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("cache scope experiment renews its fenced lease before each matrix dispatch", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  let now = 1_000_000;
  Date.now = () => now;
  seed();
  const leaseKey = [...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "chatgpt_codex", MODEL, "lease"] as const;
  const observedLeaseUntil: number[] = [];
  let inferenceCalls = 0;
  globalThis.fetch = () => {
    const lease = kv.values.get(encodeKey(leaseKey)) as { lease_until_ms?: number };
    observedLeaseUntil.push(lease.lease_until_ms ?? 0);
    inferenceCalls += 1;
    now += 1_000;
    return Promise.resolve(sseCompleted(0, inferenceCalls === 1 ? 2_560 : undefined));
  };

  try {
    await assert.rejects(
      () => runPromptCacheScopeExperiment(),
      (error: unknown) =>
        error instanceof PromptCacheScopeExperimentFailedError && error.failureCode === "execution_failed",
    );
    assert.equal(inferenceCalls, 2);
    assert.equal(observedLeaseUntil.length, 2);
    assert.ok(
      observedLeaseUntil[1]! > observedLeaseUntil[0]!,
      "the second dispatch must observe a renewed lease rather than the acquisition lease",
    );
  } finally {
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("cache scope endpoint reports an incomplete matrix as a non-successful execution", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => Promise.resolve(sseCompleted(0, undefined));

  try {
    const response = await handleAdminCodexCacheScopeExperiment(
      new Request("https://ai.ubq.fi/admin/providers/codex/cache-scope-experiment", { method: "POST" }),
    );
    assert.equal(response.status, 503);
    const body = await response.json() as { error?: { code?: string } };
    assert.equal(body.error?.code, "prompt_cache_scope_experiment_execution_failed");
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("cache scope experiment fences a replaced lease before another matrix row or evidence write", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  const leaseKey = [...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "chatgpt_codex", MODEL, "lease"] as const;
  const evidenceKey = [...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "chatgpt_codex", MODEL] as const;
  let inferenceCalls = 0;
  globalThis.fetch = () => {
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      kv.put(leaseKey, { owner: "new-owner", lease_until_ms: Date.now() + 60_000 });
    }
    return Promise.resolve(sseCompleted(0, 2_560));
  };

  try {
    await assert.rejects(
      () => runPromptCacheScopeExperiment(),
      (error: unknown) => error instanceof PromptCacheScopeExperimentFailedError && error.failureCode === "lease_lost",
    );
    assert.equal(inferenceCalls, 1);
    assert.equal(kv.values.has(encodeKey(evidenceKey)), false);
    const replacementLease = kv.values.get(encodeKey(leaseKey)) as { owner?: string };
    assert.equal(replacementLease.owner, "new-owner");
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("cache scope experiment fails closed when the provider/model lease is held", async () => {
  seed();
  const leaseKey = [...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "chatgpt_codex", MODEL, "lease"] as const;
  kv.put(leaseKey, { owner: "other", lease_until_ms: Date.now() + 60_000 });

  await assert.rejects(() => runPromptCacheScopeExperiment(), PromptCacheScopeExperimentBusyError);
});
