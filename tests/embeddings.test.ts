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
const kvExpirations = new Map<string, number | undefined>();
const VOYAGE_RATE_LIMIT_KEY: Deno.KvKey = ["embeddings", "v1", "rate", "voyage"];
const EMBEDDINGS_JOB_TTL_MS = 24 * 60 * 60_000;
const resetVoyageRateLimit = () => void kvStore.delete(keyToString(VOYAGE_RATE_LIMIT_KEY));
type TestInputType = "query" | "document";
type TestDimension = 256 | 512 | 1024 | 2048;
type TestEncodingFormat = "float" | "base64";

const embeddingsProfileKey = (
  inputType: TestInputType = "document",
  dimensions: TestDimension = 1024,
  encodingFormat: TestEncodingFormat = "float",
  truncation = true,
): string =>
  JSON.stringify([
    "voyage-profile-v2",
    "voyage-4-large",
    inputType,
    dimensions,
    "float",
    encodingFormat,
    truncation,
  ]);

const embeddingsCacheKey = (
  hash: string,
  inputType: TestInputType = "document",
  dimensions: TestDimension = 1024,
  encodingFormat: TestEncodingFormat = "float",
  truncation = true,
): Deno.KvKey => [
  "embeddings",
  "v2",
  "cache",
  embeddingsProfileKey(inputType, dimensions, encodingFormat, truncation),
  hash,
];

const embeddingsCacheGlobalIndexKey = (
  createdAtMs: number,
  cacheProfileKey: string,
  hash: string,
): Deno.KvKey => ["embeddings", "v2", "cache_index_global", createdAtMs, cacheProfileKey, hash];

const embeddingsJobKey = (
  tokenHash: string,
  cacheProfileKey: string,
  jobId: string,
): Deno.KvKey => ["embeddings", "jobs", "v2", tokenHash, cacheProfileKey, jobId];

const embeddingsJobLookupKey = (tokenHash: string, jobId: string): Deno.KvKey => [
  "embeddings",
  "jobs",
  "v2",
  "lookup",
  tokenHash,
  jobId,
];

const testVector = (dimensions: TestDimension, seed = 0): number[] =>
  Array.from({ length: dimensions }, (_, index) => seed + index / Math.max(1, dimensions));
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
  set: (key: Deno.KvKey, value: unknown, options?: { expireIn?: number }) => {
    kvStore.set(keyToString(key), value);
    kvExpirations.set(keyToString(key), options?.expireIn);
    return Promise.resolve({ ok: true } as const);
  },
  delete: (key: Deno.KvKey) => {
    kvStore.delete(keyToString(key));
    kvExpirations.delete(keyToString(key));
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
    const ops: Array<{ type: "set" | "delete"; key: Deno.KvKey; value?: unknown; expireIn?: number }> = [];
    const chain = {
      check: (entry: Deno.KvEntryMaybe<unknown>) => {
        checks.push(entry);
        return chain;
      },
      set: (key: Deno.KvKey, value: unknown, options?: { expireIn?: number }) => {
        ops.push({ type: "set", key, value, expireIn: options?.expireIn });
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
          if (op.type === "set") {
            kvStore.set(keyToString(op.key), op.value);
            kvExpirations.set(keyToString(op.key), op.expireIn);
          } else {
            kvStore.delete(keyToString(op.key));
            kvExpirations.delete(keyToString(op.key));
          }
        }
        return Promise.resolve({ ok: true } as const);
      },
    };
    return chain;
  },
  close: () => {},
} as unknown as Deno.Kv;

(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kvStub);

const { handleEmbeddings, handleEmbeddingsJobCreate, handleEmbeddingsJobGet, handleUosEmbeddings } = await import(
  "../src/openai.ts"
);
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

const voyageOkResponse = (count: number, dimensions: TestDimension = 1024): Response => {
  const vectors = Array.from({ length: count }, (_, i) => ({
    embedding: testVector(dimensions, i + 0.1),
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
      const body = JSON.parse(bodyText ?? "null") as Record<string, unknown>;
      assert.deepEqual(body, {
        model: "voyage-4-large",
        input: "hello",
        input_type: "document",
        output_dimension: 1024,
        output_dtype: "float",
        truncation: true,
      });
      assert.equal("encoding_format" in body, false);
      assert.equal("output_encoding" in body, false);
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
  assert.equal((payload.data[0]?.embedding as unknown[]).length, 1024);
  assert.equal(response.headers.get("x-ubq-upstream"), "voyage");
});

Deno.test("uos embeddings: forwards synchronous query and document profiles", async () => {
  const cases = [
    {
      inputType: "query" as const,
      dimensions: 256 as const,
      truncation: false,
    },
    {
      inputType: "document" as const,
      dimensions: 2048 as const,
      truncation: true,
    },
  ];
  const seenBodies: Record<string, unknown>[] = [];

  await withFetchMock(
    (_url, bodyText) => {
      const body = JSON.parse(bodyText ?? "null") as Record<string, unknown>;
      seenBodies.push(body);
      return voyageOkResponse(1, body.output_dimension as TestDimension);
    },
    async () => {
      for (const item of cases) {
        resetVoyageRateLimit();
        const response = await handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "voyage-4-large",
              input: `${item.inputType}-${crypto.randomUUID()}`,
              input_type: item.inputType,
              dimensions: item.dimensions,
              truncation: item.truncation,
            }),
          }),
        );
        assert.equal(response.status, 200);
        assert.equal(response.headers.get("x-ubq-upstream"), "voyage");
        const payload = await response.json() as { data?: Array<{ embedding?: unknown }> };
        assert.equal((payload.data?.[0]?.embedding as unknown[]).length, item.dimensions);
      }
    },
  );

  assert.equal(seenBodies.length, 2);
  for (let index = 0; index < cases.length; index += 1) {
    const expected = cases[index]!;
    const body = seenBodies[index]!;
    assert.equal(body.model, "voyage-4-large");
    assert.equal(body.input_type, expected.inputType);
    assert.equal(body.output_dimension, expected.dimensions);
    assert.equal(body.output_dtype, "float");
    assert.equal(body.truncation, expected.truncation);
    assert.equal("encoding_format" in body, false);
    assert.equal("output_encoding" in body, false);
  }
});

