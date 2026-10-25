import { createFetchMock } from "./helpers/fetch-mock.ts";
import { test } from "bun:test";
import type { Kv } from "#kv";
import assert from "node:assert/strict";

import { handleAdminModelsCatalogGet } from "../src/admin/codex.ts";
import { openRouterCodexModels } from "../src/catalog/models.ts";
import { handleDecisions } from "../src/decisions/handlers.ts";
import { kernelQuotaRouteForRequest, terminalRouteForRequest } from "../src/handler/http.ts";
import { setKvForTest } from "../src/kv.ts";
import { buildModelCatalogSnapshot, handleModelCapabilities, handleModels } from "../src/models/catalog.ts";
import {
  fetchOpenRouterModels,
  openRouterModelSupportsDecisions,
  openRouterModelSupportsText,
  openRouterModelsFromPayload,
  resetOpenRouterModelsCacheForTest,
  setOpenRouterModelsFetchForTest,
} from "../src/models/openrouter-models.ts";
import { getResponseTelemetry, type UsageContext } from "../src/openai-telemetry.ts";
import { OPENROUTER_DECISIONS_URL, openRouterDecisionModelIds, openRouterTextModelIds } from "../src/provider/openrouter.ts";
import { resetProviderHealthThrottleForTest, getOpenRouterProviderHealth } from "../src/provider/health.ts";
import { resetProviderSelectionCacheForTest } from "../src/provider/selection.ts";
import { CountingKv } from "./helpers/counting-kv.ts";

const textModel = {
  id: "vendor/text",
  context_length: 1_000,
  top_provider: { context_length: 1_000, max_completion_tokens: 500 },
  architecture: { input_modalities: ["text"], output_modalities: ["text"] },
  reasoning: { supported_efforts: ["high"], default_effort: "high" },
};

const elephant = {
  id: "baseten/elephant-1:free",
  context_length: 16_384,
  top_provider: { context_length: 16_384, max_completion_tokens: 14_745 },
  architecture: { input_modalities: ["text"], output_modalities: ["decisions"] },
  reasoning: null,
};

const jevAlias = {
  id: "~typesafe/jev-latest",
  context_length: 262_144,
  top_provider: { context_length: 262_144, max_completion_tokens: 235_929 },
  architecture: { input_modalities: ["text", "image"], output_modalities: ["decisions"] },
  reasoning: null,
};

const catalogue = { data: [textModel, elephant, jevAlias] };

const jsonResponse = (body: unknown, status = 200): Response => new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } });

const urlOf = (input: RequestInfo | URL): string => {
  if (typeof input === "string") return input;
  if (input instanceof URL) return input.href;
  return input.url;
};

const bodyOf = (init?: RequestInit): Record<string, unknown> | null => {
  if (typeof init?.body !== "string") return null;
  return JSON.parse(init.body) as Record<string, unknown>;
};

const withServedCatalogue = async (fn: () => Promise<void> | void, payload: unknown = catalogue): Promise<void> => {
  resetOpenRouterModelsCacheForTest();
  resetProviderSelectionCacheForTest();
  resetProviderHealthThrottleForTest();
  setKvForTest(new CountingKv() as unknown as Kv);
  process.env.OPENROUTER_API_KEY = "fixture-openrouter-key";
  setOpenRouterModelsFetchForTest(createFetchMock(() => Promise.resolve(jsonResponse(payload))));
  try {
    await fetchOpenRouterModels();
    await fn();
  } finally {
    setOpenRouterModelsFetchForTest(null);
    resetOpenRouterModelsCacheForTest();
    resetProviderSelectionCacheForTest();
    resetProviderHealthThrottleForTest();
    setKvForTest(null);
    Reflect.deleteProperty(process.env, "OPENROUTER_API_KEY");
  }
};

const request = (body: unknown): Request =>
  new Request("https://ai.ubq.fi/v1/decisions", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });

