import assert from "node:assert/strict";

if (typeof Deno.KvU64 !== "function") {
  (Deno as unknown as { KvU64: typeof Deno.KvU64 }).KvU64 = class {
    constructor(readonly value: bigint) {}
  } as typeof Deno.KvU64;
}

const encodeKey = (key: Deno.KvKey): string => JSON.stringify(key);

type AtomicWrite = Readonly<{ type: "set" | "delete"; key: Deno.KvKey; value?: unknown }>;

class ExperimentKv {
  readonly values = new Map<string, unknown>();
  private readonly revisions = new Map<string, number>();
  private nextRevision = 1;
  beforeGet: ((key: Deno.KvKey) => void | Promise<void>) | null = null;
  afterAtomicCommit: ((writes: readonly AtomicWrite[]) => void) | null = null;

  clear(): void {
    this.values.clear();
    this.revisions.clear();
    this.nextRevision = 1;
    this.beforeGet = null;
    this.afterAtomicCommit = null;
  }

  put(key: Deno.KvKey, value: unknown): void {
    this.write(key, value);
  }

  private versionstamp(key: Deno.KvKey): string | null {
    const revision = this.revisions.get(encodeKey(key));
    return revision === undefined ? null : String(revision).padStart(20, "0");
  }

  private write(key: Deno.KvKey, value: unknown, revision = this.nextRevision++): void {
    const encoded = encodeKey(key);
    this.values.set(encoded, value);
    this.revisions.set(encoded, revision);
  }

  private remove(key: Deno.KvKey, _revision = this.nextRevision++): void {
    this.values.delete(encodeKey(key));
    this.revisions.delete(encodeKey(key));
  }

  async get<T>(key: Deno.KvKey, options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
    if (options) assert.equal(options.consistency, "strong");
    await this.beforeGet?.(key);
    return {
      key,
      value: (this.values.get(encodeKey(key)) ?? null) as T | null,
      versionstamp: this.versionstamp(key),
    } as Deno.KvEntryMaybe<T>;
  }

  set(key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }): Promise<Deno.KvCommitResult> {
    this.write(key, value);
    return Promise.resolve({ ok: true, versionstamp: this.versionstamp(key)! });
  }

  delete(key: Deno.KvKey): Promise<void> {
    this.remove(key);
    return Promise.resolve();
  }

  atomic(): Deno.AtomicOperation {
    const checks: Array<{ key: Deno.KvKey; versionstamp: string | null }> = [];
    const writes: AtomicWrite[] = [];
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
        const revision = this.nextRevision++;
        for (const write of writes) {
          if (write.type === "set") this.write(write.key, write.value, revision);
          else this.remove(write.key, revision);
        }
        this.afterAtomicCommit?.(writes);
        return Promise.resolve({ ok: true, versionstamp: String(revision).padStart(20, "0") } as const);
      },
    };
    return chain as unknown as Deno.AtomicOperation;
  }
}

const kv = new ExperimentKv();
(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kv as unknown as Deno.Kv);

const { setKvForTest } = await import("../src/kv.ts");
const {
  resetCodexAuthCacheForTest,
  CODEX_AUTH_POOL_KV_KEY,
  CODEX_MODELS_KV_KEY,
  storeCodexModelsSnapshot,
} = await import("../src/codex.ts");
const { resetRuntimeConfigCacheForTest, RUNTIME_CONFIG_V2_KEY } = await import("../src/runtime_config.ts");
const {
  PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX,
  PromptCacheScopeExperimentBusyError,
  PromptCacheScopeExperimentUnavailableError,
  assertPromptCacheScopeExperimentTelemetryBaseline,
  readPromptCacheScopeExperimentCompletedUsage,
  runPromptCacheScopeExperiment,
} = await import("../src/prompt_cache_scope_experiment.ts");
const { promoteCodexPromptCacheScope } = await import("../src/codex_catalog.ts");
const { resolvePromptCacheTelemetryCounterKeys } = await import("../src/prompt_cache_telemetry_gate.ts");
const { loadPromptCacheScopeTargetInventory } = await import("../src/prompt_cache_scope_targets.ts");
const { handleAdminCodexCacheScopeExperiment } = await import("../src/admin.ts");

const MODEL = "gpt-5.6-cache-scope-fixture";
const TELEMETRY_RELEASE = "0123456789abcdef0123456789abcdef01234567";
const targetKeyParts = (model: string): readonly string[] => [
  "codex_chatgpt",
  "chatgpt_codex",
  "codex_account_pool",
  "responses_explicit_input_text_keyed_30m",
  model,
];
const evidenceKeyFor = (model: string): Deno.KvKey => [
  ...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX,
  "evidence",
  ...targetKeyParts(model),
];
const stateKeyFor = (model: string): Deno.KvKey => [
  ...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX,
  "state",
  ...targetKeyParts(model),
];
const evidenceKey = evidenceKeyFor(MODEL);
const stateKey = stateKeyFor(MODEL);
const leaseKey = (model: string): Deno.KvKey => [
  ...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX,
  "cycle_lease",
  "codex_chatgpt",
  "chatgpt_codex",
  "codex_account_pool",
  "responses_explicit_input_text_keyed_30m",
  model,
];
const campaignLeaseKey: Deno.KvKey = [
  ...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX,
  "campaign",
  "codex_chatgpt",
  "chatgpt_codex",
  "responses_explicit_input_text_keyed_30m",
];

const makeAuth = (label: string) => ({
  access_token: `access-${label}`,
  refresh_token: `refresh-${label}`,
  account_id: `account-${label}`,
  updated_at_ms: Date.now(),
});

const authPoolVersionstamp = async (): Promise<string> => {
  const versionstamp = (await kv.get(CODEX_AUTH_POOL_KV_KEY, { consistency: "strong" })).versionstamp;
  if (!versionstamp) throw new Error("missing seeded Codex auth-pool versionstamp");
  return versionstamp;
};

const catalogVersionstamp = async (): Promise<string> => {
  const versionstamp = (await kv.get(CODEX_MODELS_KV_KEY, { consistency: "strong" })).versionstamp;
  if (!versionstamp) throw new Error("missing seeded Codex catalog versionstamp");
  return versionstamp;
};

const runtimeVersionstamp = async (): Promise<string> => {
  const versionstamp = (await kv.get(RUNTIME_CONFIG_V2_KEY, { consistency: "strong" })).versionstamp;
  if (!versionstamp) throw new Error("missing seeded runtime versionstamp");
  return versionstamp;
};

const promotionBinding = async () => ({
  catalogVersionstamp: await catalogVersionstamp(),
  runtimeVersionstamp: await runtimeVersionstamp(),
});

const cacheControls = {
  key: true,
  explicit_breakpoints: true,
  modes: ["explicit"],
  ttls: ["30m"],
  expected_usage_fields: ["cached_tokens", "cache_write_tokens"],
  source: "catalog",
  verified_at_ms: 1_000,
};

const modelRecord = (model: string, controls = cacheControls): Record<string, unknown> => ({
  slug: model,
  supported_reasoning_levels: ["none"],
  prompt_cache: {
    version: 1,
    providers: [{ id: "codex_chatgpt", controls }],
  },
});

const seed = (options: Readonly<{ models?: readonly string[]; defaultModel?: string }> = {}): void => {
  kv.clear();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAuthCacheForTest();
  resetRuntimeConfigCacheForTest();
  const models = options.models ?? [MODEL];
  const defaultModel = options.defaultModel ?? MODEL;
  const snapshot = {
    source: "chatgpt_codex",
    client_version: "0.201.0",
    updated_at_ms: Date.now(),
    models: models.map((model) => modelRecord(model)),
  };
  kv.put(CODEX_AUTH_POOL_KV_KEY, {
    accounts: [makeAuth("one"), makeAuth("two")],
    updated_at_ms: Date.now(),
  });
  kv.put(CODEX_MODELS_KV_KEY, snapshot);
  kv.put(RUNTIME_CONFIG_V2_KEY, {
    version: 2,
    default_model: defaultModel,
    default_reasoning_effort: "none",
    codex_models: snapshot,
    updated_at_ms: Date.now(),
  });
};