Deno.test("v1 embeddings: accepts every supported standard dimension and preserves requested model", async () => {
  const dimensions: TestDimension[] = [256, 512, 1024, 2048];

  await withFetchMock(
    (_url, bodyText) => {
      const body = JSON.parse(bodyText ?? "null") as Record<string, unknown>;
      assert.equal(body.model, "voyage-4-large");
      assert.equal(body.input_type, "document");
      assert.equal(body.output_dtype, "float");
      assert.equal(body.truncation, true);
      assert.equal("encoding_format" in body, false);
      assert.equal("output_encoding" in body, false);
      return voyageOkResponse(1, body.output_dimension as TestDimension);
    },
    async () => {
      for (const dimension of dimensions) {
        resetVoyageRateLimit();
        const requestedModel = dimension === 2048 ? "voyage-4-large" : "text-embedding-3-large";
        const response = await handleEmbeddings(
          new Request("https://ai.ubq.fi/v1/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: requestedModel,
              input: `v1-dim-${dimension}-${crypto.randomUUID()}`,
              dimensions: dimension,
            }),
          }),
        );
        assert.equal(response.status, 200);
        const payload = await response.json() as {
          model?: unknown;
          data?: Array<{ embedding?: unknown }>;
        };
        assert.equal(payload.model, requestedModel);
        assert.equal((payload.data?.[0]?.embedding as unknown[]).length, dimension);
      }
    },
  );
});

Deno.test("embedding contracts: reject cross-contract and unsupported options", async () => {
  const requests: Array<() => Promise<Response>> = [
    () =>
      handleEmbeddings(
        new Request("https://ai.ubq.fi/v1/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "text-embedding-3-small",
            input: "x",
            input_type: "query",
          }),
        }),
      ),
    () =>
      handleEmbeddings(
        new Request("https://ai.ubq.fi/v1/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-3-large", input: "x" }),
        }),
      ),
    () =>
      handleEmbeddings(
        new Request("https://ai.ubq.fi/v1/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "text-embedding-3-small", input: "x", dimensions: 768 }),
        }),
      ),
    () =>
      handleEmbeddings(
        new Request("https://ai.ubq.fi/v1/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "text-embedding-3-small", input: "x", dimensions: 256.5 }),
        }),
      ),
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-4-large", input: "x" }),
        }),
      ),
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "text-embedding-3-small",
            input: "x",
            input_type: "document",
          }),
        }),
      ),
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "voyage-4-large",
            input: "x",
            input_type: "document",
            encoding_format: "base64",
          }),
        }),
      ),
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "voyage-4-large",
            input: "x",
            input_type: "document",
            user: "not-allowed",
          }),
        }),
      ),
  ];

  for (const makeRequest of requests) {
    const response = await makeRequest();
    assert.equal(response.status, 400);
    assert.equal(response.headers.get("x-ubq-upstream"), "voyage");
  }
});

Deno.test("embeddings: serves cache hits without calling upstream", async () => {
  resetVoyageRateLimit();
  const model = "text-embedding-3-small";
  const input = `cache-hit-${crypto.randomUUID()}`;
  const hash = await sha256Hex(input);
  const cacheKey = embeddingsCacheKey(hash);
  const cachedEmbedding = testVector(1024, 9.9);
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

Deno.test("embeddings cache: separates query, document, dimensions, encoding, and truncation profiles", async () => {
  const input = `profile-separation-${crypto.randomUUID()}`;
  const hash = await sha256Hex(input);
  const oldIncompleteKey: Deno.KvKey = ["embeddings", "v1", "text-embedding-3-small", hash];
  kvStore.set(keyToString(oldIncompleteKey), {
    embedding: testVector(256, 99),
    created_at: new Date().toISOString(),
  });

  const requests = [
    {
      key: embeddingsCacheKey(hash, "query", 256, "float", true),
      run: () =>
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "voyage-4-large",
              input,
              input_type: "query",
              dimensions: 256,
            }),
          }),
        ),
    },
    {
      key: embeddingsCacheKey(hash, "document", 256, "float", true),
      run: () =>
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "voyage-4-large",
              input,
              input_type: "document",
              dimensions: 256,
            }),
          }),
        ),
    },
    {
      key: embeddingsCacheKey(hash, "query", 512, "float", true),
      run: () =>
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "voyage-4-large",
              input,
              input_type: "query",
              dimensions: 512,
            }),
          }),
        ),
    },
    {
      key: embeddingsCacheKey(hash, "query", 256, "float", false),
      run: () =>
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "voyage-4-large",
              input,
              input_type: "query",
              dimensions: 256,
              truncation: false,
            }),
          }),
        ),
    },
    {
      key: embeddingsCacheKey(hash, "document", 256, "base64", true),
      run: () =>
        handleEmbeddings(
          new Request("https://ai.ubq.fi/v1/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "text-embedding-3-small",
              input,
              dimensions: 256,
              encoding_format: "base64",
            }),
          }),
        ),
    },
  ];

  let upstreamCalls = 0;
  try {
    await withFetchMock(
      (_url, bodyText) => {
        upstreamCalls += 1;
        const body = JSON.parse(bodyText ?? "null") as Record<string, unknown>;
        return voyageOkResponse(1, body.output_dimension as TestDimension);
      },
      async () => {
        for (const item of requests) {
          resetVoyageRateLimit();
          assert.equal((await item.run()).status, 200);
        }
        assert.equal(upstreamCalls, requests.length);

        for (const item of requests) {
          resetVoyageRateLimit();
          assert.equal((await item.run()).status, 200);
        }
      },
    );

    assert.equal(upstreamCalls, requests.length);
    for (const item of requests) {
      assert.ok(kvStore.get(keyToString(item.key)));
    }
  } finally {
    kvStore.delete(keyToString(oldIncompleteKey));
    for (const item of requests) kvStore.delete(keyToString(item.key));
  }
});

