import assert from "node:assert/strict";

import { buildDeepSeekWaterfallCodexRecord } from "../src/catalog/models.ts";
import { handleDeepSeekWaterfallResponses, type DeepSeekWaterfallDispatch } from "../src/deepseek/waterfall-handler.ts";
import {
  DEEPSEEK_WATERFALL_DEFAULT_REASONING_LEVEL,
  DEEPSEEK_WATERFALL_DISPLAY_NAME,
  DEEPSEEK_WATERFALL_MODEL_ID,
  DEEPSEEK_WATERFALL_ORDER,
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

Deno.test("waterfall: the synthetic id normalizes and resolves to the measured provider order", () => {
  assert.equal(isDeepSeekWaterfallModel("  Ubiquity/DeepSeek-V4.1-Flash "), true);
  assert.equal(isDeepSeekWaterfallModel("deepseek-flash"), false);
  assert.deepEqual(DEEPSEEK_WATERFALL_ORDER, ["openrouter", "lithos", "deepseek"]);
});

Deno.test("waterfall: the plan keeps order and drops switched-off or uncredentialed hops", () => {
  assert.deepEqual(deepSeekWaterfallPlan(allEnabled), ["openrouter", "lithos", "deepseek"]);
  assert.deepEqual(
    deepSeekWaterfallPlan({
      enabled: (provider) => provider !== "lithos",
      configured: (provider) => provider !== "deepseek",
    }),
    ["openrouter"]
  );
  assert.deepEqual(deepSeekWaterfallPlan({ enabled: () => false, configured: () => true }), []);
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
  const response = await handleDeepSeekWaterfallResponses(request(), raw, undefined, { selection: null, dispatch });
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
    if (provider === "openrouter") return Promise.resolve(Response.json({ error: { message: "down" } }, { status: 503 }));
    return Promise.resolve(Response.json({ id: "resp_ok", object: "response" }, { headers: { "x-uos-upstream": "lithos" } }));
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), context, {
    ...allEnabled,
    selection: null,
    dispatch,
  });
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("x-uos-attempted-providers"), "openrouter,lithos");
  assert.deepEqual(
    seen.map((entry) => [entry.provider, entry.model, entry.recordModel]),
    [
      ["openrouter", DEEPSEEK_WATERFALL_PROVIDER_MODEL.openrouter, DEEPSEEK_WATERFALL_PROVIDER_MODEL.openrouter],
      ["lithos", DEEPSEEK_WATERFALL_PROVIDER_MODEL.lithos, DEEPSEEK_WATERFALL_PROVIDER_MODEL.lithos],
    ]
  );
  assert.deepEqual(telemetry.attemptedProviders, ["openrouter", "lithos"]);
  assert.equal(telemetry.fallbackReason, "deepseek_waterfall:openrouter:503");
});

Deno.test("waterfall: a client-facing 4xx is the answer and never spends another provider", async () => {
  const attempted: string[] = [];
  const dispatch: DeepSeekWaterfallDispatch = ({ provider }) => {
    attempted.push(provider);
    return Promise.resolve(Response.json({ error: { message: "bad field", type: "invalid_request_error" } }, { status: 400 }));
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), undefined, {
    ...allEnabled,
    selection: null,
    dispatch,
  });
  assert.equal(response.status, 400);
  assert.deepEqual(attempted, ["openrouter"]);
  assert.equal(response.headers.get("x-uos-attempted-providers"), "openrouter");
});

Deno.test("waterfall: a transport failure advances and an exhausted chain returns the last failure", async () => {
  const { context, telemetry } = usageContextWithTelemetry();
  const attempted: string[] = [];
  const dispatch: DeepSeekWaterfallDispatch = ({ provider }) => {
    attempted.push(provider);
    if (provider === "openrouter") return Promise.reject(new Error("connect refused"));
    return Promise.resolve(Response.json({ error: { message: "upstream down" } }, { status: 500 }));
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), context, {
    ...allEnabled,
    selection: null,
    dispatch,
  });
  assert.equal(response.status, 500);
  assert.deepEqual(attempted, ["openrouter", "lithos", "deepseek"]);
  assert.equal(response.headers.get("x-uos-attempted-providers"), "openrouter,lithos,deepseek");
  assert.equal(telemetry.fallbackReason, "deepseek_waterfall:openrouter:transport_failure");
});

Deno.test("waterfall: no usable hop is a 503 without dispatching", async () => {
  let calls = 0;
  const dispatch: DeepSeekWaterfallDispatch = () => {
    calls += 1;
    return Promise.resolve(Response.json({}));
  };
  const response = await handleDeepSeekWaterfallResponses(request(), record(DEEPSEEK_WATERFALL_MODEL_ID), undefined, {
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
