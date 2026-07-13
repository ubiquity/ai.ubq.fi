import assert from "node:assert/strict";
import { keyToJSON } from "@deno/kv-utils/json";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const stringEntryLine = (key: Deno.KvKey, value: string): string =>
  JSON.stringify({
    key: keyToJSON(key),
    value: { type: "string", value },
    versionstamp: "00000000000000000000",
  });

const kvStore = new Map<string, unknown>();
let failNextAtomicCommit = false;

const compareKvKeyPart = (left: Deno.KvKeyPart, right: Deno.KvKeyPart): number => {
  if (left === right) return 0;
  if (typeof left === "number" && typeof right === "number") return left - right;
  const leftText = String(left);
  const rightText = String(right);
  return leftText < rightText ? -1 : 1;
};

const compareKvKeys = (left: Deno.KvKey, right: Deno.KvKey): number => {
  const maxLength = Math.max(left.length, right.length);
  for (let index = 0; index < maxLength; index += 1) {
    if (index >= left.length) return -1;
    if (index >= right.length) return 1;
    const comparison = compareKvKeyPart(left[index], right[index]);
    if (comparison !== 0) return comparison;
  }
  return 0;
};

const matchesPrefix = (key: Deno.KvKey, prefix: Deno.KvKey): boolean =>
  prefix.every((part, index) => key[index] === part);

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
  list: async function* (selector: Deno.KvListSelector, options: Deno.KvListOptions = {}) {
    const prefix = "prefix" in selector ? selector.prefix : [];
    let entries = [...kvStore.entries()]
      .map(([encodedKey, value]) => ({
        key: JSON.parse(encodedKey) as Deno.KvKey,
        value,
        versionstamp: "00000000000000000000",
      }))
      .filter((entry) => matchesPrefix(entry.key, prefix))
      .sort((left, right) => compareKvKeys(left.key, right.key));
    if (options.reverse) entries = entries.reverse();
    if (typeof options.limit === "number") entries = entries.slice(0, options.limit);
    for (const entry of entries) yield entry;
  },
  atomic: () => {
    const ops: Array<{ type: "set" | "delete"; key: Deno.KvKey; value?: unknown }> = [];
    const chain = {
      check: () => chain,
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
          failNextAtomicCommit = false;
          return Promise.resolve({ ok: false } as const);
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

const {
  handleAdminApiKeysRequests,
  handleAdminCodexAuth,
  handleAdminDefaults,
  handleAdminKvMigrationImport,
} = await import("../src/admin.ts");
const { listApiKeyRequestLogs, recordApiKeyRequestLog } = await import("../src/analytics.ts");
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

Deno.test("admin codex auth stores live upstream model catalog as source of truth", async () => {
  kvStore.clear();
  const originalFetch = globalThis.fetch;
  const fetchUrls: string[] = [];

  globalThis.fetch = (input: RequestInfo | URL) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    fetchUrls.push(url);
    return Promise.resolve(
      new Response(
        JSON.stringify({
          models: [{
            slug: "gpt-5.3-codex-spark",
            display_name: "GPT-5.3-Codex-Spark",
            visibility: "list",
            supported_in_api: false,
            default_reasoning_level: "high",
            supported_reasoning_levels: ["low", "medium", "high", "xhigh", "max", "ultra"],
          }, {
            slug: "codex-auto-review",
            display_name: "Codex Auto Review",
            visibility: "hide",
            supported_in_api: true,
          }],
        }),
        {
          status: 200,
          headers: { "Content-Type": "application/json" },
        },
      ),
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
            slug: "stale-local-model",
            display_name: "Stale Local Model",
            context_window: 272000,
            max_context_window: 1000000,
            auto_compact_token_limit: null,
            supported_reasoning_levels: [{ effort: null }, "low", "medium", "high", "xhigh"],
          }, {
            slug: "codex-auto-review",
            display_name: "Codex Auto Review",
            visibility: "hide",
            supported_in_api: true,
          }],
        },
      }),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as { models?: { count?: number; source?: string } };
    assert.equal(payload.models?.count, 1);
    assert.equal(payload.models?.source, "chatgpt_codex");
    assert.equal(fetchUrls.length, 1);

    const stored = kvStore.get(keyToString(["ubq_ai", "codex_models"])) as
      | {
        source?: string;
        client_version?: string;
        models?: Array<{
          slug?: string;
          context_window?: number;
          max_context_window?: number;
          auto_compact_token_limit?: number | null;
          supported_in_api?: boolean;
          default_reasoning_level?: string;
          supported_reasoning_levels?: string[];
        }>;
      }
      | undefined;
    assert.equal(stored?.source, "chatgpt_codex");
    assert.equal(stored?.client_version, "0.126.0");
    assert.deepEqual(stored?.models?.map((model) => model.slug), ["gpt-5.3-codex-spark"]);
    assert.equal(stored?.models?.[0]?.supported_in_api, false);
    assert.equal(stored?.models?.[0]?.default_reasoning_level, "high");
    assert.deepEqual(stored?.models?.[0]?.supported_reasoning_levels, ["low", "medium", "high", "xhigh", "max"]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("admin codex auth stores live model catalog without caller model snapshot", async () => {
  kvStore.clear();
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(JSON.stringify({ models: [{ slug: "gpt-5.3-codex-spark", visibility: "list" }] }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
    );

  try {
    const response = await handleAdminCodexAuth(makeRequest({ auth: authPayload }));
    assert.equal(response.status, 200);
    const stored = kvStore.get(keyToString(["ubq_ai", "codex_models"])) as { models?: Array<{ slug?: string }> };
    assert.deepEqual(stored?.models?.map((model) => model.slug), ["gpt-5.3-codex-spark"]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("admin codex auth stores auth and model snapshot atomically", async () => {
  kvStore.clear();
  failNextAtomicCommit = true;
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(JSON.stringify({ models: [{ slug: "gpt-5.5" }] }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
    );

  try {
    const response = await handleAdminCodexAuth(
      makeRequest({
        auth: authPayload,
        models: {
          source: "codex_cli",
          client_version: "0.126.0",
          updated_at_ms: 123,
          models: [{ slug: "gpt-5.5", display_name: "GPT-5.5" }],
        },
      }),
    );

    assert.equal(response.status, 500);
    assert.equal(kvStore.has(keyToString(["ubq_ai", "codex_auth"])), false);
    assert.equal(kvStore.has(keyToString(["ubq_ai", "codex_models"])), false);
  } finally {
    globalThis.fetch = originalFetch;
    failNextAtomicCommit = false;
  }
});

Deno.test("admin KV migration import stays dry-run unless write is explicit", async () => {
  kvStore.clear();

  const response = await handleAdminKvMigrationImport(
    new Request("https://ai.ubq.fi/admin/kv-migration/import?profile=prod&dry_run=false&overwrite=true", {
      method: "POST",
      body: stringEntryLine(["default", "model"], "gpt-5.5"),
    }),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as { dry_run?: boolean; imported?: number };
  assert.equal(payload.dry_run, true);
  assert.equal(payload.imported, 1);
  assert.equal(kvStore.has(keyToString(["default", "model"])), false);
});

Deno.test("admin defaults accepts none when the model supports none", async () => {
  kvStore.clear();
  kvStore.set(keyToString(["ubq_ai", "codex_models"]), {
    source: "codex_cli",
    updated_at_ms: 123,
    models: [{
      slug: "gpt-5.5",
      display_name: "GPT-5.5",
      default_reasoning_level: "medium",
      supported_reasoning_levels: ["none", "low", "medium", "high", "xhigh"],
    }],
  });

  const response = await handleAdminDefaults(
    new Request("https://ai.ubq.fi/admin/defaults", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: "gpt-5.5",
        reasoning_effort: "none",
      }),
    }),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as { defaults?: { reasoning_effort?: string } };
  assert.equal(payload.defaults?.reasoning_effort, "none");
  assert.equal(kvStore.get(keyToString(["default", "reasoning_effort"])), "none");
});

Deno.test("admin defaults rejects none when the model does not support none", async () => {
  kvStore.clear();
  kvStore.set(keyToString(["ubq_ai", "codex_models"]), {
    source: "codex_cli",
    updated_at_ms: 123,
    models: [{
      slug: "gpt-5.5",
      display_name: "GPT-5.5",
      default_reasoning_level: "medium",
      supported_reasoning_levels: ["low", "medium", "high", "xhigh"],
    }],
  });

  const response = await handleAdminDefaults(
    new Request("https://ai.ubq.fi/admin/defaults", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: "gpt-5.5",
        reasoning_effort: "none",
      }),
    }),
  );

  assert.equal(response.status, 400);
  const payload = await response.json() as { error?: { message?: string } };
  assert.match(payload.error?.message ?? "", /reasoning_effort must be one of: low, medium, high, xhigh/);
  assert.equal(kvStore.has(keyToString(["default", "reasoning_effort"])), false);
});

Deno.test("admin defaults rejects null reasoning effort", async () => {
  kvStore.clear();
  kvStore.set(keyToString(["ubq_ai", "codex_models"]), {
    source: "codex_cli",
    updated_at_ms: 123,
    models: [{
      slug: "gpt-5.5",
      display_name: "GPT-5.5",
      default_reasoning_level: "medium",
      supported_reasoning_levels: ["none", "low", "medium", "high", "xhigh"],
    }],
  });

  const response = await handleAdminDefaults(
    new Request("https://ai.ubq.fi/admin/defaults", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: "gpt-5.5",
        reasoning_effort: null,
      }),
    }),
  );

  assert.equal(response.status, 400);
  const payload = await response.json() as { error?: { message?: string } };
  assert.match(payload.error?.message ?? "", /reasoning_effort must be a string/);
  assert.equal(kvStore.has(keyToString(["default", "reasoning_effort"])), false);
});