Deno.test("embeddings cache: ignores a cached vector with the wrong resolved dimension", async () => {
  resetVoyageRateLimit();
  const input = `wrong-cache-length-${crypto.randomUUID()}`;
  const hash = await sha256Hex(input);
  const cacheKey = embeddingsCacheKey(hash, "document", 512);
  kvStore.set(keyToString(cacheKey), {
    embedding: testVector(256),
    created_at: new Date().toISOString(),
  });

  let upstreamCalls = 0;
  try {
    const response = await withFetchMock(
      (_url, bodyText) => {
        upstreamCalls += 1;
        const body = JSON.parse(bodyText ?? "null") as Record<string, unknown>;
        return voyageOkResponse(1, body.output_dimension as TestDimension);
      },
      () =>
        handleEmbeddings(
          new Request("https://ai.ubq.fi/v1/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "text-embedding-3-small",
              input,
              dimensions: 512,
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.equal(upstreamCalls, 1);
    const payload = await response.json() as { data?: Array<{ embedding?: unknown }> };
    assert.equal((payload.data?.[0]?.embedding as unknown[]).length, 512);
  } finally {
    kvStore.delete(keyToString(cacheKey));
  }
});

Deno.test("embeddings: writes cache entries on upstream misses", async () => {
  resetVoyageRateLimit();
  const model = "text-embedding-3-small";
  const input = `cache-miss-${crypto.randomUUID()}`;
  const hash = await sha256Hex(input);
  const cacheProfileKey = embeddingsProfileKey();
  const cacheKey = embeddingsCacheKey(hash);
  const byHashKey: Deno.KvKey = ["embeddings", "v2", "cache_index_by_hash", cacheProfileKey, hash];
  const fixedNowMs = 1_700_000_000_000;
  const indexKey: Deno.KvKey = ["embeddings", "v2", "cache_index", cacheProfileKey, fixedNowMs, hash];
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
  const cacheProfileKey = embeddingsProfileKey();
  const input = `cache-atomic-fail-${crypto.randomUUID()}`;
  const hash = await sha256Hex(input);
  const cacheKey = embeddingsCacheKey(hash);
  const pointerMs = 1_700_000_000_000;
  const byHashKey: Deno.KvKey = ["embeddings", "v2", "cache_index_by_hash", cacheProfileKey, hash];
  const indexKey: Deno.KvKey = ["embeddings", "v2", "cache_index", cacheProfileKey, pointerMs, hash];
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
  const cacheProfileKey = embeddingsProfileKey();
  const nowMs = 1_700_000_000_000;

  const hashA = await sha256Hex(`stale-index-${crypto.randomUUID()}`);
  const pointerMs = nowMs - 1_000;
  const staleMs = nowMs - 2_000;
  const cacheKeyA = embeddingsCacheKey(hashA);
  const byHashKeyA: Deno.KvKey = ["embeddings", "v2", "cache_index_by_hash", cacheProfileKey, hashA];
  const indexKeyStale: Deno.KvKey = ["embeddings", "v2", "cache_index", cacheProfileKey, staleMs, hashA];
  const indexKeyActive: Deno.KvKey = ["embeddings", "v2", "cache_index", cacheProfileKey, pointerMs, hashA];
  const globalIndexKeyStale = embeddingsCacheGlobalIndexKey(staleMs, cacheProfileKey, hashA);
  const globalIndexKeyActive = embeddingsCacheGlobalIndexKey(pointerMs, cacheProfileKey, hashA);
  kvStore.set(keyToString(byHashKeyA), pointerMs);
  const activeEmbedding = testVector(1024, 1);
  kvStore.set(keyToString(cacheKeyA), {
    embedding: activeEmbedding,
    created_at: new Date(pointerMs).toISOString(),
  });
  kvStore.set(keyToString(indexKeyStale), 1);
  kvStore.set(keyToString(indexKeyActive), 1);
  kvStore.set(keyToString(globalIndexKeyStale), 1);
  kvStore.set(keyToString(globalIndexKeyActive), 1);

  // Populate enough older entries so a quota-eviction batch cleans the stale index key
  // without touching the active index key for hashA.
  const oldKeyStrings: string[] = [];
  for (let i = 0; i < 511; i += 1) {
    const hashOld = `old_${i}_${crypto.randomUUID().replace(/-/g, "")}`;
    const createdAtMs = nowMs - 100_000 - i;
    const cacheKeyOld = embeddingsCacheKey(hashOld);
    const byHashKeyOld: Deno.KvKey = ["embeddings", "v2", "cache_index_by_hash", cacheProfileKey, hashOld];
    const indexKeyOld: Deno.KvKey = ["embeddings", "v2", "cache_index", cacheProfileKey, createdAtMs, hashOld];
    const globalIndexKeyOld = embeddingsCacheGlobalIndexKey(createdAtMs, cacheProfileKey, hashOld);
    kvStore.set(keyToString(cacheKeyOld), {
      embedding: testVector(1024),
      created_at: new Date(createdAtMs).toISOString(),
    });
    kvStore.set(keyToString(byHashKeyOld), createdAtMs);
    kvStore.set(keyToString(indexKeyOld), 1);
    kvStore.set(keyToString(globalIndexKeyOld), 1);
    oldKeyStrings.push(
      keyToString(cacheKeyOld),
      keyToString(byHashKeyOld),
      keyToString(indexKeyOld),
      keyToString(globalIndexKeyOld),
    );
  }

  const originalNow = Date.now;
  Date.now = () => nowMs;

  const inputB = `evict-${crypto.randomUUID()}`;
  const hashB = await sha256Hex(inputB);
  const cacheKeyB = embeddingsCacheKey(hashB);
  const byHashKeyB: Deno.KvKey = ["embeddings", "v2", "cache_index_by_hash", cacheProfileKey, hashB];
  const indexKeyB: Deno.KvKey = ["embeddings", "v2", "cache_index", cacheProfileKey, nowMs, hashB];
  const globalIndexKeyB = embeddingsCacheGlobalIndexKey(nowMs, cacheProfileKey, hashB);
  kvStore.delete(keyToString(cacheKeyB));
  kvStore.delete(keyToString(byHashKeyB));
  kvStore.delete(keyToString(indexKeyB));
  kvStore.delete(keyToString(globalIndexKeyB));

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
    assert.equal(kvStore.get(keyToString(globalIndexKeyStale)), undefined);
    assert.deepEqual(kvStore.get(keyToString(cacheKeyA)), {
      embedding: activeEmbedding,
      created_at: new Date(pointerMs).toISOString(),
    });
    assert.equal(kvStore.get(keyToString(byHashKeyA)), pointerMs);
    assert.equal(kvStore.get(keyToString(indexKeyActive)), 1);
    assert.equal(kvStore.get(keyToString(globalIndexKeyActive)), 1);
    assert.ok(kvStore.get(keyToString(cacheKeyB)));
    assert.equal(kvStore.get(keyToString(byHashKeyB)), nowMs);
    assert.equal(kvStore.get(keyToString(indexKeyB)), 1);
    assert.equal(kvStore.get(keyToString(globalIndexKeyB)), 1);
  } finally {
    Date.now = originalNow;
    failNextAtomicCommit = null;
    kvStore.delete(keyToString(byHashKeyA));
    kvStore.delete(keyToString(cacheKeyA));
    kvStore.delete(keyToString(indexKeyStale));
    kvStore.delete(keyToString(indexKeyActive));
    kvStore.delete(keyToString(globalIndexKeyStale));
    kvStore.delete(keyToString(globalIndexKeyActive));
    kvStore.delete(keyToString(cacheKeyB));
    kvStore.delete(keyToString(byHashKeyB));
    kvStore.delete(keyToString(indexKeyB));
    kvStore.delete(keyToString(globalIndexKeyB));
    for (const key of oldKeyStrings) kvStore.delete(key);
  }
});

Deno.test("embeddings cache: quota eviction frees entries owned by another profile", async () => {
  resetVoyageRateLimit();
  const oldProfileKey = embeddingsProfileKey("document", 1024, "float", false);
  const newProfileKey = embeddingsProfileKey("query", 256, "float", false);
  const oldCreatedAtMs = 1;
  const newCreatedAtMs = 2;

  const oldHash = await sha256Hex(`old-document-profile-${crypto.randomUUID()}`);
  const oldCacheKey = embeddingsCacheKey(oldHash, "document", 1024, "float", false);
  const oldByHashKey: Deno.KvKey = ["embeddings", "v2", "cache_index_by_hash", oldProfileKey, oldHash];
  const oldProfileIndexKey: Deno.KvKey = [
    "embeddings",
    "v2",
    "cache_index",
    oldProfileKey,
    oldCreatedAtMs,
    oldHash,
  ];
  const oldGlobalIndexKey = embeddingsCacheGlobalIndexKey(oldCreatedAtMs, oldProfileKey, oldHash);
  kvStore.set(keyToString(oldCacheKey), {
    embedding: testVector(1024),
    created_at: new Date(oldCreatedAtMs).toISOString(),
  });
  kvStore.set(keyToString(oldByHashKey), oldCreatedAtMs);
  kvStore.set(keyToString(oldProfileIndexKey), 1);
  kvStore.set(keyToString(oldGlobalIndexKey), 1);

  const input = `new-query-profile-${crypto.randomUUID()}`;
  const newHash = await sha256Hex(input);
  const newCacheKey = embeddingsCacheKey(newHash, "query", 256, "float", false);
  const newByHashKey: Deno.KvKey = ["embeddings", "v2", "cache_index_by_hash", newProfileKey, newHash];
  const newProfileIndexKey: Deno.KvKey = [
    "embeddings",
    "v2",
    "cache_index",
    newProfileKey,
    newCreatedAtMs,
    newHash,
  ];
  const newGlobalIndexKey = embeddingsCacheGlobalIndexKey(newCreatedAtMs, newProfileKey, newHash);
  const originalNow = Date.now;
  Date.now = () => newCreatedAtMs;

  try {
    failNextAtomicCommit = (_checks, ops) => {
      const hitsNewProfileWrite = ops.some(
        (op) => op.type === "set" && keyToString(op.key) === keyToString(newCacheKey),
      );
      return hitsNewProfileWrite ? new Error("KV quota exceeded") : false;
    };

    const response = await withFetchMock(
      () => voyageOkResponse(1, 256),
      () =>
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "voyage-4-large",
              input,
              input_type: "query",
              dimensions: 256,
              truncation: false,
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.equal(kvStore.get(keyToString(oldCacheKey)), undefined);
    assert.equal(kvStore.get(keyToString(oldByHashKey)), undefined);
    assert.equal(kvStore.get(keyToString(oldProfileIndexKey)), undefined);
    assert.equal(kvStore.get(keyToString(oldGlobalIndexKey)), undefined);
    assert.ok(kvStore.get(keyToString(newCacheKey)));
    assert.equal(kvStore.get(keyToString(newByHashKey)), newCreatedAtMs);
    assert.equal(kvStore.get(keyToString(newProfileIndexKey)), 1);
    assert.equal(kvStore.get(keyToString(newGlobalIndexKey)), 1);
  } finally {
    Date.now = originalNow;
    failNextAtomicCommit = null;
    for (
      const key of [
        oldCacheKey,
        oldByHashKey,
        oldProfileIndexKey,
        oldGlobalIndexKey,
        newCacheKey,
        newByHashKey,
        newProfileIndexKey,
        newGlobalIndexKey,
      ]
    ) {
      kvStore.delete(keyToString(key));
    }
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
    (_url, bodyText) => {
      const body = JSON.parse(bodyText ?? "null") as Record<string, unknown>;
      assert.equal(body.output_dimension, 256);
      assert.equal("encoding_format" in body, false);
      assert.equal("output_encoding" in body, false);
      const embedding = testVector(256);
      embedding[0] = 0.5;
      embedding[1] = -0.5;
      return (
        new Response(
          JSON.stringify({
            data: [{ embedding }],
          }),
          { status: 200, headers: { "Content-Type": "application/json" } },
        )
      );
    },
    () =>
      handleEmbeddings(
        new Request("https://ai.ubq.fi/v1/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "text-embedding-3-small",
            input: "hello",
            dimensions: 256,
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
  assert.equal(raw.length, 256 * 4);
  const bytes = Uint8Array.from(raw, (ch) => ch.charCodeAt(0));
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  assert.ok(Math.abs(view.getFloat32(0, true) - 0.5) < 1e-5);
  assert.ok(Math.abs(view.getFloat32(4, true) + 0.5) < 1e-5);
});

Deno.test("v1 embeddings: rejects fractional dimensions", async () => {
  const response = await handleEmbeddings(
    new Request("https://ai.ubq.fi/v1/embeddings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: "text-embedding-3-small",
        input: "fractional-dimensions",
        dimensions: 256.5,
      }),
    }),
  );

  assert.equal(response.status, 400);
  const payload = await response.json() as { error?: { message?: unknown; param?: unknown } };
  assert.match(String(payload.error?.message), /integer/);
  assert.equal(payload.error?.param, "dimensions");
});

Deno.test("uos embeddings: returns 502 when upstream vector length does not match the resolved dimension", async () => {
  resetVoyageRateLimit();
  const response = await withFetchMock(
    () =>
      new Response(
        JSON.stringify({
          data: [{ embedding: Array.from({ length: 255 }, (_, index) => index / 255) }],
        }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      ),
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "voyage-4-large",
            input: `wrong-sync-length-${crypto.randomUUID()}`,
            input_type: "query",
            dimensions: 256,
          }),
        }),
      ),
  );

  assert.equal(response.status, 502);
  assert.equal(response.headers.get("x-ubq-upstream"), "voyage");
  const payload = await response.json() as { error?: { code?: unknown; message?: unknown } };
  assert.equal(payload.error?.code, "upstream_dimension_mismatch");
  assert.match(String(payload.error?.message), /length 255; expected 256/);
});

