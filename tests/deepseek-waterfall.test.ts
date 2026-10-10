import assert from "node:assert/strict";

import { buildDeepSeekWaterfallCodexRecord } from "../src/catalog/models.ts";
import { handleDeepSeekWaterfallResponses, type DeepSeekWaterfallDispatch, type DeepSeekWaterfallPaidTail } from "../src/deepseek/waterfall-handler.ts";
import { resolvePaidRoutingState } from "../src/paid-fallback/health.ts";
import {
  DEEPSEEK_WATERFALL_DEFAULT_REASONING_LEVEL,
  DEEPSEEK_WATERFALL_DISPLAY_NAME,
  DEEPSEEK_WATERFALL_MODEL_ID,
  DEEPSEEK_WATERFALL_ORDER,
  DEEPSEEK_WATERFALL_PAID_MODEL_ID,
  DEEPSEEK_WATERFALL_PAID_PIN,
  DEEPSEEK_WATERFALL_PROVIDER_MODEL,
  DEEPSEEK_WATERFALL_REASONING_LEVELS,
  deepSeekWaterfallFailureReason,
  deepSeekWaterfallPlan,
  deepSeekWaterfallRequestedEffort,
  isDeepSeekWaterfallModel,
  isDeepSeekWaterfallReasoningLevel,
  isDeepSeekWaterfallRetryStatus,
} from "../src/deepseek/waterfall.ts";
import type { UsageContext } from "../src/openai-telemetry.ts";

const allEnabled = { enabled: () => true, configured: () => true };

type TelemetrySpy = Readonly<{ attemptedProviders: string[]; fallbackReason: string | null }>;

const usageContextWithTelemetry = (): Readonly<{ context: UsageContext; telemetry: TelemetrySpy }> => {
  const telemetry = { attemptedProviders: [] as string[], fallbackReason: null as string | null };
  const context = {
    keyId: null,
    kernelRepo: null,
    kernelOrg: null,
    requestId: "waterfall-test",
    startedAtMs: Date.now(),
    startedAtMonotonicMs: performance.now(),
    responseTelemetry: telemetry,
  } as unknown as UsageContext;
  return { context, telemetry };
};

const request = (): Request => new Request("http://127.0.0.1:7999/v1/responses", { method: "POST", body: "{}" });

const record = (model: string): Record<string, unknown> => ({ model, input: [{ type: "message", role: "user", content: "hi" }] });

Deno.test("waterfall: the synthetic id normalizes and resolves to the cost-first economy order", () => {
  assert.equal(isDeepSeekWaterfallModel("  Ubiquity/DeepSeek-V4.1-Flash "), true);
  assert.equal(isDeepSeekWaterfallModel("deepseek-flash"), false);
  assert.deepEqual(DEEPSEEK_WATERFALL_ORDER, ["lithos", "deepseek", "openrouter", "surplus", "openlux"]);
});

Deno.test("waterfall: the plan keeps order and drops switched-off or uncredentialed hops", () => {
  assert.deepEqual(deepSeekWaterfallPlan(allEnabled), ["lithos", "deepseek", "openrouter", "surplus", "openlux"]);
  assert.deepEqual(
    deepSeekWaterfallPlan({
      enabled: (provider) => provider !== "lithos",
      configured: (provider) => provider !== "deepseek",
    }),
    ["openrouter", "surplus", "openlux"]
  );
  assert.deepEqual(deepSeekWaterfallPlan({ enabled: () => false, configured: () => true }), []);
});

Deno.test("waterfall: each paid hop is pinned to its own paid tier", () => {
  assert.deepEqual(DEEPSEEK_WATERFALL_PAID_PIN, { surplus: ["surplus"], openlux: ["metered"] });
});

Deno.test("waterfall: only infrastructure or serving statuses advance the chain", () => {
  for (const status of [402, 403, 429, 500, 502, 504]) assert.equal(isDeepSeekWaterfallRetryStatus(status), true, `${status} must advance`);
  for (const status of [200, 400, 404, 422]) assert.equal(isDeepSeekWaterfallRetryStatus(status), false, `${status} must not advance`);
});