const decisionBody = {
  model: "baseten/elephant-1:free",
  state: ["Inspect the listing photo and ticket.", { type: "image_url", image_url: { url: "data:image/png;base64,AAAA" } }],
  questions: {
    is_bug: {
      type: "noul",
      instructions: "Is the customer reporting a defect?",
      criteria: { true: "Broken behavior.", false: "A question or feature request." },
    },
    team: {
      type: "choice",
      instructions: "Which team owns this?",
      criteria: { payments: "Checkout or billing.", frontend: "Rendering or browser issues.", account: null },
    },
    urgency: {
      type: "score",
      instructions: "How urgent is this?",
      criteria: ["Later", "This week", "Blocking revenue"],
    },
    product: { type: "box", instructions: "the product in the listing photo" },
  },
  provider: { allow_fallbacks: false },
  session_id: "ticket-42",
  trace: { trace_id: "trace-42", generation_name: "triage" },
  user: "operator-1",
};

const decisionPayload = {
  id: "gen-dec-test",
  model: "baseten/elephant-1-20261021",
  provider: "BaseTen",
  answers: {
    is_bug: { type: "noul", noul: 0.97 },
    team: { type: "choice", choice: "payments", confidence: 0.8, probabilities: { payments: 0.8, frontend: 0.2 } },
    urgency: { type: "score", score: 1.9, confidence: 0.95, probabilities: { "0": 0, "1": 0.1, "2": 0.9 } },
    product: { type: "box", box: { top: 0.1, bottom: 0.9, left: 0.2, right: 0.8 } },
  },
  usage: { input_tokens: 321, output_tokens: 4, cost: 0 },
};

test("OpenRouter discovery requests text and decisions and classifies both endpoint families", async () => {
  resetOpenRouterModelsCacheForTest();
  let requested = "";
  setOpenRouterModelsFetchForTest((input) => {
    requested = urlOf(input);
    return Promise.resolve(jsonResponse(catalogue));
  });
  try {
    await fetchOpenRouterModels({ force: true });
    assert.match(requested, /\/api\/v1\/models\?output_modalities=text%2Cdecisions$/);
    assert.deepEqual(openRouterTextModelIds(), ["vendor/text"]);
    assert.deepEqual(openRouterDecisionModelIds(), ["baseten/elephant-1:free", "~typesafe/jev-latest"]);
    const parsedElephant = openRouterModelsFromPayload(catalogue)[1];
    assert.equal(openRouterModelSupportsText(parsedElephant), false);
    assert.equal(openRouterModelSupportsDecisions(parsedElephant), true);
  } finally {
    setOpenRouterModelsFetchForTest(null);
    resetOpenRouterModelsCacheForTest();
  }
});

test("decisions forwards the documented envelope to OpenRouter and relays typed answers", async () => {
  await withServedCatalogue(async () => {
    const captured: { url: string; authorization: string | null; body: Record<string, unknown> | null } = {
      url: "",
      authorization: null,
      body: null,
    };
    const response = await handleDecisions(request(decisionBody), undefined, {
      apiKey: () => "or-test-key",
      fetcher: createFetchMock((input, init) => {
        captured.url = urlOf(input);
        captured.authorization = new Headers(init?.headers).get("authorization");
        captured.body = bodyOf(init);
        return Promise.resolve(jsonResponse(decisionPayload));
      }),
    });

    assert.equal(response.status, 200);
    assert.equal(captured.url, OPENROUTER_DECISIONS_URL);
    assert.equal(captured.authorization, "Bearer or-test-key");
    const sent = captured.body;
    assert.ok(sent);
    assert.equal(sent.model, "baseten/elephant-1:free");
    assert.deepEqual(sent.state, decisionBody.state);
    assert.deepEqual(sent.questions, decisionBody.questions);
    assert.deepEqual(sent.provider, decisionBody.provider);
    assert.equal(sent.session_id, "ticket-42");
    assert.deepEqual(sent.trace, decisionBody.trace);
    assert.equal(sent.user, "operator-1");
    assert.equal(response.headers.get("x-uos-upstream"), "openrouter");
    assert.deepEqual(await response.json(), decisionPayload);
  });
});