Deno.test("uos embeddings: sync retry reuses the exact resolved Voyage options", async () => {
  resetVoyageRateLimit();
  const bodies: Record<string, unknown>[] = [];
  const input = `sync-retry-options-${crypto.randomUUID()}`;

  const response = await withFetchMock(
    (_url, bodyText) => {
      const body = JSON.parse(bodyText ?? "null") as Record<string, unknown>;
      bodies.push(body);
      if (bodies.length === 1) {
        return new Response(JSON.stringify({ error: "rate limited" }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": "0.001" },
        });
      }
      return voyageOkResponse(1, 512);
    },
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "voyage-4-large",
            input,
            input_type: "query",
            dimensions: 512,
            truncation: false,
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.equal(bodies.length, 2);
  assert.deepEqual(bodies[1], bodies[0]);
  assert.deepEqual(bodies[0], {
    model: "voyage-4-large",
    input,
    input_type: "query",
    output_dimension: 512,
    output_dtype: "float",
    truncation: false,
  });
});

Deno.test("uos embeddings: exhausted upstream 429 preserves status and Retry-After", async () => {
  resetVoyageRateLimit();
  const bodies: Record<string, unknown>[] = [];
  const input = `sync-exhausted-429-${crypto.randomUUID()}`;

  const response = await withFetchMock(
    (_url, bodyText) => {
      bodies.push(JSON.parse(bodyText ?? "null") as Record<string, unknown>);
      return new Response(JSON.stringify({ error: "rate limited" }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "0.001" },
      });
    },
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "voyage-4-large",
            input,
            input_type: "query",
            dimensions: 512,
            truncation: false,
          }),
        }),
      ),
  );

  assert.equal(bodies.length, 3);
  assert.ok(bodies.every((body) => JSON.stringify(body) === JSON.stringify(bodies[0])));
  assert.equal(response.status, 429);
  assert.equal(response.headers.get("Retry-After"), "1");
  assert.equal(response.headers.get("x-ubq-upstream"), "voyage");
  const payload = await response.json() as { error?: { type?: unknown; code?: unknown } };
  assert.equal(payload.error?.type, "rate_limit_error");
  assert.equal(payload.error?.code, "rate_limit_exceeded");
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

