import assert from "node:assert/strict";

import { handleAdminProviderSelectionGet, handleAdminProviderSelectionSet } from "../src/admin.ts";
import { CODEX_AUTH_POOL_KV_KEY, CODEX_MODELS_KV_KEY, type CodexModelsSnapshot, resetCodexAuthCacheForTest } from "../src/codex.ts";
import { DEEPSEEK_OFFICIAL_MODEL_IDS } from "../src/deepseek.ts";
import { LITHOS_MODEL_IDS } from "../src/lithos.ts";
import { CODEX_MODELS_WHITELIST_KV_KEY } from "../src/codex_models_whitelist.ts";
import handler from "../src/handler.ts";
import { setKvForTest } from "../src/kv.ts";
import { handleModels } from "../src/openai.ts";
import {
  codexAccountEligibility,
  codexSubscriptionHash,
  codexSubscriptionSelectionId,
  filterCatalogEntriesByProviderSelection,
  isCodexSubscriptionEnabled,
  isProviderEnabled,
  isProviderSelectionId,
  loadProviderSelection,
  loadProviderSelectionCached,
  normalizeProviderSelection,
  normalizeSelectedProviderIds,
  PROVIDER_SELECTION_CACHE_TTL_MS,
  PROVIDER_SELECTION_KV_KEY,
  providerSelectionIsActive,
  resetProviderSelectionCacheForTest,
  SELECTABLE_PROVIDER_IDS,
  storeProviderSelection,
  type SelectableProviderId,
} from "../src/provider_selection.ts";
import { resetRuntimeConfigCacheForTest, RUNTIME_CONFIG_V2_KEY } from "../src/runtime_config.ts";

// The model-listing path reads discovery credentials from the environment.
// Clearing them keeps these tests on the credential-gated providers they own,
// and keeps discovery from reaching a network the test task does not allow.
Deno.env.delete("METERED_API_KEY");
Deno.env.delete("SURPLUS_API_KEY");

const keyOf = (key: Deno.KvKey): string => JSON.stringify(key);

/** Minimal Deno.Kv stand-in: the selection paths only read and write one key. */
class SelectionKv {
  readonly values = new Map<string, unknown>();
  failReads = false;

  get<T>(key: Deno.KvKey, _options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
    if (this.failReads) return Promise.reject(new Error("KV read failed"));
    const stored = this.values.get(keyOf(key));
    return Promise.resolve({
      key,
      value: (stored ?? null) as T | null,
      versionstamp: stored === undefined ? null : "00000000000000000001",
    } as Deno.KvEntryMaybe<T>);
  }

  set(key: Deno.KvKey, value: unknown): Promise<Deno.KvCommitResult> {
    this.values.set(keyOf(key), value);
    return Promise.resolve({ ok: true, versionstamp: "00000000000000000002" });
  }

  storedSelection(): unknown {
    return this.values.get(keyOf([...PROVIDER_SELECTION_KV_KEY]));
  }

  seedSelection(providerIds: readonly string[], updatedAtMs = 1_700_000_000_000): void {
    this.values.set(keyOf([...PROVIDER_SELECTION_KV_KEY]), { provider_ids: [...providerIds], updated_at_ms: updatedAtMs });
  }
}

const withKv = async (kv: SelectionKv | null, run: () => Promise<void>): Promise<void> => {
  resetProviderSelectionCacheForTest();
  setKvForTest(kv as unknown as Deno.Kv | null);
  try {
    await run();
  } finally {
    setKvForTest(null);
    resetProviderSelectionCacheForTest();
  }
};

const codexSnapshot = (modelIds: readonly string[]): CodexModelsSnapshot =>
  ({
    source: "codex_cli",
    client_version: "0.126.0",
    updated_at_ms: 1_700_000_000_000,
    models: modelIds.map((id) => ({ slug: id, supported_reasoning_levels: ["none", "low"] })),
  }) as unknown as CodexModelsSnapshot;

