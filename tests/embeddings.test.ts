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
const kvVersions = new Map<string, number>();
const VOYAGE_RATE_LIMIT_KEY: Deno.KvKey = ["embeddings", "v1", "rate", "voyage"];
const EMBEDDINGS_JOB_TTL_MS = 24 * 60 * 60_000;
const EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS = 256;
const EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS = 7 * 24 * 60 * 60_000;
const EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS = EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS + 24 * 60 * 60_000;
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
  accounts: [{
    access_token: "access",
    refresh_token: "refresh",
    account_id: "acct",
    updated_at_ms: Date.now(),
  }],
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

const kvVersionstamp = (rawKey: string): string | null =>
  kvStore.has(rawKey) ? String(kvVersions.get(rawKey) ?? 1).padStart(20, "0") : null;

const bumpKvVersion = (rawKey: string): void => {
  kvVersions.set(rawKey, (kvVersions.get(rawKey) ?? 0) + 1);
};

const kvStub = {
  get: (key: Deno.KvKey) => {
    const rawKey = keyToString(key);
    return Promise.resolve(
      ({
        key,
        value: kvStore.get(rawKey) ?? null,
        versionstamp: kvVersionstamp(rawKey),
      }) as Deno.KvEntryMaybe<unknown>,
    );
  },
  set: (key: Deno.KvKey, value: unknown, options?: { expireIn?: number }) => {
    const rawKey = keyToString(key);
    kvStore.set(rawKey, value);
    kvExpirations.set(rawKey, options?.expireIn);
    bumpKvVersion(rawKey);
    return Promise.resolve({ ok: true } as const);
  },
  delete: (key: Deno.KvKey) => {
    const rawKey = keyToString(key);
    kvStore.delete(rawKey);
    kvExpirations.delete(rawKey);
    bumpKvVersion(rawKey);
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
        for (const check of checks) {
          if (kvVersionstamp(keyToString(check.key)) !== check.versionstamp) {
            return Promise.resolve({ ok: false } as const);
          }
        }
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
          const rawKey = keyToString(op.key);
          if (op.type === "set") {
            kvStore.set(rawKey, op.value);
            kvExpirations.set(rawKey, op.expireIn);
          } else {
            kvStore.delete(rawKey);
            kvExpirations.delete(rawKey);
          }
          bumpKvVersion(rawKey);
        }
        return Promise.resolve({ ok: true } as const);
      },
    };
    return chain;
  },
  close: () => {},
} as unknown as Deno.Kv;

(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kvStub);

const { handleEmbeddingsJobCreate, handleEmbeddingsJobGet, handleUosEmbeddings } = await import(
  "../src/openai.ts"
);
const { getKv } = await import("../src/kv.ts");
await getKv();

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

const uosIdempotencyUsageContext = (principal: string) => ({
  keyId: null,
  kernelRepo: null,
  kernelOrg: null,
  idempotencyPrincipal: principal,
});

const uosIdempotentRequest = (
  idempotencyKey: string,
  input: string | string[],
  overrides: Partial<{
    input_type: TestInputType;
    dimensions: TestDimension;
    truncation: boolean;
  }> = {},
): Request =>
  new Request("https://ai.ubq.fi/uos/embeddings", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "Idempotency-Key": idempotencyKey,
    },
    body: JSON.stringify({
      model: "voyage-4-large",
      input,
      input_type: overrides.input_type ?? "document",
      dimensions: overrides.dimensions ?? 1024,
      truncation: overrides.truncation ?? false,
    }),
  });

const responseErrorCode = async (response: Response): Promise<string | null> => {
  const payload = await response.json() as { error?: { code?: unknown } };
  return typeof payload.error?.code === "string" ? payload.error.code : null;
};

const uosEmbeddingsIdempotencyRecordKey = async (
  principal: string,
  idempotencyKey: string,
): Promise<Deno.KvKey> => [
  "embeddings",
  "idempotency",
  "v1",
  await sha256Hex(`uos-embeddings-principal-v1:${principal}`),
  await sha256Hex(`uos-embeddings-key-v1:${idempotencyKey}`),
];

const uosEmbeddingsIdempotencyResponsePrefix = async (
  principal: string,
  idempotencyKey: string,
): Promise<Deno.KvKey> => [
  "embeddings",
  "idempotency",
  "v1",
  "response",
  await sha256Hex(`uos-embeddings-principal-v1:${principal}`),
  await sha256Hex(`uos-embeddings-key-v1:${idempotencyKey}`),
];

