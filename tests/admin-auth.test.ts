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

const { handleAdminCodexAuth, handleAdminDefaults, handleAdminKvMigrationImport } = await import("../src/admin.ts");
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
    assert.equal(payload.models?.source, "codex_cli");
    assert.equal(fetchUrls.length, 1);

    const stored = kvStore.get(keyToString(["ubq_ai", "codex_models"])) as
      | {
        source?: string;
        client_version?: string;
        models?: Array<{ slug?: string; supported_reasoning_levels?: string[] }>;
      }
      | undefined;
    assert.equal(stored?.source, "codex_cli");
    assert.equal(stored?.client_version, "0.126.0");
    assert.deepEqual(stored?.models?.map((model) => model.slug), ["gpt-5.5"]);
    assert.deepEqual(stored?.models?.[0]?.supported_reasoning_levels, ["none", "low", "medium", "high", "xhigh"]);
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

Deno.test("admin defaults accepts none for reasoning models", async () => {
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
