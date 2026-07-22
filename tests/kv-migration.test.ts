import assert from "node:assert/strict";
import { keyToJSON } from "@deno/kv-utils/json";
import { appendBooleanParam } from "../scripts/kv-migrate.ts";
import {
  classifyKvMigrationKey,
  importKvMigrationLines,
  migrateKvReadIncidentV2,
  validateKvMigrationTarget,
} from "../src/kv_migration.ts";

if (typeof Deno.KvU64 !== "function") {
  (Deno as unknown as { KvU64: typeof Deno.KvU64 }).KvU64 = class {
    constructor(readonly value: bigint) {}
  } as typeof Deno.KvU64;
}

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);

const simpleValueToJson = (value: unknown): unknown => {
  if (value === null) return { type: "null", value: null };
  if (Array.isArray(value)) return { type: "Array", value: value.map(simpleValueToJson) };
  if (typeof value === "object") {
    const entries = Object.entries(value as Record<string, unknown>).map((
      [key, item],
    ) => [key, simpleValueToJson(item)]);
    return { type: "object", value: Object.fromEntries(entries) };
  }
  return { type: typeof value, value };
};

const entryLine = (key: Deno.KvKey, value: unknown): string =>
  JSON.stringify({
    key: keyToJSON(key),
    value: simpleValueToJson(value),
    versionstamp: "00000000000000000000",
  });

const makeKvStub = (store: Map<string, unknown>): Deno.Kv =>
  ({
    get: (key: Deno.KvKey) =>
      Promise.resolve(({ key, value: store.get(keyToString(key)) ?? null }) as Deno.KvEntryMaybe<unknown>),
    set: (key: Deno.KvKey, value: unknown) => {
      store.set(keyToString(key), value);
      return Promise.resolve({ ok: true } as const);
    },
    atomic: () => {
      const writes: Array<{ key: Deno.KvKey; value: unknown }> = [];
      const operation = {
        set: (key: Deno.KvKey, value: unknown) => {
          writes.push({ key, value });
          return operation;
        },
        commit: () => {
          for (const write of writes) store.set(keyToString(write.key), write.value);
          return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" });
        },
      };
      return operation;
    },
    list: async function* (selector: Deno.KvListSelector, options?: Deno.KvListOptions) {
      const prefix = "prefix" in selector ? selector.prefix : [];
      const limit = typeof options?.limit === "number" ? options.limit : Infinity;
      let yielded = 0;
      for (const [rawKey, value] of store.entries()) {
        const key = JSON.parse(rawKey) as Deno.KvKey;
        if (!prefix.every((part, index) => key[index] === part)) continue;
        yield { key, value, versionstamp: "00000000000000000000" } as Deno.KvEntry<unknown>;
        yielded += 1;
        if (yielded >= limit) break;
      }
    },
  }) as unknown as Deno.Kv;

Deno.test("KV migration HTTP CLI serializes explicit false flags", () => {
  const url = new URL("https://ai.ubq.fi/admin/kv-migration/import");
  appendBooleanParam(url, "include_legacy", false);
  appendBooleanParam(url, "overwrite", true);

  assert.equal(url.searchParams.get("include_legacy"), "0");
  assert.equal(url.searchParams.get("overwrite"), "1");
});

Deno.test("KV migration classifies v2 incident state and skips the transient circuit", () => {
  const options = { profile: "prod", includeCache: false, includeLegacy: false } as const;
  assert.equal(classifyKvMigrationKey(["uos_ai", "api_key_usage", "v2", "id"], options).action, "import");
  assert.equal(classifyKvMigrationKey(["uos_ai", "paid_fallback", "ledger", "id"], options).action, "import");
  assert.equal(classifyKvMigrationKey(["uos_ai", "runtime_config", "v2"], options).action, "import");
  assert.deepEqual(classifyKvMigrationKey(["uos_ai", "codex_rate_limit"], options), {
    action: "skip",
    group: "codex_rate_limit",
    reason: "transient_runtime_state",
  });
});

