import assert from "node:assert/strict";
import { sha256Base64Url } from "../src/utils.ts";

if (typeof Deno.KvU64 !== "function") {
  (Deno as unknown as { KvU64: typeof Deno.KvU64 }).KvU64 = class {
    constructor(readonly value: bigint) {}
  } as typeof Deno.KvU64;
}

const encodeKey = (key: Deno.KvKey): string => JSON.stringify(key);
const textEncoder = new TextEncoder();

const encodeBase64Url = (bytes: Uint8Array): string => {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
};

const encodeJsonBase64Url = (value: unknown): string => encodeBase64Url(textEncoder.encode(JSON.stringify(value)));

const toPublicKeyPem = (spki: Uint8Array): string => {
  let binary = "";
  for (const byte of spki) binary += String.fromCharCode(byte);
  const lines = btoa(binary).match(/.{1,64}/g) ?? [];
  return `-----BEGIN PUBLIC KEY-----\n${lines.join("\n")}\n-----END PUBLIC KEY-----`;
};

class CountingKv {
  readonly values = new Map<string, unknown>();
  reads = 0;
  readUnits = 0;
  writes = 0;
  sums = 0;
  sumCommitAttempts = 0;
  failNextSumCommits = 0;
  retries = 0;
  readonly readKeys: Deno.KvKey[] = [];

  resetCounts(): void {
    this.reads = 0;
    this.readUnits = 0;
    this.writes = 0;
    this.sums = 0;
    this.sumCommitAttempts = 0;
    this.failNextSumCommits = 0;
    this.retries = 0;
    this.readKeys.length = 0;
  }

