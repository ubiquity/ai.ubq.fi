import assert from "node:assert/strict";
import { keyToJSON } from "@deno/kv-utils/json";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const stringEntryLine = (key: Deno.KvKey, value: string): string =>
  JSON.stringify({
    key: keyToJSON(key),
    value: { type: "string", value },
    versionstamp: "00000000000000000000",
  });

let resetRuntimeCache = (): void => {};
class TestKvStore extends Map<string, unknown> {
  override clear(): void {
    super.clear();
    resetRuntimeCache();
  }
}
const kvStore = new TestKvStore();
let atomicCommitsToFail = 0;

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
        if (atomicCommitsToFail > 0) {
          atomicCommitsToFail -= 1;
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
  handleAdminApiKeysCreate,
  handleAdminApiKeysDelete,
  handleAdminApiKeysPaidFallbacks,
  handleAdminApiKeysUpdate,
  handleAdminCodexAuth,
  handleAdminDefaults,
  handleAdminKvMigrationImport,
} = await import("../src/admin.ts");
const { listApiKeyRequestLogs, recordApiKeyRequestLog } = await import("../src/analytics.ts");
const { handleHealthAuth } = await import("../src/health.ts");
const { buildRuntimeConfig, cacheRuntimeConfig, resetRuntimeConfigCacheForTest } = await import(
  "../src/runtime_config.ts"
);
resetRuntimeCache = resetRuntimeConfigCacheForTest;

const seedCodexSnapshot = (snapshot: Parameters<typeof buildRuntimeConfig>[0]): void => {
  kvStore.set(keyToString(["ubq_ai", "codex_models"]), snapshot);
  const runtime = buildRuntimeConfig(snapshot);
  kvStore.set(keyToString(["uos_ai", "runtime_config", "v2"]), runtime);
  cacheRuntimeConfig(runtime);
};

const authPayload = {
  tokens: {
    access_token: "access",
    refresh_token: "refresh",
    account_id: "acct",
  },
};

const DISABLED_FALLBACK_TOKEN = `u_${"a".repeat(64)}`;
const ENABLED_FALLBACK_TOKEN = `u_${"b".repeat(64)}`;
const FAILED_FALLBACK_TOKEN = `u_${"c".repeat(64)}`;

const makeRequest = (body: unknown): Request =>
  new Request("https://ai.ubq.fi/admin/codex/auth", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

Deno.test("admin defaults includes serializable YunWu quota diagnostics without credentials", async () => {
  kvStore.clear();
  const response = await handleAdminDefaults(
    new Request("https://ai.ubq.fi/admin/defaults"),
    {
      getYunwuQuotaDiagnostics: () =>
        Promise.resolve({
          configured: true,
          available: true,
          cache_state: "fresh",
          confidence: "refill_observed",
          balance_credits: 75,
          baseline_credits: 100,
          remaining_percent: 75,
          used_percent: 25,
          observed_at_ms: 2_000_000,
          cycle_started_at_ms: 1_000_000,
          last_known_debits_credits: 1,
          last_inferred_credit_credits: 50,
          last_credit_at_ms: 1_500_000,
          latest_refill_id: "refill-2",
          latest_refill_amount_credits: 50,
          latest_refill_completed_at_ms: 1_400_000,
        }),
    },
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as Record<string, unknown> & {
    yunwu_quota?: Record<string, unknown>;
  };
  assert.equal(Object.prototype.hasOwnProperty.call(payload, "ok"), false);
  assert.equal(payload.yunwu_quota?.remaining_percent, 75);
  assert.equal(payload.yunwu_quota?.latest_refill_id, "refill-2");
  assert.equal(Object.prototype.hasOwnProperty.call(payload.yunwu_quota ?? {}, "system_token"), false);
  assert.equal(Object.prototype.hasOwnProperty.call(payload.yunwu_quota ?? {}, "user_id"), false);
});

const yunwuMetadataResponse = (url: string): Response => {
  if (url === "https://yunwu.ai/api/ratio_config") {
    return new Response(
      JSON.stringify({
        success: true,
        data: {
          model_ratio: {
            "gpt-5.6-sol": 1,
            "not-in-codex-catalog": 1,
          },
          model_price: {},
        },
      }),
      { status: 200, headers: { "Content-Type": "application/json" } },
    );
  }
  if (url === "https://yunwu.ai/api/status") {
    return new Response(
      JSON.stringify({
        success: true,
        data: { setup: true, quota_per_unit: 500_000 },
      }),
      { status: 200, headers: { "Content-Type": "application/json" } },
    );
  }
  throw new Error(`Unexpected YunWu metadata URL: ${url}`);
};

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
            supported_reasoning_levels: [
              { effort: "low", description: "Fast responses" },
              { effort: "medium", description: "Balanced reasoning" },
              { effort: "high", description: "Greater reasoning depth" },
              { effort: "xhigh", description: "Extra high reasoning depth" },
              { effort: "max", description: "Maximum reasoning depth" },
              { effort: "ultra", description: "Maximum reasoning with automatic task delegation" },
            ],
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
          reasoning_effort_wire_map?: Record<string, string>;
        }>;
      }
      | undefined;
    assert.equal(stored?.source, "chatgpt_codex");
    assert.equal(stored?.client_version, "0.126.0");
    assert.deepEqual(stored?.models?.map((model) => model.slug), ["gpt-5.3-codex-spark"]);
    assert.equal(stored?.models?.[0]?.supported_in_api, false);
    assert.equal(stored?.models?.[0]?.default_reasoning_level, "high");
    assert.deepEqual(stored?.models?.[0]?.supported_reasoning_levels, [
      "none",
      "low",
      "medium",
      "high",
      "xhigh",
      "max",
      "ultra",
    ]);
    assert.deepEqual(stored?.models?.[0]?.reasoning_effort_wire_map, { ultra: "max" });
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