const uosEmbeddingsIdempotencyFingerprint = async (
  input: string[],
  inputType: TestInputType = "document",
  dimensions: TestDimension = 1024,
  truncation = false,
): Promise<string> =>
  await sha256Hex(
    JSON.stringify([
      "uos-embeddings-idempotency-v1",
      "voyage",
      "voyage-4-large",
      inputType,
      dimensions,
      "float",
      "float",
      truncation,
      await Promise.all(input.map((item) => sha256Hex(item))),
    ]),
  );

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
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-4-large", input: "hello", user: null }),
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
  assert.equal(payload.model, "voyage-4-large");
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
  assert.equal(response.headers.get("x-uos-upstream"), "voyage");
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
        assert.equal(response.headers.get("x-uos-upstream"), "voyage");
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

Deno.test("uos embeddings idempotency: replays the stored validated response without another Voyage call", async () => {
  resetVoyageRateLimit();
  const idempotencyKey = `embedding-job-${crypto.randomUUID()}`;
  const input = `idempotency-replay-${crypto.randomUUID()}`;
  const usageContext = uosIdempotencyUsageContext("account-replay");
  let upstreamCalls = 0;

  await withFetchMock(
    () => {
      upstreamCalls += 1;
      return voyageOkResponse(1);
    },
    async () => {
      const first = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, input),
        usageContext,
      );
      assert.equal(first.status, 200);
      assert.equal(first.headers.get("x-uos-idempotency-replayed"), null);
      const firstBody = await first.text();

      const replay = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, input),
        usageContext,
      );
      assert.equal(replay.status, 200);
      assert.equal(replay.headers.get("x-uos-idempotency-replayed"), "true");
      assert.equal(await replay.text(), firstBody);
    },
  );

  assert.equal(upstreamCalls, 1);
});

Deno.test("uos embeddings idempotency: same principal and key reject a different ordered request fingerprint", async () => {
  resetVoyageRateLimit();
  const idempotencyKey = `embedding-job-${crypto.randomUUID()}`;
  const usageContext = uosIdempotencyUsageContext("account-conflict");
  let upstreamCalls = 0;

  await withFetchMock(
    () => {
      upstreamCalls += 1;
      return voyageOkResponse(2);
    },
    async () => {
      const first = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, ["first", "second"]),
        usageContext,
      );
      assert.equal(first.status, 200);

      const conflict = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, ["second", "first"]),
        usageContext,
      );
      assert.equal(conflict.status, 409);
      assert.equal(await responseErrorCode(conflict), "embedding_idempotency_conflict");
    },
  );

  assert.equal(upstreamCalls, 1);
});

Deno.test("uos embeddings idempotency: a concurrent replay cannot dispatch Voyage twice", async () => {
  resetVoyageRateLimit();
  const idempotencyKey = `embedding-job-${crypto.randomUUID()}`;
  const input = `idempotency-concurrent-${crypto.randomUUID()}`;
  const usageContext = uosIdempotencyUsageContext("account-concurrent");
  let upstreamCalls = 0;
  let signalUpstreamEntered = () => {};
  const upstreamEntered = new Promise<void>((resolve) => {
    signalUpstreamEntered = resolve;
  });
  let releaseUpstream = (_response: Response) => {};
  const upstreamResult = new Promise<Response>((resolve) => {
    releaseUpstream = resolve;
  });

  await withFetchMock(
    async () => {
      upstreamCalls += 1;
      signalUpstreamEntered();
      return await upstreamResult;
    },
    async () => {
      const firstPromise = handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, input),
        usageContext,
      );
      await upstreamEntered;

      const concurrent = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, input),
        usageContext,
      );
      assert.equal(concurrent.status, 409);
      assert.equal(concurrent.headers.get("Retry-After"), "1");
      assert.equal(await responseErrorCode(concurrent), "embedding_idempotency_in_progress");
      assert.equal(upstreamCalls, 1);

      releaseUpstream(voyageOkResponse(1));
      const first = await firstPromise;
      assert.equal(first.status, 200);
    },
  );

  assert.equal(upstreamCalls, 1);
});

Deno.test("uos embeddings idempotency: keyed requests fail before Voyage when durable KV is unavailable", async () => {
  const idempotencyKey = `embedding-job-${crypto.randomUUID()}`;
  const usageContext = uosIdempotencyUsageContext("account-no-kv");
  let upstreamCalls = 0;

  const response = await withFetchMock(
    () => {
      upstreamCalls += 1;
      return voyageOkResponse(1);
    },
    () =>
      handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, `idempotency-no-kv-${crypto.randomUUID()}`),
        usageContext,
        { kv: null },
      ),
  );

  assert.equal(response.status, 503);
  assert.equal(await responseErrorCode(response), "embedding_idempotency_unavailable");
  assert.equal(upstreamCalls, 0);
});