Deno.test("API key request logs are retained newest-first and exposed to admins", async () => {
  kvStore.clear();
  const keyId = "4ba83596-d68e-447a-9281-0f1c92e8a87e";
  kvStore.set(keyToString(["ubq_ai", "api_keys", "id", keyId]), {
    id: keyId,
    name: "Test key",
  });

  await recordApiKeyRequestLog(keyId, {
    route: "responses",
    path: "/v1/responses",
    method: "post",
    status_code: 200,
    stream: true,
    model: "gpt-5.6-sol",
    reasoning: "max",
    created_at_ms: 1_000,
  });
  await recordApiKeyRequestLog(keyId, {
    route: "chat.completions",
    path: "/v1/chat/completions",
    method: "post",
    status_code: 400,
    stream: false,
    model: "gpt-5.6-luna",
    reasoning: "high",
    created_at_ms: 2_000,
  });

  const newest = await listApiKeyRequestLogs(keyId, { limit: 1 });
  assert.equal(newest.length, 1);
  assert.equal(newest[0].created_at_ms, 2_000);
  assert.equal(newest[0].method, "POST");

  const response = await handleAdminApiKeysRequests(
    new Request(`https://ai.ubq.fi/admin/api-keys/${keyId}/requests?limit=20`),
    keyId,
  );
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("cache-control"), "no-store");
  const payload = await response.json() as {
    ok?: boolean;
    object?: string;
    data?: Array<{ created_at_ms?: number; model?: string | null; reasoning?: string | null }>;
  };
  assert.equal(Object.prototype.hasOwnProperty.call(payload, "ok"), false);
  assert.equal(payload.object, "list");
  assert.deepEqual(payload.data?.map((entry) => entry.created_at_ms), [2_000, 1_000]);
  assert.equal(payload.data?.[1]?.model, "gpt-5.6-sol");
  assert.equal(payload.data?.[1]?.reasoning, "max");
});

Deno.test("API key request log endpoint validates key existence and limit", async () => {
  kvStore.clear();
  const missing = await handleAdminApiKeysRequests(
    new Request("https://ai.ubq.fi/admin/api-keys/missing/requests?limit=20"),
    "missing",
  );
  assert.equal(missing.status, 404);

  const keyId = "existing";
  kvStore.set(keyToString(["ubq_ai", "api_keys", "id", keyId]), { id: keyId });
  const invalidLimit = await handleAdminApiKeysRequests(
    new Request(`https://ai.ubq.fi/admin/api-keys/${keyId}/requests?limit=not-a-number`),
    keyId,
  );
  assert.equal(invalidLimit.status, 400);
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
