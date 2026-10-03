import assert from "node:assert/strict";

import { withOpenRouterModels } from "../src/catalog/models.ts";
import { CODEX_MODELS_KV_KEY } from "../src/codex/index.ts";
import { setKvForTest } from "../src/kv.ts";
import { CODEX_MODELS_WHITELIST_KV_KEY } from "../src/models/codex-models-whitelist.ts";
import { handleModelCapabilities, handleModels, handlePublicModelCatalog } from "../src/models/catalog.ts";
import { fetchOpenRouterModels, resetOpenRouterModelsCacheForTest, setOpenRouterModelsFetchForTest } from "../src/models/openrouter-models.ts";
import { createResponseTelemetryState, type ResponseTelemetryState } from "../src/openai-telemetry.ts";
import { handleResponses } from "../src/responses-handler.ts";
import { isOpenRouterModelIdShape, openRouterUpstreamModelFor, resolveOpenRouterUpstreamModel } from "../src/provider/openrouter.ts";
import { handleOpenRouterChatCompletions, handleOpenRouterResponses } from "../src/provider/openrouter-handlers.ts";
import { PROVIDER_SELECTION_KV_KEY, resetProviderSelectionCacheForTest } from "../src/provider/selection.ts";
import { resetRuntimeConfigCacheForTest, RUNTIME_CONFIG_V2_KEY } from "../src/runtime-config.ts";

const catalogue = {
  data: [
    { id: "vendor/alpha", context_length: 1_000, top_provider: { context_length: 1_000 }, reasoning: { supported_efforts: ["high"], default_effort: "high" } },
    { id: "vendor/beta" },
  ],
};

const jsonResponse = (body: unknown, status = 200): Response => new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } });

const withServedCatalogue = async (fn: () => Promise<void> | void): Promise<void> => {
  resetOpenRouterModelsCacheForTest();
  Deno.env.set("OPENROUTER_API_KEY", "fixture-openrouter-key");
  setOpenRouterModelsFetchForTest((() => Promise.resolve(jsonResponse(catalogue))) as typeof fetch);
  try {
    await fetchOpenRouterModels();
    await fn();
  } finally {
    setOpenRouterModelsFetchForTest(null);
    resetOpenRouterModelsCacheForTest();
    Deno.env.delete("OPENROUTER_API_KEY");
  }
};

Deno.test("openrouter serves every cached catalogue id and refuses the rest", async () => {
  await withServedCatalogue(() => {
    assert.equal(openRouterUpstreamModelFor("vendor/alpha"), "vendor/alpha");
    assert.equal(openRouterUpstreamModelFor("vendor/beta"), "vendor/beta");
    assert.equal(openRouterUpstreamModelFor("vendor/gamma"), null);
  });
});

Deno.test("openrouter chat forwards the client body with the served model", async () => {
  await withServedCatalogue(async () => {
    const captured: { body: Record<string, unknown> | null } = { body: null };
    const response = await handleOpenRouterChatCompletions(
      new Request("https://ai.ubq.fi/v1/chat/completions", { method: "POST" }),
      { model: "vendor/alpha", messages: [{ role: "user", content: "hi" }] },
      "vendor/alpha",
      undefined,
      {
        fetchChat: (body) => {
          captured.body = body as Record<string, unknown>;
          return Promise.resolve(
            jsonResponse({
              id: "gen-1",
              object: "chat.completion",
              choices: [{ index: 0, message: { role: "assistant", content: "hello" }, finish_reason: "stop" }],
              usage: { prompt_tokens: 3, completion_tokens: 2, total_tokens: 5 },
            })
          );
        },
      }
    );
    assert.equal(response.status, 200);
    assert.equal(captured.body?.model, "vendor/alpha");
    assert.equal(response.headers.get("x-uos-upstream"), "openrouter");
    const body = (await response.json()) as { choices?: { message?: { content?: string } }[] };
    assert.equal(body.choices?.[0]?.message?.content, "hello");
  });
});