Deno.test("embeddings: quota dispatch failures release idempotency and promptly requeue jobs", async () => {
  const { ApiKeyQuotaDispatchError } = await import("../src/api_key_policy.ts");
  const idempotencyKey = `embedding-quota-dispatch-${crypto.randomUUID()}`;
  const input = `quota-dispatch-${crypto.randomUUID()}`;
  const principal = "account-quota-dispatch";
  const usageContext = uosIdempotencyUsageContext(principal);
  const quotaFailureContext = {
    ...usageContext,
    beforeProviderDispatch: () => Promise.reject(new ApiKeyQuotaDispatchError("simulated dispatch CAS failure")),
  };
  let upstreamCalls = 0;

  await withFetchMock(
    () => {
      upstreamCalls += 1;
      return voyageOkResponse(1);
    },
    async () => {
      resetVoyageRateLimit();
      const failed = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, input),
        quotaFailureContext,
      );
      assert.equal(failed.status, 503);
      assert.equal(await responseErrorCode(failed), "api_key_quota_reservation_unavailable");
      assert.equal(upstreamCalls, 0);

      const retried = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, input),
        usageContext,
      );
      assert.equal(retried.status, 200);
      assert.equal(upstreamCalls, 1);

      const jobToken = `quota-job-${crypto.randomUUID()}`;
      const jobQueued = await handleEmbeddingsJobCreate(
        new Request("https://ai.ubq.fi/uos/embedding-jobs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "voyage-4-large",
            input: `quota-job-${crypto.randomUUID()}`,
            input_type: "document",
          }),
        }),
        jobToken,
        quotaFailureContext,
      );
      assert.equal(jobQueued.status, 202);
      assert.equal(jobQueued.headers.get("Retry-After"), "1");
      const queuedJob = await jobQueued.json() as { id?: unknown; status?: unknown };
      assert.equal(queuedJob.status, "queued");
      assert.equal(typeof queuedJob.id, "string");
      assert.equal(upstreamCalls, 1);

      resetVoyageRateLimit();
      const completedJob = await handleEmbeddingsJobGet(
        new Request(`https://ai.ubq.fi/uos/embedding-jobs/${queuedJob.id}`),
        jobToken,
        queuedJob.id as string,
        usageContext,
      );
      assert.equal(completedJob.status, 200);
      const completedPayload = await completedJob.json() as { status?: unknown };
      assert.equal(completedPayload.status, "succeeded");
      assert.equal(upstreamCalls, 2);
    },
  );
});

Deno.test("uos embeddings idempotency: an outcome-unknown dispatch is durable and never sent again", async () => {
  resetVoyageRateLimit();
  const idempotencyKey = `embedding-job-${crypto.randomUUID()}`;
  const input = `idempotency-indeterminate-${crypto.randomUUID()}`;
  const usageContext = uosIdempotencyUsageContext("account-indeterminate");
  let upstreamCalls = 0;

  await withFetchMock(
    () => {
      upstreamCalls += 1;
      throw new TypeError("simulated connection loss after dispatch");
    },
    async () => {
      const first = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, input),
        usageContext,
      );
      assert.equal(first.status, 409);
      assert.equal(await responseErrorCode(first), "embedding_idempotency_indeterminate");

      const replay = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, input),
        usageContext,
      );
      assert.equal(replay.status, 409);
      assert.equal(await responseErrorCode(replay), "embedding_idempotency_indeterminate");
    },
  );

  assert.equal(upstreamCalls, 1);
});

Deno.test("uos embeddings idempotency: an abandoned dispatched ledger fails closed without Voyage", async () => {
  const idempotencyKey = `embedding-job-${crypto.randomUUID()}`;
  const input = `idempotency-abandoned-${crypto.randomUUID()}`;
  const principal = "account-abandoned";
  const ledgerKey = await uosEmbeddingsIdempotencyRecordKey(principal, idempotencyKey);
  const fingerprint = await uosEmbeddingsIdempotencyFingerprint([input]);
  const now = Date.now();
  kvStore.set(keyToString(ledgerKey), {
    v: 1,
    fingerprint,
    state: "dispatched",
    owner_request_id: "crashed-request",
    created_at_ms: now - 120_000,
    updated_at_ms: now - 120_000,
    lease_until_ms: now - 60_000,
    response_status: null,
    response_content_type: null,
    response_generation: null,
    response_chunk_count: null,
    response_sha256: null,
  });
  let upstreamCalls = 0;

  try {
    const response = await withFetchMock(
      () => {
        upstreamCalls += 1;
        return voyageOkResponse(1);
      },
      () =>
        handleUosEmbeddings(
          uosIdempotentRequest(idempotencyKey, input),
          uosIdempotencyUsageContext(principal),
        ),
    );

    assert.equal(response.status, 409);
    assert.equal(await responseErrorCode(response), "embedding_idempotency_indeterminate");
    assert.equal(upstreamCalls, 0);
    const stored = kvStore.get(keyToString(ledgerKey)) as { state?: unknown } | undefined;
    assert.equal(stored?.state, "indeterminate");
  } finally {
    kvStore.delete(keyToString(ledgerKey));
  }
});

