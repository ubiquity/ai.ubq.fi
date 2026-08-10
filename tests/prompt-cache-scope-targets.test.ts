import assert from "node:assert/strict";
import {
  derivePromptCacheScopeTargetInventory,
  loadPromptCacheScopeTargetInventory,
} from "../src/prompt_cache_scope_targets.ts";
import { CODEX_AUTH_POOL_KV_KEY, CODEX_MODELS_KV_KEY, type CodexModelsSnapshot } from "../src/codex.ts";

const encodeKey = (key: Deno.KvKey): string => JSON.stringify(key);

const qualifiedControls = {
  key: true,
  explicit_breakpoints: true,
  modes: ["explicit"],
  ttls: ["30m"],
  expected_usage_fields: ["cached_tokens", "cache_write_tokens"],
  source: "catalog",
  verified_at_ms: 1,
};

const makeModel = (
  slug: string,
  controls: Record<string, unknown> | null = qualifiedControls,
): Record<string, unknown> => ({
  slug,
  ...(controls ? { prompt_cache: { version: 1, providers: [{ id: "codex_chatgpt", controls }] } } : {}),
});

const makeSnapshot = (models: Record<string, unknown>[]): CodexModelsSnapshot => ({
  source: "chatgpt_codex",
  updated_at_ms: 1,
  client_version: "0.201.0",
  models,
});

const makePool = (slots: number): unknown => ({
  accounts: Array.from({ length: slots }, (_value, index) => ({
    access_token: `access-${index + 1}`,
    refresh_token: `refresh-${index + 1}`,
    account_id: `account-${index + 1}`,
    updated_at_ms: 1,
  })),
  updated_at_ms: 1,
});

const derive = async (overrides: Partial<Parameters<typeof derivePromptCacheScopeTargetInventory>[0]> = {}) =>
  await derivePromptCacheScopeTargetInventory({
    snapshot: makeSnapshot([makeModel("gpt-5.6"), makeModel("gpt-5.6-mini")]),
    catalogVersionstamp: "catalog-v1",
    codexAuthPool: makePool(2),
    codexAuthPoolVersionstamp: "auth-v1",
    yunwuFallbackRoster: { status: "unknown" },
    ...overrides,
  });

Deno.test("target inventory sorts and de-duplicates catalog IDs without inferring model families", async () => {
  const first = await derive({
    snapshot: makeSnapshot([makeModel("gpt-5.6-mini"), makeModel("gpt-5.6"), makeModel("gpt-5.6-mini")]),
  });
  const second = await derive({
    snapshot: makeSnapshot([makeModel("gpt-5.6-mini"), makeModel("gpt-5.6"), makeModel("gpt-5.6-mini")]),
  });

  assert.equal(first.status, "ready");
  assert.deepEqual(first.targets.map((target) => target.model), ["gpt-5.6", "gpt-5.6-mini"]);
  assert.deepEqual(first.targets.map((target) => target.id), second.targets.map((target) => target.id));
  assert.equal(first.inventory_fingerprint, second.inventory_fingerprint);
  for (const target of first.targets) {
    assert.equal(target.model_family_id, target.model);
    assert.equal(target.model_family_source, "exact_model");
  }
});

Deno.test("Codex capability identity and terminal telemetry identity stay distinct", async () => {
  const inventory = await derive();
  const target = inventory.targets.find((candidate) => candidate.provider === "codex_chatgpt");
  assert.ok(target);
  assert.equal(target.provider, "codex_chatgpt");
  assert.equal(target.telemetry_provider, "chatgpt_codex");
  assert.ok(target.id.includes(":codex_chatgpt:chatgpt_codex:codex_account_pool:"));
});

Deno.test("only a usable two-slot Codex pool is probeable", async () => {
  const twoSlots = await derive();
  const oneSlot = await derive({ codexAuthPool: makePool(1) });
  const noSlots = await derive({ codexAuthPool: makePool(0), codexAuthPoolVersionstamp: null });

  assert.deepEqual(twoSlots.targets[0]?.probeability, { status: "probeable", adapter: "codex_two_slot" });
  assert.deepEqual(oneSlot.targets[0]?.topology, {
    kind: "codex_account_pool",
    configured_slot_count: 1,
    auth_pool_versionstamp: "auth-v1",
  });
  assert.deepEqual(oneSlot.targets[0]?.probeability, { status: "unprobeable", reason: "two_codex_slots_required" });
  assert.deepEqual(noSlots.targets[0]?.probeability, { status: "unprobeable", reason: "two_codex_slots_required" });
});