const seedCodexSnapshot = (kv: SelectionKv, modelIds: readonly string[]): void => {
  const snapshot = codexSnapshot(modelIds);
  kv.values.set(keyOf([...CODEX_MODELS_KV_KEY]), snapshot);
  kv.values.set(keyOf([...RUNTIME_CONFIG_V2_KEY]), {
    version: 2,
    default_model: modelIds[0] ?? "gpt-5-fixture",
    default_reasoning_effort: "low",
    codex_models: snapshot,
    updated_at_ms: 1_700_000_000_000,
  });
  resetRuntimeConfigCacheForTest();
};

const listModelIds = async (): Promise<string[]> => {
  const response = await handleModels();
  assert.equal(response.status, 200);
  const payload = (await response.json()) as { data?: { id?: string }[] };
  return (payload.data ?? []).map((model) => model.id ?? "");
};

// ── Module contract ──────────────────────────────────────────────────────────

Deno.test("provider selection normalizes to the roster order without duplicates", () => {
  assert.deepEqual(normalizeSelectedProviderIds(["openlux", "codex", "openlux"]), ["codex", "openlux"]);
  assert.deepEqual(normalizeSelectedProviderIds([" codex "]), ["codex"]);
  assert.deepEqual(normalizeSelectedProviderIds(["unknown", "surplus"]), ["surplus"]);
  assert.deepEqual(normalizeSelectedProviderIds([1, null, {}]), []);
});

Deno.test("a stored provider selection keeps its known ids and drops retired ones", () => {
  assert.deepEqual(normalizeProviderSelection({ provider_ids: ["surplus", "retired"], updated_at_ms: 5 }), {
    provider_ids: ["surplus"],
    updated_at_ms: 5,
  });
  assert.equal(normalizeProviderSelection({ provider_ids: "codex", updated_at_ms: 5 }), null);
  assert.equal(normalizeProviderSelection({ provider_ids: [7], updated_at_ms: 5 }), null);
  assert.equal(normalizeProviderSelection({ provider_ids: [], updated_at_ms: 0 }), null);
  assert.equal(normalizeProviderSelection(null), null);
});

Deno.test("an empty or absent selection keeps every provider eligible", () => {
  for (const provider of SELECTABLE_PROVIDER_IDS) {
    assert.equal(isProviderEnabled(provider, null), true, `${provider}: absent selection is no filter`);
    assert.equal(isProviderEnabled(provider, { provider_ids: [], updated_at_ms: 1 }), true, `${provider}: empty selection is no filter`);
  }
  const narrowed = { provider_ids: ["surplus"] as SelectableProviderId[], updated_at_ms: 1 };
  assert.equal(isProviderEnabled("surplus", narrowed), true);
  assert.equal(isProviderEnabled("codex", narrowed), false);
  assert.equal(providerSelectionIsActive(narrowed), true);
  assert.equal(providerSelectionIsActive(null), false);
  assert.equal(providerSelectionIsActive({ provider_ids: [], updated_at_ms: 1 }), false);
});