Deno.test("embedding jobs: create returns job + result when not rate limited", async () => {
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
        new Request("https://ai.ubq.fi/uos/embedding-jobs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-4-large", input, input_type: "document" }),
        }),
        "test_token",
      ),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as {
    id?: unknown;
    object?: unknown;
    status?: unknown;
    upstream_model?: unknown;
    input_type?: unknown;
    dimensions?: unknown;
    output_dtype?: unknown;
    encoding_format?: unknown;
    truncation?: unknown;
    result?: { object?: unknown; data?: unknown[]; model?: unknown };
  };
  assert.equal(payload.object, "embeddings.job");
  assert.equal(typeof payload.id, "string");
  assert.equal(payload.status, "succeeded");
  assert.equal(payload.upstream_model, "voyage-4-large");
  assert.equal(payload.input_type, "document");
  assert.equal(payload.dimensions, 1024);
  assert.equal(payload.output_dtype, "float");
  assert.equal(payload.encoding_format, "float");
  assert.equal(payload.truncation, true);
  assert.equal(payload.result?.object, "list");
  assert.equal(payload.result?.model, "voyage-4-large");
  assert.ok(Array.isArray(payload.result?.data));
});

Deno.test("embedding jobs: wrong-length upstream vector is a terminal failed job", async () => {
  resetVoyageRateLimit();
  const authToken = `wrong-length-token-${crypto.randomUUID()}`;
  const created = await withFetchMock(
    () =>
      new Response(
        JSON.stringify({
          data: [{ embedding: Array.from({ length: 511 }, (_, index) => index / 511) }],
        }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      ),
    () =>
      handleEmbeddingsJobCreate(
        new Request("https://ai.ubq.fi/uos/embedding-jobs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "voyage-4-large",
            input: `wrong-job-length-${crypto.randomUUID()}`,
            input_type: "document",
            dimensions: 512,
          }),
        }),
        authToken,
      ),
  );

  assert.equal(created.status, 200);
  assert.equal(created.headers.get("x-ubq-upstream"), "voyage");
  const payload = await created.json() as {
    id?: unknown;
    status?: unknown;
    error?: { code?: unknown; message?: unknown };
  };
  assert.equal(payload.status, "failed");
  assert.equal(payload.error?.code, "embeddings_job_upstream_dimension_mismatch");
  assert.match(String(payload.error?.message), /length 511; expected 512/);
  assert.equal(typeof payload.id, "string");

  const jobId = payload.id as string;
  const polled = await withFetchMock(
    () => {
      throw new Error("A terminally failed job must not retry upstream");
    },
    () =>
      handleEmbeddingsJobGet(
        new Request(`https://ai.ubq.fi/uos/embedding-jobs/${jobId}`),
        authToken,
        jobId,
      ),
  );
  assert.equal(polled.status, 200);
  assert.equal(polled.headers.get("x-ubq-upstream"), "voyage");
  const polledPayload = await polled.json() as { status?: unknown; error?: { code?: unknown } };
  assert.equal(polledPayload.status, "failed");
  assert.equal(polledPayload.error?.code, "embeddings_job_upstream_dimension_mismatch");
});