Deno.test("waterfall: the requested effort reads both wire fields and stays inside the advertised set", () => {
  assert.equal(deepSeekWaterfallRequestedEffort({ reasoning: { effort: " max " } }), "max");
  assert.equal(deepSeekWaterfallRequestedEffort({ reasoning_effort: "low" }), "low");
  assert.equal(deepSeekWaterfallRequestedEffort({}), null);
  assert.deepEqual([...DEEPSEEK_WATERFALL_REASONING_LEVELS], ["low", "high", "max"]);
  assert.equal(isDeepSeekWaterfallReasoningLevel("high"), true);
  assert.equal(isDeepSeekWaterfallReasoningLevel("none"), false);
  assert.equal(deepSeekWaterfallFailureReason("openrouter", 503), "deepseek_waterfall:openrouter:503");
  assert.equal(deepSeekWaterfallFailureReason("lithos", "transport"), "deepseek_waterfall:lithos:transport_failure");
});

Deno.test("waterfall: handler rejects an unadvertised effort before dispatching anything", async () => {
  let calls = 0;
  const dispatch: DeepSeekWaterfallDispatch = () => {
    calls += 1;
    return Promise.resolve(Response.json({ ok: true }));
  };
  const raw = { model: DEEPSEEK_WATERFALL_MODEL_ID, reasoning: { effort: "none" } };
  const response = await handleDeepSeekWaterfallResponses(request(), raw, {}, undefined, { selection: null, dispatch });
  assert.equal(calls, 0);
  assert.equal(response.status, 400);
  const body = (await response.json()) as { error?: { code?: string } };
  assert.equal(body.error?.code, "unsupported_reasoning_effort");
});

Deno.test("waterfall: a failing first hop serves from the next provider and records the chain", async () => {
  const { context, telemetry } = usageContextWithTelemetry();
  const seen: Readonly<{ provider: string; model: string; recordModel: unknown }>[] = [];
  const dispatch: DeepSeekWaterfallDispatch = ({ provider, model, rawRecord }) => {
    seen.push({ provider, model, recordModel: rawRecord.model });
    if (provider === "lithos") return Promise.resolve(Response.json({ error: { message: "down" } }, { status: 503 }));
    return Promise.resolve(Response.json({ id: "resp_ok", object: "response" }, { headers: { "x-uos-upstream": "deepseek" } }));
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), {}, context, {
    ...allEnabled,
    selection: null,
    dispatch,
  });
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("x-uos-attempted-providers"), "lithos,deepseek");
  assert.deepEqual(
    seen.map((entry) => [entry.provider, entry.model, entry.recordModel]),
    [
      ["lithos", DEEPSEEK_WATERFALL_PROVIDER_MODEL.lithos, DEEPSEEK_WATERFALL_PROVIDER_MODEL.lithos],
      ["deepseek", DEEPSEEK_WATERFALL_PROVIDER_MODEL.deepseek, DEEPSEEK_WATERFALL_PROVIDER_MODEL.deepseek],
    ]
  );
  assert.deepEqual(telemetry.attemptedProviders, ["lithos", "deepseek"]);
  assert.equal(telemetry.fallbackReason, "deepseek_waterfall:lithos:503");
});

Deno.test("waterfall: a client-facing 4xx is the answer and never spends another provider", async () => {
  const attempted: string[] = [];
  const dispatch: DeepSeekWaterfallDispatch = ({ provider }) => {
    attempted.push(provider);
    return Promise.resolve(Response.json({ error: { message: "bad field", type: "invalid_request_error" } }, { status: 400 }));
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), {}, undefined, {
    ...allEnabled,
    selection: null,
    dispatch,
  });
  assert.equal(response.status, 400);
  assert.deepEqual(attempted, ["lithos"]);
  assert.equal(response.headers.get("x-uos-attempted-providers"), "lithos");
});