Deno.test("uos embeddings idempotency: confirmed HTTP failures re-arm the key for a later retry", async () => {
  resetVoyageRateLimit();
  const idempotencyKey = `embedding-job-${crypto.randomUUID()}`;
  const input = `idempotency-explicit-retry-${crypto.randomUUID()}`;
  const usageContext = uosIdempotencyUsageContext("account-explicit-retry");
  let upstreamCalls = 0;

  await withFetchMock(
    () => {
      upstreamCalls += 1;
      if (upstreamCalls <= 3) {
        return new Response(JSON.stringify({ error: "rate limited" }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": "0.001" },
        });
      }
      return voyageOkResponse(1);
    },
    async () => {
      const failed = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, input),
        usageContext,
      );
      assert.equal(failed.status, 429);
      assert.equal(await responseErrorCode(failed), "rate_limit_exceeded");

      const retry = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, input),
        usageContext,
      );
      assert.equal(retry.status, 200);
      assert.equal(retry.headers.get("x-uos-idempotency-replayed"), null);
    },
  );

  assert.equal(upstreamCalls, 4);
});

Deno.test("uos embeddings idempotency: an expired owner cannot overwrite the published response generation", async () => {
  resetVoyageRateLimit();
  const idempotencyKey = `embedding-job-${crypto.randomUUID()}`;
  const input = `idempotency-owner-generation-${crypto.randomUUID()}`;
  const principal = "account-owner-generation";
  const usageContext = uosIdempotencyUsageContext(principal);
  const ledgerKey = await uosEmbeddingsIdempotencyRecordKey(principal, idempotencyKey);
  const responsePrefix = await uosEmbeddingsIdempotencyResponsePrefix(principal, idempotencyKey);
  const fingerprint = await uosEmbeddingsIdempotencyFingerprint([input]);
  const expiredGeneration = "expired-owner";
  const now = Date.now();
  kvStore.set(keyToString(ledgerKey), {
    v: 1,
    fingerprint,
    state: "reserved",
    owner_request_id: expiredGeneration,
    created_at_ms: now - 120_000,
    updated_at_ms: now - 120_000,
    lease_until_ms: now - 60_000,
    response_status: null,
    response_content_type: null,
    response_generation: null,
    response_chunk_count: null,
    response_sha256: null,
  });
  bumpKvVersion(keyToString(ledgerKey));
  let upstreamCalls = 0;

  try {
    await withFetchMock(
      () => {
        upstreamCalls += 1;
        return voyageOkResponse(1);
      },
      async () => {
        const first = await handleUosEmbeddings(
          uosIdempotentRequest(idempotencyKey, input),
          usageContext,
        );
        assert.equal(first.status, 200);
        const firstBody = await first.text();
        const stored = kvStore.get(keyToString(ledgerKey)) as {
          state?: unknown;
          response_generation?: unknown;
          response_chunk_count?: unknown;
        };
        assert.equal(stored.state, "succeeded");
        assert.equal(typeof stored.response_generation, "string");
        assert.notEqual(stored.response_generation, expiredGeneration);
        assert.equal(typeof stored.response_chunk_count, "number");

        const publishedGeneration = stored.response_generation as string;
        const publishedChunkKey: Deno.KvKey = [...responsePrefix, publishedGeneration, 0];
        assert.equal(kvExpirations.get(keyToString(ledgerKey)), EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS);
        assert.equal(
          kvExpirations.get(keyToString(publishedChunkKey)),
          EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS,
        );
        assert(
          EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS > EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS,
        );

        // A late write from the expired owner lands in its own generation and
        // cannot corrupt the response generation already published by CAS.
        const expiredChunkKey: Deno.KvKey = [...responsePrefix, expiredGeneration, 0];
        await kvStub.set(
          expiredChunkKey,
          "late stale owner body",
          { expireIn: EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS },
        );
        assert.equal(
          kvExpirations.get(keyToString(expiredChunkKey)),
          EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS,
        );

        const replay = await handleUosEmbeddings(
          uosIdempotentRequest(idempotencyKey, input),
          usageContext,
        );
        assert.equal(replay.status, 200);
        assert.equal(replay.headers.get("x-uos-idempotency-replayed"), "true");
        assert.equal(await replay.text(), firstBody);
      },
    );
    assert.equal(upstreamCalls, 1);
  } finally {
    for (const rawKey of [...kvStore.keys()]) {
      const key = JSON.parse(rawKey) as Deno.KvKey;
      if (keyHasPrefix(key, ledgerKey) || keyHasPrefix(key, responsePrefix)) {
        kvStore.delete(rawKey);
        kvExpirations.delete(rawKey);
      }
    }
  }
});