test("decisions keeps the stable Jev alias and rejects text or unknown model ids", async () => {
  await withServedCatalogue(async () => {
    const seen: string[] = [];
    const fetcher = createFetchMock((_input, init) => {
      seen.push((bodyOf(init) as { model: string }).model);
      return Promise.resolve(jsonResponse(decisionPayload));
    });
    const alias = await handleDecisions(
      request({ model: "typesafe/jev-latest", state: "x", questions: { yes: { type: "noul", instructions: "Is x true?" } } }),
      undefined,
      { apiKey: () => "or-test-key", fetcher }
    );
    assert.equal(alias.status, 200);
    assert.deepEqual(seen, ["~typesafe/jev-latest"]);

    for (const model of ["vendor/text", "unknown/model", "baseten/elephant-1:free"]) {
      const response = await handleDecisions(request({ model, state: "x", questions: { yes: { type: "noul", instructions: "Is x true?" } } }), undefined, {
        apiKey: () => "or-test-key",
        fetcher,
      });
      assert.equal(response.status, model === "baseten/elephant-1:free" ? 200 : 400);
      if (response.status === 400) assert.equal((await response.json()).error.param, "model");
    }
    assert.deepEqual(seen, ["~typesafe/jev-latest", "baseten/elephant-1:free"]);
  });
});

test("decisions validates state, question primitives, metadata and unsupported keys", async () => {
  await withServedCatalogue(async () => {
    const fetcher = createFetchMock(() => Promise.reject(new Error("must not reach upstream")));
    const valid = { model: "baseten/elephant-1:free", state: "x", questions: { yes: { type: "noul", instructions: "Is x true?" } } };
    const cases: unknown[] = [
      { ...valid, state: undefined },
      { ...valid, state: 4 },
      { ...valid, questions: {} },
      { ...valid, questions: { q: { type: "unsupported", instructions: "x" } } },
      { ...valid, questions: { q: { type: "noul" } } },
      { ...valid, questions: { q: { type: "noul", instructions: "x", criteria: { true: "yes" } } } },
      { ...valid, questions: { q: { type: "choice", instructions: "x", criteria: {} } } },
      { ...valid, questions: { q: { type: "score", instructions: "x", criteria: [] } } },
      { ...valid, questions: { q: { type: "box", instructions: "x", criteria: { top: 0 } } } },
      { ...valid, questions: { q: { type: "noul", instructions: "x", extra: true } } },
      { ...valid, provider: "openai" },
      { ...valid, trace: [] },
      { ...valid, session_id: "x".repeat(257) },
      { ...valid, user: "" },
      { ...valid, extra: true },
    ];
    for (const body of cases) {
      const response = await handleDecisions(request(body), undefined, { apiKey: () => "or-test-key", fetcher });
      assert.equal(response.status, 400, JSON.stringify(body));
      assert.equal((await response.json()).error.code, "decisions_request_invalid");
    }
  });
});

test("decisions maps upstream and configuration failures without dispatching twice", async () => {
  await withServedCatalogue(async () => {
    const missingKey = await handleDecisions(
      request({ model: "baseten/elephant-1:free", state: "x", questions: { yes: { type: "noul", instructions: "Is x true?" } } }),
      undefined,
      { apiKey: () => null, fetcher: createFetchMock(() => Promise.reject(new Error("must not dispatch"))) }
    );
    assert.equal(missingKey.status, 503);
    assert.equal((await missingKey.json()).error.code, "openrouter_api_key_missing");

    for (const [upstream, expected] of [
      [400, 400],
      [413, 413],
      [429, 429],
      [500, 502],
      [503, 502],
    ] as const) {
      const response = await handleDecisions(
        request({ model: "baseten/elephant-1:free", state: "x", questions: { yes: { type: "noul", instructions: "Is x true?" } } }),
        undefined,
        { apiKey: () => "or-test-key", fetcher: createFetchMock(() => Promise.resolve(jsonResponse({}, upstream))) }
      );
      assert.equal(response.status, expected);
    }

    const invalid = await handleDecisions(
      request({ model: "baseten/elephant-1:free", state: "x", questions: { yes: { type: "noul", instructions: "Is x true?" } } }),
      undefined,
      { apiKey: () => "or-test-key", fetcher: createFetchMock(() => Promise.resolve(jsonResponse({ answers: null }))) }
    );
    assert.equal(invalid.status, 502);
    assert.equal((await invalid.json()).error.code, "openrouter_upstream_invalid_response");
  });
});