Deno.test("a subscription selection narrows the Codex tier instead of switching it off", async () => {
  const first = codexSubscriptionSelectionId(await codexSubscriptionHash("account-one"));
  const second = codexSubscriptionSelectionId(await codexSubscriptionHash("account-two"));
  assert.notEqual(first, second);
  assert.equal(first, `codex:${await codexSubscriptionHash("account-one")}`, "the hash memoizes to one stable id");
  assert.equal(isProviderSelectionId(first), true);
  assert.equal(isProviderSelectionId("codex:not-a-hash"), false);
  assert.equal(isProviderSelectionId("codex:"), false);

  const onlySecond = { provider_ids: [second], updated_at_ms: 1 } as const;
  assert.equal(isProviderEnabled("codex", onlySecond), true, "one selected subscription is an enabled Codex tier");
  assert.equal(isProviderEnabled("surplus", onlySecond), false);
  assert.deepEqual(codexAccountEligibility(onlySecond), { kind: "only", hashes: [await codexSubscriptionHash("account-two")] });
  assert.equal(isCodexSubscriptionEnabled(await codexSubscriptionHash("account-two"), onlySecond), true);
  assert.equal(isCodexSubscriptionEnabled(await codexSubscriptionHash("account-one"), onlySecond), false);

  assert.deepEqual(codexAccountEligibility(null), { kind: "all" }, "no selection restricts nothing");
  assert.deepEqual(codexAccountEligibility({ provider_ids: [], updated_at_ms: 1 }), { kind: "all" });
  assert.deepEqual(codexAccountEligibility({ provider_ids: ["codex"], updated_at_ms: 1 }), { kind: "all" }, "the umbrella means every subscription");
  assert.deepEqual(codexAccountEligibility({ provider_ids: ["surplus"], updated_at_ms: 1 }), { kind: "none" }, "no Codex id at all switches the tier off");
  assert.equal(isProviderEnabled("codex", { provider_ids: ["surplus"], updated_at_ms: 1 }), false);
});

Deno.test("the codex umbrella absorbs subscription ids so storage stays unambiguous", () => {
  const subscription = codexSubscriptionSelectionId("a".repeat(64));
  const other = codexSubscriptionSelectionId("b".repeat(64));
  assert.deepEqual(normalizeSelectedProviderIds([other, "codex", subscription]), ["codex"]);
  assert.deepEqual(normalizeSelectedProviderIds([other, "surplus", subscription]), ["surplus", subscription, other]);
  assert.deepEqual(normalizeSelectedProviderIds(["codex:not-a-hash", "codex"]), ["codex"]);
  assert.deepEqual(normalizeSelectedProviderIds(["codex:not-a-hash"]), []);
  assert.deepEqual(normalizeProviderSelection({ provider_ids: [subscription, "codex"], updated_at_ms: 5 }), {
    provider_ids: ["codex"],
    updated_at_ms: 5,
  });
});

Deno.test("a catalog row keeps only the providers that are still active", () => {
  const entries = [
    { id: "codex-only", providers: [{ id: "codex" }] },
    { id: "shared", providers: [{ id: "codex" }, { id: "surplus" }] },
    { id: "openlux-only", providers: [{ id: "openlux" }] },
  ];
  assert.deepEqual(filterCatalogEntriesByProviderSelection(entries, null), entries, "no selection filters nothing");
  assert.deepEqual(
    filterCatalogEntriesByProviderSelection(entries, { provider_ids: ["surplus"], updated_at_ms: 1 }),
    [{ id: "shared", providers: [{ id: "surplus" }] }],
    "a switched-off provider is dropped from every row, and a row with no enabled provider disappears"
  );
  const subscriptionOnly = { provider_ids: [codexSubscriptionSelectionId("c".repeat(64))], updated_at_ms: 1 } as const;
  assert.deepEqual(
    filterCatalogEntriesByProviderSelection(entries, subscriptionOnly),
    [
      { id: "codex-only", providers: [{ id: "codex" }] },
      { id: "shared", providers: [{ id: "codex" }] },
    ],
    "a narrowed Codex tier still advertises every model Codex serves"
  );
});