Deno.test("waterfall: a transport failure advances and an exhausted chain returns the last failure", async () => {
  const { context, telemetry } = usageContextWithTelemetry();
  const attempted: string[] = [];
  const dispatch: DeepSeekWaterfallDispatch = ({ provider }) => {
    attempted.push(provider);
    if (provider === "lithos") return Promise.reject(new Error("connect refused"));
    return Promise.resolve(Response.json({ error: { message: "upstream down" } }, { status: 500 }));
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), {}, context, {
    ...allEnabled,
    selection: null,
    dispatch,
  });
  assert.equal(response.status, 500);
  assert.deepEqual(attempted, ["lithos", "deepseek", "openrouter", "surplus", "openlux"]);
  assert.equal(response.headers.get("x-uos-attempted-providers"), "lithos,deepseek,openrouter,surplus,openlux");
  assert.equal(telemetry.fallbackReason, "deepseek_waterfall:lithos:transport_failure");
});

Deno.test("waterfall: no usable hop is a 503 without dispatching", async () => {
  let calls = 0;
  const dispatch: DeepSeekWaterfallDispatch = () => {
    calls += 1;
    return Promise.resolve(Response.json({}));
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), {}, undefined, {
    enabled: () => false,
    configured: () => true,
    selection: null,
    dispatch,
  });
  assert.equal(response.status, 503);
  assert.equal(calls, 0);
});

Deno.test("waterfall: the Codex catalogue record carries the advertised contract", () => {
  const row = buildDeepSeekWaterfallCodexRecord();
  assert.equal(row.slug, DEEPSEEK_WATERFALL_MODEL_ID);
  assert.equal(row.display_name, DEEPSEEK_WATERFALL_DISPLAY_NAME);
  assert.equal(row.default_reasoning_level, DEEPSEEK_WATERFALL_DEFAULT_REASONING_LEVEL);
  assert.deepEqual(
    (row.supported_reasoning_levels as Readonly<{ effort: string }>[]).map((level) => level.effort),
    [...DEEPSEEK_WATERFALL_REASONING_LEVELS]
  );
  assert.deepEqual(row.supported_endpoint_types, ["openai-response"]);
  assert.equal(row.context_window, 1_048_576);
});

const sseBody = (events: readonly Record<string, unknown>[]): string =>
  events.map((event) => `event: ${String(event.type)}\ndata: ${JSON.stringify(event)}\n\n`).join("");

const sseResponse = (body: string): Response => new Response(body, { headers: { "content-type": "text/event-stream" } });

const createdEvent = (id: string): Record<string, unknown> => ({
  type: "response.created",
  sequence_number: 0,
  response: { id, object: "response", status: "in_progress" },
});

const deltaEvent = (): Record<string, unknown> => ({
  type: "response.output_text.delta",
  sequence_number: 1,
  item_id: "msg_wf",
  output_index: 0,
  content_index: 0,
  delta: "he",
});

const completedEvent = (id: string): Record<string, unknown> => ({
  type: "response.completed",
  sequence_number: 2,
  response: { id, object: "response", status: "completed", output: [] },
});

Deno.test("waterfall: the surplus hop re-enters the paid pipeline under the paid catalogue id, pinned to surplus", async () => {
  const seen: Record<string, unknown>[] = [];
  const pins: (readonly ("metered" | "surplus")[] | null | undefined)[] = [];
  const paidTail: DeepSeekWaterfallPaidTail = async (req, rawRecord, rawBody, _usageContext, options) => {
    seen.push(JSON.parse(await req.text()) as Record<string, unknown>);
    pins.push(options?.allowedPaidProviders);
    assert.equal(rawRecord.model, DEEPSEEK_WATERFALL_PAID_MODEL_ID);
    assert.equal(rawBody.model, DEEPSEEK_WATERFALL_PAID_MODEL_ID);
    assert.equal(options?.deepSeekWaterfallPaidHop, "surplus");
    assert.equal(rawRecord.deepSeekWaterfallPaidHop, undefined);
    return Response.json({ id: "resp_paid" });
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), { model: DEEPSEEK_WATERFALL_MODEL_ID }, undefined, {
    enabled: (provider) => provider === "surplus",
    configured: () => true,
    selection: null,
    paidTail,
  });
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("x-uos-attempted-providers"), "surplus");
  assert.equal(seen.length, 1);
  assert.equal(seen[0].model, DEEPSEEK_WATERFALL_PAID_MODEL_ID);
  assert.deepEqual(pins, [["surplus"]], "the surplus hop must never silently advance to metered");
});