  get<T>(key: Deno.KvKey, _options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
    this.reads += 1;
    this.readKeys.push(key);
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
        if (mutations.some((mutation) => mutation.kind === "sum")) {
          this.sumCommitAttempts += 1;
          if (this.failNextSumCommits > 0) {
            this.failNextSumCommits -= 1;
            return Promise.reject(new Error("injected API-key usage sum failure"));
          }
        }
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
const { handleResponses } = await import("../src/openai.ts");
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
const codexAuthPool = (accountCount = 1) => ({
  accounts: Array.from({ length: accountCount }, (_, index) => ({
    access_token: `access-${index + 1}`,
    refresh_token: `refresh-${index + 1}`,
    account_id: `acct-${index + 1}`,
    updated_at_ms: Date.now(),
  })),
  updated_at_ms: Date.now(),
});

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

const seedPaidFallbackKey = async (token: string, id: string) => {
  const hash = await sha256Base64Url(token);
  const resetAtMs = Date.now() + 60_000;
  const policy = {
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: -1,
    usage_requests: 0,
    usage_reset_at_ms: resetAtMs,
    window_ms: 60_000,
    paid_fallback_enabled: true,
    paid_fallback_limit_microcredits: 1_000_000,
    paid_fallback_spent_microcredits: 0,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
  };
  kv.values.set(encodeKey(["ubq_ai", "api_keys", "hash", hash]), { id, ...policy });
  kv.values.set(encodeKey(["ubq_ai", "api_keys", "id", id]), {
    id,
    name: "First fallback quota",
    prefix: token.slice(0, 12),
    hash,
    created_at_ms: Date.now(),
    ...policy,
    paid_fallback_model_ids: [MODEL],
    paid_fallback_quota_per_credit: 500_000,
    paid_fallback_max_exposure_microcredits: { [MODEL]: 250_000 },
    paid_fallback_pricing_checked_at_ms: Date.now(),
  });
};

const request = (token: string): Request =>
  new Request("https://ai.ubq.fi/v1/responses", {
    method: "POST",
    headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
    body: JSON.stringify({ input: "ping" }),
  });

const streamingRequest = (token: string, route: "responses" | "chat"): Request =>
  new Request(
    route === "responses" ? "https://ai.ubq.fi/v1/responses" : "https://ai.ubq.fi/v1/chat/completions",
    {
      method: "POST",
      headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
      body: JSON.stringify(
        route === "responses"
          ? { input: "ping", stream: true }
          : { messages: [{ role: "user", content: "ping" }], stream: true },
      ),
    },
  );

const completedSseEvent = (inputTokens = 1, outputTokens = 1): string =>
  `data: ${
    JSON.stringify({
      type: "response.completed",
      response: {
        model: MODEL,
        output: [],
        usage: {
          input_tokens: inputTokens,
          output_tokens: outputTokens,
          total_tokens: inputTokens + outputTokens,
        },
      },
    })
  }\n\n`;

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
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(["uos_ai", "runtime_config", "v2"]), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
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

Deno.test("completed nonstream inference survives one failed quota increment attempt", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"a".repeat(64)}`;
  const { hash, record } = await seedKey(token, "failed-nonstream-accounting", 100);
  const policy = apiKeyPolicyFromHashRecord(hash, record, now);
  assert.ok(policy);
  const usageKey = apiKeyUsageV2Key(policy);
  kv.values.set(encodeKey(usageKey), new Deno.KvU64(0n));

  const originalFetch = globalThis.fetch;
  const originalWarn = console.warn;
  const warnings: unknown[][] = [];
  globalThis.fetch = () => Promise.resolve(sse());
  console.warn = (...args: unknown[]) => {
    warnings.push(args);
    throw new Error("injected logger failure");
  };
  try {
    kv.resetCounts();
    kv.failNextSumCommits = 1;
    const response = await handler(request(token));
    assert.equal(response.status, 200);
    const payload = await response.json() as { model?: string };
    assert.equal(payload.model, MODEL);
    assert.equal(kv.sumCommitAttempts, 1, "quota accounting retried after its terminal failure");
    assert.equal(kv.sums, 0);
    assert.equal((kv.values.get(encodeKey(usageKey)) as Deno.KvU64).value, 0n);

    const accountingWarnings = warnings.filter((entry) => entry[0] === "[ai.ubq.fi] quota_accounting_failed");
    assert.equal(accountingWarnings.length, 1);
    assert.deepEqual(JSON.parse(String(accountingWarnings[0]?.[1])), {
      route: "responses",
      key_id: "failed-nonstream-accounting",
      request_id: response.headers.get("x-uos-request-id"),
      errors: ["api_key: injected API-key usage sum failure"],
    });
  } finally {
    console.warn = originalWarn;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("KV budget: concurrent bounded successes keep every increment and gate the next request", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"b".repeat(64)}`;
  const { hash, record } = await seedKey(token, "concurrent-bounded", 1);
  const policy = apiKeyPolicyFromHashRecord(hash, record, now);
  assert.ok(policy);
  const usageKey = apiKeyUsageV2Key(policy);
  kv.values.set(encodeKey(usageKey), new Deno.KvU64(0n));

  const concurrency = 8;
  let fetchCalls = 0;
  let releaseUpstreams: () => void = () => {};
  let resolveAllDispatched: () => void = () => {};
  const upstreamGate = new Promise<void>((resolve) => {
    releaseUpstreams = resolve;
  });
  const allDispatched = new Promise<void>((resolve) => {
    resolveAllDispatched = resolve;
  });
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => {
    fetchCalls += 1;
    if (fetchCalls === concurrency) resolveAllDispatched();
    await upstreamGate;
    return sse();
  };
  try {
    kv.resetCounts();
    const pending = Array.from({ length: concurrency }, () => handler(request(token)));
    await allDispatched;
    assert.equal(kv.sums, 0, "usage was reserved before a successful completion");
    releaseUpstreams();

    const responses = await Promise.all(pending);
    assert.deepEqual(responses.map((response) => response.status), Array(concurrency).fill(200));
    assert.equal(kv.sumCommitAttempts, concurrency);
    assert.equal(kv.sums, concurrency);
    assert.equal(kv.retries, 0);
    assert.equal((kv.values.get(encodeKey(usageKey)) as Deno.KvU64).value, BigInt(concurrency));

    const rejected = await handler(request(token));
    assert.equal(rejected.status, 429);
    assert.equal(fetchCalls, concurrency, "an over-limit request reached the upstream");
    assert.equal((kv.values.get(encodeKey(usageKey)) as Deno.KvU64).value, BigInt(concurrency));
  } finally {
    releaseUpstreams();
    globalThis.fetch = originalFetch;
  }
});

