import assert from "node:assert/strict";
import { DEFAULT_REASONING_EFFORT_KEY } from "../src/defaults.ts";
import { sha256Hex } from "../src/utils.ts";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const keyHasPrefix = (key: Deno.KvKey, prefix: Deno.KvKey): boolean => {
  if (key.length < prefix.length) return false;
  for (let i = 0; i < prefix.length; i += 1) {
    if (key[i] !== prefix[i]) return false;
  }
  return true;
};

const compareKeyPart = (a: unknown, b: unknown): number => {
  if (a === b) return 0;
  if (typeof a === "number" && typeof b === "number") return a - b;
  if (typeof a === "string" && typeof b === "string") return a.localeCompare(b);
  return String(a).localeCompare(String(b));
};

const compareKeys = (a: Deno.KvKey, b: Deno.KvKey): number => {
  const len = Math.max(a.length, b.length);
  for (let i = 0; i < len; i += 1) {
    if (i >= a.length) return -1;
    if (i >= b.length) return 1;
    const diff = compareKeyPart(a[i], b[i]);
    if (diff !== 0) return diff;
  }
  return 0;
};

const kvStore = new Map<string, unknown>();
const VOYAGE_RATE_LIMIT_KEY: Deno.KvKey = ["embeddings", "v1", "rate", "voyage"];
const resetVoyageRateLimit = () => void kvStore.delete(keyToString(VOYAGE_RATE_LIMIT_KEY));
// Keep these in sync with tests/openai-compat.test.ts so whichever test imports
// src/openai.ts first doesn't change behavior.
kvStore.set(keyToString(DEFAULT_REASONING_EFFORT_KEY), "low");
kvStore.set(keyToString(["ubq_ai", "codex_auth"]), {
  access_token: "access",
  refresh_token: "refresh",
  account_id: "acct",
  updated_at_ms: Date.now(),
});
kvStore.set(keyToString(["ubq_ai", "codex_models"]), {
  source: "chatgpt_codex",
  client_version: "0.125.0",
  updated_at_ms: Date.now(),
  models: [{
    slug: "gpt-5-fixture-default",
    display_name: "GPT-5 Fixture Default",
    default_reasoning_level: "medium",
    supported_reasoning_levels: ["low", "medium", "high", "xhigh"],
  }],
});
kvStore.set(keyToString(["uos_ai", "voyage_api_key"]), "voyage_test_key");

const originalOpenKv = (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv;
const originalVoyageApiKey = Deno.env.get("VOYAGEAI_API_KEY");
Deno.env.delete("VOYAGEAI_API_KEY");

let failNextAtomicCommit:
  | ((
    checks: ReadonlyArray<Deno.KvEntryMaybe<unknown>>,
    ops: ReadonlyArray<{ type: string; key: Deno.KvKey }>,
  ) => boolean | Error)
  | null = null;

const kvStub = {
  get: (key: Deno.KvKey) =>
    Promise.resolve(({ key, value: kvStore.get(keyToString(key)) ?? null }) as Deno.KvEntryMaybe<unknown>),
  set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
    kvStore.set(keyToString(key), value);
    return Promise.resolve({ ok: true } as const);
  },
  delete: (key: Deno.KvKey) => {
    kvStore.delete(keyToString(key));
    return Promise.resolve();
  },
  list: async function* (selector: Deno.KvListSelector, options?: Deno.KvListOptions) {
    const prefix = "prefix" in selector ? selector.prefix : null;
    if (!prefix) {
      yield* [];
      return;
    }
    const limit = Math.max(0, Math.trunc(options?.limit ?? Infinity));
    const entries: Array<Deno.KvEntry<unknown>> = [];
    for (const [rawKey, value] of kvStore.entries()) {
      let key: unknown = null;
      try {
        key = JSON.parse(rawKey);
      } catch {
        key = null;
      }
      if (!Array.isArray(key)) continue;
      if (!keyHasPrefix(key as Deno.KvKey, prefix)) continue;
      entries.push({ key: key as Deno.KvKey, value } as Deno.KvEntry<unknown>);
    }
    entries.sort((a, b) => compareKeys(a.key, b.key));
    for (const entry of entries.slice(0, limit)) {
      yield entry;
    }
  },
  atomic: () => {
    const checks: Array<Deno.KvEntryMaybe<unknown>> = [];
    const ops: Array<{ type: "set" | "delete"; key: Deno.KvKey; value?: unknown }> = [];
    const chain = {
      check: (entry: Deno.KvEntryMaybe<unknown>) => {
        checks.push(entry);
        return chain;
      },
      set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
        ops.push({ type: "set", key, value });
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        ops.push({ type: "delete", key });
        return chain;
      },
      commit: () => {
        if (failNextAtomicCommit) {
          const failure = failNextAtomicCommit(
            checks,
            ops.map((op) => ({ type: op.type, key: op.key })),
          );
          if (failure) {
            failNextAtomicCommit = null;
            if (failure instanceof Error) throw failure;
            return Promise.resolve({ ok: false } as const);
          }
        }
        for (const op of ops) {
          if (op.type === "set") kvStore.set(keyToString(op.key), op.value);
          else kvStore.delete(keyToString(op.key));
        }
        return Promise.resolve({ ok: true } as const);
      },
    };
    return chain;
  },
  close: () => {},
} as unknown as Deno.Kv;