Deno.test("waterfall: the openlux hop re-enters the paid pipeline pinned to metered", async () => {
  const seen: Record<string, unknown>[] = [];
  const pins: (readonly ("metered" | "surplus")[] | null | undefined)[] = [];
  const paidTail: DeepSeekWaterfallPaidTail = async (req, rawRecord, rawBody, _usageContext, options) => {
    seen.push(JSON.parse(await req.text()) as Record<string, unknown>);
    pins.push(options?.allowedPaidProviders);
    assert.equal(rawRecord.model, DEEPSEEK_WATERFALL_PAID_MODEL_ID);
    assert.equal(rawBody.model, DEEPSEEK_WATERFALL_PAID_MODEL_ID);
    assert.equal(options?.deepSeekWaterfallPaidHop, "openlux");
    assert.equal(rawRecord.deepSeekWaterfallPaidHop, undefined);
    return Response.json({ id: "resp_openlux" });
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), { model: DEEPSEEK_WATERFALL_MODEL_ID }, undefined, {
    enabled: (provider) => provider === "openlux",
    configured: () => true,
    selection: null,
    paidTail,
  });
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("x-uos-attempted-providers"), "openlux");
  assert.equal(seen.length, 1);
  assert.equal(seen[0].model, DEEPSEEK_WATERFALL_PAID_MODEL_ID);
  assert.deepEqual(pins, [["metered"]], "the routing layer names OpenLux 'metered'");
});

Deno.test("waterfall: openlux as the last hop serves when every prior paid hop fails, pinned to metered", async () => {
  const { context, telemetry } = usageContextWithTelemetry();
  const pins: (readonly ("metered" | "surplus")[] | null | undefined)[] = [];
  // Only the two paid hops are configured, so the plan is [surplus, openlux]
  // and every hop runs through the production default dispatch into the tail.
  const paidTail: DeepSeekWaterfallPaidTail = async (req, _rawRecord, _rawBody, _usageContext, options) => {
    pins.push(options?.allowedPaidProviders);
    const body = JSON.parse(await req.text()) as Record<string, unknown>;
    assert.equal(body.model, DEEPSEEK_WATERFALL_PAID_MODEL_ID);
    if (options?.allowedPaidProviders?.includes("surplus")) {
      return Response.json({ error: { message: "surplus capacity exhausted" } }, { status: 503 });
    }
    return Response.json({ id: "resp_openlux", object: "response" });
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), {}, context, {
    enabled: () => true,
    configured: (provider) => provider === "surplus" || provider === "openlux",
    selection: null,
    paidTail,
  });
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("x-uos-attempted-providers"), "surplus,openlux");
  assert.deepEqual(pins, [["surplus"], ["metered"]], "the surplus hop is pinned to surplus and the openlux hop to metered");
  assert.equal(telemetry.fallbackReason, "deepseek_waterfall:surplus:503");
});