Deno.test("streaming limits increment once only after response.completed", async () => {
  const originalFetch = globalThis.fetch;
  const encoder = new TextEncoder();
  const prepare = async (tokenDigit: string, keyId: string) => {
    kv.values.clear();
    resetApiKeyPolicyCacheForTest();
    resetRuntimeConfigCacheForTest();
    resetCodexAuthCacheForTest();
    kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
    kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
    const token = `u_${tokenDigit.repeat(64)}`;
    const { hash, record } = await seedKey(token, keyId, 100);
    const policy = apiKeyPolicyFromHashRecord(hash, record, now);
    assert.ok(policy);
    const usageKey = apiKeyUsageV2Key(policy);
    kv.values.set(encodeKey(usageKey), new Deno.KvU64(0n));
    kv.resetCounts();
    return { token, usageKey };
  };

  try {
    for (const [route, tokenDigit] of [["responses", "a"], ["chat", "b"]] as const) {
      const { token, usageKey } = await prepare(tokenDigit, `stream-completed-${route}`);
      const upstream = { controller: null as ReadableStreamDefaultController<Uint8Array> | null };
      globalThis.fetch = () =>
        Promise.resolve(
          new Response(
            new ReadableStream<Uint8Array>({
              start(controller) {
                upstream.controller = controller;
                controller.enqueue(
                  encoder.encode(`data: ${JSON.stringify({ type: "response.created", response: { id: route } })}\n\n`),
                );
              },
            }),
            { status: 200, headers: { "Content-Type": "text/event-stream" } },
          ),
        );

      const response = await handler(streamingRequest(token, route));
      assert.equal(response.status, 200);
      assert.equal(kv.sums, 0, `${route} counted before response.completed`);
      const reader = response.body!.getReader();
      if (route === "responses") {
        const created = await reader.read();
        assert.equal(created.done, false);
        assert.equal(kv.sums, 0, "Responses counted after only response.created");
      }

      const completedChunk = reader.read();
      upstream.controller!.enqueue(encoder.encode(completedSseEvent(3, 4)));
      upstream.controller!.enqueue(encoder.encode(completedSseEvent(5, 6)));
      upstream.controller!.close();
      assert.equal((await completedChunk).done, false);
      assert.equal(kv.sums, 1, `${route} did not count before exposing its completion chunk`);
      while (!(await reader.read()).done) {
        // Drain any trailing [DONE] or duplicate upstream completion chunks.
      }
      assert.equal(kv.sums, 1, `${route} counted one response more than once`);
      assert.equal((kv.values.get(encodeKey(usageKey)) as Deno.KvU64).value, 1n);
    }

    for (const [route, tokenDigit] of [["responses", "c"], ["chat", "d"]] as const) {
      const { token, usageKey } = await prepare(tokenDigit, `stream-truncated-${route}`);
      globalThis.fetch = () =>
        Promise.resolve(
          new Response(
            `data: ${JSON.stringify({ type: "response.created", response: { id: route } })}\n\n`,
            { status: 200, headers: { "Content-Type": "text/event-stream" } },
          ),
        );
      const response = await handler(streamingRequest(token, route));
      assert.equal(response.status, 200);
      await response.text();
      assert.equal(kv.sums, 0, `${route} counted a truncated stream`);
      assert.equal((kv.values.get(encodeKey(usageKey)) as Deno.KvU64).value, 0n);
    }

    for (const [route, tokenDigit] of [["responses", "e"], ["chat", "f"]] as const) {
      const { token, usageKey } = await prepare(tokenDigit, `stream-cancelled-${route}`);
      const upstream = { controller: null as ReadableStreamDefaultController<Uint8Array> | null };
      globalThis.fetch = () =>
        Promise.resolve(
          new Response(
            new ReadableStream<Uint8Array>({
              start(controller) {
                upstream.controller = controller;
                controller.enqueue(
                  encoder.encode(`data: ${JSON.stringify({ type: "response.created", response: { id: route } })}\n\n`),
                );
              },
            }),
            { status: 200, headers: { "Content-Type": "text/event-stream" } },
          ),
        );
      const response = await handler(streamingRequest(token, route));
      assert.equal(response.status, 200);
      if (route === "responses") {
        const reader = response.body!.getReader();
        assert.equal((await reader.read()).done, false);
        await reader.cancel("test cancelled before completion");
      } else {
        await response.body!.cancel("test cancelled before completion");
      }
      try {
        upstream.controller?.close();
      } catch {
        // Cancellation may already have closed the upstream source.
      }
      assert.equal(kv.sums, 0, `${route} counted a cancelled stream`);
      assert.equal((kv.values.get(encodeKey(usageKey)) as Deno.KvU64).value, 0n);
    }
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("streaming completion increments API-key and kernel limits together exactly once", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());

  const token = `u_${"0".repeat(64)}`;
  const { hash, record } = await seedKey(token, "stream-kernel-and-key", 100);
  const policy = apiKeyPolicyFromHashRecord(hash, record, now);
  assert.ok(policy);
  const usageKey = apiKeyUsageV2Key(policy);
  kv.values.set(encodeKey(usageKey), new Deno.KvU64(0n));

  const keyPair = await crypto.subtle.generateKey(
    {
      name: "RSASSA-PKCS1-v1_5",
      modulusLength: 2048,
      publicExponent: new Uint8Array([0x01, 0x00, 0x01]),
      hash: "SHA-256",
    },
    true,
    ["sign", "verify"],
  );
  const publicKey = new Uint8Array(await crypto.subtle.exportKey("spki", keyPair.publicKey));
  kv.values.set(encodeKey(["uos_ai", "kernel_pubkeys"]), [{ pem: toPublicKeyPem(publicKey) }]);
  const nowSeconds = Math.floor(Date.now() / 1000);
  const header = encodeJsonBase64Url({ alg: "RS256", typ: "JWT" });
  const makeKernelToken = async (): Promise<string> => {
    const payload = encodeJsonBase64Url({
      iss: "ubiquity-os-kernel",
      aud: "ai.ubq.fi",
      iat: nowSeconds,
      exp: nowSeconds + 600,
      jti: `jti_${crypto.randomUUID()}`,
      owner: "lifecycle-org",
      repo: "lifecycle-repo",
      installation_id: null,
      auth_token_sha256: await sha256Base64Url(token),
      state_id: "state_lifecycle",
    });
    const signingInput = `${header}.${payload}`;
    const signature = new Uint8Array(
      await crypto.subtle.sign("RSASSA-PKCS1-v1_5", keyPair.privateKey, textEncoder.encode(signingInput)),
    );
    return `${signingInput}.${encodeBase64Url(signature)}`;
  };
  const kernelToken = await makeKernelToken();

  const upstream = { controller: null as ReadableStreamDefaultController<Uint8Array> | null };
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(
        new ReadableStream<Uint8Array>({
          start(controller) {
            upstream.controller = controller;
            controller.enqueue(
              textEncoder.encode(
                `data: ${JSON.stringify({ type: "response.created", response: { id: "kernel" } })}\n\n`,
              ),
            );
          },
        }),
        { status: 200, headers: { "Content-Type": "text/event-stream" } },
      ),
    );
  try {
    const baseRequest = streamingRequest(token, "responses");
    const headers = new Headers(baseRequest.headers);
    headers.set("X-Ubiquity-Kernel-Token", kernelToken);
    const response = await handler(new Request(baseRequest, { headers }));
    assert.equal(response.status, 200);
    const kernelOrgLimitKey = ["ubq_ai", "kernel_auth", "org_limits", "lifecycle-org"] as const;
    assert.equal((kv.values.get(encodeKey(usageKey)) as Deno.KvU64).value, 0n);
    assert.equal(kv.values.has(encodeKey(kernelOrgLimitKey)), false);

    const body = response.text();
    upstream.controller!.enqueue(textEncoder.encode(completedSseEvent(2, 3)));
    upstream.controller!.enqueue(textEncoder.encode(completedSseEvent(4, 5)));
    upstream.controller!.close();
    await body;

    assert.equal((kv.values.get(encodeKey(usageKey)) as Deno.KvU64).value, 1n);
    const kernelLimit = kv.values.get(encodeKey(kernelOrgLimitKey)) as { usage_requests?: number } | undefined;
    assert.equal(kernelLimit?.usage_requests, 1);

    const originalWarn = console.warn;
    const warnings: unknown[][] = [];
    console.warn = (...args: unknown[]) => warnings.push(args);
    try {
      kv.resetCounts();
      kv.failNextSumCommits = 1;
      const failedAccountingRequest = streamingRequest(token, "responses");
      const failedAccountingHeaders = new Headers(failedAccountingRequest.headers);
      failedAccountingHeaders.set("X-Ubiquity-Kernel-Token", await makeKernelToken());
      const failedAccountingResponse = await handler(
        new Request(failedAccountingRequest, {
          headers: failedAccountingHeaders,
        }),
      );
      assert.equal(failedAccountingResponse.status, 200);
      const failedAccountingBody = failedAccountingResponse.text();
      upstream.controller!.enqueue(textEncoder.encode(completedSseEvent(6, 7)));
      upstream.controller!.close();
      assert.match(await failedAccountingBody, /response\.completed/);

      assert.equal(kv.sumCommitAttempts, 1, "API-key accounting retried after failing once");
      assert.equal((kv.values.get(encodeKey(usageKey)) as Deno.KvU64).value, 1n);
      const updatedKernelLimit = kv.values.get(encodeKey(kernelOrgLimitKey)) as
        | { usage_requests?: number }
        | undefined;
      assert.equal(updatedKernelLimit?.usage_requests, 2, "API-key failure skipped independent kernel accounting");
      const accountingWarnings = warnings.filter((entry) => entry[0] === "[ai.ubq.fi] quota_accounting_failed");
      assert.equal(accountingWarnings.length, 1);
      assert.deepEqual(JSON.parse(String(accountingWarnings[0]?.[1])), {
        route: "responses",
        key_id: "stream-kernel-and-key",
        request_id: failedAccountingResponse.headers.get("x-uos-request-id"),
        errors: ["api_key: injected API-key usage sum failure"],
      });
    } finally {
      console.warn = originalWarn;
    }
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("KV budget: warm kernel inference writes no ordinary usage aggregates", async () => {
  kv.values.clear();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => Promise.resolve(sse());
  const kernelContext = {
    keyId: null,
    kernelRepo: { owner: "ubiquity", repo: "kernel" },
    kernelOrg: { owner: "ubiquity" },
    paidFallbackEnabled: false,
    requestId: "kernel-telemetry-budget",
    startedAtMs: Date.now(),
  };
  const kernelRequest = () =>
    new Request("https://ai.ubq.fi/v1/responses", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ input: "ping" }),
    });
  try {
    assert.equal((await handleResponses(kernelRequest(), kernelContext)).status, 200);
    kv.resetCounts();
    assert.equal((await handleResponses(kernelRequest(), kernelContext)).status, 200);
    assert.deepEqual({ reads: kv.reads, writes: kv.writes }, { reads: 0, writes: 0 });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("terminal inference telemetry includes resolved defaults and response usage", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"5".repeat(64)}`;
  await seedKey(token, "telemetry", -1);
  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const originalGitRevision = Deno.env.get("GIT_REVISION");
  const originalGithubSha = Deno.env.get("GITHUB_SHA");
  const originalBuildId = Deno.env.get("DENO_DEPLOY_BUILD_ID");
  const originalDeploymentId = Deno.env.get("DENO_DEPLOYMENT_ID");
  const logs: unknown[][] = [];
  globalThis.fetch = () => Promise.resolve(sse());
  console.info = (...args: unknown[]) => logs.push(args);
  Deno.env.delete("GIT_REVISION");
  Deno.env.delete("GITHUB_SHA");
  Deno.env.delete("DENO_DEPLOY_BUILD_ID");
  Deno.env.delete("DENO_DEPLOYMENT_ID");
  try {
    const response = await handler(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
        body: JSON.stringify({ input: "ping" }),
      }),
    );
    assert.equal(response.status, 200);
    const terminal = logs.find((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.ok(terminal);
    const requestId = response.headers.get("x-uos-request-id");
    assert.ok(requestId);
    assert.deepEqual(JSON.parse(String(terminal[1])), {
      request_id: requestId,
      route: "responses",
      status: 200,
      provider: "chatgpt_codex",
      latency_ms: JSON.parse(String(terminal[1])).latency_ms,
      model: MODEL,
      reasoning: "medium",
      key_id: "telemetry",
      fallback_reason: null,
      stream_terminal_type: "response.completed",
      git_sha: "unknown",
      deno_revision: "unknown",
      router_revision: null,
    });
    const accepted = logs.find((entry) => entry[0] === "[ai.ubq.fi] request_accepted");
    assert.ok(accepted);
    assert.equal(JSON.parse(String(accepted[1])).request_id, requestId);
  } finally {
    console.info = originalInfo;
    globalThis.fetch = originalFetch;
    if (originalGitRevision === undefined) Deno.env.delete("GIT_REVISION");
    else Deno.env.set("GIT_REVISION", originalGitRevision);
    if (originalGithubSha === undefined) Deno.env.delete("GITHUB_SHA");
    else Deno.env.set("GITHUB_SHA", originalGithubSha);
    if (originalBuildId === undefined) Deno.env.delete("DENO_DEPLOY_BUILD_ID");
    else Deno.env.set("DENO_DEPLOY_BUILD_ID", originalBuildId);
    if (originalDeploymentId === undefined) Deno.env.delete("DENO_DEPLOYMENT_ID");
    else Deno.env.set("DENO_DEPLOYMENT_ID", originalDeploymentId);
  }
});

Deno.test("streaming inference emits one terminal log only after the response body completes", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"7".repeat(64)}`;
  await seedKey(token, "stream-telemetry", -1);

  const encoder = new TextEncoder();
  let resolveUpstreamController: (controller: ReadableStreamDefaultController<Uint8Array>) => void = () => {};
  const upstreamControllerPromise = new Promise<ReadableStreamDefaultController<Uint8Array>>((resolve) => {
    resolveUpstreamController = resolve;
  });
  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const logs: unknown[][] = [];
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(
        new ReadableStream<Uint8Array>({
          start(controller) {
            resolveUpstreamController(controller);
            controller.enqueue(
              encoder.encode(`data: ${JSON.stringify({ type: "response.created", response: { id: "stream" } })}\n\n`),
            );
          },
        }),
        { status: 200, headers: { "Content-Type": "text/event-stream" } },
      ),
    );
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    const response = await handler(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
        body: JSON.stringify({ input: "ping", stream: true }),
      }),
    );
    assert.equal(response.status, 200);
    assert.equal(logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal").length, 0);

    const bodyPromise = response.text();
    const upstreamController = await upstreamControllerPromise;
    upstreamController.enqueue(
      encoder.encode(
        `data: ${
          JSON.stringify({
            type: "response.completed",
            response: { model: MODEL, output: [], usage: { input_tokens: 3, output_tokens: 4, total_tokens: 7 } },
          })
        }\n\n`,
      ),
    );
    upstreamController.close();
    await bodyPromise;

    const terminalLogs = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalLogs.length, 1);
    const terminal = JSON.parse(String(terminalLogs[0]?.[1])) as Record<string, unknown>;
    assert.equal(terminal.provider, "chatgpt_codex");
    assert.equal(terminal.model, MODEL);
    assert.equal(terminal.reasoning, "medium");
    assert.equal(terminal.stream_terminal_type, "response.completed");
    assert.equal(Object.prototype.hasOwnProperty.call(terminal, "input_tokens"), false);
    assert.equal(Object.prototype.hasOwnProperty.call(terminal, "output_tokens"), false);
    assert.equal(terminal.request_id, response.headers.get("x-uos-request-id"));
  } finally {
    console.info = originalInfo;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("first bounded paid fallback response exposes settled spend without counting its reservation", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool(2));
  const token = `u_${"8".repeat(64)}`;
  const keyId = "first-fallback-quota";
  await seedPaidFallbackKey(token, keyId);

  const originalFetch = globalThis.fetch;
  const originalYunwuApiKey = Deno.env.get("YUNWU_API_KEY");
  let calls = 0;
  const primaryAccountIds: string[] = [];
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  globalThis.fetch = (input, init) => {
    calls += 1;
    const request = new Request(input, init);
    const url = request.url;
    if (url === "https://yunwu.ai/v1/responses") {
      const response = sse();
      const headers = new Headers(response.headers);
      headers.set("X-Oneapi-Request-Id", "first-fallback-provider-request");
      return Promise.resolve(new Response(response.body, { status: 200, headers }));
    }
    primaryAccountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
        status: 429,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };
  try {
    kv.resetCounts();
    const response = await handler(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
        body: JSON.stringify({ input: "ping", stream: true }),
      }),
    );
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-codex-primary-used-percent"), "0");
    assert.equal(calls, 3);
    assert.equal(primaryAccountIds.length, 2);
    assert.equal(new Set(primaryAccountIds).size, 2);
    // V3 admission reads the immutable request, window, and deletion guard
    // together before its atomic reservation; that adds one read over the
    // legacy single-slot path while avoiding shared reservation contention.
    assert.ok(kv.reads <= 10, `fallback response unexpectedly reread KV (${kv.reads} reads)`);
    await response.body?.cancel();
  } finally {
    globalThis.fetch = originalFetch;
    if (originalYunwuApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalYunwuApiKey);
  }
});