const seedStage0Baseline = async (model: string): Promise<void> => {
  const counterKeys = await resolvePromptCacheTelemetryCounterKeys(
    { provider: "chatgpt_codex", model },
    { release: TELEMETRY_RELEASE },
  );
  if (!counterKeys) throw new Error("missing Stage 0 counter keys");
  for (const route of counterKeys.routes) {
    kv.put(route.completed, new Deno.KvU64(5_000n));
    kv.put(route.reported, new Deno.KvU64(5_000n));
    kv.put(route.cache_write_reported, new Deno.KvU64(5_000n));
  }
};

/**
 * Unit fixtures intentionally exercise the fenced runner without fabricating
 * a deploy-attested Stage 0 baseline. The public admin route obtains the
 * corresponding binding from the real baseline assertion.
 */
const scopeBaselineFor = async (model = MODEL) => {
  const inventory = await loadPromptCacheScopeTargetInventory({ kv: kv as unknown as Deno.Kv });
  if (inventory.status !== "ready") throw new Error("missing seeded target inventory");
  const target = inventory.targets.find((candidate) =>
    candidate.provider === "codex_chatgpt" && candidate.model === model
  );
  const runtimeStamp = await runtimeVersionstamp();
  if (
    !target || target.probeability.status !== "probeable" || !target.catalog_versionstamp ||
    !target.codex_auth_pool_versionstamp || !target.codex_auth_pool_identity_fingerprint ||
    !inventory.inventory_fingerprint
  ) throw new Error("missing seeded probeable target");
  return {
    target: {
      id: target.id,
      provider: "codex_chatgpt" as const,
      telemetry_provider: "chatgpt_codex" as const,
      topology_kind: "codex_account_pool" as const,
      model,
      probe_profile: "responses_explicit_input_text_keyed_30m" as const,
      capability_fingerprint: target.capability_fingerprint,
      inventory_fingerprint: inventory.inventory_fingerprint,
      catalog_versionstamp: target.catalog_versionstamp,
      runtime_versionstamp: runtimeStamp,
      auth_pool_versionstamp: target.codex_auth_pool_versionstamp,
      auth_pool_identity_fingerprint: target.codex_auth_pool_identity_fingerprint,
      catalog_client_version: target.catalog_client_version,
    },
  } satisfies Parameters<typeof runPromptCacheScopeExperiment>[0];
};

const runExperiment = async (model = MODEL) => {
  return await runPromptCacheScopeExperiment(await scopeBaselineFor(model));
};

const sseCompleted = (cachedTokens: number, cacheWriteTokens: number, model = MODEL): Response => {
  const usage = {
    input_tokens: 3_000,
    output_tokens: 1,
    total_tokens: 3_001,
    input_tokens_details: { cached_tokens: cachedTokens, cache_write_tokens: cacheWriteTokens },
  };
  const event = `data: ${JSON.stringify({ type: "response.completed", response: { model, usage } })}\n\n`;
  const stream = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(new TextEncoder().encode(event));
      controller.close();
    },
  });
  return new Response(stream, { status: 200, headers: { "Content-Type": "text/event-stream" } });
};

Deno.test("a missing or mismatched pinned slot is an inconclusive scope result", async () => {
  const response = sseCompleted(0, 2_560);
  const result = await readPromptCacheScopeExperimentCompletedUsage(
    response,
    1,
    MODEL,
    performance.now(),
    new AbortController().signal,
  );

  assert.deepEqual(result, { status: "inconclusive", reason: "slot_drift" });
});