Deno.test("waterfall: a stream that dies before output advances, and one that dies after output does not", async () => {
  const { context, telemetry } = usageContextWithTelemetry();
  const attemptedProviders: string[] = [];
  const dispatch: DeepSeekWaterfallDispatch = ({ provider }) => {
    attemptedProviders.push(provider);
    if (provider === "lithos") return Promise.resolve(sseResponse(sseBody([createdEvent("resp_li")])));
    return Promise.resolve(sseResponse(sseBody([createdEvent("resp_or"), deltaEvent(), completedEvent("resp_or")])));
  };
  const advanced = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), {}, context, {
    enabled: (provider) => provider === "lithos" || provider === "openrouter",
    configured: () => true,
    selection: null,
    dispatch,
  });
  assert.equal(advanced.status, 200);
  assert.equal(advanced.headers.get("x-uos-attempted-providers"), "lithos,openrouter");
  assert.deepEqual(attemptedProviders, ["lithos", "openrouter"]);
  assert.equal(telemetry.fallbackReason, "deepseek_waterfall:lithos:stream_failure");
  const advancedBody = await advanced.text();
  assert.match(advancedBody, /"delta":"he"/);

  const committedProviders: string[] = [];
  const committedDispatch: DeepSeekWaterfallDispatch = ({ provider }) => {
    committedProviders.push(provider);
    return Promise.resolve(sseResponse(sseBody([createdEvent("resp_li"), deltaEvent(), completedEvent("resp_li")])));
  };
  const committed = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), {}, undefined, {
    enabled: () => true,
    configured: () => true,
    selection: null,
    dispatch: committedDispatch,
  });
  assert.equal(committed.status, 200);
  assert.deepEqual(committedProviders, ["lithos"]);
  assert.equal(committed.headers.get("x-uos-attempted-providers"), "lithos");
  assert.match(await committed.text(), /"delta":"he"/);
});

Deno.test("waterfall: allowedPaidProviders pins the Surplus hop without changing the default order", () => {
  const savedSurplus = Deno.env.get("SURPLUS_API_KEY");
  const savedMetered = Deno.env.get("METERED_API_KEY");
  Deno.env.set("SURPLUS_API_KEY", "fixture-surplus-key");
  Deno.env.set("METERED_API_KEY", "fixture-metered-key");
  try {
    const meteredCatalog = { models: [{ id: "deepseek-v4.1-flash", supported_endpoint_types: ["openai-response"] }], updated_at_ms: Date.now() };
    const surplusCatalog = {
      models: [
        {
          id: "deepseek-v4.1-flash",
          supported_endpoint_types: ["openai-response"],
          supports_tools: true,
          input_price_per_token: 0.0000003,
          output_price_per_token: 0.0000012,
        },
      ],
      updated_at_ms: Date.now(),
    };
    const base = {
      meteredCatalog,
      surplusCatalog,
      codexModelKnown: false,
      endpointType: "openai-response",
      requestUsesTools: true,
      model: "deepseek-v4.1-flash",
      selection: null,
    };
    assert.deepEqual(resolvePaidRoutingState(base as never).paidProviders, ["surplus", "metered"]);
    assert.deepEqual(resolvePaidRoutingState({ ...base, allowedPaidProviders: ["surplus"] } as never).paidProviders, ["surplus"]);
    assert.deepEqual(resolvePaidRoutingState({ ...base, allowedPaidProviders: ["metered"] } as never).paidProviders, ["metered"]);
    const gatewaySelection = { provider_ids: ["ubiquity"], updated_at_ms: 1 };
    assert.deepEqual(resolvePaidRoutingState({ ...base, selection: gatewaySelection } as never).paidProviders, []);
    assert.deepEqual(
      resolvePaidRoutingState({ ...base, selection: gatewaySelection, allowedPaidProviders: ["surplus"], deepSeekWaterfallPaidHop: "surplus" } as never)
        .paidProviders,
      ["surplus"]
    );
    assert.deepEqual(
      resolvePaidRoutingState({ ...base, selection: gatewaySelection, allowedPaidProviders: ["metered"], deepSeekWaterfallPaidHop: "openlux" } as never)
        .paidProviders,
      ["metered"]
    );
  } finally {
    if (savedSurplus === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", savedSurplus);
    if (savedMetered === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", savedMetered);
  }
});

Deno.test("waterfall: an active selection without the gateway identity is a 503 without dispatching", async () => {
  let calls = 0;
  const dispatch: DeepSeekWaterfallDispatch = () => {
    calls += 1;
    return Promise.resolve(Response.json({}));
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), {}, undefined, {
    selection: { provider_ids: ["openrouter"] as const, updated_at_ms: 1 },
    configured: () => true,
    dispatch,
  });
  assert.equal(response.status, 503);
  assert.equal(calls, 0);
});

Deno.test("waterfall: checking the gateway identity serves the configured chain", async () => {
  const seen: string[] = [];
  const dispatch: DeepSeekWaterfallDispatch = ({ provider }) => {
    seen.push(provider);
    return Promise.resolve(Response.json({ id: "resp_ok", object: "response" }));
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), {}, undefined, {
    selection: { provider_ids: ["ubiquity"] as const, updated_at_ms: 1 },
    configured: () => true,
    dispatch,
  });
  assert.equal(response.status, 200);
  assert.deepEqual(seen, ["lithos"], "the first configured hop of the fixed order serves");
  assert.equal(response.headers.get("x-uos-attempted-providers"), "lithos");
});

