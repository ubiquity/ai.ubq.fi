import assert from "node:assert/strict";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);

const kvStore = new Map<string, unknown>();

const kvStub = {
  get: (key: Deno.KvKey) =>
    Promise.resolve(({ key, value: kvStore.get(keyToString(key)) ?? null }) as Deno.KvEntryMaybe<unknown>),
  set: (key: Deno.KvKey, value: unknown) => {
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
      set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
        kvStore.set(keyToString(key), value);
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        kvStore.delete(keyToString(key));
        return chain;
      },
      commit: () => Promise.resolve({ ok: true } as const),
    };
    return chain;
  },
  close: () => {},
} as unknown as Deno.Kv;

(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kvStub);

const { handleAdminCodexAuth } = await import("../src/admin.ts");
const { handleHealthAuth } = await import("../src/health.ts");

const authPayload = {
  tokens: {
    access_token: "access",
    refresh_token: "refresh",
    account_id: "acct",
  },
};

const makeRequest = (body: unknown): Request =>
  new Request("https://ai.ubq.fi/admin/codex/auth", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

Deno.test("admin codex auth stores CLI model snapshot as source of truth", async () => {
  kvStore.clear();
  const originalFetch = globalThis.fetch;
  const fetchUrls: string[] = [];

  globalThis.fetch = (input: RequestInfo | URL) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    fetchUrls.push(url);
    return Promise.resolve(
      new Response(JSON.stringify({ models: [{ slug: "stale-live-model" }] }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };

  try {
    const response = await handleAdminCodexAuth(
      makeRequest({
        auth: authPayload,
        models: {
          source: "codex_cli",
          client_version: "0.126.0",
          updated_at_ms: 123,
          models: [{
            slug: "gpt-5.5",
            display_name: "GPT-5.5",
            supported_reasoning_levels: ["low", "medium", "high", "xhigh"],
          }],
        },
      }),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as { models?: { count?: number; source?: string } };
    assert.equal(payload.models?.count, 1);
    assert.equal(payload.models?.source, "codex_cli");
    assert.equal(fetchUrls.length, 1);

    const stored = kvStore.get(keyToString(["ubq_ai", "codex_models"])) as
      | { source?: string; client_version?: string; models?: Array<{ slug?: string }> }
      | undefined;
    assert.equal(stored?.source, "codex_cli");
    assert.equal(stored?.client_version, "0.126.0");
    assert.deepEqual(stored?.models?.map((model) => model.slug), ["gpt-5.5"]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("admin codex auth rejects uploads without CLI model snapshot", async () => {
  kvStore.clear();
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => {
    throw new Error("auth upload without models should fail before upstream validation");
  };

  try {
    const response = await handleAdminCodexAuth(makeRequest({ auth: authPayload }));
    assert.equal(response.status, 400);
    const payload = await response.json() as { error?: { message?: string } };
    assert.match(payload.error?.message ?? "", /Codex CLI models array/);
    assert.equal(kvStore.has(keyToString(["ubq_ai", "codex_auth"])), false);
    assert.equal(kvStore.has(keyToString(["ubq_ai", "codex_models"])), false);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("health auth summary does not refresh Codex auth", async () => {
  kvStore.clear();
  kvStore.set(keyToString(["ubq_ai", "codex_auth"]), {
    access_token: "access",
    refresh_token: "refresh",
    account_id: "acct",
    updated_at_ms: Date.now(),
  });

  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => {
    throw new Error("/health/auth should not contact upstream auth");
  };

  try {
    const response = await handleHealthAuth();
    assert.equal(response.status, 200);
    const payload = await response.json() as {
      upstream?: string;
      auth?: { source?: string; access_token_expired?: boolean | null; refresh_recommended?: boolean | null };
    };
    assert.equal(payload.upstream, "chatgpt_codex");
    assert.equal(payload.auth?.source, "kv");
    assert.equal(payload.auth?.access_token_expired, null);
    assert.equal(payload.auth?.refresh_recommended, null);
  } finally {
    globalThis.fetch = originalFetch;
  }
});