Deno.test("paid fallback terminal telemetry records YunWu lifecycle", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"9".repeat(64)}`;
  await seedPaidFallbackKey(token, "fallback-terminal-telemetry");

  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const originalYunwuApiKey = Deno.env.get("YUNWU_API_KEY");
  const logs: unknown[][] = [];
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  globalThis.fetch = (input) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    if (url === "https://yunwu.ai/v1/responses") return Promise.resolve(sse());
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
        status: 429,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    const response = await handler(request(token));
    assert.equal(response.status, 200);
    await response.text();
    const terminalLogs = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalLogs.length, 1);
    const terminal = JSON.parse(String(terminalLogs[0]?.[1])) as Record<string, unknown>;
    assert.equal(terminal.provider, "yunwu");
    assert.equal(terminal.fallback_reason, "primary_429");
    assert.equal(terminal.stream_terminal_type, "response.completed");
    assert.equal(terminal.request_id, response.headers.get("x-uos-request-id"));
  } finally {
    console.info = originalInfo;
    globalThis.fetch = originalFetch;
    if (originalYunwuApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalYunwuApiKey);
  }
});

Deno.test("paid fallback cancellation telemetry records a cancelled YunWu lifecycle", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"a".repeat(64)}`;
  const keyId = "fallback-cancel-telemetry";
  await seedPaidFallbackKey(token, keyId);

  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const originalYunwuApiKey = Deno.env.get("YUNWU_API_KEY");
  const logs: unknown[][] = [];
  const encoder = new TextEncoder();
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  globalThis.fetch = (input) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    if (url === "https://yunwu.ai/v1/responses") {
      return Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(
                encoder.encode(
                  `data: ${JSON.stringify({ type: "response.created", response: { id: "cancelled" } })}\n\n`,
                ),
              );
            },
          }),
          { status: 200, headers: { "Content-Type": "text/event-stream" } },
        ),
      );
    }
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
        status: 429,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    const response = await handler(streamingRequest(token, "responses"));
    assert.equal(response.status, 200);
    assert.ok(response.body);
    const reader = response.body.getReader();
    assert.equal((await reader.read()).done, false);
    await reader.cancel("client disconnected");

    const terminalLogs = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalLogs.length, 1);
    const terminal = JSON.parse(String(terminalLogs[0]?.[1])) as Record<string, unknown>;
    assert.equal(terminal.provider, "yunwu");
    assert.equal(terminal.fallback_reason, "primary_429");
    assert.equal(terminal.stream_terminal_type, "cancelled");
  } finally {
    console.info = originalInfo;
    globalThis.fetch = originalFetch;
    if (originalYunwuApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalYunwuApiKey);
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

Deno.test("KV budget: automatic window advancement keeps the active counter identity", async () => {
  const token = `u_${"6".repeat(64)}`;
  const { hash, record } = await seedKey(token, "window-advance", 100);
  const expired = { ...record, usage_reset_at_ms: now - 1 };
  const beforeAdvance = apiKeyPolicyFromHashRecord(hash, expired, now);
  assert.ok(beforeAdvance);
  const afterAdvance = apiKeyPolicyFromHashRecord(
    hash,
    { ...expired, usage_reset_at_ms: beforeAdvance.usage_reset_at_ms },
    now,
  );
  const explicitlyReset = apiKeyPolicyFromHashRecord(
    hash,
    { ...expired, usage_reset_at_ms: now + expired.window_ms },
    now,
  );
  assert.ok(afterAdvance && explicitlyReset);
  assert.deepEqual(apiKeyUsageV2Key(afterAdvance), apiKeyUsageV2Key(beforeAdvance));
  assert.notDeepEqual(apiKeyUsageV2Key(explicitlyReset), apiKeyUsageV2Key(beforeAdvance));
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

Deno.test("KV budget: failed runtime configuration refresh backs off with stale configuration", async () => {
  resetRuntimeConfigCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  assert.equal((await loadRuntimeConfig(kv as unknown as Deno.Kv, now))?.default_model, MODEL);

  let failedReads = 0;
  const unavailableKv = {
    get: () => {
      failedReads += 1;
      return Promise.reject(new Error("runtime config KV unavailable"));
    },
  } as unknown as Deno.Kv;
  assert.equal(
    (await loadRuntimeConfig(unavailableKv, now + RUNTIME_CONFIG_CACHE_TTL_MS + 1))?.default_model,
    MODEL,
  );
  assert.equal(
    (await loadRuntimeConfig(unavailableKv, now + RUNTIME_CONFIG_CACHE_TTL_MS * 2))?.default_model,
    MODEL,
  );
  assert.equal(failedReads, 1);
});

Deno.test("KV budget: malformed tokens are rejected without KV and policy expiry refreshes revocation", async () => {
  const originalInfo = console.info;
  const logs: unknown[][] = [];
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    kv.resetCounts();
    const malformedResponse = await handler(request("malformed"));
    assert.equal(malformedResponse.status, 401);
    assert.deepEqual({ reads: kv.reads, writes: kv.writes }, { reads: 0, writes: 0 });
    const terminalLogs = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalLogs.length, 1);
    const terminal = JSON.parse(String(terminalLogs[0]?.[1])) as Record<string, unknown>;
    assert.equal(terminal.status, 401);
    assert.equal(terminal.provider, "gateway");
    assert.equal(terminal.key_id, null);
    assert.equal(terminal.request_id, malformedResponse.headers.get("x-uos-request-id"));
    assert.equal(Object.prototype.hasOwnProperty.call(terminal, "input_tokens"), false);
    assert.equal(Object.prototype.hasOwnProperty.call(terminal, "output_tokens"), false);
  } finally {
    console.info = originalInfo;
  }

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