Deno.test("the routing selection read is cached, primed by a save, and fails open", async () => {
  const kv = new SelectionKv();
  await withKv(kv, async () => {
    assert.equal(await loadProviderSelection(kv as unknown as Deno.Kv), null, "an absent selection reads as no filter");

    const stored = await storeProviderSelection(kv as unknown as Deno.Kv, ["openlux"]);
    assert.ok(stored, "a save must persist a selection");
    assert.deepEqual(stored.provider_ids, ["openlux"]);
    assert.deepEqual(kv.storedSelection(), { provider_ids: ["openlux"], updated_at_ms: stored.updated_at_ms });
    assert.deepEqual(await loadProviderSelectionCached(), { provider_ids: ["openlux"], updated_at_ms: stored.updated_at_ms }, "a save primes the cache");

    // A later external write stays invisible until the cache expires, and a
    // failed read keeps serving the last known value instead of erroring.
    kv.seedSelection([]);
    assert.deepEqual((await loadProviderSelectionCached())?.provider_ids, ["openlux"]);
    assert.deepEqual((await loadProviderSelectionCached(Date.now() + PROVIDER_SELECTION_CACHE_TTL_MS + 1))?.provider_ids, []);
    kv.failReads = true;
    assert.deepEqual((await loadProviderSelectionCached(Date.now() + 2 * PROVIDER_SELECTION_CACHE_TTL_MS))?.provider_ids, []);
  });

  const unavailable = new SelectionKv();
  unavailable.failReads = true;
  await withKv(unavailable, async () => {
    assert.equal(await loadProviderSelectionCached(), null, "with nothing cached a failed read is no filter, not an error");
  });
});

// ── Admin API ────────────────────────────────────────────────────────────────

const catalogFixture = () => ({
  models: [
    { id: "gpt-5.6-sol", providers: [{ id: "codex" as const, owned_by: "openai", supported_endpoints: ["/v1/responses"] }] },
    { id: "gpt-5.6-sol", providers: [{ id: "surplus" as const, owned_by: "moonshot", supported_endpoints: ["/v1/responses"] }] },
    { id: "kimi-k2", providers: [{ id: "surplus" as const, owned_by: "moonshot", supported_endpoints: ["/v1/chat/completions"] }] },
    { id: "deepseek-flash", providers: [{ id: "deepseek" as const, owned_by: "deepseek", supported_endpoints: ["/v1/chat/completions"] }] },
  ],
  sources: {
    codex: { status: "available" as const, count: 1, updated_at_ms: 1 },
    openlux: { status: "unavailable" as const, count: 0, updated_at_ms: null },
    surplus: { status: "available" as const, count: 2, updated_at_ms: 2 },
    deepseek: { status: "available" as const, count: 1, updated_at_ms: null, configured: true },
    cerebras: { status: "unavailable" as const, count: 0, updated_at_ms: null, configured: false },
    // The catalog source id union gained the LithosAI provider; this fixture is
    // typed as a whole snapshot, so it must name every source id.
    lithos: { status: "unavailable" as const, count: 0, updated_at_ms: null, configured: false },
    openrouter: { status: "available" as const, count: 2, updated_at_ms: 3 },
  },
});

Deno.test("admin provider picker reports the roster, catalog counts, and the saved selection", async () => {
  const kv = new SelectionKv();
  kv.seedSelection(["surplus"]);
  await withKv(kv, async () => {
    const response = await handleAdminProviderSelectionGet({ buildCatalog: () => Promise.resolve(catalogFixture()) });
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("cache-control"), "no-store");
    const body = await response.json();
    assert.deepEqual(
      body.data.providers,
      [
        { id: "codex", model_count: 1, status: "available", configured: true, subscriptions: [] },
        { id: "surplus", model_count: 2, status: "available", configured: true },
        { id: "openlux", model_count: 0, status: "unavailable", configured: false },
        { id: "deepseek", model_count: 1, status: "available", configured: true },
        { id: "cerebras", model_count: 0, status: "unavailable", configured: false },
        { id: "lithos", model_count: 0, status: "unavailable", configured: false },
      ],
      "the roster is fixed and every provider carries its catalog entry count"
    );
    assert.deepEqual(body.data.selection.provider_ids, ["surplus"]);
    assert.equal(body.data.filter_active, true);
  });
});

const stripBase64Padding = (base64: string): string => {
  let end = base64.length;
  while (end > 0 && base64[end - 1] === "=") end -= 1;
  return base64.slice(0, end);
};

const tokenWithPayload = (payload: unknown): string =>
  `${stripBase64Padding(btoa(JSON.stringify({ alg: "none" })))
    .replace(/\+/g, "-")
    .replace(/\//g, "_")}.${stripBase64Padding(btoa(JSON.stringify(payload)))
    .replace(/\+/g, "-")
    .replace(/\//g, "_")}.signature`;

