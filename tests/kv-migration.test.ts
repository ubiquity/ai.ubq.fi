import assert from "node:assert/strict";
import { keyToJSON } from "@deno/kv-utils/json";
import { importKvMigrationLines } from "../src/kv_migration.ts";

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
  }) as unknown as Deno.Kv;

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