Deno.test("uos embeddings idempotency: rejects an oversized stored response chunk count without reading chunks", async () => {
  const idempotencyKey = `embedding-job-${crypto.randomUUID()}`;
  const input = `idempotency-chunk-bound-${crypto.randomUUID()}`;
  const principal = "account-chunk-bound";
  const ledgerKey = await uosEmbeddingsIdempotencyRecordKey(principal, idempotencyKey);
  const fingerprint = await uosEmbeddingsIdempotencyFingerprint([input]);
  const oversizedChunkCount = EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS + 1;
  const now = Date.now();
  kvStore.set(keyToString(ledgerKey), {
    v: 1,
    fingerprint,
    state: "succeeded",
    owner_request_id: null,
    created_at_ms: now,
    updated_at_ms: now,
    lease_until_ms: null,
    response_status: 200,
    response_content_type: "application/json",
    response_generation: "oversized-generation",
    response_chunk_count: oversizedChunkCount,
    response_sha256: await sha256Hex("oversized"),
  });
  bumpKvVersion(keyToString(ledgerKey));
  let upstreamCalls = 0;

  try {
    const response = await withFetchMock(
      () => {
        upstreamCalls += 1;
        return voyageOkResponse(1);
      },
      () =>
        handleUosEmbeddings(
          uosIdempotentRequest(idempotencyKey, input),
          uosIdempotencyUsageContext(principal),
        ),
    );

    assert.equal(response.status, 409);
    assert.equal(await responseErrorCode(response), "embedding_idempotency_indeterminate");
    assert.equal(upstreamCalls, 0);
    const stored = kvStore.get(keyToString(ledgerKey)) as {
      state?: unknown;
      response_chunk_count?: unknown;
    };
    assert.equal(stored.state, "succeeded");
    assert.equal(stored.response_chunk_count, oversizedChunkCount);
  } finally {
    kvStore.delete(keyToString(ledgerKey));
  }
});

Deno.test("uos embeddings idempotency: a malformed stored value is not mistaken for an absent CAS entry", async () => {
  const idempotencyKey = `embedding-job-${crypto.randomUUID()}`;
  const principal = "account-malformed-ledger";
  const ledgerKey = await uosEmbeddingsIdempotencyRecordKey(principal, idempotencyKey);
  kvStore.set(keyToString(ledgerKey), null);
  bumpKvVersion(keyToString(ledgerKey));
  let upstreamCalls = 0;

  try {
    const response = await withFetchMock(
      () => {
        upstreamCalls += 1;
        return voyageOkResponse(1);
      },
      () =>
        handleUosEmbeddings(
          uosIdempotentRequest(
            idempotencyKey,
            `idempotency-malformed-ledger-${crypto.randomUUID()}`,
          ),
          uosIdempotencyUsageContext(principal),
        ),
    );

    assert.equal(response.status, 409);
    assert.equal(await responseErrorCode(response), "embedding_idempotency_indeterminate");
    assert.equal(upstreamCalls, 0);
    assert.equal(kvStore.has(keyToString(ledgerKey)), true);
    assert.equal(kvStore.get(keyToString(ledgerKey)), null);
  } finally {
    kvStore.delete(keyToString(ledgerKey));
  }
});

Deno.test("uos embeddings: accepts every supported standard dimension with the canonical model", async () => {
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
        const response = await handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "voyage-4-large",
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
        assert.equal(payload.model, "voyage-4-large");
        assert.equal((payload.data?.[0]?.embedding as unknown[]).length, dimension);
      }
    },
  );
});

Deno.test("uos embeddings: reject malformed synchronous fields", async () => {
  const requests: Array<() => Promise<Response>> = [
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "voyage-3-large",
            input: "x",
          }),
        }),
      ),
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-4-large", input: "x", dimensions: 768 }),
        }),
      ),
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-4-large", input: "x", dimensions: 256.5 }),
        }),
      ),
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-4-large", input: "x", input_type: "index" }),
        }),
      ),
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-4-large", input: "x", encoding_format: "binary" }),
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
            truncation: "false",
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
            user: 42,
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
            input: [1],
          }),
        }),
      ),
    () =>
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-4-large", input: "x", unsupported: true }),
        }),
      ),
  ];

  for (const makeRequest of requests) {
    const response = await makeRequest();
    assert.equal(response.status, 400);
    assert.equal(response.headers.get("x-uos-upstream"), "voyage");
  }
});

Deno.test("uos embeddings: rejects OpenAI model names without dispatching Voyage", async () => {
  let upstreamCalls = 0;
  await withFetchMock(
    () => {
      upstreamCalls += 1;
      return voyageOkResponse(1);
    },
    async () => {
      for (const model of ["text-embedding-3-small", "text-embedding-3-large"]) {
        const response = await handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model, input: "must-not-dispatch" }),
          }),
        );
        assert.equal(response.status, 400);
        const payload = await response.json() as { error?: { code?: unknown; param?: unknown } };
        assert.equal(payload.error?.code, "model_not_found");
        assert.equal(payload.error?.param, "model");
      }
    },
  );
  assert.equal(upstreamCalls, 0);
});