(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kvStub);

const { handleEmbeddings, handleEmbeddingsJobCreate, handleEmbeddingsJobGet } = await import("../src/openai.ts");
const { kvPromise } = await import("../src/kv.ts");

type FetchMockQueue = {
  chain: Promise<void>;
};

const fetchMockQueue: FetchMockQueue = (() => {
  const key = "__uosFetchMockQueue";
  const globalRecord = globalThis as unknown as Record<string, unknown>;
  const existing = globalRecord[key];
  if (existing && typeof existing === "object") {
    const chain = (existing as { chain?: unknown }).chain;
    if (chain instanceof Promise) return existing as FetchMockQueue;
  }
  const created: FetchMockQueue = { chain: Promise.resolve() };
  globalRecord[key] = created;
  return created;
})();

const withFetchMock = async <T>(
  handler: (url: string, bodyText: string | null, headers: Headers) => Response | Promise<Response>,
  fn: () => Promise<T>,
): Promise<T> => {
  const prev = fetchMockQueue.chain;
  let release = () => {};
  fetchMockQueue.chain = new Promise<void>((resolve) => {
    release = () => resolve(undefined);
  });
  await prev;

  const originalFetch = globalThis.fetch;
  globalThis.fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    const bodyText = typeof init?.body === "string" ? init.body : null;
    const headers = new Headers(init?.headers);
    return await handler(url, bodyText, headers);
  };
  try {
    return await fn();
  } finally {
    globalThis.fetch = originalFetch;
    release();
  }
};