Deno.test("openrouter refuses a model outside the catalogue on both wires", async () => {
  await withServedCatalogue(async () => {
    const chat = await handleOpenRouterChatCompletions(
      new Request("https://ai.ubq.fi/v1/chat/completions", { method: "POST" }),
      { model: "vendor/gamma" },
      "vendor/gamma"
    );
    assert.equal(chat.status, 400);
    const responses = await handleOpenRouterResponses(
      new Request("https://ai.ubq.fi/v1/responses", { method: "POST" }),
      { model: "vendor/gamma", input: "hi" },
      "vendor/gamma"
    );
    assert.equal(responses.status, 400);
  });
});

Deno.test("openrouter responses forwards the client body and reports the upstream", async () => {
  await withServedCatalogue(async () => {
    const captured: { body: Record<string, unknown> | null } = { body: null };
    const response = await handleOpenRouterResponses(
      new Request("https://ai.ubq.fi/v1/responses", { method: "POST" }),
      { model: "vendor/beta", input: "hi" },
      "vendor/beta",
      undefined,
      {
        fetchResponses: (body) => {
          captured.body = body as Record<string, unknown>;
          return Promise.resolve(
            jsonResponse({
              id: "resp_1",
              object: "response",
              status: "completed",
              output: [{ type: "message", role: "assistant", content: [{ type: "output_text", text: "ok" }] }],
              usage: { input_tokens: 4, output_tokens: 1, total_tokens: 5 },
            })
          );
        },
      }
    );
    assert.equal(response.status, 200);
    assert.equal(captured.body?.model, "vendor/beta");
    assert.equal(response.headers.get("x-uos-upstream"), "openrouter");
  });
});

const keyOf = (key: Deno.KvKey): string => JSON.stringify(key);

/** Minimal Deno.Kv stand-in for the model-listing paths. */
class CatalogKv {
  readonly values = new Map<string, unknown>();