Deno.test("prod KV migration imports only modern durable rows by default", async () => {
  const store = new Map<string, unknown>();
  const result = await importKvMigrationLines(makeKvStub(store), [
    entryLine(["default", "model"], "gpt-5.4"),
    entryLine(["ubq_ai", "codex_models"], { models: [{ slug: "gpt-5.4" }] }),
    entryLine(["key", "config", "1"], { apiKey: "legacy" }),
    entryLine(["uos_ai", "auth", "sessions", "session-id"], { user_id: "user-id" }),
  ], {
    profile: "prod",
    includeCache: false,
    includeLegacy: false,
    overwrite: true,
    dryRun: false,
  });

  assert.equal(result.total, 4);
  assert.equal(result.imported, 1);
  assert.equal(result.skipped, 3);
  assert.equal(result.errors, 0);
  assert.equal(store.get(keyToString(["default", "model"])), "gpt-5.4");
  assert.equal(store.has(keyToString(["ubq_ai", "codex_models"])), false);
  assert.equal(store.has(keyToString(["key", "config", "1"])), false);
  assert.equal(store.has(keyToString(["uos_ai", "auth", "sessions", "session-id"])), false);
});

Deno.test("local KV migration keeps legacy and Codex bootstrap rows for replay", async () => {
  const store = new Map<string, unknown>();
  const result = await importKvMigrationLines(makeKvStub(store), [
    entryLine(["ubq_ai", "codex_models"], { models: [{ slug: "gpt-5.4" }] }),
    entryLine(["key", "health", "1"], { status: "ok" }),
  ], {
    profile: "local",
    includeCache: false,
    includeLegacy: true,
    overwrite: true,
    dryRun: false,
  });

  assert.equal(result.imported, 2);
  assert.equal(store.has(keyToString(["ubq_ai", "codex_models"])), true);
  assert.equal(store.has(keyToString(["key", "health", "1"])), true);
});

Deno.test("KV migration preserves embeddings idempotency ledgers and response chunks", async () => {
  const store = new Map<string, unknown>();
  const ledgerKey: Deno.KvKey = [
    "embeddings",
    "idempotency",
    "v1",
    "principal-hash",
    "idempotency-key-hash",
  ];
  const responseChunkKey: Deno.KvKey = [
    "embeddings",
    "idempotency",
    "v1",
    "response",
    "principal-hash",
    "idempotency-key-hash",
    "response-generation",
    0,
  ];
  const transientJobKey: Deno.KvKey = [
    "embeddings",
    "jobs",
    "v2",
    "token-hash",
    "profile",
    "job-id",
  ];
  const ledger = { v: 1, state: "succeeded", response_chunk_count: 1 };
  const result = await importKvMigrationLines(makeKvStub(store), [
    entryLine(ledgerKey, ledger),
    entryLine(responseChunkKey, '{"data":[]}'),
    entryLine(transientJobKey, { status: "succeeded" }),
  ], {
    profile: "prod",
    includeCache: false,
    includeLegacy: false,
    overwrite: true,
    dryRun: false,
  });

  assert.equal(result.imported, 2);
  assert.equal(result.skipped, 1);
  assert.equal(result.groups.embeddings_idempotency, 1);
  assert.equal(result.groups.embeddings_idempotency_responses, 1);
  assert.equal(result.groups.embeddings_jobs, 1);
  assert.deepEqual(store.get(keyToString(ledgerKey)), ledger);
  assert.equal(store.get(keyToString(responseChunkKey)), '{"data":[]}');
  assert.equal(store.has(keyToString(transientJobKey)), false);
});

Deno.test("KV migration imports only the option-aware v2 embedding cache when requested", async () => {
  const store = new Map<string, unknown>();
  const v1Key: Deno.KvKey = ["embeddings", "v1", "legacy-model", "text-hash"];
  const v2Key: Deno.KvKey = [
    "embeddings",
    "v2",
    "voyage-4-large|document|1024|float|float|false",
    "text-hash",
  ];
  const result = await importKvMigrationLines(makeKvStub(store), [
    entryLine(v1Key, { embedding: [1] }),
    entryLine(v2Key, { embedding: [2] }),
  ], {
    profile: "prod",
    includeCache: true,
    includeLegacy: false,
    overwrite: true,
    dryRun: false,
  });

  assert.equal(result.imported, 1);
  assert.equal(result.skipped, 1);
  assert.equal(store.has(keyToString(v1Key)), false);
  assert.deepEqual(store.get(keyToString(v2Key)), { embedding: [2] });
});