Deno.test("Codex cache qualification comes from existing catalog capability metadata", async () => {
  const inventory = await derive({
    snapshot: makeSnapshot([
      makeModel("qualified"),
      makeModel("unqualified", {
        key: true,
        explicit_breakpoints: true,
        modes: ["explicit"],
        ttls: ["30m"],
        expected_usage_fields: ["cached_tokens"],
        source: "catalog",
        verified_at_ms: 1,
      }),
    ]),
  });
  const qualified = inventory.targets.find((target) => target.model === "qualified");
  const unqualified = inventory.targets.find((target) => target.model === "unqualified");

  assert.equal(qualified?.codex_cache_qualification, "qualified");
  assert.deepEqual(qualified?.probeability, { status: "probeable", adapter: "codex_two_slot" });
  assert.equal(unqualified?.codex_cache_qualification, "unqualified");
  assert.deepEqual(unqualified?.probeability, { status: "unprobeable", reason: "codex_cache_unqualified" });
});

Deno.test("an authoritative Yunwu roster creates only catalog intersections and reports the rest", async () => {
  const inventory = await derive({
    yunwuFallbackRoster: { status: "authoritative", model_ids: ["missing", "gpt-5.6-mini", "gpt-5.6-mini"] },
  });
  const yunwuTargets = inventory.targets.filter((target) => target.provider === "yunwu");

  assert.deepEqual(yunwuTargets.map((target) => target.model), ["gpt-5.6-mini"]);
  assert.deepEqual(inventory.yunwu_fallback_roster, {
    status: "authoritative",
    model_ids: ["gpt-5.6-mini", "missing"],
    non_catalog_model_ids: ["missing"],
  });
});

Deno.test("Yunwu targets describe their single-credential topology and current adapter boundary", async () => {
  const inventory = await derive({
    yunwuFallbackRoster: { status: "authoritative", model_ids: ["gpt-5.6"] },
  });
  const target = inventory.targets.find((candidate) => candidate.provider === "yunwu");

  assert.ok(target);
  assert.equal(target.provider, "yunwu");
  assert.equal(target.telemetry_provider, "yunwu");
  assert.deepEqual(target.topology, { kind: "single_credential" });
  assert.deepEqual(target.probeability, {
    status: "unprobeable",
    reason: "current_two_slot_adapter_does_not_apply",
  });
  assert.equal(target.codex_auth_pool_versionstamp, null);
});

Deno.test("the same exact model has isolated Codex and Yunwu target identities", async () => {
  const inventory = await derive({
    yunwuFallbackRoster: { status: "authoritative", model_ids: ["gpt-5.6"] },
  });
  const codex = inventory.targets.find((target) => target.provider === "codex_chatgpt" && target.model === "gpt-5.6");
  const yunwu = inventory.targets.find((target) => target.provider === "yunwu" && target.model === "gpt-5.6");

  assert.ok(codex);
  assert.ok(yunwu);
  assert.notEqual(codex.id, yunwu.id);
  assert.notEqual(codex.capability_fingerprint, yunwu.capability_fingerprint);
  assert.notEqual(codex.telemetry_provider, yunwu.telemetry_provider);
  assert.notEqual(codex.topology.kind, yunwu.topology.kind);
  assert.equal(yunwu.probeability.status, "unprobeable");
  assert.deepEqual(
    inventory.targets.filter((target) => target.probeability.status === "probeable").map((target) => target.provider),
    ["codex_chatgpt", "codex_chatgpt"],
  );
});