Deno.test("embedding jobs: remain resolvable across token refresh when scoped to kernel repo", async () => {
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
        new Request("https://ai.ubq.fi/uos/embedding-jobs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-4-large", input, input_type: "document" }),
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
        new Request(`https://ai.ubq.fi/uos/embedding-jobs/${jobId}`),
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

Deno.test("embedding jobs: create queues with 202 + Retry-After when KV rate limited", async () => {
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
          new Request("https://ai.ubq.fi/uos/embedding-jobs", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: "voyage-4-large", input, input_type: "query" }),
          }),
          "test_token",
        ),
    );

    assert.equal(response.status, 202);
    assert.equal(response.headers.get("x-ubq-upstream"), "voyage");
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

Deno.test("embedding jobs: queued query and document profiles persist through poll", async () => {
  const kv = await kvPromise;
  assert.ok(kv);
  await kv.set(VOYAGE_RATE_LIMIT_KEY, { window_start_ms: Date.now(), requests: 3, tokens: 0 });
  const authToken = `queued-profiles-${crypto.randomUUID()}`;
  const cases = [
    {
      input: `queued-query-${crypto.randomUUID()}`,
      inputType: "query" as const,
      dimensions: 256 as const,
      truncation: false,
    },
    {
      input: `queued-document-${crypto.randomUUID()}`,
      inputType: "document" as const,
      dimensions: 2048 as const,
      truncation: true,
    },
  ];
  const jobIds: string[] = [];

  try {
    for (const item of cases) {
      const created = await withFetchMock(
        () => {
          throw new Error("The saturated gateway limit must queue before upstream");
        },
        () =>
          handleEmbeddingsJobCreate(
            new Request("https://ai.ubq.fi/uos/embedding-jobs", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: "voyage-4-large",
                input: item.input,
                input_type: item.inputType,
                dimensions: item.dimensions,
                truncation: item.truncation,
              }),
            }),
            authToken,
          ),
      );

      assert.equal(created.status, 202);
      assert.equal(created.headers.get("x-ubq-upstream"), "voyage");
      const body = await created.json() as {
        id?: unknown;
        status?: unknown;
        upstream_model?: unknown;
        input_type?: unknown;
        dimensions?: unknown;
        output_dtype?: unknown;
        encoding_format?: unknown;
        truncation?: unknown;
      };
      assert.equal(body.status, "queued");
      assert.equal(body.upstream_model, "voyage-4-large");
      assert.equal(body.input_type, item.inputType);
      assert.equal(body.dimensions, item.dimensions);
      assert.equal(body.output_dtype, "float");
      assert.equal(body.encoding_format, "float");
      assert.equal(body.truncation, item.truncation);
      assert.equal(typeof body.id, "string");
      jobIds.push(body.id as string);
    }

    const tokenHash = await sha256Hex(authToken);
    for (let index = 0; index < jobIds.length; index += 1) {
      const jobId = jobIds[index]!;
      const item = cases[index]!;
      const profileKey = embeddingsProfileKey(item.inputType, item.dimensions, "float", item.truncation);
      const jobKey = embeddingsJobKey(tokenHash, profileKey, jobId);
      const lookupKey = embeddingsJobLookupKey(tokenHash, jobId);
      assert.ok(kvStore.has(keyToString(jobKey)));
      assert.deepEqual(kvStore.get(keyToString(lookupKey)), { cache_profile_key: profileKey });
      assert.equal(kvExpirations.get(keyToString(jobKey)), EMBEDDINGS_JOB_TTL_MS);
      assert.equal(kvExpirations.get(keyToString(lookupKey)), EMBEDDINGS_JOB_TTL_MS);

      const other = cases[(index + 1) % cases.length]!;
      const otherProfileKey = embeddingsProfileKey(
        other.inputType,
        other.dimensions,
        "float",
        other.truncation,
      );
      assert.equal(kvStore.has(keyToString(embeddingsJobKey(tokenHash, otherProfileKey, jobId))), false);
      assert.equal(
        kvStore.has(keyToString(["embeddings", "jobs", "v2", tokenHash, jobId])),
        false,
      );
    }

    await kv.delete(VOYAGE_RATE_LIMIT_KEY);
    const seenBodies: Record<string, unknown>[] = [];
    await withFetchMock(
      (_url, bodyText) => {
        const body = JSON.parse(bodyText ?? "null") as Record<string, unknown>;
        seenBodies.push(body);
        return voyageOkResponse(1, body.output_dimension as TestDimension);
      },
      async () => {
        for (let index = 0; index < jobIds.length; index += 1) {
          const jobId = jobIds[index]!;
          const polled = await handleEmbeddingsJobGet(
            new Request(`https://ai.ubq.fi/uos/embedding-jobs/${jobId}`),
            authToken,
            jobId,
          );
          assert.equal(polled.status, 200);
          assert.equal(polled.headers.get("x-ubq-upstream"), "voyage");
          const payload = await polled.json() as {
            status?: unknown;
            input_type?: unknown;
            dimensions?: unknown;
            truncation?: unknown;
            result?: { data?: Array<{ embedding?: unknown }> };
          };
          const expected = cases[index]!;
          assert.equal(payload.status, "succeeded");
          assert.equal(payload.input_type, expected.inputType);
          assert.equal(payload.dimensions, expected.dimensions);
          assert.equal(payload.truncation, expected.truncation);
          assert.equal((payload.result?.data?.[0]?.embedding as unknown[]).length, expected.dimensions);
        }
      },
    );

    assert.equal(seenBodies.length, cases.length);
    for (let index = 0; index < cases.length; index += 1) {
      const expected = cases[index]!;
      assert.deepEqual(seenBodies[index], {
        model: "voyage-4-large",
        input: expected.input,
        input_type: expected.inputType,
        output_dimension: expected.dimensions,
        output_dtype: "float",
        truncation: expected.truncation,
      });
    }
  } finally {
    await kv.delete(VOYAGE_RATE_LIMIT_KEY);
  }
});