Deno.test("admin codex auth rotation replaces a prior account snapshot even at an older version", async () => {
  kvStore.clear();
  const originalFetch = globalThis.fetch;
  globalThis.fetch = (input: RequestInfo | URL) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    const version = new URL(url).searchParams.get("client_version") ?? "missing";
    return Promise.resolve(
      new Response(JSON.stringify({ models: [{ slug: `gpt-${version}`, rich_field: { preserved: true } }] }), {
        status: 200,
        headers: { "Content-Type": "application/json", ETag: `"${version}"` },
      }),
    );
  };

  try {
    const newer = await handleAdminCodexAuth(makeRequest({
      auth: authPayload,
      models: { client_version: "0.201.0" },
    }));
    assert.equal(newer.status, 200);
    const firstGeneration = kvStore.get(keyToString(["ubq_ai", "codex_catalog_auth_generation"]));
    assert.equal(typeof firstGeneration, "string");

    const older = await handleAdminCodexAuth(makeRequest({
      auth: authPayload,
      models: { client_version: "0.200.0" },
    }));
    assert.equal(older.status, 200);
    const olderPayload = await older.json() as { normalized_snapshot_updated?: boolean; ok?: boolean };
    assert.equal(olderPayload.normalized_snapshot_updated, true);
    assert.equal(Object.prototype.hasOwnProperty.call(olderPayload, "ok"), false);

    const secondGeneration = kvStore.get(keyToString(["ubq_ai", "codex_catalog_auth_generation"]));
    assert.equal(typeof secondGeneration, "string");
    assert.notEqual(secondGeneration, firstGeneration);
    const snapshot = kvStore.get(keyToString(["ubq_ai", "codex_models"])) as {
      client_version?: string;
      models?: Array<{ slug?: string }>;
    };
    assert.equal(snapshot.client_version, "0.200.0");
    assert.equal(snapshot.models?.[0]?.slug, "gpt-0.200.0");

    const seededMetadata = kvStore.get(keyToString(["ubq_ai", "codex_catalog", "0.200.0"])) as {
      auth_generation?: string;
      etag?: string;
    };
    assert.equal(seededMetadata.auth_generation, secondGeneration);
    assert.equal(seededMetadata.etag, '"0.200.0"');
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("admin codex auth retries transient snapshot contention", async () => {
  kvStore.clear();
  atomicCommitsToFail = 1;
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

    assert.equal(response.status, 200);
    assert.equal(kvStore.has(keyToString(["ubq_ai", "codex_auth"])), true);
    assert.equal(kvStore.has(keyToString(["ubq_ai", "codex_models"])), true);
  } finally {
    globalThis.fetch = originalFetch;
    atomicCommitsToFail = 0;
  }
});

Deno.test("admin codex auth fails atomically after snapshot contention retries are exhausted", async () => {
  kvStore.clear();
  atomicCommitsToFail = 3;
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
        models: { client_version: "0.126.0" },
      }),
    );

    assert.equal(response.status, 500);
    assert.equal(kvStore.has(keyToString(["ubq_ai", "codex_auth"])), false);
    assert.equal(kvStore.has(keyToString(["ubq_ai", "codex_models"])), false);
  } finally {
    globalThis.fetch = originalFetch;
    atomicCommitsToFail = 0;
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
  seedCodexSnapshot({
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
  assert.equal(
    (kvStore.get(keyToString(["uos_ai", "runtime_config", "v2"])) as { default_reasoning_effort?: string })
      .default_reasoning_effort,
    "none",
  );
});

Deno.test("admin defaults accepts a tier advertised by the Codex CLI catalog", async () => {
  kvStore.clear();
  seedCodexSnapshot({
    source: "codex_cli",
    updated_at_ms: 123,
    models: [{
      slug: "gpt-5.6-sol",
      display_name: "GPT-5.6 Sol",
      default_reasoning_level: "medium",
      supported_reasoning_levels: ["low", "medium", "high", "xhigh", "max", "ultra"],
    }],
  });

  const response = await handleAdminDefaults(
    new Request("https://ai.ubq.fi/admin/defaults", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: "gpt-5.6-sol",
        reasoning_effort: "ultra",
      }),
    }),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as { defaults?: { reasoning_effort?: string } };
  assert.equal(payload.defaults?.reasoning_effort, "ultra");
  assert.equal(
    (kvStore.get(keyToString(["uos_ai", "runtime_config", "v2"])) as { default_reasoning_effort?: string })
      .default_reasoning_effort,
    "ultra",
  );
});