test("decisions propagates downstream cancellation as a bounded 499", async () => {
  await withServedCatalogue(async () => {
    const controller = new AbortController();
    const usageContext: UsageContext = {
      keyId: null,
      kernelRepo: null,
      kernelOrg: null,
      downstreamSignal: controller.signal,
    };
    const response = await handleDecisions(request(decisionBody), usageContext, {
      apiKey: () => "or-test-key",
      fetcher: createFetchMock((_input, init) => {
        const signal = init?.signal;
        assert.ok(signal);
        return new Promise<Response>((_resolve, reject) => {
          signal.addEventListener(
            "abort",
            () => {
              reject(new DOMException("Aborted", "AbortError"));
            },
            { once: true }
          );
          controller.abort();
        });
      }),
    });
    assert.equal(response.status, 499);
    assert.equal((await response.json()).error.code, "request_cancelled");
  });
});

test("decisions attaches usage telemetry and records provider health", async () => {
  const kv = new CountingKv();
  setKvForTest(kv as unknown as Kv);
  resetProviderHealthThrottleForTest();
  try {
    await withServedCatalogue(async () => {
      const response = await handleDecisions(request(decisionBody), undefined, {
        apiKey: () => "or-test-key",
        fetcher: createFetchMock(() => Promise.resolve(jsonResponse(decisionPayload))),
      });
      assert.equal(response.status, 200);
      const telemetry = getResponseTelemetry(response);
      assert.ok(telemetry);
      assert.equal(telemetry.provider, "openrouter");
      assert.equal(telemetry.model, decisionBody.model);
      assert.equal(telemetry.inputTokens, 321);
      assert.equal(telemetry.cachedInputTokens, 0);
      assert.equal(telemetry.outputTokens, 4);
      assert.equal(telemetry.totalTokens, 325);
      assert.equal(telemetry.usageObserved, true);
      assert.equal(telemetry.completed, true);
      await new Promise<void>((resolve) => setTimeout(resolve, 0));
      assert.equal((await getOpenRouterProviderHealth()).state, "healthy");
    });
  } finally {
    await new Promise<void>((resolve) => setTimeout(resolve, 0));
    setKvForTest(null);
    resetProviderHealthThrottleForTest();
  }
});

test("decision models are exposed on the decision route and hidden from Codex text selection", async () => {
  const kv = new CountingKv();
  setKvForTest(kv as unknown as Kv);
  resetProviderSelectionCacheForTest();
  try {
    await withServedCatalogue(async () => {
      const models = await handleModels();
      const modelBody = (await models.json()) as { data: { id: string }[] };
      assert.ok(modelBody.data.some((row) => row.id === elephant.id));
      assert.ok(modelBody.data.some((row) => row.id === jevAlias.id));

      const capabilities = await handleModelCapabilities();
      const capabilityBody = (await capabilities.json()) as {
        data: { id: string; supported_endpoints: string[] }[];
      };
      assert.deepEqual(capabilityBody.data.find((row) => row.id === elephant.id)?.supported_endpoints, ["/v1/decisions"]);
      assert.deepEqual(capabilityBody.data.find((row) => row.id === textModel.id)?.supported_endpoints, ["/v1/responses", "/v1/chat/completions"]);

      const catalog = await buildModelCatalogSnapshot();
      const entry = catalog.models.find((row) => row.id === elephant.id);
      assert.deepEqual(entry?.providers[0].supported_endpoints, ["/v1/decisions"]);
      assert.equal(catalog.sources.openrouter.count, 4);

      const admin = await handleAdminModelsCatalogGet();
      assert.equal(admin.status, 200);
      const adminBody = (await admin.json()) as {
        data: { models: { id: string; providers: { id: string; supported_endpoints: string[] }[] }[] };
      };
      assert.deepEqual(adminBody.data.models.find((row) => row.id === elephant.id)?.providers[0].supported_endpoints, ["/v1/decisions"]);

      const codex = openRouterCodexModels();
      assert.deepEqual(
        codex.map((row) => row.slug),
        [textModel.id]
      );
    });
  } finally {
    setKvForTest(null);
    resetProviderSelectionCacheForTest();
  }
});

test("decisions is a terminal inference route with its own admission and quota route", () => {
  assert.equal(terminalRouteForRequest("POST", "/v1/decisions"), "decisions");
  assert.equal(kernelQuotaRouteForRequest("POST", "/v1/decisions"), "decisions");
  assert.equal(terminalRouteForRequest("GET", "/v1/decisions"), null);
});