Deno.test("prompt-cache scope uses three fixed cycles, publishes canonical scope, and stores only redacted evidence", async () => {
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
          { headers: { "Content-Type": "application/json" } },
        ),
      );
    }
    requests.push({
      account: request.headers.get("chatgpt-account-id"),
      conversation: request.headers.get("conversation_id"),
      body: String(init?.body ?? ""),
    });
    const step = responseIndex++ % 10;
    const cachedTokens = [0, 2_560, 0, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560][step]!;
    // A cache read and a cache write may be reported together. The scope
    // probe must still treat cached_tokens as a conclusive read signal.
    const cacheWriteTokens = [2_560, 2_560, 2_560, 0, 0, 0, 0, 0, 0, 0][step]!;
    return Promise.resolve(sseCompleted(cachedTokens, cacheWriteTokens));
  };

  try {
    const first = await runExperiment();
    const second = await runExperiment();
    const third = await runExperiment();
    assert.equal(first.status, "in_progress");
    assert.equal(first.completed_cycles, 1);
    assert.equal(second.status, "in_progress");
    assert.equal(second.completed_cycles, 2);
    assert.equal(third.status, "completed");
    assert.deepEqual(third.scope, {
      probe_profile: "responses_explicit_input_text_keyed_30m",
      account_slots: "account_scoped",
      token_refresh: "preserved",
      conversation_id: "independent",
      effective_model: MODEL,
    });
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
      assert.equal(rows.every((row) => row.body === rows[0]?.body), true);
      const body = JSON.parse(rows[0]?.body ?? "") as Record<string, unknown>;
      assert.deepEqual(Object.keys(body).sort(), [
        "input",
        "max_output_tokens",
        "model",
        "prompt_cache_key",
        "prompt_cache_options",
        "reasoning",
        "store",
        "stream",
      ]);
      assert.equal(body.model, MODEL);
      assert.equal(body.store, false);
      assert.equal(body.stream, true);
      assert.equal(body.max_output_tokens, 16);
      assert.deepEqual(body.reasoning, { effort: "none" });
      assert.deepEqual(body.prompt_cache_options, { mode: "explicit", ttl: "30m" });
      assert.match(String(body.prompt_cache_key), /^uos-cache-scope-v3-[0-9a-f-]{36}$/);
      assert.equal("cache_affinity" in body, false);
      const input = body.input as Array<Record<string, unknown>>;
      assert.equal(input.length, 2);
      assert.deepEqual(input[0]?.type, "message");
      assert.deepEqual(input[0]?.role, "developer");
      const prefix = input[0]?.content as Array<Record<string, unknown>>;
      assert.deepEqual(prefix[0]?.type, "input_text");
      assert.deepEqual(prefix[0]?.prompt_cache_breakpoint, { mode: "explicit" });
      assert.match(String(prefix[0]?.text), /^cache(?: cache){2,}\n\ncache-scope-cycle:/);
      const request = input[1]?.content as Array<Record<string, unknown>>;
      assert.deepEqual(input[1]?.role, "user");
      assert.deepEqual(request, [{ type: "input_text", text: "Reply with exactly: cache scope experiment." }]);
      assert.equal(rows[0]?.conversation, rows[6]?.conversation);
      assert.notEqual(rows[0]?.conversation, rows[7]?.conversation);
      assert.equal(rows[0]?.conversation, rows[9]?.conversation);
    }

    const snapshot = kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)) as {
      models?: Array<{
        prompt_cache?: { providers?: Array<{ id?: string; scope?: unknown }> };
      }>;
    };
    const provider = snapshot.models?.[0]?.prompt_cache?.providers?.[0];
    assert.equal(provider?.id, "codex_chatgpt");
    assert.deepEqual(provider?.scope, {
      probe_profile: "responses_explicit_input_text_keyed_30m",
      account_slots: "account_scoped",
      token_refresh: "preserved",
      conversation_id: "independent",
      effective_model: MODEL,
      reproducible_cycles: 3,
      source: "live_probe",
      verified_at_ms: provider?.scope && (provider.scope as { verified_at_ms?: unknown }).verified_at_ms,
    });

    const runtime = kv.values.get(encodeKey(RUNTIME_CONFIG_V2_KEY)) as {
      codex_models?: {
        models?: Array<{
          prompt_cache?: { providers?: Array<{ scope?: unknown }> };
        }>;
      };
    };
    assert.equal(runtime.codex_models?.models?.[0]?.prompt_cache?.providers?.[0]?.scope, undefined);

    assert.equal(
      await storeCodexModelsSnapshot({
        source: "admin-refresh",
        updated_at_ms: Date.now(),
        models: [{
          slug: MODEL,
          supported_reasoning_levels: ["none"],
          prompt_cache: { version: 1, providers: [{ id: "codex_chatgpt", controls: cacheControls }] },
        }],
      }),
      true,
    );
    const refreshedSnapshot = kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)) as {
      models?: Array<{
        prompt_cache?: { providers?: Array<{ scope?: unknown }> };
      }>;
    };
    assert.equal(
      (refreshedSnapshot.models?.[0]?.prompt_cache?.providers?.[0]?.scope as { account_slots?: string } | undefined)
        ?.account_slots,
      "account_scoped",
    );

    const evidence = kv.values.get(encodeKey(evidenceKey)) as {
      outcome?: string;
      cycles?: Array<{ samples?: Array<{ usage?: Record<string, unknown>; raw_usage?: Record<string, unknown> }> }>;
    };
    assert.equal(evidence.outcome, "completed");
    assert.equal(evidence.cycles?.length, 3);
    const samples = evidence.cycles?.flatMap((cycle) => cycle.samples ?? []) ?? [];
    assert.equal(samples.length, 30);
    for (const sample of samples) {
      assert.deepEqual(sample.raw_usage, sample.usage);
      assert.deepEqual(Object.keys(sample.raw_usage ?? {}).sort(), [
        "cache_write_tokens",
        "cached_tokens",
        "input_tokens",
        "output_tokens",
        "total_tokens",
      ]);
      assert.equal(Object.values(sample.raw_usage ?? {}).every((value) => typeof value === "number"), true);
    }
    const serializedEvidence = JSON.stringify(evidence);
    assert.equal(serializedEvidence.includes("account-one"), false);
    assert.equal(serializedEvidence.includes("uos-cache-scope-v2"), false);
    assert.equal(serializedEvidence.includes("cache-scope-cycle"), false);
    assert.equal(serializedEvidence.includes(requests[0]?.conversation ?? "missing-conversation"), false);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("prompt-cache scope makes a sample with neither read nor write telemetry inconclusive", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  let inferenceCalls = 0;
  globalThis.fetch = (input) => {
    const url = input instanceof Request ? input.url : input instanceof URL ? input.toString() : input;
    if (url === "https://auth.openai.com/oauth/token") throw new Error("refresh must not run");
    inferenceCalls += 1;
    return Promise.resolve(sseCompleted(0, 0));
  };

  try {
    const result = await runExperiment();
    assert.equal(result.status, "inconclusive");
    assert.equal(result.inconclusive_reason, "invalid_cache_signal");
    assert.equal(inferenceCalls, 1);
    const snapshot = JSON.stringify(kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)));
    assert.equal(snapshot.includes('"scope"'), false);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("a target-scoped cycle lease blocks a concurrent same-target invocation", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  let releaseFirst = () => {};
  let markFirstDispatched = () => {};
  const firstDispatched = new Promise<void>((resolve) => markFirstDispatched = resolve);
  const heldResponse = new Promise<Response>((resolve) => releaseFirst = () => resolve(sseCompleted(0, 0)));
  let inferenceCalls = 0;
  globalThis.fetch = (input) => {
    const url = input instanceof Request ? input.url : input instanceof URL ? input.toString() : input;
    if (url === "https://auth.openai.com/oauth/token") throw new Error("refresh must not run");
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      markFirstDispatched();
      return heldResponse;
    }
    return Promise.resolve(sseCompleted(0, 0));
  };
  let first: ReturnType<typeof runPromptCacheScopeExperiment> | null = null;
  try {
    first = runExperiment();
    await firstDispatched;
    await assert.rejects(() => runExperiment(), PromptCacheScopeExperimentBusyError);
    assert.equal(inferenceCalls, 1);

    releaseFirst();
    const firstResult = await first;
    assert.equal(firstResult.status, "inconclusive");
    assert.equal(firstResult.inconclusive_reason, "invalid_cache_signal");
  } finally {
    releaseFirst();
    await first?.catch(() => {});
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("a provider campaign lease blocks a sibling target between cycles and prevents OAuth interleaving", async () => {
  const modelA = "gpt-5.5-cache-scope-target-a";
  const modelB = "gpt-5.6-cache-scope-target-b";
  seed({ models: [modelB, modelA], defaultModel: modelB });
  const originalFetch = globalThis.fetch;
  let inferenceCalls = 0;
  let refreshes = 0;
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url === "https://auth.openai.com/oauth/token") {
      refreshes += 1;
      return Promise.resolve(
        new Response(
          JSON.stringify({ access_token: `refresh-access-${refreshes}`, refresh_token: `refresh-token-${refreshes}` }),
          { headers: { "Content-Type": "application/json" } },
        ),
      );
    }
    const body = JSON.parse(String(init?.body ?? "")) as { model?: unknown };
    if (typeof body.model !== "string") throw new Error("scope request lost its target model");
    const step = inferenceCalls++ % 10;
    const cachedTokens = [0, 2_560, 0, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560][step]!;
    const cacheWriteTokens = [2_560, 0, 2_560, 0, 0, 0, 0, 0, 0, 0][step]!;
    return Promise.resolve(sseCompleted(cachedTokens, cacheWriteTokens, body.model));
  };

  try {
    const firstA = await runExperiment(modelA);
    assert.equal(firstA.status, "in_progress");
    assert.equal(firstA.completed_cycles, 1);
    const aState = JSON.stringify(kv.values.get(encodeKey(stateKeyFor(modelA))));
    const aEvidence = JSON.stringify(kv.values.get(encodeKey(evidenceKeyFor(modelA))));

    await assert.rejects(() => runExperiment(modelB), PromptCacheScopeExperimentBusyError);
    assert.equal(inferenceCalls, 10);
    assert.equal(refreshes, 1);
    assert.equal(kv.values.has(encodeKey(stateKeyFor(modelB))), false);
    assert.equal(kv.values.has(encodeKey(evidenceKeyFor(modelB))), false);
    const stateAfterB = JSON.stringify(kv.values.get(encodeKey(stateKeyFor(modelA))));
    const evidenceAfterB = JSON.stringify(kv.values.get(encodeKey(evidenceKeyFor(modelA))));
    assert.equal(stateAfterB, aState);
    assert.equal(evidenceAfterB, aEvidence);

    const secondA = await runExperiment(modelA);
    assert.equal(secondA.status, "in_progress");
    assert.equal(secondA.completed_cycles, 2);
    assert.equal(inferenceCalls, 20);
    assert.equal(refreshes, 2);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("an expired target session clears its prior evidence before starting a fresh cycle", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  let inferenceCalls = 0;
  let refreshes = 0;
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url === "https://auth.openai.com/oauth/token") {
      refreshes += 1;
      return Promise.resolve(
        new Response(
          JSON.stringify({
            access_token: `expired-access-${refreshes}`,
            refresh_token: `expired-refresh-${refreshes}`,
          }),
          { headers: { "Content-Type": "application/json" } },
        ),
      );
    }
    const step = inferenceCalls++ % 10;
    const cachedTokens = [0, 2_560, 0, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560][step]!;
    const cacheWriteTokens = [2_560, 0, 2_560, 0, 0, 0, 0, 0, 0, 0][step]!;
    return Promise.resolve(sseCompleted(cachedTokens, cacheWriteTokens));
  };

  try {
    assert.equal((await runExperiment()).status, "in_progress");
    const priorState = kv.values.get(encodeKey(stateKey)) as {
      campaign_owner: string;
      target: { id: string; inventory_fingerprint: string };
      [key: string]: unknown;
    };
    const priorEvidence = kv.values.get(encodeKey(evidenceKey)) as Record<string, unknown>;
    const staleStartedAtMs = Date.now() - 10_000;
    kv.put(stateKey, {
      ...priorState,
      started_at_ms: staleStartedAtMs,
      expires_at_ms: staleStartedAtMs + 1,
    });
    kv.put(evidenceKey, { ...priorEvidence, started_at_ms: staleStartedAtMs });
    kv.put(campaignLeaseKey, {
      owner: priorState.campaign_owner,
      target_id: priorState.target.id,
      inventory_fingerprint: priorState.target.inventory_fingerprint,
      lease_until_ms: staleStartedAtMs + 1,
    });

    const restarted = await runExperiment();
    assert.equal(restarted.status, "in_progress");
    assert.equal(restarted.completed_cycles, 1);
    assert.equal(inferenceCalls, 20);
    assert.equal(refreshes, 2);
    const freshState = kv.values.get(encodeKey(stateKey)) as { campaign_owner?: unknown; started_at_ms?: unknown };
    const freshEvidence = kv.values.get(encodeKey(evidenceKey)) as { started_at_ms?: unknown; cycles?: unknown[] };
    assert.notEqual(freshState.campaign_owner, priorState.campaign_owner);
    assert.notEqual(freshState.started_at_ms, staleStartedAtMs);
    assert.equal(freshEvidence.started_at_ms, freshState.started_at_ms);
    assert.equal(freshEvidence.cycles?.length, 1);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("a same-account credential rotation during forced refresh is inconclusive auth-pool drift", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  let inferenceCalls = 0;
  let rotationApplied = false;
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url === "https://auth.openai.com/oauth/token") {
      if (rotationApplied) throw new Error("the forced refresh must not retry after credential rotation");
      rotationApplied = true;
      const pool = kv.values.get(encodeKey(CODEX_AUTH_POOL_KV_KEY)) as {
        accounts: Array<ReturnType<typeof makeAuth>>;
        updated_at_ms: number;
      } | undefined;
      const first = pool?.accounts[0];
      const second = pool?.accounts[1];
      if (!pool || !first || !second) throw new Error("missing seeded Codex auth pool");
      kv.put(CODEX_AUTH_POOL_KV_KEY, {
        ...pool,
        accounts: [
          {
            ...first,
            access_token: "externally-rotated-access",
            refresh_token: "externally-rotated-refresh",
            updated_at_ms: Date.now(),
          },
          second,
        ],
        updated_at_ms: Date.now(),
      });
      return Promise.resolve(
        new Response(
          JSON.stringify({ access_token: "experiment-refresh-access", refresh_token: "experiment-refresh-refresh" }),
          { headers: { "Content-Type": "application/json" } },
        ),
      );
    }

    const step = inferenceCalls++ % 10;
    const cachedTokens = [0, 2_560, 0, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560][step]!;
    const cacheWriteTokens = [2_560, 0, 2_560, 0, 0, 0, 0, 0, 0, 0][step]!;
    return Promise.resolve(sseCompleted(cachedTokens, cacheWriteTokens));
  };

  try {
    const result = await runExperiment();
    assert.equal(rotationApplied, true);
    assert.equal(result.status, "inconclusive");
    assert.equal(result.inconclusive_reason, "auth_pool_drift");
    assert.equal(inferenceCalls, 5);
    assert.equal(JSON.stringify(kv.values.get(encodeKey(CODEX_MODELS_KV_KEY))).includes('"scope"'), false);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("malformed v3 target state or evidence fails closed before any outbound work", async () => {
  const originalFetch = globalThis.fetch;
  let fetchCalls = 0;
  globalThis.fetch = () => {
    fetchCalls += 1;
    throw new Error("malformed durable data must fail closed before upstream work");
  };

  try {
    for (const kind of ["state", "evidence"] as const) {
      seed();
      const baseline = await scopeBaselineFor();
      const key = kind === "state" ? stateKey : evidenceKey;
      const malformed = kind === "state" ? { v: 3, target: baseline.target, next_cycle: 2 } : {
        v: 3,
        target: baseline.target,
        outcome: "completed",
        started_at_ms: Date.now(),
        verified_at_ms: Date.now(),
        cycles: [],
      };
      kv.put(key, malformed);
      const persisted = JSON.stringify(kv.values.get(encodeKey(key)));

      const response = await handleAdminCodexCacheScopeExperiment(
        new Request("https://ai.ubq.fi/admin/providers/codex/cache-scope-experiment", { method: "POST" }),
      );
      assert.equal(response.status, 503, kind);
      const body = await response.json() as { error?: { code?: string } };
      assert.equal(body.error?.code, "prompt_cache_scope_experiment_unavailable", kind);
      await assert.rejects(
        () => runPromptCacheScopeExperiment(baseline),
        PromptCacheScopeExperimentUnavailableError,
        kind,
      );
      assert.equal(JSON.stringify(kv.values.get(encodeKey(key))), persisted, kind);
    }
    assert.equal(fetchCalls, 0);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("semantically malformed v3 timestamps fail closed before any outbound work", async () => {
  const originalFetch = globalThis.fetch;
  let fetchCalls = 0;
  globalThis.fetch = () => {
    fetchCalls += 1;
    throw new Error("semantically malformed durable timestamps must fail closed before upstream work");
  };

  try {
    for (const kind of ["future_state", "inverted_evidence", "future_evidence"] as const) {
      seed();
      const baseline = await scopeBaselineFor();
      const now = Date.now();
      const owner = `timestamp-owner-${kind}`;
      const key = kind === "future_state" ? stateKey : evidenceKey;
      const malformed = kind === "future_state"
        ? {
          v: 3,
          target: baseline.target,
          campaign_owner: owner,
          started_at_ms: now + 60_000,
          expires_at_ms: now + 120_000,
          auth_pool_versionstamp: baseline.target.auth_pool_versionstamp,
          next_cycle: 1,
          classifications: [],
        }
        : {
          v: 3,
          target: baseline.target,
          outcome: "failed",
          started_at_ms: kind === "inverted_evidence" ? now : now + 60_000,
          verified_at_ms: kind === "inverted_evidence" ? now - 1 : now + 120_000,
          cycles: [],
        };
      kv.put(key, malformed);
      if (kind === "future_state") {
        kv.put(campaignLeaseKey, {
          owner,
          target_id: baseline.target.id,
          inventory_fingerprint: baseline.target.inventory_fingerprint,
          lease_until_ms: now + 120_000,
        });
      }
      const persisted = JSON.stringify(kv.values.get(encodeKey(key)));

      await assert.rejects(
        () =>
          assertPromptCacheScopeExperimentTelemetryBaseline({
            kv: kv as unknown as Deno.Kv,
            release: TELEMETRY_RELEASE,
          }),
        PromptCacheScopeExperimentUnavailableError,
        kind,
      );
      await assert.rejects(
        () => runPromptCacheScopeExperiment(baseline),
        PromptCacheScopeExperimentUnavailableError,
        kind,
      );
      assert.equal(JSON.stringify(kv.values.get(encodeKey(key))), persisted, kind);
    }
    assert.equal(fetchCalls, 0);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("semantically inconsistent ready-to-promote state cannot publish a scope", async () => {
  seed();
  const baseline = await scopeBaselineFor();
  const startedAtMs = Date.now();
  const observation = {
    probe_profile: "responses_explicit_input_text_keyed_30m",
    account_slots: "account_scoped",
    token_refresh: "preserved",
    conversation_id: "independent",
    effective_model: MODEL,
  } as const;
  const disagreeingObservation = { ...observation, account_slots: "shared" as const };
  const owner = "inconsistent-ready-owner";
  kv.put(stateKey, {
    v: 3,
    target: baseline.target,
    campaign_owner: owner,
    started_at_ms: startedAtMs,
    expires_at_ms: startedAtMs + 60_000,
    auth_pool_versionstamp: baseline.target.auth_pool_versionstamp,
    next_cycle: 4,
    classifications: [observation, disagreeingObservation, observation],
    pending_scope: observation,
  });
  kv.put(campaignLeaseKey, {
    owner,
    target_id: baseline.target.id,
    inventory_fingerprint: baseline.target.inventory_fingerprint,
    lease_until_ms: startedAtMs + 60_000,
  });
  const originalFetch = globalThis.fetch;
  let fetchCalls = 0;
  globalThis.fetch = () => {
    fetchCalls += 1;
    throw new Error("a malformed ready state must not dispatch or refresh");
  };

  try {
    await assert.rejects(
      () => runPromptCacheScopeExperiment(baseline),
      PromptCacheScopeExperimentUnavailableError,
    );
    assert.equal(fetchCalls, 0);
    assert.equal(JSON.stringify(kv.values.get(encodeKey(CODEX_MODELS_KV_KEY))).includes('"scope"'), false);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("a completed target advances the bodyless campaign to the next exact model", async () => {
  const modelA = "gpt-5.5-cache-scope-completed-a";
  const modelB = "gpt-5.6-cache-scope-pending-b";
  seed({ models: [modelB, modelA], defaultModel: modelB });
  const originalFetch = globalThis.fetch;
  let inferenceCalls = 0;
  let refreshes = 0;
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url === "https://auth.openai.com/oauth/token") {
      refreshes += 1;
      return Promise.resolve(
        new Response(
          JSON.stringify({
            access_token: `progress-access-${refreshes}`,
            refresh_token: `progress-refresh-${refreshes}`,
          }),
          { headers: { "Content-Type": "application/json" } },
        ),
      );
    }
    const body = JSON.parse(String(init?.body ?? "")) as { model?: unknown };
    if (typeof body.model !== "string") throw new Error("scope request lost its target model");
    const step = inferenceCalls++ % 10;
    const cachedTokens = [0, 2_560, 0, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560][step]!;
    const cacheWriteTokens = [2_560, 0, 2_560, 0, 0, 0, 0, 0, 0, 0][step]!;
    return Promise.resolve(sseCompleted(cachedTokens, cacheWriteTokens, body.model));
  };

  try {
    assert.equal((await runExperiment(modelA)).status, "in_progress");
    assert.equal((await runExperiment(modelA)).status, "in_progress");
    const completed = await runExperiment(modelA);
    assert.equal(completed.status, "completed");
    assert.equal(inferenceCalls, 30);
    assert.equal(refreshes, 3);
    const aEvidence = kv.values.get(encodeKey(evidenceKeyFor(modelA))) as { outcome?: unknown };
    assert.equal(aEvidence.outcome, "completed");

    await seedStage0Baseline(modelB);
    const nextBaseline = await assertPromptCacheScopeExperimentTelemetryBaseline({
      kv: kv as unknown as Deno.Kv,
      release: TELEMETRY_RELEASE,
    });
    assert.equal(nextBaseline.target.model, modelB);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("an inconclusive target is retried before the campaign advances to another model", async () => {
  const modelA = "gpt-5.5-cache-scope-retry-a";
  const modelB = "gpt-5.6-cache-scope-pending-b";
  seed({ models: [modelB, modelA], defaultModel: modelB });
  const originalFetch = globalThis.fetch;
  globalThis.fetch = (input) => {
    const url = input instanceof Request ? input.url : input instanceof URL ? input.toString() : input;
    if (url === "https://auth.openai.com/oauth/token") throw new Error("refresh must not run");
    return Promise.resolve(sseCompleted(0, 0, modelA));
  };

  try {
    const inconclusive = await runExperiment(modelA);
    assert.equal(inconclusive.status, "inconclusive");
    assert.equal(inconclusive.inconclusive_reason, "invalid_cache_signal");

    await seedStage0Baseline(modelA);
    await seedStage0Baseline(modelB);
    const retryBaseline = await assertPromptCacheScopeExperimentTelemetryBaseline({
      kv: kv as unknown as Deno.Kv,
      release: TELEMETRY_RELEASE,
    });
    assert.equal(retryBaseline.target.model, modelA);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("completed evidence survives token rotation but is reprobeable after an account-pool membership change", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  let inferenceCalls = 0;
  let refreshes = 0;
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url === "https://auth.openai.com/oauth/token") {
      refreshes += 1;
      return Promise.resolve(
        new Response(
          JSON.stringify({
            access_token: `identity-refresh-access-${refreshes}`,
            refresh_token: `identity-refresh-token-${refreshes}`,
          }),
          { headers: { "Content-Type": "application/json" } },
        ),
      );
    }
    const step = inferenceCalls++ % 10;
    const cachedTokens = [0, 2_560, 0, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560][step]!;
    const cacheWriteTokens = [2_560, 0, 2_560, 0, 0, 0, 0, 0, 0, 0][step]!;
    return Promise.resolve(sseCompleted(cachedTokens, cacheWriteTokens));
  };

  try {
    assert.equal((await runExperiment()).status, "in_progress");
    assert.equal((await runExperiment()).status, "in_progress");
    assert.equal((await runExperiment()).status, "completed");
    assert.equal(inferenceCalls, 30);

    const tokenOnlyPool = kv.values.get(encodeKey(CODEX_AUTH_POOL_KV_KEY)) as {
      accounts: Array<ReturnType<typeof makeAuth>>;
      updated_at_ms: number;
    } | undefined;
    const tokenOnlyFirst = tokenOnlyPool?.accounts[0];
    if (!tokenOnlyPool || !tokenOnlyFirst) throw new Error("missing seeded Codex auth pool");
    kv.put(CODEX_AUTH_POOL_KV_KEY, {
      ...tokenOnlyPool,
      accounts: [{
        ...tokenOnlyFirst,
        access_token: "token-only-rotation-access",
        refresh_token: "token-only-rotation-refresh",
        updated_at_ms: Date.now(),
      }, ...tokenOnlyPool.accounts.slice(1)],
      updated_at_ms: Date.now(),
    });
    const tokenOnlyBaseline = await scopeBaselineFor();
    const completedEvidence = kv.values.get(encodeKey(evidenceKey)) as {
      target?: { auth_pool_identity_fingerprint?: unknown };
    } | undefined;
    assert.equal(
      completedEvidence?.target?.auth_pool_identity_fingerprint,
      tokenOnlyBaseline.target.auth_pool_identity_fingerprint,
    );
    await seedStage0Baseline(MODEL);
    await assert.rejects(
      () =>
        assertPromptCacheScopeExperimentTelemetryBaseline({
          kv: kv as unknown as Deno.Kv,
          release: TELEMETRY_RELEASE,
        }),
      PromptCacheScopeExperimentUnavailableError,
    );
    assert.equal(inferenceCalls, 30);

    const membershipChangedPool = kv.values.get(encodeKey(CODEX_AUTH_POOL_KV_KEY)) as {
      accounts: Array<ReturnType<typeof makeAuth>>;
      updated_at_ms: number;
    } | undefined;
    const membershipFirst = membershipChangedPool?.accounts[0];
    if (!membershipChangedPool || !membershipFirst) throw new Error("missing Codex auth pool after token rotation");
    kv.put(CODEX_AUTH_POOL_KV_KEY, {
      ...membershipChangedPool,
      accounts: [{
        ...membershipFirst,
        account_id: "replacement-account-id",
        access_token: "membership-change-access",
        refresh_token: "membership-change-refresh",
        updated_at_ms: Date.now(),
      }, ...membershipChangedPool.accounts.slice(1)],
      updated_at_ms: Date.now(),
    });
    const membershipBaseline = await assertPromptCacheScopeExperimentTelemetryBaseline({
      kv: kv as unknown as Deno.Kv,
      release: TELEMETRY_RELEASE,
    });
    assert.notEqual(
      membershipBaseline.target.auth_pool_identity_fingerprint,
      tokenOnlyBaseline.target.auth_pool_identity_fingerprint,
    );
    assert.equal((await runPromptCacheScopeExperiment(membershipBaseline)).status, "in_progress");
    assert.equal(inferenceCalls, 40);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("prompt-cache scope admin trigger rejects request fields before any outbound work", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => {
    throw new Error("request fields must be rejected before dispatch");
  };
  try {
    const response = await handleAdminCodexCacheScopeExperiment(
      new Request("https://ai.ubq.fi/admin/providers/codex/cache-scope-experiment", {
        method: "POST",
        body: JSON.stringify({ model: "forbidden", cache_affinity: "forbidden" }),
      }),
    );
    assert.equal(response.status, 400);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("prompt-cache scope admin trigger requires a current-release Stage 0 baseline before any outbound work", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  let fetchCalls = 0;
  globalThis.fetch = () => {
    fetchCalls += 1;
    throw new Error("the Stage 0 gate must run before any upstream work");
  };
  try {
    const response = await handleAdminCodexCacheScopeExperiment(
      new Request("https://ai.ubq.fi/admin/providers/codex/cache-scope-experiment", { method: "POST" }),
    );
    assert.equal(response.status, 503);
    const body = await response.json() as { error?: { code?: string; message?: string } };
    assert.equal(body.error?.code, "prompt_cache_scope_experiment_unavailable");
    assert.match(body.error?.message ?? "", /Stage 0 telemetry baseline/);
    assert.equal(fetchCalls, 0);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("prompt-cache scope Stage 0 baseline uses the Codex telemetry provider identity", async () => {
  seed();
  const counterKeys = await resolvePromptCacheTelemetryCounterKeys(
    { provider: "chatgpt_codex", model: MODEL },
    { release: TELEMETRY_RELEASE },
  );
  assert.ok(counterKeys);
  for (const route of counterKeys.routes) {
    kv.put(route.completed, new Deno.KvU64(5_000n));
    kv.put(route.reported, new Deno.KvU64(5_000n));
    kv.put(route.cache_write_reported, new Deno.KvU64(5_000n));
  }

  await assert.doesNotReject(() =>
    assertPromptCacheScopeExperimentTelemetryBaseline({
      kv: kv as unknown as Deno.Kv,
      release: TELEMETRY_RELEASE,
    })
  );
});

Deno.test("prompt-cache scope Stage 0 baseline rejects reported usage without cache-write fields before any scope traffic", async () => {
  seed();
  const counterKeys = await resolvePromptCacheTelemetryCounterKeys(
    { provider: "chatgpt_codex", model: MODEL },
    { release: TELEMETRY_RELEASE },
  );
  assert.ok(counterKeys);
  for (const route of counterKeys.routes) {
    kv.put(route.completed, new Deno.KvU64(5_000n));
    kv.put(route.reported, new Deno.KvU64(5_000n));
  }
  const originalFetch = globalThis.fetch;
  let fetchCalls = 0;
  globalThis.fetch = () => {
    fetchCalls += 1;
    throw new Error("incomplete Stage 0 telemetry must block scope traffic");
  };

  try {
    await assert.rejects(
      () =>
        assertPromptCacheScopeExperimentTelemetryBaseline({
          kv: kv as unknown as Deno.Kv,
          release: TELEMETRY_RELEASE,
        }),
      PromptCacheScopeExperimentUnavailableError,
    );
    assert.equal(fetchCalls, 0);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("prompt-cache scope Stage 0 baseline requires Responses telemetry for its paid transport", async () => {
  seed();
  const counterKeys = await resolvePromptCacheTelemetryCounterKeys(
    { provider: "chatgpt_codex", model: MODEL },
    { release: TELEMETRY_RELEASE },
  );
  assert.ok(counterKeys);
  const chat = counterKeys.routes.find((route) => route.route === "chat.completions");
  assert.ok(chat);
  kv.put(chat.completed, new Deno.KvU64(10_000n));
  kv.put(chat.reported, new Deno.KvU64(10_000n));
  kv.put(chat.cache_write_reported, new Deno.KvU64(10_000n));

  const originalFetch = globalThis.fetch;
  let fetchCalls = 0;
  globalThis.fetch = () => {
    fetchCalls += 1;
    throw new Error("Chat-only telemetry must not unlock paid Responses scope traffic");
  };

  try {
    await assert.rejects(
      () =>
        assertPromptCacheScopeExperimentTelemetryBaseline({
          kv: kv as unknown as Deno.Kv,
          release: TELEMETRY_RELEASE,
        }),
      PromptCacheScopeExperimentUnavailableError,
    );
    assert.equal(fetchCalls, 0);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("a catalog roster change after the Stage 0 gate stops before paid scope traffic", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  let fetchCalls = 0;
  globalThis.fetch = () => {
    fetchCalls += 1;
    throw new Error("a stale Stage 0 binding must fail before any outbound work");
  };

  try {
    const counterKeys = await resolvePromptCacheTelemetryCounterKeys(
      { provider: "chatgpt_codex", model: MODEL },
      { release: TELEMETRY_RELEASE },
    );
    assert.ok(counterKeys);
    for (const route of counterKeys.routes) {
      kv.put(route.completed, new Deno.KvU64(5_000n));
      kv.put(route.reported, new Deno.KvU64(5_000n));
      kv.put(route.cache_write_reported, new Deno.KvU64(5_000n));
    }
    const baseline = await assertPromptCacheScopeExperimentTelemetryBaseline({
      kv: kv as unknown as Deno.Kv,
      release: TELEMETRY_RELEASE,
    });

    const alternateModel = `${MODEL}-alternate-after-gate`;
    const snapshot = kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)) as {
      source: string;
      client_version: string;
      updated_at_ms: number;
      models: Array<Record<string, unknown>>;
    };
    const snapshotWithAlternate = {
      ...snapshot,
      models: [...snapshot.models, modelRecord(alternateModel)],
    };
    kv.put(CODEX_MODELS_KV_KEY, snapshotWithAlternate);

    await assert.rejects(
      () => runPromptCacheScopeExperiment(baseline),
      PromptCacheScopeExperimentUnavailableError,
    );
    assert.equal(fetchCalls, 0);
    assert.equal(kv.values.has(encodeKey(stateKey)), false);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("a runtime default-only switch preserves a selected non-default target", async () => {
  const targetModel = "gpt-5.5-cache-scope-non-default";
  const firstDefault = "gpt-5.7-cache-scope-default-one";
  const secondDefault = "gpt-5.8-cache-scope-default-two";
  seed({ models: [targetModel, firstDefault, secondDefault], defaultModel: firstDefault });
  await seedStage0Baseline(targetModel);
  const baseline = await assertPromptCacheScopeExperimentTelemetryBaseline({
    kv: kv as unknown as Deno.Kv,
    release: TELEMETRY_RELEASE,
  });
  assert.equal(baseline.target.model, targetModel);

  const originalFetch = globalThis.fetch;
  let inferenceCalls = 0;
  let refreshes = 0;
  const requestedModels: string[] = [];
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url === "https://auth.openai.com/oauth/token") {
      refreshes += 1;
      return Promise.resolve(
        new Response(
          JSON.stringify({
            access_token: `refreshed-access-${refreshes}`,
            refresh_token: `refreshed-refresh-${refreshes}`,
          }),
          { headers: { "Content-Type": "application/json" } },
        ),
      );
    }
    const body = JSON.parse(String(init?.body ?? "")) as { model?: unknown };
    if (typeof body.model !== "string") throw new Error("scope request did not preserve the selected model");
    requestedModels.push(body.model);
    const step = inferenceCalls++;
    if (step === 0) {
      const runtime = kv.values.get(encodeKey(RUNTIME_CONFIG_V2_KEY)) as {
        version: number;
        default_model: string;
        default_reasoning_effort: string;
        codex_models: unknown;
        updated_at_ms: number;
      };
      kv.put(RUNTIME_CONFIG_V2_KEY, { ...runtime, default_model: secondDefault, updated_at_ms: Date.now() });
    }
    const cachedTokens = [0, 2_560, 0, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560, 2_560][step]!;
    const cacheWriteTokens = [2_560, 0, 2_560, 0, 0, 0, 0, 0, 0, 0][step]!;
    return Promise.resolve(sseCompleted(cachedTokens, cacheWriteTokens, body.model));
  };

  try {
    const result = await runPromptCacheScopeExperiment(baseline);
    assert.equal(result.status, "in_progress");
    assert.equal(result.model, targetModel);
    assert.equal(inferenceCalls, 10);
    assert.equal(refreshes, 1);
    assert.deepEqual(requestedModels, Array(10).fill(targetModel));
    const runtime = kv.values.get(encodeKey(RUNTIME_CONFIG_V2_KEY)) as { default_model?: unknown };
    assert.equal(runtime.default_model, secondDefault);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("a catalog roster drift during a scope cycle stops before the next paid sample", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  let inferenceCalls = 0;

  const snapshot = kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)) as {
    source: string;
    client_version: string;
    updated_at_ms: number;
    models: Array<Record<string, unknown>>;
  };
  const alternateModel = `${MODEL}-alternate-during-cycle`;
  const snapshotWithAlternate = {
    ...snapshot,
    models: [
      ...snapshot.models,
      {
        slug: alternateModel,
        supported_reasoning_levels: ["none"],
        prompt_cache: { version: 1, providers: [{ id: "codex_chatgpt", controls: cacheControls }] },
      },
    ],
  };
  const runtime = kv.values.get(encodeKey(RUNTIME_CONFIG_V2_KEY)) as {
    version: number;
    default_model: string;
    default_reasoning_effort: string;
    codex_models: unknown;
    updated_at_ms: number;
  };

  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url === "https://auth.openai.com/oauth/token") {
      throw new Error("a model change after sample one must stop before credential refresh");
    }
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      kv.put(CODEX_MODELS_KV_KEY, snapshotWithAlternate);
      kv.put(RUNTIME_CONFIG_V2_KEY, { ...runtime, updated_at_ms: Date.now() });
    }
    return Promise.resolve(sseCompleted(0, 2_560));
  };

  try {
    const result = await runExperiment();
    assert.equal(result.status, "inconclusive");
    assert.equal(result.inconclusive_reason, "inventory_drift");
    assert.equal(inferenceCalls, 1);
    assert.equal(JSON.stringify(kv.values.get(encodeKey(CODEX_MODELS_KV_KEY))).includes('"scope"'), false);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("a target capability drift stops before the next paid sample", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  let inferenceCalls = 0;
  const snapshot = kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)) as {
    source: string;
    client_version: string;
    updated_at_ms: number;
    models: Array<Record<string, unknown>>;
  };
  globalThis.fetch = (input) => {
    const url = input instanceof Request ? input.url : input instanceof URL ? input.toString() : input;
    if (url === "https://auth.openai.com/oauth/token") {
      throw new Error("capability drift must stop before credential refresh");
    }
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      kv.put(CODEX_MODELS_KV_KEY, {
        ...snapshot,
        models: [
          modelRecord(MODEL, { ...cacheControls, expected_usage_fields: ["cached_tokens"] }),
        ],
        updated_at_ms: Date.now(),
      });
    }
    return Promise.resolve(sseCompleted(0, 2_560));
  };

  try {
    const result = await runExperiment();
    assert.equal(result.status, "inconclusive");
    assert.equal(result.inconclusive_reason, "target_catalog_drift");
    assert.equal(inferenceCalls, 1);
    assert.equal(JSON.stringify(kv.values.get(encodeKey(CODEX_MODELS_KV_KEY))).includes('"scope"'), false);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("a catalog client-version drift stops before the next paid sample", async () => {
  seed();
  const originalFetch = globalThis.fetch;
  let inferenceCalls = 0;
  const snapshot = kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)) as {
    source: string;
    client_version: string;
    updated_at_ms: number;
    models: Array<Record<string, unknown>>;
  };
  globalThis.fetch = (input) => {
    const url = input instanceof Request ? input.url : input instanceof URL ? input.toString() : input;
    if (url === "https://auth.openai.com/oauth/token") {
      throw new Error("client-version drift must stop before credential refresh");
    }
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      kv.put(CODEX_MODELS_KV_KEY, { ...snapshot, client_version: "0.202.0", updated_at_ms: Date.now() });
    }
    return Promise.resolve(sseCompleted(0, 2_560));
  };

  try {
    const result = await runExperiment();
    assert.equal(result.status, "inconclusive");
    assert.equal(result.inconclusive_reason, "target_catalog_drift");
    assert.equal(inferenceCalls, 1);
    assert.equal(JSON.stringify(kv.values.get(encodeKey(CODEX_MODELS_KV_KEY))).includes('"scope"'), false);
  } finally {
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("scope promotion rejects unknown dimensions and a renamed target model without mutating the catalog", async () => {
  seed();
  const lease = {
    key: leaseKey(MODEL),
    owner: "test-owner",
  };
  kv.put(lease.key, { owner: lease.owner, lease_until_ms: Date.now() + 60_000 });
  const common = {
    probe_profile: "responses_explicit_input_text_keyed_30m" as const,
    effective_model: MODEL,
    reproducible_cycles: 3,
    source: "live_probe" as const,
    verified_at_ms: Date.now(),
  };
  const unknown = await promoteCodexPromptCacheScope(kv as unknown as Deno.Kv, {
    model: MODEL,
    lease,
    authPoolVersionstamp: await authPoolVersionstamp(),
    ...(await promotionBinding()),
    scope: {
      ...common,
      account_slots: "unknown",
      token_refresh: "preserved",
      conversation_id: "independent",
    },
  });
  assert.deepEqual(unknown, { status: "inconclusive", reason: "invalid_scope" });

  const snapshot = kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)) as {
    source: string;
    client_version: string;
    updated_at_ms: number;
  };
  kv.put(CODEX_MODELS_KV_KEY, { ...snapshot, models: [{ slug: `${MODEL}-renamed` }] });
  const renamed = await promoteCodexPromptCacheScope(kv as unknown as Deno.Kv, {
    model: MODEL,
    lease,
    authPoolVersionstamp: await authPoolVersionstamp(),
    ...(await promotionBinding()),
    scope: {
      ...common,
      account_slots: "account_scoped",
      token_refresh: "preserved",
      conversation_id: "independent",
    },
  });
  assert.deepEqual(renamed, { status: "inconclusive", reason: "model_drift" });
  assert.equal(JSON.stringify(kv.values.get(encodeKey(CODEX_MODELS_KV_KEY))).includes('"scope"'), false);
});

Deno.test("scope promotion publishes a non-default target and preserves the runtime default", async () => {
  seed();
  const alternateModel = `${MODEL}-alternate`;
  const initialSnapshot = kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)) as {
    source: string;
    client_version: string;
    updated_at_ms: number;
    models: Array<{
      slug: string;
      supported_reasoning_levels: string[];
      prompt_cache?: unknown;
    }>;
  };
  const snapshotWithAlternate = {
    ...initialSnapshot,
    models: [
      ...initialSnapshot.models,
      {
        slug: alternateModel,
        supported_reasoning_levels: ["none"],
        prompt_cache: { version: 1, providers: [{ id: "codex_chatgpt", controls: cacheControls }] },
      },
    ],
  };
  kv.put(CODEX_MODELS_KV_KEY, snapshotWithAlternate);

  const initialRuntime = kv.values.get(encodeKey(RUNTIME_CONFIG_V2_KEY)) as {
    version: number;
    default_model: string;
    default_reasoning_effort: string;
    codex_models: unknown;
    updated_at_ms: number;
  };
  const runtimeBeforeSwitch = {
    ...initialRuntime,
    codex_models: snapshotWithAlternate,
    updated_at_ms: Date.now(),
  };
  kv.put(RUNTIME_CONFIG_V2_KEY, runtimeBeforeSwitch);
  const runtimeAfterSwitch = {
    ...runtimeBeforeSwitch,
    default_model: alternateModel,
    updated_at_ms: Date.now(),
  };
  kv.put(RUNTIME_CONFIG_V2_KEY, runtimeAfterSwitch);

  const lease = { key: leaseKey(MODEL), owner: "runtime-switch-owner" };
  kv.put(lease.key, { owner: lease.owner, lease_until_ms: Date.now() + 60_000 });
  const snapshotBeforePromotion = JSON.stringify(kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)));
  const runtimeBeforePromotion = JSON.stringify(kv.values.get(encodeKey(RUNTIME_CONFIG_V2_KEY)));

  const result = await promoteCodexPromptCacheScope(kv as unknown as Deno.Kv, {
    model: MODEL,
    lease,
    authPoolVersionstamp: await authPoolVersionstamp(),
    ...(await promotionBinding()),
    scope: {
      probe_profile: "responses_explicit_input_text_keyed_30m",
      account_slots: "account_scoped",
      token_refresh: "preserved",
      conversation_id: "independent",
      effective_model: MODEL,
      reproducible_cycles: 3,
      source: "live_probe",
      verified_at_ms: Date.now(),
    },
  });

  assert.deepEqual(result, { status: "promoted" });
  const publishedSnapshot = kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)) as {
    models?: Array<{
      slug?: unknown;
      prompt_cache?: { providers?: Array<{ id?: unknown; scope?: unknown }> };
    }>;
  };
  const publishedProvider = publishedSnapshot.models?.find((model) => model.slug === MODEL)?.prompt_cache?.providers
    ?.find(
      (provider) => provider.id === "codex_chatgpt",
    );
  assert.ok(publishedProvider?.scope);
  const publishedRuntime = kv.values.get(encodeKey(RUNTIME_CONFIG_V2_KEY)) as { default_model?: unknown };
  assert.equal(publishedRuntime.default_model, alternateModel);
  assert.notEqual(JSON.stringify(kv.values.get(encodeKey(CODEX_MODELS_KV_KEY))), snapshotBeforePromotion);
  assert.notEqual(JSON.stringify(kv.values.get(encodeKey(RUNTIME_CONFIG_V2_KEY))), runtimeBeforePromotion);
  assert.equal(snapshotBeforePromotion.includes('"scope"'), false);
});

Deno.test("successful scope promotion atomically extends its same-owner lease", async () => {
  seed();
  const lease = { key: leaseKey(MODEL), owner: "promotion-handoff-owner" };
  const beforePromotionMs = Date.now();
  const nearExpiryMs = beforePromotionMs + 1_000;
  kv.put(lease.key, { owner: lease.owner, lease_until_ms: nearExpiryMs });

  const result = await promoteCodexPromptCacheScope(kv as unknown as Deno.Kv, {
    model: MODEL,
    lease,
    authPoolVersionstamp: await authPoolVersionstamp(),
    ...(await promotionBinding()),
    scope: {
      probe_profile: "responses_explicit_input_text_keyed_30m",
      account_slots: "account_scoped",
      token_refresh: "preserved",
      conversation_id: "independent",
      effective_model: MODEL,
      reproducible_cycles: 3,
      source: "live_probe",
      verified_at_ms: Date.now(),
    },
  });

  assert.deepEqual(result, { status: "promoted" });
  const renewedLease = kv.values.get(encodeKey(lease.key)) as {
    owner?: unknown;
    lease_until_ms?: unknown;
  } | undefined;
  assert.equal(renewedLease?.owner, lease.owner);
  const renewedUntilMs = renewedLease?.lease_until_ms;
  if (typeof renewedUntilMs !== "number") throw new Error("successful promotion did not retain its lease");
  assert.equal(renewedUntilMs >= beforePromotionMs + 90_000, true);
  assert.equal(renewedUntilMs > nearExpiryMs, true);
});

Deno.test("scope promotion fences auth-pool drift before catalog publication", async () => {
  seed();
  const expectedAuthPoolVersionstamp = await authPoolVersionstamp();
  const lease = { key: leaseKey(MODEL), owner: "auth-pool-fence-owner" };
  kv.put(lease.key, { owner: lease.owner, lease_until_ms: Date.now() + 60_000 });
  const snapshotBeforePromotion = JSON.stringify(kv.values.get(encodeKey(CODEX_MODELS_KV_KEY)));
  const runtimeBeforePromotion = JSON.stringify(kv.values.get(encodeKey(RUNTIME_CONFIG_V2_KEY)));
  const pool = kv.values.get(encodeKey(CODEX_AUTH_POOL_KV_KEY)) as {
    accounts: Array<ReturnType<typeof makeAuth>>;
    updated_at_ms: number;
  } | undefined;
  const first = pool?.accounts[0];
  const second = pool?.accounts[1];
  if (!pool || !first || !second) throw new Error("missing seeded Codex auth pool");
  kv.put(CODEX_AUTH_POOL_KV_KEY, {
    ...pool,
    accounts: [
      {
        ...first,
        access_token: "rotated-before-promotion-access",
        refresh_token: "rotated-before-promotion-refresh",
        updated_at_ms: Date.now(),
      },
      second,
    ],
    updated_at_ms: Date.now(),
  });

  const result = await promoteCodexPromptCacheScope(kv as unknown as Deno.Kv, {
    model: MODEL,
    lease,
    authPoolVersionstamp: expectedAuthPoolVersionstamp,
    ...(await promotionBinding()),
    scope: {
      probe_profile: "responses_explicit_input_text_keyed_30m",
      account_slots: "account_scoped",
      token_refresh: "preserved",
      conversation_id: "independent",
      effective_model: MODEL,
      reproducible_cycles: 3,
      source: "live_probe",
      verified_at_ms: Date.now(),
    },
  });

  assert.deepEqual(result, { status: "inconclusive", reason: "auth_pool_drift" });
  assert.equal(JSON.stringify(kv.values.get(encodeKey(CODEX_MODELS_KV_KEY))), snapshotBeforePromotion);
  assert.equal(JSON.stringify(kv.values.get(encodeKey(RUNTIME_CONFIG_V2_KEY))), runtimeBeforePromotion);
});