Deno.test("KV migration dry-run reports destination collisions like writes", async () => {
  const store = new Map<string, unknown>();
  store.set(keyToString(["default", "model"]), "existing-model");

  const result = await importKvMigrationLines(makeKvStub(store), [
    entryLine(["default", "model"], "new-model"),
  ], {
    profile: "prod",
    includeCache: false,
    includeLegacy: false,
    overwrite: false,
    dryRun: true,
  });

  assert.equal(result.imported, 0);
  assert.equal(result.skipped, 1);
  assert.equal(store.get(keyToString(["default", "model"])), "existing-model");
});

Deno.test("KV migration validation rejects unusable codex model snapshots for configured defaults", async () => {
  const store = new Map<string, unknown>();
  store.set(keyToString(["default", "model"]), "gpt-5.5");
  store.set(keyToString(["ubq_ai", "codex_models"]), { models: [] });

  const result = await validateKvMigrationTarget(makeKvStub(store));

  assert.match(result.errors.join("\n"), /codex model snapshot is empty or malformed/);
});

Deno.test("KV incident migration creates counters, runtime config, and only pending fallback ledger rows", async () => {
  const store = new Map<string, unknown>();
  const now = Date.now();
  const id = "bounded-key";
  const hash = "bounded-hash";
  const fallback = {
    paid_fallback_enabled: true,
    paid_fallback_limit_microcredits: 1_000_000,
    paid_fallback_spent_microcredits: 100_000,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
  };
  store.set(keyToString(["ubq_ai", "codex_models"]), {
    models: [{ slug: "gpt-5.5", supported_reasoning_levels: ["none", "medium"] }],
    source: "chatgpt_codex",
    updated_at_ms: now,
  });
  store.set(keyToString(["default", "model"]), "gpt-5.5");
  store.set(keyToString(["default", "reasoning_effort"]), "medium");
  store.set(keyToString(["ubq_ai", "api_keys", "id", id]), {
    id,
    name: "Bounded",
    prefix: "u_bounded",
    hash,
    created_at_ms: now,
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: 10,
    usage_requests: 4,
    usage_reset_at_ms: now + 60_000,
    window_ms: 60_000,
    ...fallback,
    paid_fallback_model_ids: ["gpt-5.5"],
    paid_fallback_quota_per_credit: 500_000,
    paid_fallback_pricing_checked_at_ms: now,
  });
  store.set(keyToString(["ubq_ai", "api_keys", "hash", hash]), {
    id,
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: 10,
    usage_requests: 4,
    usage_reset_at_ms: now + 60_000,
    window_ms: 60_000,
    ...fallback,
  });
  for (const [requestId, billingStatus] of [["pending", "pending"], ["done", "reconciled"]] as const) {
    store.set(keyToString(["ubq_ai", "api_keys", "request_log", id, now, requestId]), {
      id: requestId,
      key_id: id,
      provider: "yunwu",
      billing_status: billingStatus,
      created_at_ms: now,
    });
  }

  const result = await migrateKvReadIncidentV2(makeKvStub(store));
  assert.deepEqual(result, {
    api_keys: 1,
    bounded_counters: 1,
    paid_fallback_records: 1,
    runtime_config_written: true,
  });
  assert.equal(store.has(keyToString(["uos_ai", "runtime_config", "v2"])), true);
  assert.equal(store.has(keyToString(["uos_ai", "paid_fallback", "ledger", id, now, "pending"])), true);
  assert.equal(store.has(keyToString(["uos_ai", "paid_fallback", "ledger", id, now, "done"])), false);
  const validation = await validateKvMigrationTarget(makeKvStub(store));
  assert.deepEqual(validation.errors, []);
});