Deno.test("embedding jobs retain their strict Voyage float profile", async () => {
  const requests = [
    { model: "text-embedding-3-small", input: "x", input_type: "document" },
    { model: "voyage-4-large", input: "x" },
    { model: "voyage-4-large", input: "x", input_type: "document", encoding_format: "base64" },
    { model: "voyage-4-large", input: "x", input_type: "document", user: "not-supported" },
  ];
  let upstreamCalls = 0;

  await withFetchMock(
    () => {
      upstreamCalls += 1;
      return voyageOkResponse(1);
    },
    async () => {
      for (const body of requests) {
        const response = await handleEmbeddingsJobCreate(
          new Request("https://ai.ubq.fi/uos/embedding-jobs", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(body),
          }),
          `jobs-strict-${crypto.randomUUID()}`,
        );
        assert.equal(response.status, 400);
        assert.equal(response.headers.get("x-uos-upstream"), "voyage");
      }
    },
  );

  assert.equal(upstreamCalls, 0);
});

Deno.test("embeddings: serves cache hits without calling upstream", async () => {
  resetVoyageRateLimit();
  const model = "voyage-4-large";
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
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
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
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "voyage-4-large",
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
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: "voyage-4-large",
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
  const model = "voyage-4-large";
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
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
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
  const model = "voyage-4-large";
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
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
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
  const model = "voyage-4-large";
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
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
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
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-4-large", input: ["a", "b"] }),
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
  const response = await handleUosEmbeddings(
    new Request("https://ai.ubq.fi/uos/embeddings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ model: "voyage-4-large", input: ["a", 2] }),
    }),
  );
  assert.equal(response.status, 400);
  const payload = await response.json() as { error?: { param?: unknown } };
  assert.equal(payload.error?.param, "input");
});

Deno.test("embeddings: rejects too many inputs", async () => {
  const inputs = Array.from({ length: 129 }, (_, i) => `x${i}`);
  const response = await handleUosEmbeddings(
    new Request("https://ai.ubq.fi/uos/embeddings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ model: "voyage-4-large", input: inputs }),
    }),
  );
  assert.equal(response.status, 400);
});