Deno.test("admin defaults does not reject an unlisted reasoning tier", async () => {
  kvStore.clear();
  seedCodexSnapshot({
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
        reasoning_effort: "future-tier",
      }),
    }),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as { defaults?: { reasoning_effort?: string } };
  assert.equal(payload.defaults?.reasoning_effort, "future-tier");
  assert.equal(
    (kvStore.get(keyToString(["uos_ai", "runtime_config", "v2"])) as { default_reasoning_effort?: string })
      .default_reasoning_effort,
    "future-tier",
  );
});

Deno.test("admin defaults rejects null reasoning effort", async () => {
  kvStore.clear();
  seedCodexSnapshot({
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
  assert.match(payload.error?.message ?? "", /reasoning_effort must be a non-empty string/);
  assert.equal(kvStore.has(keyToString(["default", "reasoning_effort"])), false);
});

Deno.test("paid fallback pricing initializes only when a key becomes enabled", async () => {
  kvStore.clear();
  seedCodexSnapshot({
    source: "chatgpt_codex",
    updated_at_ms: Date.now(),
    models: [
      { slug: "gpt-5.6-sol" },
      { slug: "codex-only-model" },
    ],
  });

  const originalFetch = globalThis.fetch;
  const originalApiKey = Deno.env.get("YUNWU_API_KEY");
  const metadataUrls: string[] = [];
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  globalThis.fetch = (input: RequestInfo | URL) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    metadataUrls.push(url);
    return Promise.resolve(yunwuMetadataResponse(url));
  };

  try {
    const createResponse = await handleAdminApiKeysCreate(
      new Request("https://ai.ubq.fi/admin/api-keys", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          name: "Overflow disabled",
          token: DISABLED_FALLBACK_TOKEN,
          paid_fallback_enabled: false,
          paid_fallback_limit_credits: 2,
        }),
      }),
    );
    assert.equal(createResponse.status, 200);
    assert.equal(metadataUrls.length, 0);
    const created = await createResponse.json() as {
      id: string;
      paid_fallback_enabled: boolean;
      paid_fallback_limit_credits: number;
      paid_fallback_pricing_checked_at_ms: number | null;
    };
    assert.equal(created.paid_fallback_enabled, false);
    assert.equal(created.paid_fallback_limit_credits, 2);
    assert.equal(created.paid_fallback_pricing_checked_at_ms, null);

    const enableResponse = await handleAdminApiKeysUpdate(
      new Request("https://ai.ubq.fi/admin/api-keys", {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ id: created.id, paid_fallback_enabled: true }),
      }),
    );
    assert.equal(enableResponse.status, 200);
    assert.deepEqual(metadataUrls.sort(), [
      "https://yunwu.ai/api/ratio_config",
      "https://yunwu.ai/api/status",
    ]);
    const enabled = await enableResponse.json() as {
      paid_fallback_enabled: boolean;
      paid_fallback_model_ids: string[];
      paid_fallback_pricing_checked_at_ms: number | null;
    };
    assert.equal(enabled.paid_fallback_enabled, true);
    assert.deepEqual(enabled.paid_fallback_model_ids, ["gpt-5.6-sol"]);
    assert.equal(typeof enabled.paid_fallback_pricing_checked_at_ms, "number");

    metadataUrls.length = 0;
    const capResponse = await handleAdminApiKeysUpdate(
      new Request("https://ai.ubq.fi/admin/api-keys", {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ id: created.id, paid_fallback_limit_credits: -1 }),
      }),
    );
    assert.equal(capResponse.status, 200);
    assert.equal(metadataUrls.length, 0);
    const unlimited = await capResponse.json() as { paid_fallback_limit_credits: number };
    assert.equal(unlimited.paid_fallback_limit_credits, -1);
    const storedUnlimitedId = kvStore.get(keyToString(["ubq_ai", "api_keys", "id", created.id])) as {
      hash: string;
      paid_fallback_limit_microcredits: number;
    };
    const storedUnlimitedHash = kvStore.get(
      keyToString(["ubq_ai", "api_keys", "hash", storedUnlimitedId.hash]),
    ) as { paid_fallback_limit_microcredits: number };
    assert.equal(storedUnlimitedId.paid_fallback_limit_microcredits, -1);
    assert.equal(storedUnlimitedHash.paid_fallback_limit_microcredits, -1);

    const disableResponse = await handleAdminApiKeysUpdate(
      new Request("https://ai.ubq.fi/admin/api-keys", {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ id: created.id, paid_fallback_enabled: false }),
      }),
    );
    assert.equal(disableResponse.status, 200);
    assert.equal(metadataUrls.length, 0);

    const reenableResponse = await handleAdminApiKeysUpdate(
      new Request("https://ai.ubq.fi/admin/api-keys", {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ id: created.id, paid_fallback_enabled: true }),
      }),
    );
    assert.equal(reenableResponse.status, 200);
    assert.deepEqual(metadataUrls.sort(), [
      "https://yunwu.ai/api/ratio_config",
      "https://yunwu.ai/api/status",
    ]);
  } finally {
    globalThis.fetch = originalFetch;
    if (originalApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalApiKey);
  }
});