Deno.test("embedding jobs: retryable upstream failures requeue and preserve the resolved profile", async () => {
  const retryableStatuses = [429, 500, 502, 503, 504] as const;

  for (const status of retryableStatuses) {
    resetVoyageRateLimit();
    const authToken = `job-retry-${status}-${crypto.randomUUID()}`;
    const input = `job-retry-input-${status}-${crypto.randomUUID()}`;
    const bodies: Record<string, unknown>[] = [];

    const created = await withFetchMock(
      (_url, bodyText) => {
        bodies.push(JSON.parse(bodyText ?? "null") as Record<string, unknown>);
        return new Response(JSON.stringify({ error: "temporarily unavailable" }), {
          status,
          headers: {
            "Content-Type": "application/json",
            ...(status === 429 ? { "Retry-After": "0.001" } : {}),
          },
        });
      },
      () =>
        handleEmbeddingsJobCreate(
          new Request("https://ai.ubq.fi/uos/embedding-jobs", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "voyage-4-large",
              input,
              input_type: "query",
              dimensions: 512,
              truncation: false,
            }),
          }),
          authToken,
        ),
    );

    assert.equal(created.status, 202);
    assert.ok(created.headers.get("Retry-After"));
    const createdPayload = await created.json() as {
      id?: unknown;
      status?: unknown;
      input_type?: unknown;
      dimensions?: unknown;
      truncation?: unknown;
      error?: unknown;
    };
    assert.equal(createdPayload.status, "queued");
    assert.equal(createdPayload.input_type, "query");
    assert.equal(createdPayload.dimensions, 512);
    assert.equal(createdPayload.truncation, false);
    assert.equal(createdPayload.error, null);
    assert.equal(typeof createdPayload.id, "string");
    const jobId = createdPayload.id as string;

    const polled = await withFetchMock(
      (_url, bodyText) => {
        bodies.push(JSON.parse(bodyText ?? "null") as Record<string, unknown>);
        return voyageOkResponse(1, 512);
      },
      () =>
        handleEmbeddingsJobGet(
          new Request(`https://ai.ubq.fi/uos/embedding-jobs/${jobId}`),
          authToken,
          jobId,
        ),
    );

    assert.equal(polled.status, 200);
    const polledPayload = await polled.json() as {
      status?: unknown;
      input_type?: unknown;
      dimensions?: unknown;
      truncation?: unknown;
    };
    assert.equal(polledPayload.status, "succeeded");
    assert.equal(polledPayload.input_type, "query");
    assert.equal(polledPayload.dimensions, 512);
    assert.equal(polledPayload.truncation, false);
    assert.equal(bodies.length, 2);
    assert.deepEqual(bodies[1], bodies[0]);
    assert.deepEqual(bodies[0], {
      model: "voyage-4-large",
      input,
      input_type: "query",
      output_dimension: 512,
      output_dtype: "float",
      truncation: false,
    });
  }
});