const voyageOkResponse = (count: number): Response => {
  const vectors = Array.from({ length: count }, (_, i) => ({
    embedding: [i + 0.1, i + 0.2, i + 0.3],
  }));
  const totalTokens = count * 5;
  return new Response(JSON.stringify({ data: vectors, usage: { total_tokens: totalTokens } }), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
};

Deno.test("embeddings: normalizes string input", async () => {
  resetVoyageRateLimit();
  const response = await withFetchMock(
    (url, bodyText, headers) => {
      assert.equal(url, "https://api.voyageai.com/v1/embeddings");
      assert.equal(headers.get("authorization"), "Bearer voyage_test_key");
      const body = JSON.parse(bodyText ?? "null") as { input?: unknown };
      const count = Array.isArray(body.input) ? body.input.length : 1;
      return voyageOkResponse(count);
    },
    () =>
      handleEmbeddings(
        new Request("https://ai.ubq.fi/v1/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "text-embedding-3-small", input: "hello" }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as {
    object?: string;
    model?: string;
    data?: Array<{ object?: string; index?: number; embedding?: unknown }>;
    usage?: { prompt_tokens?: unknown; total_tokens?: unknown };
  };
  assert.equal(payload.object, "list");
  assert.equal(payload.model, "text-embedding-3-small");
  assert.equal(typeof payload.usage?.prompt_tokens, "number");
  assert.equal(typeof payload.usage?.total_tokens, "number");
  assert.equal(payload.usage?.prompt_tokens, 5);
  assert.equal(payload.usage?.total_tokens, 5);
  assert.ok(Array.isArray(payload.data));
  assert.equal(payload.data.length, 1);
  assert.equal(payload.data[0]?.object, "embedding");
  assert.equal(payload.data[0]?.index, 0);
  assert.ok(Array.isArray(payload.data[0]?.embedding));
});

Deno.test("embeddings: serves cache hits without calling upstream", async () => {
  resetVoyageRateLimit();
  const model = "text-embedding-3-small";
  const input = `cache-hit-${crypto.randomUUID()}`;
  const hash = await sha256Hex(input);
  const cacheKey: Deno.KvKey = ["embeddings", "v1", model.toLowerCase(), hash];
  const cachedEmbedding = [9.9, 8.8, 7.7];
  kvStore.set(keyToString(cacheKey), { embedding: cachedEmbedding, created_at: new Date().toISOString() });

  try {
    const response = await withFetchMock(
      () => {
        throw new Error("Embeddings should not hit upstream when cache is populated");
      },
      () =>
        handleEmbeddings(
          new Request("https://ai.ubq.fi/v1/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model, input }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as { data?: Array<{ embedding?: unknown }> };
    assert.ok(Array.isArray(payload.data));
    assert.deepEqual(payload.data?.[0]?.embedding, cachedEmbedding);
  } finally {
    kvStore.delete(keyToString(cacheKey));
  }
});

Deno.test("embeddings: writes cache entries on upstream misses", async () => {
  resetVoyageRateLimit();
  const model = "text-embedding-3-small";
  const input = `cache-miss-${crypto.randomUUID()}`;
  const hash = await sha256Hex(input);
  const cacheKey: Deno.KvKey = ["embeddings", "v1", model.toLowerCase(), hash];
  const cacheModelKey = model.toLowerCase();
  const byHashKey: Deno.KvKey = ["embeddings", "v1", "cache_index_by_hash", cacheModelKey, hash];
  const fixedNowMs = 1_700_000_000_000;
  const indexKey: Deno.KvKey = ["embeddings", "v1", "cache_index", cacheModelKey, fixedNowMs, hash];
  kvStore.delete(keyToString(cacheKey));
  kvStore.delete(keyToString(byHashKey));
  kvStore.delete(keyToString(indexKey));
  const originalNow = Date.now;
  Date.now = () => fixedNowMs;

  try {
    const response = await withFetchMock(
      (_url, bodyText) => {
        const body = JSON.parse(bodyText ?? "null") as { input?: unknown };
        const count = Array.isArray(body.input) ? body.input.length : 1;
        return voyageOkResponse(count);
      },
      () =>
        handleEmbeddings(
          new Request("https://ai.ubq.fi/v1/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model, input }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    const stored = kvStore.get(keyToString(cacheKey)) as { embedding?: unknown } | undefined;
    assert.ok(stored);
    assert.ok(Array.isArray(stored.embedding));
    assert.equal(kvStore.get(keyToString(byHashKey)), fixedNowMs);
    assert.equal(kvStore.get(keyToString(indexKey)), 1);
  } finally {
    Date.now = originalNow;
    kvStore.delete(keyToString(cacheKey));
    kvStore.delete(keyToString(byHashKey));
    kvStore.delete(keyToString(indexKey));
  }
});

Deno.test("embeddings cache: retries cache write when atomic commit fails", async () => {
  resetVoyageRateLimit();
  const model = "text-embedding-3-small";
  const cacheModelKey = model.toLowerCase();
  const input = `cache-atomic-fail-${crypto.randomUUID()}`;
  const hash = await sha256Hex(input);
  const cacheKey: Deno.KvKey = ["embeddings", "v1", cacheModelKey, hash];
  const pointerMs = 1_700_000_000_000;
  const byHashKey: Deno.KvKey = ["embeddings", "v1", "cache_index_by_hash", cacheModelKey, hash];
  const indexKey: Deno.KvKey = ["embeddings", "v1", "cache_index", cacheModelKey, pointerMs, hash];
  kvStore.set(keyToString(byHashKey), pointerMs);
  kvStore.delete(keyToString(cacheKey));
  kvStore.delete(keyToString(indexKey));

  failNextAtomicCommit = (checks) => checks.some((entry) => keyToString(entry.key) === keyToString(byHashKey));

  try {
    const response = await withFetchMock(
      (_url, bodyText) => {
        const body = JSON.parse(bodyText ?? "null") as { input?: unknown };
        const count = Array.isArray(body.input) ? body.input.length : 1;
        return voyageOkResponse(count);
      },
      () =>
        handleEmbeddings(
          new Request("https://ai.ubq.fi/v1/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model, input }),
          }),
        ),
    );
    assert.equal(response.status, 200);
    const stored = kvStore.get(keyToString(cacheKey)) as { embedding?: unknown; created_at?: unknown } | undefined;
    assert.ok(stored);
    assert.ok(Array.isArray(stored.embedding));
    assert.equal(stored.created_at, new Date(pointerMs).toISOString());
    assert.equal(kvStore.get(keyToString(byHashKey)), pointerMs);
    assert.equal(kvStore.get(keyToString(indexKey)), 1);
  } finally {
    failNextAtomicCommit = null;
    kvStore.delete(keyToString(byHashKey));
    kvStore.delete(keyToString(cacheKey));
    kvStore.delete(keyToString(indexKey));
  }
});

Deno.test("embeddings cache: eviction cleans stale duplicate index keys without deleting embeddings", async () => {
  resetVoyageRateLimit();
  const model = "text-embedding-3-small";
  const cacheModelKey = model.toLowerCase();
  const nowMs = 1_700_000_000_000;

  const hashA = await sha256Hex(`stale-index-${crypto.randomUUID()}`);
  const pointerMs = nowMs - 1_000;
  const staleMs = nowMs - 2_000;
  const cacheKeyA: Deno.KvKey = ["embeddings", "v1", cacheModelKey, hashA];
  const byHashKeyA: Deno.KvKey = ["embeddings", "v1", "cache_index_by_hash", cacheModelKey, hashA];
  const indexKeyStale: Deno.KvKey = ["embeddings", "v1", "cache_index", cacheModelKey, staleMs, hashA];
  const indexKeyActive: Deno.KvKey = ["embeddings", "v1", "cache_index", cacheModelKey, pointerMs, hashA];
  kvStore.set(keyToString(byHashKeyA), pointerMs);
  kvStore.set(keyToString(cacheKeyA), { embedding: [1, 2, 3], created_at: new Date(pointerMs).toISOString() });
  kvStore.set(keyToString(indexKeyStale), 1);
  kvStore.set(keyToString(indexKeyActive), 1);

  // Populate enough older entries so a quota-eviction batch cleans the stale index key
  // without touching the active index key for hashA.
  const oldKeyStrings: string[] = [];
  for (let i = 0; i < 511; i += 1) {
    const hashOld = `old_${i}_${crypto.randomUUID().replace(/-/g, "")}`;
    const createdAtMs = nowMs - 100_000 - i;
    const cacheKeyOld: Deno.KvKey = ["embeddings", "v1", cacheModelKey, hashOld];
    const byHashKeyOld: Deno.KvKey = ["embeddings", "v1", "cache_index_by_hash", cacheModelKey, hashOld];
    const indexKeyOld: Deno.KvKey = ["embeddings", "v1", "cache_index", cacheModelKey, createdAtMs, hashOld];
    kvStore.set(keyToString(cacheKeyOld), { embedding: [0, 0, 0], created_at: new Date(createdAtMs).toISOString() });
    kvStore.set(keyToString(byHashKeyOld), createdAtMs);
    kvStore.set(keyToString(indexKeyOld), 1);
    oldKeyStrings.push(keyToString(cacheKeyOld), keyToString(byHashKeyOld), keyToString(indexKeyOld));
  }

  const originalNow = Date.now;
  Date.now = () => nowMs;

  const inputB = `evict-${crypto.randomUUID()}`;
  const hashB = await sha256Hex(inputB);
  const cacheKeyB: Deno.KvKey = ["embeddings", "v1", cacheModelKey, hashB];
  const byHashKeyB: Deno.KvKey = ["embeddings", "v1", "cache_index_by_hash", cacheModelKey, hashB];
  const indexKeyB: Deno.KvKey = ["embeddings", "v1", "cache_index", cacheModelKey, nowMs, hashB];
  kvStore.delete(keyToString(cacheKeyB));
  kvStore.delete(keyToString(byHashKeyB));
  kvStore.delete(keyToString(indexKeyB));

  try {
    // Simulate KV storage quota failure on the first attempt to cache inputB.
    failNextAtomicCommit = (_checks, ops) => {
      const hitsCacheWrite = ops.some(
        (op) => op.type === "set" && keyToString(op.key) === keyToString(cacheKeyB),
      );
      return hitsCacheWrite ? new Error("KV quota exceeded") : false;
    };

    const response = await withFetchMock(
      (_url, bodyText) => {
        const body = JSON.parse(bodyText ?? "null") as { input?: unknown };
        const count = Array.isArray(body.input) ? body.input.length : 1;
        return voyageOkResponse(count);
      },
      () =>
        handleEmbeddings(
          new Request("https://ai.ubq.fi/v1/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model, input: inputB }),
          }),
        ),
    );
    assert.equal(response.status, 200);
    assert.equal(kvStore.get(keyToString(indexKeyStale)), undefined);
    assert.deepEqual(kvStore.get(keyToString(cacheKeyA)), {
      embedding: [1, 2, 3],
      created_at: new Date(pointerMs).toISOString(),
    });
    assert.equal(kvStore.get(keyToString(byHashKeyA)), pointerMs);
    assert.equal(kvStore.get(keyToString(indexKeyActive)), 1);
    assert.ok(kvStore.get(keyToString(cacheKeyB)));
    assert.equal(kvStore.get(keyToString(byHashKeyB)), nowMs);
    assert.equal(kvStore.get(keyToString(indexKeyB)), 1);
  } finally {
    Date.now = originalNow;
    failNextAtomicCommit = null;
    kvStore.delete(keyToString(byHashKeyA));
    kvStore.delete(keyToString(cacheKeyA));
    kvStore.delete(keyToString(indexKeyStale));
    kvStore.delete(keyToString(indexKeyActive));
    kvStore.delete(keyToString(cacheKeyB));
    kvStore.delete(keyToString(byHashKeyB));
    kvStore.delete(keyToString(indexKeyB));
    for (const key of oldKeyStrings) kvStore.delete(key);
  }
});

Deno.test("embeddings: returns one data item per array input", async () => {
  resetVoyageRateLimit();
  const response = await withFetchMock(
    (_url, bodyText) => {
      const body = JSON.parse(bodyText ?? "null") as { input?: unknown };
      const count = Array.isArray(body.input) ? body.input.length : 1;
      return voyageOkResponse(count);
    },
    () =>
      handleEmbeddings(
        new Request("https://ai.ubq.fi/v1/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "text-embedding-3-small", input: ["a", "b"] }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as { data?: Array<{ index?: number }> };
  assert.ok(Array.isArray(payload.data));
  assert.equal(payload.data.length, 2);
  assert.equal(payload.data[0]?.index, 0);
  assert.equal(payload.data[1]?.index, 1);
});

Deno.test("embeddings: rejects non-string array inputs", async () => {
  const response = await handleEmbeddings(
    new Request("https://ai.ubq.fi/v1/embeddings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ model: "text-embedding-3-small", input: ["a", 2] }),
    }),
  );
  assert.equal(response.status, 400);
  const payload = await response.json() as { error?: { param?: unknown } };
  assert.equal(payload.error?.param, "input");
});

Deno.test("embeddings: rejects too many inputs", async () => {
  const inputs = Array.from({ length: 129 }, (_, i) => `x${i}`);
  const response = await handleEmbeddings(
    new Request("https://ai.ubq.fi/v1/embeddings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ model: "text-embedding-3-small", input: inputs }),
    }),
  );
  assert.equal(response.status, 400);
});

Deno.test("embeddings: rejects too-large inputs", async () => {
  const tooLarge = "a".repeat(20_001);
  const response = await handleEmbeddings(
    new Request("https://ai.ubq.fi/v1/embeddings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ model: "text-embedding-3-small", input: tooLarge }),
    }),
  );
  assert.equal(response.status, 400);
});

Deno.test("embeddings: encoding_format=base64 returns base64 string embeddings", async () => {
  resetVoyageRateLimit();
  const response = await withFetchMock(
    () =>
      new Response(
        JSON.stringify({
          data: [{ embedding: [0.5, -0.5] }],
        }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      ),
    () =>
      handleEmbeddings(
        new Request("https://ai.ubq.fi/v1/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "text-embedding-3-small",
            input: "hello",
            encoding_format: "base64",
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as { data?: Array<{ embedding?: unknown }> };
  const emb = payload.data?.[0]?.embedding;
  assert.equal(typeof emb, "string");

  const raw = atob(emb as string);
  assert.equal(raw.length, 8);
  const bytes = Uint8Array.from(raw, (ch) => ch.charCodeAt(0));
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  assert.ok(Math.abs(view.getFloat32(0, true) - 0.5) < 1e-5);
  assert.ok(Math.abs(view.getFloat32(4, true) + 0.5) < 1e-5);
});

Deno.test("embeddings: 429 includes Retry-After when KV rate limited", async () => {
  const kv = await kvPromise;
  assert.ok(kv);
  await kv.set(VOYAGE_RATE_LIMIT_KEY, { window_start_ms: Date.now(), requests: 3, tokens: 0 });
  try {
    const response = await withFetchMock(
      () => {
        throw new Error("Embeddings should be rate limited before upstream fetch");
      },
      () =>
        handleEmbeddings(
          new Request("https://ai.ubq.fi/v1/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: "text-embedding-3-small", input: "rate-limit-test" }),
          }),
        ),
    );
    assert.equal(response.status, 429);
    const retryAfter = response.headers.get("Retry-After");
    assert.ok(retryAfter);
    const retryAfterSeconds = Number(retryAfter);
    assert.ok(Number.isFinite(retryAfterSeconds));
    assert.ok(retryAfterSeconds >= 1);
    assert.ok(retryAfterSeconds <= 60);
  } finally {
    await kv.delete(VOYAGE_RATE_LIMIT_KEY);
  }
});

Deno.test("embeddings jobs: create returns job + result when not rate limited", async () => {
  resetVoyageRateLimit();
  const input = `job-ok-${crypto.randomUUID()}`;
  const response = await withFetchMock(
    (_url, bodyText) => {
      const body = JSON.parse(bodyText ?? "null") as { input?: unknown };
      const count = Array.isArray(body.input) ? body.input.length : 1;
      return voyageOkResponse(count);
    },
    () =>
      handleEmbeddingsJobCreate(
        new Request("https://ai.ubq.fi/uos/embeddings/jobs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "text-embedding-3-small", input }),
        }),
        "test_token",
      ),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as {
    id?: unknown;
    object?: unknown;
    status?: unknown;
    result?: { object?: unknown; data?: unknown[]; model?: unknown };
  };
  assert.equal(payload.object, "embeddings.job");
  assert.equal(typeof payload.id, "string");
  assert.equal(payload.status, "succeeded");
  assert.equal(payload.result?.object, "list");
  assert.equal(payload.result?.model, "text-embedding-3-small");
  assert.ok(Array.isArray(payload.result?.data));
});

Deno.test("embeddings jobs: remain resolvable across token refresh when scoped to kernel repo", async () => {
  resetVoyageRateLimit();
  const usageContext = {
    keyId: null,
    kernelRepo: { owner: "ubiquity", repo: "ai.ubq.fi" },
    kernelOrg: { owner: "ubiquity" },
  };

  const input = `job-token-refresh-${crypto.randomUUID()}`;
  const created = await withFetchMock(
    (_url, bodyText) => {
      const body = JSON.parse(bodyText ?? "null") as { input?: unknown };
      const count = Array.isArray(body.input) ? body.input.length : 1;
      return voyageOkResponse(count);
    },
    () =>
      handleEmbeddingsJobCreate(
        new Request("https://ai.ubq.fi/uos/embeddings/jobs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "text-embedding-3-small", input }),
        }),
        "token_a",
        usageContext,
      ),
  );

  assert.equal(created.status, 200);
  const createdPayload = await created.json() as { id?: unknown; status?: unknown };
  assert.equal(createdPayload.status, "succeeded");
  assert.equal(typeof createdPayload.id, "string");
  const jobId = createdPayload.id as string;

  const got = await withFetchMock(
    () => {
      throw new Error("Embeddings job get should not hit upstream when already succeeded");
    },
    () =>
      handleEmbeddingsJobGet(
        new Request(`https://ai.ubq.fi/uos/embeddings/jobs/${jobId}`),
        "token_b",
        jobId,
        usageContext,
      ),
  );

  assert.equal(got.status, 200);
  const gotPayload = await got.json() as { id?: unknown; status?: unknown };
  assert.equal(gotPayload.id, jobId);
  assert.equal(gotPayload.status, "succeeded");
});

Deno.test("embeddings jobs: create queues with 202 + Retry-After when KV rate limited", async () => {
  const kv = await kvPromise;
  assert.ok(kv);
  const rateKey: Deno.KvKey = ["embeddings", "v1", "rate", "voyage"];
  await kv.set(rateKey, { window_start_ms: Date.now(), requests: 3, tokens: 0 });
  try {
    const input = `job-queued-${crypto.randomUUID()}`;
    const response = await withFetchMock(
      () => {
        throw new Error("Embeddings job should be queued before upstream fetch");
      },
      () =>
        handleEmbeddingsJobCreate(
          new Request("https://ai.ubq.fi/uos/embeddings/jobs", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: "text-embedding-3-small", input }),
          }),
          "test_token",
        ),
    );

    assert.equal(response.status, 202);
    const retryAfter = response.headers.get("Retry-After");
    assert.ok(retryAfter);
    const retryAfterSeconds = Number(retryAfter);
    assert.ok(Number.isFinite(retryAfterSeconds));
    assert.ok(retryAfterSeconds >= 1);
    assert.ok(retryAfterSeconds <= 60);

    const payload = await response.json() as { status?: unknown; id?: unknown };
    assert.equal(payload.status, "queued");
    assert.equal(typeof payload.id, "string");
  } finally {
    await kv.delete(rateKey);
  }
});

Deno.test("embeddings jobs: poll runs queued job to completion", async () => {
  const kv = await kvPromise;
  assert.ok(kv);
  const rateKey: Deno.KvKey = ["embeddings", "v1", "rate", "voyage"];
  await kv.set(rateKey, { window_start_ms: Date.now(), requests: 3, tokens: 0 });

  const input = `job-poll-${crypto.randomUUID()}`;
  const created = await withFetchMock(
    () => {
      throw new Error("Embeddings job should be queued before upstream fetch");
    },
    () =>
      handleEmbeddingsJobCreate(
        new Request("https://ai.ubq.fi/uos/embeddings/jobs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "text-embedding-3-small", input }),
        }),
        "test_token",
      ),
  );
  assert.equal(created.status, 202);
  const createdPayload = await created.json() as { id?: unknown };
  assert.equal(typeof createdPayload.id, "string");
  const jobId = createdPayload.id as string;

  await kv.delete(rateKey);

  const polled = await withFetchMock(
    (_url, bodyText) => {
      const body = JSON.parse(bodyText ?? "null") as { input?: unknown };
      const count = Array.isArray(body.input) ? body.input.length : 1;
      return voyageOkResponse(count);
    },
    () => handleEmbeddingsJobGet(new Request(`https://ai.ubq.fi/uos/embeddings/jobs/${jobId}`), "test_token", jobId),
  );

  assert.equal(polled.status, 200);
  const payload = await polled.json() as { status?: unknown; result?: { data?: unknown[] } };
  assert.equal(payload.status, "succeeded");
  assert.ok(Array.isArray(payload.result?.data));
});

addEventListener("unload", () => {
  (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = originalOpenKv;
  if (originalVoyageApiKey === undefined) Deno.env.delete("VOYAGEAI_API_KEY");
  else Deno.env.set("VOYAGEAI_API_KEY", originalVoyageApiKey);
});