Deno.test("API key creation rejects custom tokens outside the v2 routable shape", async () => {
  kvStore.clear();
  const response = await handleAdminApiKeysCreate(
    new Request("https://ai.ubq.fi/admin/api-keys", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        name: "Legacy custom token",
        token: "legacy_custom_token_that_was_previously_accepted",
      }),
    }),
  );
  assert.equal(response.status, 400);
  const payload = await response.json() as { error?: { message?: string } };
  assert.match(payload.error?.message ?? "", /u_ prefix followed by 64 lowercase hexadecimal/);
  assert.equal([...kvStore.keys()].some((key) => key.includes('"api_keys"')), false);
});

Deno.test("enabled key creation initializes once and failed enable leaves the key disabled", async () => {
  kvStore.clear();
  seedCodexSnapshot({
    source: "chatgpt_codex",
    updated_at_ms: Date.now(),
    models: [{ slug: "gpt-5.6-sol" }],
  });

  const originalFetch = globalThis.fetch;
  const originalApiKey = Deno.env.get("YUNWU_API_KEY");
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  let metadataCalls = 0;
  globalThis.fetch = (input: RequestInfo | URL) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    metadataCalls += 1;
    return Promise.resolve(yunwuMetadataResponse(url));
  };

  try {
    const enabledCreate = await handleAdminApiKeysCreate(
      new Request("https://ai.ubq.fi/admin/api-keys", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          name: "Overflow enabled",
          token: ENABLED_FALLBACK_TOKEN,
          paid_fallback_enabled: true,
          paid_fallback_limit_credits: 1,
        }),
      }),
    );
    assert.equal(enabledCreate.status, 200);
    assert.equal(metadataCalls, 2);
    const enabledPayload = await enabledCreate.json() as Record<string, unknown>;
    assert.equal(Object.prototype.hasOwnProperty.call(enabledPayload, "ok"), false);
    assert.deepEqual(enabledPayload.paid_fallback_model_ids, ["gpt-5.6-sol"]);

    metadataCalls = 0;
    const duplicateCreate = await handleAdminApiKeysCreate(
      new Request("https://ai.ubq.fi/admin/api-keys", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          name: "Duplicate overflow key",
          token: ENABLED_FALLBACK_TOKEN,
          paid_fallback_enabled: true,
          paid_fallback_limit_credits: 1,
        }),
      }),
    );
    assert.equal(duplicateCreate.status, 409);
    assert.equal(metadataCalls, 0);

    const disabledCreate = await handleAdminApiKeysCreate(
      new Request("https://ai.ubq.fi/admin/api-keys", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          name: "Enable failure",
          token: FAILED_FALLBACK_TOKEN,
          paid_fallback_enabled: false,
          paid_fallback_limit_credits: 1,
        }),
      }),
    );
    const disabledPayload = await disabledCreate.json() as { id: string };

    globalThis.fetch = (input: RequestInfo | URL) => {
      const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
      if (url.endsWith("/api/status")) {
        return Promise.resolve(new Response("upstream unavailable", { status: 503 }));
      }
      return Promise.resolve(yunwuMetadataResponse(url));
    };
    const failedEnable = await handleAdminApiKeysUpdate(
      new Request("https://ai.ubq.fi/admin/api-keys", {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ id: disabledPayload.id, paid_fallback_enabled: true }),
      }),
    );
    assert.equal(failedEnable.status, 502);
    const stored = kvStore.get(keyToString(["ubq_ai", "api_keys", "id", disabledPayload.id])) as {
      paid_fallback_enabled?: boolean;
      paid_fallback_pricing_checked_at_ms?: number | null;
    };
    assert.equal(stored.paid_fallback_enabled, false);
    assert.equal(stored.paid_fallback_pricing_checked_at_ms, null);
  } finally {
    globalThis.fetch = originalFetch;
    if (originalApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalApiKey);
  }
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
    provider: "voyage",
  });

  const newest = await listApiKeyRequestLogs(keyId, { limit: 1 });
  assert.equal(newest.length, 1);
  assert.equal(newest[0].created_at_ms, 2_000);
  assert.equal(newest[0].method, "POST");

  const response = await handleAdminApiKeysPaidFallbacks(
    new Request(`https://ai.ubq.fi/admin/api-keys/${keyId}/paid-fallbacks?limit=20`),
    keyId,
  );
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("cache-control"), "no-store");
  const payload = await response.json() as {
    ok?: boolean;
    object?: string;
    data?: Array<{
      created_at_ms?: number;
      model?: string | null;
      reasoning?: string | null;
      provider?: string;
    }>;
  };
  assert.equal(Object.prototype.hasOwnProperty.call(payload, "ok"), false);
  assert.equal(payload.object, "list");
  assert.deepEqual(payload.data?.map((entry) => entry.created_at_ms), [2_000, 1_000]);
  assert.equal(payload.data?.[1]?.model, "gpt-5.6-sol");
  assert.equal(payload.data?.[1]?.reasoning, "max");
  assert.equal(payload.data?.[0]?.provider, "voyage");
});