Deno.test("published scope evidence does not change the dispatch capability or inventory identity", async () => {
  const baseline = await derive();
  const scopedModel = {
    ...makeModel("gpt-5.6"),
    prompt_cache: {
      version: 1,
      providers: [{
        id: "codex_chatgpt",
        controls: qualifiedControls,
        scope: {
          probe_profile: "responses_explicit_input_text_keyed_30m",
          account_slots: "account_scoped",
          token_refresh: "preserved",
          conversation_id: "independent",
          effective_model: "gpt-5.6",
          reproducible_cycles: 3,
          source: "live_probe",
          verified_at_ms: 2,
        },
      }],
    },
  };
  const published = await derive({
    snapshot: makeSnapshot([scopedModel, makeModel("gpt-5.6-mini")]),
  });
  const baselineTarget = baseline.targets.find((target) =>
    target.provider === "codex_chatgpt" && target.model === "gpt-5.6"
  );
  const publishedTarget = published.targets.find((target) =>
    target.provider === "codex_chatgpt" && target.model === "gpt-5.6"
  );

  assert.ok(baselineTarget);
  assert.ok(publishedTarget);
  assert.equal(publishedTarget.capability_fingerprint, baselineTarget.capability_fingerprint);
  assert.equal(published.inventory_fingerprint, baseline.inventory_fingerprint);
});

Deno.test("an unknown Yunwu roster fails closed with no Yunwu targets", async () => {
  const inventory = await derive({ yunwuFallbackRoster: { status: "unknown" } });

  assert.equal(inventory.yunwu_fallback_roster.status, "unknown");
  assert.deepEqual(inventory.targets.filter((target) => target.provider === "yunwu"), []);
});

Deno.test("the inventory loader has a two-key read-only KV footprint", async () => {
  class ReadOnlyKv {
    readonly reads: string[] = [];
    listCalls = 0;
    writeCalls = 0;

    get<T>(key: Deno.KvKey, options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
      assert.deepEqual(options, { consistency: "strong" });
      this.reads.push(encodeKey(key));
      if (encodeKey(key) === encodeKey(CODEX_MODELS_KV_KEY)) {
        return Promise.resolve({
          key,
          value: makeSnapshot([makeModel("gpt-5.6")]) as T,
          versionstamp: "catalog-v1",
        } as Deno.KvEntryMaybe<T>);
      }
      if (encodeKey(key) === encodeKey(CODEX_AUTH_POOL_KV_KEY)) {
        return Promise.resolve({ key, value: makePool(2) as T, versionstamp: "auth-v1" } as Deno.KvEntryMaybe<T>);
      }
      throw new Error(`unexpected KV key: ${encodeKey(key)}`);
    }

    list(): AsyncIterable<Deno.KvEntry<unknown>> {
      this.listCalls += 1;
      throw new Error("target inventory must not list KV records");
    }

    set(): Promise<Deno.KvCommitResult> {
      this.writeCalls += 1;
      throw new Error("target inventory must not write KV records");
    }
  }

  const kv = new ReadOnlyKv();
  const inventory = await loadPromptCacheScopeTargetInventory({ kv: kv as unknown as Deno.Kv });

  assert.equal(inventory.status, "ready");
  assert.deepEqual(new Set(kv.reads), new Set([encodeKey(CODEX_MODELS_KV_KEY), encodeKey(CODEX_AUTH_POOL_KV_KEY)]));
  assert.equal(kv.reads.length, 2);
  assert.equal(kv.listCalls, 0);
  assert.equal(kv.writeCalls, 0);
});

Deno.test("missing or invalid catalogs fail closed", async () => {
  const missing = await derive({ snapshot: null });
  const invalid = await derive({
    snapshot: { source: "chatgpt_codex", updated_at_ms: 1, models: [] },
  });

  assert.deepEqual({ status: missing.status, reason: missing.reason, targets: missing.targets }, {
    status: "unavailable",
    reason: "catalog_unavailable",
    targets: [],
  });
  assert.deepEqual({ status: invalid.status, reason: invalid.reason, targets: invalid.targets }, {
    status: "unavailable",
    reason: "catalog_invalid",
    targets: [],
  });

  const mixed = await derive({
    snapshot: makeSnapshot([makeModel("gpt-5.6"), {}]),
  });
  assert.deepEqual({ status: mixed.status, reason: mixed.reason, targets: mixed.targets }, {
    status: "unavailable",
    reason: "catalog_invalid",
    targets: [],
  });
});