/** Two configured subscriptions: one with a profile email, one without. */
const seedCodexAuthPool = (kv: SelectionKv): void => {
  kv.values.set(keyOf([...CODEX_AUTH_POOL_KV_KEY]), {
    accounts: [
      {
        access_token: tokenWithPayload({ "https://api.openai.com/profile": { email: "first@example.com" } }),
        refresh_token: "refresh-one",
        account_id: "account-one",
        updated_at_ms: 1,
      },
      { access_token: "not-a-jwt", refresh_token: "refresh-two", account_id: "account-two", updated_at_ms: 1 },
    ],
    updated_at_ms: 1,
  });
  resetCodexAuthCacheForTest();
};

Deno.test("admin provider picker lists each configured Codex subscription under its opaque id", async () => {
  const kv = new SelectionKv();
  seedCodexAuthPool(kv);
  try {
    await withKv(kv, async () => {
      const response = await handleAdminProviderSelectionGet({ buildCatalog: () => Promise.resolve(catalogFixture()) });
      const body = await response.json();
      const codex = body.data.providers.find((provider: { id: string }) => provider.id === "codex");
      assert.deepEqual(codex.subscriptions, [
        { id: codexSubscriptionSelectionId(await codexSubscriptionHash("account-one")), label: "first@example.com", slot: 1 },
        { id: codexSubscriptionSelectionId(await codexSubscriptionHash("account-two")), label: "Codex account 2", slot: 2 },
      ]);
      assert.equal(
        body.data.providers.some((provider: { id: string; subscriptions?: unknown }) => provider.subscriptions !== undefined && provider.id !== "codex"),
        false,
        "only the Codex tier carries subscriptions"
      );
      for (const provider of body.data.providers) {
        assert.equal(JSON.stringify(provider).includes("account-one"), false, "a raw account id never crosses the response boundary");
      }

      // One subscription round-trips through the write path as an active Codex tier.
      const saved = await handleAdminProviderSelectionSet(
        new Request("https://ai.ubq.fi/admin/providers/selection", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ provider_ids: [codex.subscriptions[1].id, "surplus"] }),
        })
      );
      assert.equal(saved.status, 200);
      assert.deepEqual((await saved.json()).provider_ids, ["surplus", codex.subscriptions[1].id]);
      const reloaded = await handleAdminProviderSelectionGet({ buildCatalog: () => Promise.resolve(catalogFixture()) });
      assert.equal((await reloaded.json()).data.filter_active, true);
    });
  } finally {
    resetCodexAuthCacheForTest();
  }
});

Deno.test("an empty saved provider selection is reported as no filter", async () => {
  await withKv(new SelectionKv(), async () => {
    const response = await handleAdminProviderSelectionGet({ buildCatalog: () => Promise.resolve(catalogFixture()) });
    const body = await response.json();
    assert.deepEqual(body.data.selection, { provider_ids: [], updated_at_ms: 0 });
    assert.equal(body.data.filter_active, false);
  });
});

Deno.test("the provider picker refuses to render without KV", async () => {
  await withKv(null, async () => {
    const response = await handleAdminProviderSelectionGet({
      buildCatalog: () => Promise.reject(new Error("the catalog must not be built without KV")),
    });
    assert.equal(response.status, 500);
  });
});