Deno.test("embeddings: rejects too-large inputs", async () => {
  const tooLarge = "a".repeat(20_001);
  const response = await handleUosEmbeddings(
    new Request("https://ai.ubq.fi/uos/embeddings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ model: "voyage-4-large", input: tooLarge }),
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
      handleUosEmbeddings(
        new Request("https://ai.ubq.fi/uos/embeddings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "voyage-4-large",
            input: "hello",
            dimensions: 256,
            encoding_format: "base64",
            user: "migration-client",
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

Deno.test("uos embeddings: rejects fractional dimensions", async () => {
  const response = await handleUosEmbeddings(
    new Request("https://ai.ubq.fi/uos/embeddings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: "voyage-4-large",
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
  assert.equal(response.headers.get("x-uos-upstream"), "voyage");
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
  assert.equal(response.headers.get("x-uos-upstream"), "voyage");
  const payload = await response.json() as { error?: { type?: unknown; code?: unknown } };
  assert.equal(payload.error?.type, "rate_limit_error");
  assert.equal(payload.error?.code, "rate_limit_exceeded");
});

Deno.test("embeddings: 429 includes Retry-After when KV rate limited", async () => {
  const kv = await getKv();
  assert.ok(kv);
  await kv.set(VOYAGE_RATE_LIMIT_KEY, { window_start_ms: Date.now(), requests: 3, tokens: 0 });
  try {
    const response = await withFetchMock(
      () => {
        throw new Error("Embeddings should be rate limited before upstream fetch");
      },
      () =>
        handleUosEmbeddings(
          new Request("https://ai.ubq.fi/uos/embeddings", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: "voyage-4-large", input: "rate-limit-test" }),
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
  assert.equal(created.headers.get("x-uos-upstream"), "voyage");
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
  assert.equal(polled.headers.get("x-uos-upstream"), "voyage");
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
  const kv = await getKv();
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
    assert.equal(response.headers.get("x-uos-upstream"), "voyage");
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
  const kv = await getKv();
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
      assert.equal(created.headers.get("x-uos-upstream"), "voyage");
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
          assert.equal(polled.headers.get("x-uos-upstream"), "voyage");
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
  const kv = await getKv();
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
    assert.equal(locked.headers.get("x-uos-upstream"), "voyage");

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
    assert.equal(contended.headers.get("x-uos-upstream"), "voyage");
  } finally {
    failNextAtomicCommit = null;
    await kv.delete(VOYAGE_RATE_LIMIT_KEY);
  }
});

Deno.test("embedding jobs: poll runs queued job to completion", async () => {
  const kv = await getKv();
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

Deno.test("handler: an exhausted key still serves local embeddings paths but blocks a dispatch", async () => {
  const { handleAdminApiKeysCreate } = await import("../src/admin.ts");
  const { default: handler } = await import("../src/handler.ts");
  const token = `u_${crypto.randomUUID().replace(/-/g, "").padEnd(64, "a")}`;
  const created = await handleAdminApiKeysCreate(
    new Request("https://ai.ubq.fi/admin/api-keys", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        name: "exhausted embeddings local paths",
        token,
        usage_limit_requests: 1,
        paid_fallback_enabled: false,
      }),
    }),
  );
  assert.equal(created.status, 200);
  const createdPayload = await created.json() as { id?: unknown };
  assert.equal(typeof createdPayload.id, "string");
  const keyId = createdPayload.id as string;
  const idempotencyKey = `exhausted-replay-${crypto.randomUUID()}`;
  const idempotencyInput = `exhausted-idempotency-${crypto.randomUUID()}`;
  const jobInput = `exhausted-job-${crypto.randomUUID()}`;
  let jobId = "";

  resetVoyageRateLimit();
  await withFetchMock(
    () => voyageOkResponse(1),
    async () => {
      const seededReplay = await handleUosEmbeddings(
        uosIdempotentRequest(idempotencyKey, idempotencyInput),
        uosIdempotencyUsageContext(`api-key:${keyId}`),
      );
      assert.equal(seededReplay.status, 200);
      const seededJob = await handleEmbeddingsJobCreate(
        new Request("https://ai.ubq.fi/uos/embedding-jobs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: "voyage-4-large", input: jobInput, input_type: "document" }),
        }),
        token,
        { keyId, kernelRepo: null, kernelOrg: null },
      );
      assert.equal(seededJob.status, 200);
      const seededJobPayload = await seededJob.json() as { id?: unknown; status?: unknown };
      assert.equal(seededJobPayload.status, "succeeded");
      assert.equal(typeof seededJobPayload.id, "string");
      jobId = seededJobPayload.id as string;
    },
  );

  const cacheInput = `exhausted-cache-${crypto.randomUUID()}`;
  const cacheHash = await sha256Hex(cacheInput);
  kvStore.set(
    keyToString(embeddingsCacheKey(cacheHash)),
    { embedding: testVector(1024, 7.7), created_at: new Date().toISOString() },
  );
  const quotaWindowPrefix: Deno.KvKey = ["uos_ai", "api_key_usage", "v3", "window", keyId];
  const quotaEntry = [...kvStore.entries()].find(([rawKey]) =>
    keyHasPrefix(JSON.parse(rawKey) as Deno.KvKey, quotaWindowPrefix)
  );
  assert.ok(quotaEntry);
  const [rawQuotaKey, rawQuotaWindow] = quotaEntry;
  const quotaKv = await getKv();
  assert.ok(quotaKv);
  await quotaKv.set(
    JSON.parse(rawQuotaKey) as Deno.KvKey,
    {
      ...(rawQuotaWindow as Record<string, unknown>),
      committed_requests: 1,
      reserved_requests: 0,
      updated_at_ms: Date.now(),
    },
  );

  const embeddingsRequest = (input: string, replayKey?: string): Request =>
    new Request("https://ai.ubq.fi/uos/embeddings", {
      method: "POST",
      headers: {
        Authorization: `Bearer ${token}`,
        "Content-Type": "application/json",
        ...(replayKey ? { "Idempotency-Key": replayKey } : {}),
      },
      body: JSON.stringify({ model: "voyage-4-large", input, truncation: replayKey ? false : true }),
    });

  let voyageCalls = 0;
  resetVoyageRateLimit();
  try {
    await withFetchMock(
      () => {
        voyageCalls += 1;
        return voyageOkResponse(1);
      },
      async () => {
        const replay = await handler(embeddingsRequest(idempotencyInput, idempotencyKey));
        assert.equal(replay.status, 200);
        assert.equal(replay.headers.get("x-uos-idempotency-replayed"), "true");

        const cached = await handler(embeddingsRequest(cacheInput));
        assert.equal(cached.status, 200);

        const terminalJob = await handler(
          new Request(`https://ai.ubq.fi/uos/embedding-jobs/${jobId}`, {
            headers: { Authorization: `Bearer ${token}` },
          }),
        );
        assert.equal(terminalJob.status, 200);
        assert.equal((await terminalJob.json() as { status?: unknown }).status, "succeeded");

        const blocked = await handler(embeddingsRequest(`exhausted-miss-${crypto.randomUUID()}`));
        assert.equal(blocked.status, 429);
        assert.ok(blocked.headers.get("Retry-After"));
        assert.equal(blocked.headers.get("RateLimit-Limit"), "1");
        assert.equal(blocked.headers.get("RateLimit-Remaining"), "0");
        assert.match(blocked.headers.get("RateLimit-Policy") ?? "", /^1;w=\d+$/);
        assert.match(blocked.headers.get("RateLimit") ?? "", /^limit=1, remaining=0, reset=\d+$/);
        assert.equal((await blocked.json() as { error?: { type?: unknown } }).error?.type, "rate_limit_error");
      },
    );
    assert.equal(voyageCalls, 0);
  } finally {
    resetVoyageRateLimit();
  }
});

Deno.test("handler: authenticated legacy v1 embeddings is a generic 404 without Voyage dispatch", async () => {
  const { handleAdminApiKeysCreate } = await import("../src/admin.ts");
  const token = `u_${crypto.randomUUID().replace(/-/g, "").padEnd(64, "a")}`;
  const created = await handleAdminApiKeysCreate(
    new Request("https://ai.ubq.fi/admin/api-keys", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        name: "legacy embeddings route check",
        token,
        usage_limit_requests: 1,
        paid_fallback_enabled: false,
      }),
    }),
  );
  assert.equal(created.status, 200);
  const createdPayload = await created.json() as { id?: unknown };
  assert.equal(typeof createdPayload.id, "string");
  const keyId = createdPayload.id as string;
  const quotaWindowPrefix: Deno.KvKey = ["uos_ai", "api_key_usage", "v3", "window", keyId];
  const quotaWindow = (): { committed_requests?: unknown; reserved_requests?: unknown } | null => {
    for (const [rawKey, value] of kvStore.entries()) {
      const key = JSON.parse(rawKey) as Deno.KvKey;
      if (keyHasPrefix(key, quotaWindowPrefix)) {
        return value as { committed_requests?: unknown; reserved_requests?: unknown };
      }
    }
    return null;
  };
  const quotaBefore = quotaWindow();
  assert.ok(quotaBefore);
  assert.equal(quotaBefore.committed_requests, 0);
  assert.equal(quotaBefore.reserved_requests, 0);

  const { default: handler } = await import("../src/handler.ts");
  let voyageCalls = 0;
  const response = await withFetchMock(
    () => {
      voyageCalls += 1;
      return voyageOkResponse(1);
    },
    () =>
      handler(
        new Request("https://ai.ubq.fi/v1/embeddings", {
          method: "POST",
          headers: {
            Authorization: `Bearer ${token}`,
            "Content-Type": "application/json",
          },
          body: JSON.stringify({ model: "voyage-4-large", input: "must-not-dispatch" }),
        }),
      ),
  );

  assert.equal(response.status, 404);
  assert.equal(response.headers.get("x-uos-upstream"), null);
  assert.equal(await responseErrorCode(response), "not_found");
  assert.equal(voyageCalls, 0);
  const quotaAfter = quotaWindow();
  assert.ok(quotaAfter);
  assert.equal(quotaAfter.committed_requests, 0);
  assert.equal(quotaAfter.reserved_requests, 0);
});

Deno.test("handler: idempotency principals survive allowlist token rotation and preserve account scopes", async () => {
  const { resolveIdempotencyPrincipal } = await import("../src/handler.ts");

  for (
    const kind of [
      "auth_tokens_allowlist",
      "admin_allowlist",
      "deno_deploy_token",
    ] as const
  ) {
    const first = await resolveIdempotencyPrincipal({
      token: "first-rotating-secret",
      method: { kind },
    });
    const rotated = await resolveIdempotencyPrincipal({
      token: "second-rotating-secret",
      method: { kind },
    });
    assert.equal(first, `auth-method:${kind}`);
    assert.equal(rotated, first);
    assert.equal(first.includes("rotating-secret"), false);
  }

  assert.equal(
    await resolveIdempotencyPrincipal({
      token: "kv-secret-one",
      method: { kind: "kv_api_key", key_id: "stable-key-id" },
    }),
    await resolveIdempotencyPrincipal({
      token: "kv-secret-two",
      method: { kind: "kv_api_key", key_id: "stable-key-id" },
    }),
  );
  assert.equal(
    await resolveIdempotencyPrincipal({
      token: "github-secret-one",
      method: {
        kind: "github_token",
        owner: "Ubiquity",
        repo: "AI.UBQ.FI",
        state_id: "state-one",
        limit_scope: "repo",
      },
    }),
    "github-repo:ubiquity/ai.ubq.fi",
  );
  assert.equal(
    await resolveIdempotencyPrincipal({
      token: "passkey-session-one",
      method: {
        kind: "passkey_session",
        user_id: "user-47",
        handle: "user",
        is_admin: false,
        credential_count: 1,
      },
    }),
    "passkey-user:user-47",
  );
});

addEventListener("unload", () => {
  (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = originalOpenKv;
  if (originalVoyageApiKey === undefined) Deno.env.delete("VOYAGEAI_API_KEY");
  else Deno.env.set("VOYAGEAI_API_KEY", originalVoyageApiKey);
});
