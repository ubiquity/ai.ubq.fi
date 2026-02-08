import assert from "node:assert/strict";
import { DEFAULT_MODEL_KEY, DEFAULT_REASONING_EFFORT_KEY } from "../src/defaults.ts";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);

const kvStore = new Map<string, unknown>();
// Keep these in sync with tests/openai-compat.test.ts so whichever test imports
// src/openai.ts first doesn't change behavior.
kvStore.set(keyToString(DEFAULT_MODEL_KEY), "gpt-5.2");
kvStore.set(keyToString(DEFAULT_REASONING_EFFORT_KEY), "low");
kvStore.set(keyToString(["ubq_ai", "codex_auth"]), {
  access_token: "access",
  refresh_token: "refresh",
  account_id: "acct",
  updated_at_ms: Date.now(),
});
kvStore.set(keyToString(["uos_ai", "voyage_api_key"]), "voyage_test_key");

const originalOpenKv = (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv;

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
  list: async function* (_selector: Deno.KvListSelector, _options?: Deno.KvListOptions) {
    yield* [];
  },
  atomic: () => {
    const chain = {
      check: () => chain,
      set: () => chain,
      delete: () => chain,
      commit: () => Promise.resolve({ ok: true } as const),
    };
    return chain;
  },
  close: () => {},
} as unknown as Deno.Kv;

(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kvStub);

const { handleEmbeddings } = await import("../src/openai.ts");

const withFetchMock = async <T>(
  handler: (url: string, bodyText: string | null, headers: Headers) => Response | Promise<Response>,
  fn: () => Promise<T>,
): Promise<T> => {
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
  }
};

const voyageOkResponse = (count: number): Response => {
  const vectors = Array.from({ length: count }, (_, i) => ({
    embedding: [i + 0.1, i + 0.2, i + 0.3],
  }));
  return new Response(JSON.stringify({ data: vectors }), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
};

Deno.test("embeddings: normalizes string input", async () => {
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
  assert.ok(Array.isArray(payload.data));
  assert.equal(payload.data.length, 1);
  assert.equal(payload.data[0]?.object, "embedding");
  assert.equal(payload.data[0]?.index, 0);
  assert.ok(Array.isArray(payload.data[0]?.embedding));
});

Deno.test("embeddings: returns one data item per array input", async () => {
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

if (originalOpenKv) {
  Deno.test("embeddings: restore openKv", () => {
    (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = originalOpenKv;
  });
}