Deno.test("saving a provider selection canonicalizes it and rejects unknown ids", async () => {
  const kv = new SelectionKv();
  await withKv(kv, async () => {
    const response = await handleAdminProviderSelectionSet(
      new Request("https://ai.ubq.fi/admin/providers/selection", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ provider_ids: ["cerebras", "codex", "cerebras"] }),
      })
    );
    assert.equal(response.status, 200);
    const body = await response.json();
    assert.deepEqual(body.provider_ids, ["codex", "cerebras"], "storage and the wire response share the roster order");
    assert.equal(body.stored, true);
    assert.deepEqual(kv.storedSelection(), { provider_ids: ["codex", "cerebras"], updated_at_ms: body.updated_at_ms });

    for (const payload of [{ provider_ids: ["codex", "nope"] }, { provider_ids: [42] }, { provider_ids: "codex" }, {}]) {
      const invalid = await handleAdminProviderSelectionSet(
        new Request("https://ai.ubq.fi/admin/providers/selection", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        })
      );
      assert.equal(invalid.status, 400, `${JSON.stringify(payload)} must be rejected`);
    }
    assert.deepEqual(kv.storedSelection(), { provider_ids: ["codex", "cerebras"], updated_at_ms: body.updated_at_ms }, "a rejected write stores nothing");

    const empty = await handleAdminProviderSelectionSet(
      new Request("https://ai.ubq.fi/admin/providers/selection", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ provider_ids: [] }),
      })
    );
    assert.equal(empty.status, 200);
    assert.deepEqual(kv.storedSelection(), { provider_ids: [], updated_at_ms: (await empty.json()).updated_at_ms }, "an empty list clears the filter");
  });
});

Deno.test("the provider picker routes are registered and stay behind admin auth", async () => {
  const unauthenticated = await handler(new Request("https://ai.ubq.fi/admin/providers/selection"));
  assert.equal(unauthenticated.status, 401, "an unauthenticated selection read reaches the admin gate instead of 404");
  const unknown = await handler(new Request("https://ai.ubq.fi/admin/providers/selection-unknown"));
  assert.equal(unknown.status, 404);
});

// ── Model listing enforcement ────────────────────────────────────────────────

Deno.test("/v1/models hides the models of a switched-off provider", async () => {
  Deno.env.set("DEEPSEEK_API_KEY", "fixture-deepseek-key");
  Deno.env.set("CEREBRAS_API_KEY", "fixture-cerebras-key");
  // This test owns every credential-gated provider, so it configures the
  // LithosAI key itself instead of leaving the row set to the ambient
  // environment.
  Deno.env.set("LITHOSAI_API_KEY", "fixture-lithos-key");
  const kv = new SelectionKv();
  seedCodexSnapshot(kv, ["gpt-5.6-sol"]);
  try {
    await withKv(kv, async () => {
      assert.deepEqual(
        await listModelIds(),
        ["gpt-5.6-sol", "gpt-oss-120b", ...DEEPSEEK_OFFICIAL_MODEL_IDS, ...LITHOS_MODEL_IDS],
        "no filter lists every provider"
      );

      kv.seedSelection(["deepseek", "cerebras"]);
      resetProviderSelectionCacheForTest();
      assert.deepEqual(await listModelIds(), ["gpt-oss-120b", ...DEEPSEEK_OFFICIAL_MODEL_IDS], "a switched-off Codex provider contributes no rows");

      kv.seedSelection(["codex"]);
      resetProviderSelectionCacheForTest();
      assert.deepEqual(await listModelIds(), ["gpt-5.6-sol"], "switched-off credential-gated providers contribute no rows");
    });
  } finally {
    Deno.env.delete("DEEPSEEK_API_KEY");
    Deno.env.delete("CEREBRAS_API_KEY");
    Deno.env.delete("LITHOSAI_API_KEY");
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("/v1/models still applies the model whitelist on top of the provider selection", async () => {
  const kv = new SelectionKv();
  seedCodexSnapshot(kv, ["gpt-5.6-sol", "gpt-5.6-terra"]);
  kv.values.set(keyOf([...CODEX_MODELS_WHITELIST_KV_KEY]), { model_ids: ["gpt-5.6-terra"], updated_at_ms: 1_700_000_000_000 });
  try {
    await withKv(kv, async () => {
      kv.seedSelection(["codex"]);
      resetProviderSelectionCacheForTest();
      assert.deepEqual(await listModelIds(), ["gpt-5.6-terra"], "the operator whitelist still narrows the surviving rows");
    });
  } finally {
    resetRuntimeConfigCacheForTest();
  }
});