Deno.test("authenticated UOS embeddings do not write ordinary request history", async () => {
  kvStore.clear();
  kvStore.set(keyToString(["uos_ai", "voyage_api_key"]), "voyage-test-key");
  const token = `u_${"a".repeat(64)}`;
  const createdResponse = await handleAdminApiKeysCreate(
    new Request("https://ai.ubq.fi/admin/api-keys", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        name: "Voyage analytics route",
        token,
        usage_limit_requests: -1,
        paid_fallback_enabled: false,
      }),
    }),
  );
  assert.equal(createdResponse.status, 200);
  const created = await createdResponse.json() as { id?: unknown };
  assert.equal(typeof created.id, "string");
  const keyId = created.id as string;

  const originalFetch = globalThis.fetch;
  globalThis.fetch = (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    assert.equal(url, "https://api.voyageai.com/v1/embeddings");
    const body = JSON.parse(typeof init?.body === "string" ? init.body : "null") as Record<string, unknown>;
    assert.equal(body.model, "voyage-4-large");
    assert.equal(body.input_type, "document");
    assert.equal(body.output_dimension, 1024);
    assert.equal(body.output_dtype, "float");
    assert.equal(body.truncation, false);
    return Promise.resolve(
      new Response(
        JSON.stringify({
          data: [{ embedding: Array.from({ length: 1024 }, (_, index) => index / 1024) }],
          usage: { total_tokens: 3 },
        }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      ),
    );
  };

  try {
    const { default: handler } = await import("../src/handler.ts");
    const response = await handler(
      new Request("https://ai.ubq.fi/uos/embeddings", {
        method: "POST",
        headers: {
          "Authorization": `Bearer ${token}`,
          "Content-Type": "application/json",
        },
        body: JSON.stringify({
          model: "voyage-4-large",
          input: "analytics provider proof",
          input_type: "document",
          dimensions: 1024,
          truncation: false,
          encoding_format: "float",
        }),
      }),
    );
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-ubq-upstream"), "voyage");

    assert.deepEqual(await listApiKeyRequestLogs(keyId, { limit: 10 }), []);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("API key request log endpoint validates key existence and limit", async () => {
  kvStore.clear();
  const missing = await handleAdminApiKeysPaidFallbacks(
    new Request("https://ai.ubq.fi/admin/api-keys/missing/paid-fallbacks?limit=20"),
    "missing",
  );
  assert.equal(missing.status, 404);

  const keyId = "existing";
  kvStore.set(keyToString(["ubq_ai", "api_keys", "id", keyId]), { id: keyId });
  const invalidLimit = await handleAdminApiKeysPaidFallbacks(
    new Request(`https://ai.ubq.fi/admin/api-keys/${keyId}/paid-fallbacks?limit=not-a-number`),
    keyId,
  );
  assert.equal(invalidLimit.status, 400);
});

Deno.test("deleting a revoked API key removes its mirrored policy and analytics", async () => {
  kvStore.clear();
  const keyId = "key-delete-cleanup";
  const hash = "hash-delete-cleanup";
  const commonPolicy = {
    paid_fallback_enabled: false,
    paid_fallback_limit_microcredits: 2_000_000,
    paid_fallback_spent_microcredits: 0,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
  };
  kvStore.set(keyToString(["ubq_ai", "api_keys", "id", keyId]), {
    id: keyId,
    name: "Delete cleanup",
    prefix: "u_delete",
    hash,
    created_at_ms: Date.now() - 10_000,
    expires_at_ms: -1,
    revoked_at_ms: Date.now() - 1_000,
    usage_limit_requests: 50,
    usage_requests: 1,
    usage_reset_at_ms: Date.now() + 60_000,
    window_ms: 60_000,
    ...commonPolicy,
    paid_fallback_model_ids: [],
    paid_fallback_quota_per_credit: 0,
    paid_fallback_pricing_checked_at_ms: null,
  });
  kvStore.set(keyToString(["ubq_ai", "api_keys", "hash", hash]), {
    id: keyId,
    expires_at_ms: -1,
    revoked_at_ms: Date.now() - 1_000,
    usage_limit_requests: 50,
    usage_requests: 1,
    usage_reset_at_ms: Date.now() + 60_000,
    window_ms: 60_000,
    ...commonPolicy,
  });
  kvStore.set(keyToString(["ubq_ai", "api_keys", "usage", keyId]), { key_id: keyId });
  kvStore.set(keyToString(["ubq_ai", "api_keys", "usage_daily", keyId]), { key_id: keyId, days: [] });
  kvStore.set(
    keyToString(["uos_ai", "paid_fallback", "ledger", keyId, Date.now(), "request-delete"]),
    { id: "request-delete", key_id: keyId },
  );

  const response = await handleAdminApiKeysDelete(
    new Request("https://ai.ubq.fi/admin/api-keys", {
      method: "DELETE",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id: keyId }),
    }),
  );
  assert.equal(response.status, 200);
  assert.deepEqual(await response.json(), { id: keyId });
  for (const encodedKey of kvStore.keys()) {
    assert.equal(encodedKey.includes(keyId), false);
    assert.equal(encodedKey.includes(hash), false);
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