  get<T>(key: Deno.KvKey, _options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
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
}

Deno.test("the public catalogue lists only operator-enabled ids while an empty whitelist filters none", async () => {
  const meteredKey = Deno.env.get("METERED_API_KEY");
  const surplusKey = Deno.env.get("SURPLUS_API_KEY");
  Deno.env.delete("METERED_API_KEY");
  Deno.env.delete("SURPLUS_API_KEY");
  const kv = new CatalogKv();
  const codexSnapshot = {
    source: "chatgpt_codex",
    client_version: "0.125.0",
    updated_at_ms: Date.now(),
    models: [{ slug: "hidden-codex-id" }, { slug: "listed-codex-id" }],
  };
  kv.values.set(keyOf([...CODEX_MODELS_KV_KEY]), codexSnapshot);
  kv.values.set(keyOf([...RUNTIME_CONFIG_V2_KEY]), {
    version: 2,
    default_model: "listed-codex-id",
    default_reasoning_effort: "low",
    codex_models: codexSnapshot,
    updated_at_ms: Date.now(),
  });
  kv.values.set(keyOf([...CODEX_MODELS_WHITELIST_KV_KEY]), { model_ids: ["listed-codex-id", "vendor/alpha"], updated_at_ms: 1 });
  resetProviderSelectionCacheForTest();
  resetRuntimeConfigCacheForTest();
  setKvForTest(kv as unknown as Deno.Kv);
  try {
    await withServedCatalogue(async () => {
      const list = await handleModels();
      assert.equal(list.status, 200);
      const listedIds = ((await list.json()) as { data: { id: string }[] }).data.map((model) => model.id);
      assert.equal(listedIds.includes("listed-codex-id"), true, "a whitelisted gateway id stays listed");
      assert.equal(listedIds.includes("hidden-codex-id"), false, "the whitelist still hides gateway ids");
      assert.equal(listedIds.includes("vendor/alpha"), true, "every served OpenRouter id is listed");
      assert.equal(listedIds.includes("vendor/beta"), true, "every served OpenRouter id is listed");
      assert.equal(listedIds.includes("typesafe/jev-latest"), true, "the System One id stays listed");

      const capabilities = await handleModelCapabilities();
      assert.equal(capabilities.status, 200);
      const capabilityIds = ((await capabilities.json()) as { data: { id: string }[] }).data.map((model) => model.id);
      assert.equal(capabilityIds.includes("vendor/alpha"), true, "capabilities advertise the served OpenRouter ids");
      assert.equal(capabilityIds.includes("vendor/beta"), true, "capabilities advertise the served OpenRouter ids");
      assert.equal(capabilityIds.includes("hidden-codex-id"), false, "capabilities still honor the whitelist");

      const catalog = await handlePublicModelCatalog();
      assert.equal(catalog.status, 200);
      const entries = ((await catalog.json()) as { data: { id: string; providers: { id: string }[] }[] }).data;
      assert.deepEqual(
        entries.find((entry) => entry.id === "vendor/alpha")?.providers.map((provider) => provider.id),
        ["openrouter"],
        "an enabled OpenRouter row stays on the public page"
      );
      assert.equal(
        entries.some((entry) => entry.id === "listed-codex-id"),
        true,
        "an enabled gateway id stays on the public page"
      );
      assert.equal(
        entries.some((entry) => entry.id === "hidden-codex-id"),
        false,
        "the public catalogue still honors the whitelist"
      );
      assert.equal(
        entries.some((entry) => entry.id === "vendor/beta"),
        false,
        "a disabled OpenRouter row leaves the public page"
      );

      // An empty whitelist is the documented no-filter contract, so the dynamic
      // OpenRouter rows return without an operator re-save.
      kv.values.set(keyOf([...CODEX_MODELS_WHITELIST_KV_KEY]), { model_ids: [], updated_at_ms: 2 });
      const unfiltered = await handlePublicModelCatalog();
      const unfilteredIds = ((await unfiltered.json()) as { data: { id: string }[] }).data.map((entry) => entry.id);
      assert.equal(unfilteredIds.includes("vendor/beta"), true, "an empty whitelist filters no OpenRouter row");
      assert.equal(unfilteredIds.includes("hidden-codex-id"), true, "an empty whitelist restores every gateway id");
    });
  } finally {
    setKvForTest(null);
    resetProviderSelectionCacheForTest();
    resetRuntimeConfigCacheForTest();
    if (meteredKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", meteredKey);
    if (surplusKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", surplusKey);
  }
});

Deno.test("openrouter codex rows carry only source-stated metadata", async () => {
  await withServedCatalogue(() => {
    const rows = withOpenRouterModels([{ slug: "vendor/alpha", display_name: "stored alpha" }]);
    assert.deepEqual(
      rows.map((row) => row.slug),
      ["vendor/alpha", "vendor/beta"]
    );
    assert.equal(rows[0].display_name, "stored alpha", "an existing row keeps precedence over the appended id");
    const beta = rows[1];
    assert.equal(beta.display_name, "vendor/beta");
    assert.equal(beta.owned_by, "vendor");
    assert.deepEqual(beta.supported_endpoint_types, ["openai-response", "openai-chat"]);
    assert.deepEqual(beta.supported_reasoning_levels, [{ effort: "none", description: "No reasoning" }]);
    assert.equal(beta.default_reasoning_level, "none");
  });
});

/** One recorded upstream SSE body. */
const sseResponse = (frames: readonly string[]): Response =>
  new Response(
    new ReadableStream<Uint8Array>({
      start(controller) {
        const encoder = new TextEncoder();
        for (const frame of frames) controller.enqueue(encoder.encode(frame));
        controller.close();
      },
    }),
    { status: 200, headers: { "content-type": "text/event-stream" } }
  );

const streamUsageContext = (responseTelemetry: ResponseTelemetryState) => ({
  keyId: null,
  kernelRepo: null,
  kernelOrg: null,
  requestId: "openrouter-stream-test",
  startedAtMs: Date.now(),
  startedAtMonotonicMs: performance.now(),
  responseTelemetry,
});

Deno.test("openrouter chat relays validated stream frames and records usage", async () => {
  await withServedCatalogue(async () => {
    const telemetry = createResponseTelemetryState();
    const chunks = [
      { id: "gen-1", object: "chat.completion.chunk", choices: [{ index: 0, delta: { role: "assistant", content: "pi" }, finish_reason: null }] },
      { id: "gen-1", object: "chat.completion.chunk", choices: [], usage: { prompt_tokens: 3, completion_tokens: 2, total_tokens: 5 } },
    ];
    const response = await handleOpenRouterChatCompletions(
      new Request("https://ai.ubq.fi/v1/chat/completions", { method: "POST" }),
      { model: "vendor/alpha", messages: [{ role: "user", content: "hi" }], stream: true },
      "vendor/alpha",
      streamUsageContext(telemetry),
      {
        fetchChat: (body) => {
          assert.equal(body.stream, true);
          assert.deepEqual(body.stream_options, { include_usage: true });
          return Promise.resolve(sseResponse([`data: ${JSON.stringify(chunks[0])}\n\n`, `data: ${JSON.stringify(chunks[1])}\n\n`, "data: [DONE]\n\n"]));
        },
      }
    );
    assert.equal(response.status, 200);
    const body = await response.text();
    assert.match(body, /"content":"pi"/);
    assert.match(body, /data: \[DONE\]/);
    assert.equal(telemetry.inputTokens, 3);
    assert.equal(telemetry.outputTokens, 2);
    assert.equal(telemetry.usageObserved, true);
    assert.equal(telemetry.streamTerminalType, "response.completed");
  });
});

Deno.test("openrouter chat reports a malformed stream frame as a stream error", async () => {
  await withServedCatalogue(async () => {
    const telemetry = createResponseTelemetryState();
    const response = await handleOpenRouterChatCompletions(
      new Request("https://ai.ubq.fi/v1/chat/completions", { method: "POST" }),
      { model: "vendor/alpha", stream: true },
      "vendor/alpha",
      streamUsageContext(telemetry),
      { fetchChat: () => Promise.resolve(sseResponse(["data: {not json}\n\n", "data: [DONE]\n\n"])) }
    );
    assert.equal(response.status, 200);
    const body = await response.text();
    assert.match(body, /openrouter_upstream_stream_error/);
    assert.equal(telemetry.streamTerminalType, "error");
    assert.equal(telemetry.failureKind, "malformed_event");
  });
});

Deno.test("openrouter responses relays native events and records the terminal", async () => {
  await withServedCatalogue(async () => {
    const telemetry = createResponseTelemetryState();
    const events = [
      { type: "response.created", response: { id: "resp_1" } },
      { type: "response.output_text.delta", delta: "ok" },
      { type: "response.completed", response: { id: "resp_1", usage: { input_tokens: 4, output_tokens: 1, total_tokens: 5 } } },
    ];
    const response = await handleOpenRouterResponses(
      new Request("https://ai.ubq.fi/v1/responses", { method: "POST" }),
      { model: "vendor/beta", input: "hi", stream: true },
      "vendor/beta",
      streamUsageContext(telemetry),
      { fetchResponses: () => Promise.resolve(sseResponse(events.map((event) => `event: ${event.type}\ndata: ${JSON.stringify(event)}\n\n`))) }
    );
    assert.equal(response.status, 200);
    const body = await response.text();
    assert.match(body, /response.output_text.delta/);
    assert.match(body, /response.completed/);
    assert.equal(telemetry.streamTerminalType, "response.completed");
    assert.equal(telemetry.outputTokens, 1);
  });
});

Deno.test("openrouter resolves a served id from a cold catalogue and stays cache-authoritative once warm", async () => {
  resetOpenRouterModelsCacheForTest();
  Deno.env.set("OPENROUTER_API_KEY", "fixture-openrouter-key");
  let fetches = 0;
  setOpenRouterModelsFetchForTest((() => {
    fetches += 1;
    return Promise.resolve(jsonResponse(catalogue));
  }) as typeof fetch);
  try {
    assert.equal(await resolveOpenRouterUpstreamModel("vendor/alpha"), "vendor/alpha", "a cold cache refreshes before refusing a served id");
    assert.equal(fetches, 1);
    assert.equal(await resolveOpenRouterUpstreamModel("vendor/gamma"), null, "a warm cache stays authoritative");
    assert.equal(fetches, 1, "no extra refresh once the catalogue is warm");
    Deno.env.delete("OPENROUTER_API_KEY");
    resetOpenRouterModelsCacheForTest();
    assert.equal(await resolveOpenRouterUpstreamModel("vendor/alpha"), null, "an unconfigured upstream never fetches");
    assert.equal(fetches, 1);
  } finally {
    setOpenRouterModelsFetchForTest(null);
    resetOpenRouterModelsCacheForTest();
    Deno.env.delete("OPENROUTER_API_KEY");
  }
});

Deno.test("isOpenRouterModelIdShape validates canonical author/slug structure", () => {
  assert.equal(isOpenRouterModelIdShape("vendor/alpha"), true);
  assert.equal(isOpenRouterModelIdShape("openai/gpt-4o"), true);
  assert.equal(isOpenRouterModelIdShape("anthropic/claude-3.5-sonnet"), true);
  assert.equal(isOpenRouterModelIdShape("meta-llama/llama-3.1-70b-instruct"), true);
  assert.equal(isOpenRouterModelIdShape("mistralai/mixtral-8x7b"), true);

  assert.equal(isOpenRouterModelIdShape("gpt-4o"), false);
  assert.equal(isOpenRouterModelIdShape("o3-mini"), false);
  assert.equal(isOpenRouterModelIdShape("claude-3-5-sonnet"), false);
  assert.equal(isOpenRouterModelIdShape("text-embedding-3-small"), false);
  assert.equal(isOpenRouterModelIdShape(""), false);
  assert.equal(isOpenRouterModelIdShape("   "), false);
  assert.equal(isOpenRouterModelIdShape("/vendor/alpha"), false);
  assert.equal(isOpenRouterModelIdShape("vendor/alpha/"), false);
  assert.equal(isOpenRouterModelIdShape("vendor/alpha/beta"), false);
  assert.equal(isOpenRouterModelIdShape("vendor/ alpha"), false);
  assert.equal(isOpenRouterModelIdShape("vendor /alpha"), false);
  assert.equal(isOpenRouterModelIdShape("vendor/alpha beta"), false);
});

Deno.test("cold openrouter catalogue refresh is skipped for non-namespaced model ids", async () => {
  resetOpenRouterModelsCacheForTest();
  Deno.env.set("OPENROUTER_API_KEY", "fixture-openrouter-key");
  let fetches = 0;
  setOpenRouterModelsFetchForTest((() => {
    fetches += 1;
    return Promise.resolve(jsonResponse(catalogue));
  }) as typeof fetch);
  try {
    assert.equal(await resolveOpenRouterUpstreamModel("gpt-4o"), null);
    assert.equal(await resolveOpenRouterUpstreamModel("o3-mini"), null);
    assert.equal(await resolveOpenRouterUpstreamModel("chatgpt-4o-latest"), null);
    assert.equal(fetches, 0, "non-namespaced models must not trigger an OpenRouter catalogue fetch");
    assert.equal(await resolveOpenRouterUpstreamModel("vendor/alpha"), "vendor/alpha");
    assert.equal(fetches, 1, "namespaced model triggers catalogue fetch on cold cache");
  } finally {
    setOpenRouterModelsFetchForTest(null);
    resetOpenRouterModelsCacheForTest();
    Deno.env.delete("OPENROUTER_API_KEY");
  }
});

Deno.test("responses wire does not trigger cold openrouter catalogue fetch when openrouter is disabled", async () => {
  const kv = new CatalogKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetOpenRouterModelsCacheForTest();
  resetProviderSelectionCacheForTest();
  Deno.env.set("OPENROUTER_API_KEY", "fixture-openrouter-key");

  // Disable openrouter by explicitly selecting only codex
  await kv.set(PROVIDER_SELECTION_KV_KEY, {
    provider_ids: ["codex"],
    updated_at_ms: Date.now(),
  });

  let fetches = 0;
  setOpenRouterModelsFetchForTest((() => {
    fetches += 1;
    return Promise.resolve(jsonResponse(catalogue));
  }) as typeof fetch);

  try {
    const req = new Request("https://ai.ubq.fi/v1/responses", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ model: "vendor/alpha", input: "test" }),
    });

    await handleResponses(req);
    assert.equal(fetches, 0, "disabled openrouter provider must not trigger catalogue fetch");
  } finally {
    setOpenRouterModelsFetchForTest(null);
    resetOpenRouterModelsCacheForTest();
    resetProviderSelectionCacheForTest();
    Deno.env.delete("OPENROUTER_API_KEY");
    setKvForTest(null);
  }
});