Deno.test("embedding jobs: locked and CAS-contention 202 responses identify Voyage", async () => {
  const kv = await kvPromise;
  assert.ok(kv);
  await kv.set(VOYAGE_RATE_LIMIT_KEY, { window_start_ms: Date.now(), requests: 3, tokens: 0 });
  const authToken = `job-lock-${crypto.randomUUID()}`;
  const created = await withFetchMock(
    () => {
      throw new Error("The saturated gateway limit must queue before upstream");
    },
    () =>
      handleEmbeddingsJobCreate(
        new Request("https://ai.ubq.fi/uos/embedding-jobs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "voyage-4-large",
            input: `job-lock-input-${crypto.randomUUID()}`,
            input_type: "query",
          }),
        }),
        authToken,
      ),
  );
  const createdBody = await created.json() as { id?: unknown };
  assert.equal(typeof createdBody.id, "string");
  const jobId = createdBody.id as string;
  const tokenHash = await sha256Hex(authToken);
  const jobKey = embeddingsJobKey(tokenHash, embeddingsProfileKey("query"), jobId);
  const stored = kvStore.get(keyToString(jobKey)) as Record<string, unknown> | undefined;
  assert.ok(stored);
  await kv.delete(VOYAGE_RATE_LIMIT_KEY);

  try {
    kvStore.set(keyToString(jobKey), {
      ...stored,
      status: "running",
      locked_until_ms: Date.now() + 30_000,
    });
    const locked = await withFetchMock(
      () => {
        throw new Error("A locked job must not call upstream");
      },
      () =>
        handleEmbeddingsJobGet(
          new Request(`https://ai.ubq.fi/uos/embedding-jobs/${jobId}`),
          authToken,
          jobId,
        ),
    );
    assert.equal(locked.status, 202);
    assert.equal(locked.headers.get("x-ubq-upstream"), "voyage");

    kvStore.set(keyToString(jobKey), {
      ...stored,
      status: "queued",
      locked_until_ms: null,
    });
    failNextAtomicCommit = (_checks, ops) =>
      ops.some((op) => op.type === "set" && keyToString(op.key) === keyToString(jobKey));
    const contended = await withFetchMock(
      () => {
        throw new Error("A contended job lock must not call upstream");
      },
      () =>
        handleEmbeddingsJobGet(
          new Request(`https://ai.ubq.fi/uos/embedding-jobs/${jobId}`),
          authToken,
          jobId,
        ),
    );
    assert.equal(contended.status, 202);
    assert.equal(contended.headers.get("x-ubq-upstream"), "voyage");
  } finally {
    failNextAtomicCommit = null;
    await kv.delete(VOYAGE_RATE_LIMIT_KEY);
  }
});

Deno.test("embedding jobs: poll runs queued job to completion", async () => {
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
        new Request("https://ai.ubq.fi/uos/embedding-jobs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "voyage-4-large",
            input,
            input_type: "query",
            dimensions: 512,
            truncation: false,
          }),
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
      const body = JSON.parse(bodyText ?? "null") as Record<string, unknown>;
      assert.equal(body.model, "voyage-4-large");
      assert.equal(body.input_type, "query");
      assert.equal(body.output_dimension, 512);
      assert.equal(body.output_dtype, "float");
      assert.equal(body.truncation, false);
      assert.equal("encoding_format" in body, false);
      assert.equal("output_encoding" in body, false);
      const count = Array.isArray(body.input) ? body.input.length : 1;
      return voyageOkResponse(count, 512);
    },
    () => handleEmbeddingsJobGet(new Request(`https://ai.ubq.fi/uos/embedding-jobs/${jobId}`), "test_token", jobId),
  );

  assert.equal(polled.status, 200);
  const payload = await polled.json() as {
    status?: unknown;
    input_type?: unknown;
    dimensions?: unknown;
    truncation?: unknown;
    result?: { data?: Array<{ embedding?: unknown }> };
  };
  assert.equal(payload.status, "succeeded");
  assert.equal(payload.input_type, "query");
  assert.equal(payload.dimensions, 512);
  assert.equal(payload.truncation, false);
  assert.ok(Array.isArray(payload.result?.data));
  assert.equal((payload.result?.data?.[0]?.embedding as unknown[]).length, 512);
});

Deno.test("handler: /uos/embeddings reaches authentication instead of the 404 guard", async () => {
  const { default: handler } = await import("../src/handler.ts");
  const response = await handler(
    new Request("https://ai.ubq.fi/uos/embeddings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: "voyage-4-large",
        input: "route-reachability",
        input_type: "document",
      }),
    }),
  );

  assert.equal(response.status, 401);
  assert.notEqual(response.status, 404);
});

addEventListener("unload", () => {
  (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = originalOpenKv;
  if (originalVoyageApiKey === undefined) Deno.env.delete("VOYAGEAI_API_KEY");
  else Deno.env.set("VOYAGEAI_API_KEY", originalVoyageApiKey);
});