Deno.test("waterfall: verified OpenLux Responses capability stays scoped to the exact synthetic paid hop", () => {
  const savedMetered = Deno.env.get("METERED_API_KEY");
  Deno.env.set("METERED_API_KEY", "fixture-metered-key");
  try {
    const model = {
      id: DEEPSEEK_WATERFALL_PAID_MODEL_ID,
      object: "model" as const,
      created: 0,
      owned_by: "openlux",
      supported_endpoint_types: ["openai", "anthropic"],
    };
    const base: Parameters<typeof resolvePaidRoutingState>[0] = {
      meteredCatalog: { models: [model], updated_at_ms: Date.now() },
      surplusCatalog: null,
      codexModelKnown: false,
      endpointType: "openai-response",
      requestUsesTools: true,
      model: DEEPSEEK_WATERFALL_PAID_MODEL_ID,
      selection: { provider_ids: ["ubiquity"], updated_at_ms: 1 },
      allowedPaidProviders: ["metered"],
      deepSeekWaterfallPaidHop: "openlux",
    };
    assert.deepEqual(resolvePaidRoutingState(base).paidProviders, ["metered"]);
    const rejected: Parameters<typeof resolvePaidRoutingState>[0][] = [
      { ...base, deepSeekWaterfallPaidHop: null },
      { ...base, selection: null, deepSeekWaterfallPaidHop: null },
      { ...base, meteredCatalog: null },
      { ...base, meteredCatalog: { models: [], updated_at_ms: Date.now() } },
      { ...base, model: "unverified-model", meteredCatalog: { models: [{ ...model, id: "unverified-model" }], updated_at_ms: Date.now() } },
      { ...base, endpointType: "anthropic" },
      { ...base, selection: { provider_ids: ["codex"], updated_at_ms: 1 } },
      { ...base, allowedPaidProviders: null },
      { ...base, allowedPaidProviders: ["surplus", "metered"] },
      { ...base, allowedPaidProviders: ["surplus"] },
      { ...base, deepSeekWaterfallPaidHop: "surplus" },
    ];
    for (const input of rejected) assert.deepEqual(resolvePaidRoutingState(input).paidProviders, []);
    Deno.env.delete("METERED_API_KEY");
    assert.deepEqual(resolvePaidRoutingState(base).paidProviders, [], "a capability exception never supplies credentials");
  } finally {
    if (savedMetered === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", savedMetered);
  }
});

Deno.test("waterfall: the verified OpenLux hop dispatches through paid admission and records its completed terminal", async () => {
  const {
    handleResponses,
    fetchMeteredModels,
    keyToString,
    kvStore,
    resetMeteredModelsCacheForTest,
    resetSurplusModelsCacheForTest,
    responsesRequest,
    seedPaidFallbackKey,
    waitForPaidFallbackTerminal,
    withDiscoveryKeys,
    withFetchMock,
    withProviderSelection,
  } = await import("./helpers/openai-compat-harness.ts");
  const keyId = "waterfall-openlux-native";
  const requestId = "waterfall-openlux-native-request";
  const calls: string[] = [];
  const dispatched: string[] = [];
  const beforeProviderDispatch: UsageContext["beforeProviderDispatch"] = (provider) => {
    dispatched.push(provider);
    return Promise.resolve(undefined);
  };
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();
  seedPaidFallbackKey(keyId, { modelIds: [DEEPSEEK_WATERFALL_PAID_MODEL_ID] });
  const keyRecordKey = keyToString(["ubq_ai", "api_keys", "id", keyId]);
  const keyRecord = kvStore.get(keyRecordKey) as Record<string, unknown>;
  kvStore.set(keyRecordKey, { ...keyRecord, paid_fallback_max_exposure_microcredits: { [DEEPSEEK_WATERFALL_PAID_MODEL_ID]: 250_000 } });
  try {
    await withDiscoveryKeys("fixture-openlux-key", () =>
      withProviderSelection(["ubiquity"], () =>
        withFetchMock(
          (url, bodyText) => {
            calls.push(url);
            if (url === "https://api.openlux.ai/v1/models") {
              return Response.json({ data: [{ id: DEEPSEEK_WATERFALL_PAID_MODEL_ID, supported_endpoint_types: ["openai", "anthropic"] }] });
            }
            assert.equal(url, "https://api.openlux.ai/v1/responses");
            const wireBody = JSON.parse(bodyText ?? "null") as Record<string, unknown>;
            assert.equal(wireBody.model, DEEPSEEK_WATERFALL_PAID_MODEL_ID);
            assert.deepEqual(wireBody.reasoning, { effort: "max" });
            assert.equal(wireBody.deepSeekWaterfallPaidHop, undefined, "the trusted routing context must never enter the upstream wire body");
            return new Response(sseResponse(sseBody([createdEvent("resp_openlux_native"), deltaEvent(), completedEvent("resp_openlux_native")])).body, {
              headers: { "content-type": "text/event-stream", "x-request-id": "upstream-openlux-native" },
            });
          },
          async () => {
            await fetchMeteredModels({ fetcher: globalThis.fetch, force: true });
            const context: UsageContext = {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId,
              startedAtMs: Date.now(),
              paidFallbackEnabled: true,
              beforeProviderDispatch,
            };
            const response = await handleResponses(
              responsesRequest({ model: DEEPSEEK_WATERFALL_MODEL_ID, reasoning: { effort: "max" }, stream: false }),
              context
            );
            const body = (await response.json()) as Record<string, unknown>;
            assert.equal(response.status, 200, JSON.stringify(body));
            assert.equal(body.status, "completed");
            assert.equal(response.headers.get("x-uos-upstream"), "metered");
            assert.equal(response.headers.get("x-uos-attempted-providers"), "openlux");
            assert.deepEqual(dispatched, ["metered"]);
            assert.deepEqual(calls, ["https://api.openlux.ai/v1/models", "https://api.openlux.ai/v1/responses"]);
            const stored = await waitForPaidFallbackTerminal(keyId, requestId, "completed");
            assert.equal(stored.provider, "metered");
            assert.equal(stored.provider_request_id, "upstream-openlux-native");
            const denied = await handleResponses(responsesRequest({ model: DEEPSEEK_WATERFALL_MODEL_ID, stream: false }), {
              ...context,
              requestId: "waterfall-openlux-disabled-request",
              paidFallbackEnabled: false,
            });
            assert.equal(denied.status, 403, "the verified capability cannot override the caller's paid-provider policy");
            const ordinary = await handleResponses(responsesRequest({ model: DEEPSEEK_WATERFALL_PAID_MODEL_ID, stream: false }));
            assert.equal(ordinary.status, 404, "the ordinary route still requires catalog evidence of Responses support");
            const forged = await handleResponses(responsesRequest({ model: DEEPSEEK_WATERFALL_MODEL_ID, deepSeekWaterfallPaidHop: "openlux" }));
            assert.equal(forged.status, 400, "a client cannot set the trusted synthetic-hop option");
            assert.equal(calls.length, 2, "the rejected client field never dispatches");
          }
        )
      )
    );
  } finally {
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
  }
});
