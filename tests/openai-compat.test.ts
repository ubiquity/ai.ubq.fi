import assert from "node:assert/strict";
import { DEFAULT_MODEL_KEY, DEFAULT_REASONING_EFFORT_KEY } from "../src/defaults.ts";
import { sha256Base64Url } from "../src/utils.ts";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const DEFAULT_TEST_MODEL = "gpt-5-fixture-default";
const TEST_CODEX_MODELS_KEY = ["ubq_ai", "codex_models"] as const;

const kvStore = new Map<string, unknown>();
kvStore.set(keyToString(DEFAULT_REASONING_EFFORT_KEY), "low");
kvStore.set(keyToString(["ubq_ai", "codex_auth"]), {
  access_token: "access",
  refresh_token: "refresh",
  account_id: "acct",
  updated_at_ms: Date.now(),
});
kvStore.set(keyToString(TEST_CODEX_MODELS_KEY), {
  source: "chatgpt_codex",
  client_version: "0.125.0",
  updated_at_ms: Date.now(),
  models: [{
    slug: DEFAULT_TEST_MODEL,
    display_name: "GPT-5 Fixture Default",
    context_window: 272000,
    max_context_window: 1000000,
    auto_compact_token_limit: null,
    default_reasoning_level: "medium",
    supported_reasoning_levels: ["none", "low", "medium", "high", "xhigh"],
  }],
});
kvStore.set(keyToString(["uos_ai", "voyage_api_key"]), "voyage_test_key");

const originalOpenKv = (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv;

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

const { handleChatCompletions, handleModelCapabilities, handleModels, handleResponses } = await import(
  "../src/openai.ts"
);
const { withCors } = await import("../src/http.ts");

const TEXT_ENCODER = new TextEncoder();

const encodeBase64 = (bytes: Uint8Array): string => {
  let binary = "";
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary);
};

const encodeBase64Url = (bytes: Uint8Array): string =>
  encodeBase64(bytes).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");

const encodeJsonBase64Url = (value: unknown): string => encodeBase64Url(TEXT_ENCODER.encode(JSON.stringify(value)));

const toPublicKeyPem = (spki: Uint8Array): string => {
  const b64 = encodeBase64(spki);
  const lines = b64.match(/.{1,64}/g) ?? [];
  return `-----BEGIN PUBLIC KEY-----\n${lines.join("\n")}\n-----END PUBLIC KEY-----`;
};

const sseResponse = (chunks: string[]): Response => {
  const stream = new ReadableStream<Uint8Array>({
    start(controller) {
      for (const chunk of chunks) controller.enqueue(TEXT_ENCODER.encode(chunk));
      controller.close();
    },
  });
  return new Response(stream, { status: 200, headers: { "Content-Type": "text/event-stream" } });
};

const baseSseChunks = () => [
  `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_test", created_at: 0 } })}\n\n`,
  `data: ${JSON.stringify({ type: "response.output_text.delta", delta: "pong" })}\n\n`,
  `data: ${
    JSON.stringify({
      type: "response.completed",
      response: {
        model: DEFAULT_TEST_MODEL,
        output: [],
        usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
      },
    })
  }\n\n`,
];

const parseWarnings = (value: string | null): string[] =>
  value ? value.split(",").map((entry) => entry.trim()).filter(Boolean) : [];

const extractResponseOutputText = (payload: Record<string, unknown>): string => {
  const output = payload.output;
  if (!Array.isArray(output)) return "";
  const chunks: string[] = [];
  for (const item of output) {
    if (!item || typeof item !== "object") continue;
    const content = (item as { content?: unknown }).content;
    if (!Array.isArray(content)) continue;
    for (const contentItem of content) {
      if (!contentItem || typeof contentItem !== "object") continue;
      const part = contentItem as { type?: unknown; text?: unknown };
      if (part.type === "output_text" && typeof part.text === "string") chunks.push(part.text);
    }
  }
  return chunks.join("");
};

type FetchMockQueue = {
  chain: Promise<void>;
};

const fetchMockQueue: FetchMockQueue = (() => {
  const key = "__uosFetchMockQueue";
  const globalRecord = globalThis as unknown as Record<string, unknown>;
  const existing = globalRecord[key];
  if (existing && typeof existing === "object") {
    const chain = (existing as { chain?: unknown }).chain;
    if (chain instanceof Promise) return existing as FetchMockQueue;
  }
  const created: FetchMockQueue = { chain: Promise.resolve() };
  globalRecord[key] = created;
  return created;
})();

const withFetchMock = async <T>(
  handler: (url: string, bodyText: string | null) => Response | Promise<Response>,
  fn: () => Promise<T>,
): Promise<T> => {
  const prev = fetchMockQueue.chain;
  let release = () => {};
  fetchMockQueue.chain = new Promise<void>((resolve) => {
    release = () => resolve(undefined);
  });
  await prev;

  const originalFetch = globalThis.fetch;
  globalThis.fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    const bodyText = typeof init?.body === "string" ? init.body : null;
    return await handler(url, bodyText);
  };
  try {
    return await fn();
  } finally {
    globalThis.fetch = originalFetch;
    release();
  }
};

Deno.test("openai: defaults + ignore temperature", async (t) => {
  await t.step("chat uses default model/reasoning and ignores temperature", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              messages: [{ role: "user", content: "ping" }],
              temperature: 0.2,
              max_tokens: 12,
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as { model?: string };
    assert.equal(payload.model, DEFAULT_TEST_MODEL);
    const warnings = parseWarnings(response.headers.get("x-uos-warning"));
    assert.ok(warnings.includes("temperature_ignored"));
    assert.ok(warnings.includes("max_output_tokens_ignored"));
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal(recorded["model"], DEFAULT_TEST_MODEL);
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
    assert.equal("temperature" in recorded, false);
    assert.equal("max_output_tokens" in recorded, false);
  });

  await t.step("chat accepts none reasoning effort", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              messages: [{ role: "user", content: "ping" }],
              reasoning_effort: "none",
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal(Object.prototype.hasOwnProperty.call(recorded, "reasoning"), true);
    assert.deepEqual(recorded["reasoning"], { effort: "none" });
  });

  await t.step("chat rejects null reasoning effort", async () => {
    const response = await withFetchMock(
      () => {
        throw new Error("invalid reasoning requests should not fetch upstream");
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              messages: [{ role: "user", content: "ping" }],
              reasoning_effort: null,
            }),
          }),
        ),
    );

    assert.equal(response.status, 400);
    const payload = await response.json() as { error?: { message?: string; param?: string | null } };
    assert.equal(payload.error?.param, "reasoning_effort");
    assert.match(payload.error?.message ?? "", /must be a string/);
  });

  await t.step("responses uses default model/reasoning and ignores temperature", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              input: "ping",
              temperature: 0.7,
              max_output_tokens: 24,
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as Record<string, unknown> & { model?: string; reasoning?: unknown };
    assert.equal(payload.model, DEFAULT_TEST_MODEL);
    assert.equal(extractResponseOutputText(payload), "pong");
    const warnings = parseWarnings(response.headers.get("x-uos-warning"));
    assert.ok(warnings.includes("temperature_ignored"));
    assert.ok(warnings.includes("max_output_tokens_ignored"));
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal(recorded["model"], DEFAULT_TEST_MODEL);
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
    assert.equal("temperature" in recorded, false);
    assert.equal("max_output_tokens" in recorded, false);
  });
});

Deno.test("openai: default model requires configured model or stored snapshot", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const defaultModelKey = keyToString(DEFAULT_MODEL_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  const previousDefault = kvStore.get(defaultModelKey);
  kvStore.delete(snapshotKey);
  kvStore.delete(defaultModelKey);

  try {
    const response = await withFetchMock(
      () => {
        throw new Error("no-model requests should not fetch upstream defaults");
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ input: "ping" }),
          }),
        ),
    );

    assert.equal(response.status, 503);
    const payload = await response.json() as { error?: { message?: string; code?: string } };
    assert.equal(payload.error?.code, "server_error");
    assert.match(payload.error?.message ?? "", /no configured default model or Codex model snapshot/);
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
    if (previousDefault === undefined) kvStore.delete(defaultModelKey);
    else kvStore.set(defaultModelKey, previousDefault);
  }
});

Deno.test("openai: default reasoning comes from stored model metadata, not model name", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const defaultModelKey = keyToString(DEFAULT_MODEL_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  const previousDefault = kvStore.get(defaultModelKey);
  const modelWithoutReasoningMetadata = "gpt-5-no-reasoning-metadata";
  kvStore.set(snapshotKey, {
    source: "codex_cli",
    client_version: "0.126.0",
    updated_at_ms: Date.now(),
    models: [{ slug: modelWithoutReasoningMetadata, display_name: "No Reasoning Metadata" }],
  });
  kvStore.delete(defaultModelKey);

  let recordedBody: Record<string, unknown> | null = null;
  try {
    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse([
          `data: ${
            JSON.stringify({ type: "response.created", response: { id: "resp_no_reasoning", created_at: 0 } })
          }\n\n`,
          `data: ${JSON.stringify({ type: "response.output_text.delta", delta: "pong" })}\n\n`,
          `data: ${
            JSON.stringify({
              type: "response.completed",
              response: {
                model: modelWithoutReasoningMetadata,
                output: [],
                usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
              },
            })
          }\n\n`,
        ]);
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ input: "ping" }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    assert.equal((recordedBody as Record<string, unknown>).model, modelWithoutReasoningMetadata);
    assert.equal("reasoning" in (recordedBody as Record<string, unknown>), false);
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
    if (previousDefault === undefined) kvStore.delete(defaultModelKey);
    else kvStore.set(defaultModelKey, previousDefault);
  }
});

Deno.test("openai: default reasoning level is accepted when supported levels are absent", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  const modelWithDefaultOnly = "gpt-5-default-reasoning-only";
  kvStore.set(snapshotKey, {
    source: "codex_cli",
    client_version: "0.126.0",
    updated_at_ms: Date.now(),
    models: [{
      slug: modelWithDefaultOnly,
      display_name: "Default Reasoning Only",
      default_reasoning_level: "medium",
    }],
  });

  let recordedBody: Record<string, unknown> | null = null;
  try {
    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse([
          `data: ${
            JSON.stringify({ type: "response.created", response: { id: "resp_default_only", created_at: 0 } })
          }\n\n`,
          `data: ${JSON.stringify({ type: "response.output_text.delta", delta: "pong" })}\n\n`,
          `data: ${
            JSON.stringify({
              type: "response.completed",
              response: {
                model: modelWithDefaultOnly,
                output: [],
                usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
              },
            })
          }\n\n`,
        ]);
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: modelWithDefaultOnly,
              input: "ping",
              reasoning: { effort: "medium" },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "medium" });
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
  }
});

Deno.test("openai: none reasoning disables reasoning even when snapshot levels omit none", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);

  kvStore.set(snapshotKey, {
    source: "chatgpt_codex",
    updated_at_ms: Date.now(),
    models: [{
      slug: DEFAULT_TEST_MODEL,
      display_name: "GPT-5 Fixture Default",
      default_reasoning_level: "medium",
      supported_reasoning_levels: ["low", "medium", "high", "xhigh"],
    }],
  });

  try {
    const capabilitiesResponse = await withFetchMock(
      () => {
        throw new Error("model capability reads should not fetch upstream");
      },
      () => handleModelCapabilities(),
    );
    assert.equal(capabilitiesResponse.status, 200);
    const capabilitiesPayload = await capabilitiesResponse.json() as {
      data?: Array<{ supported_reasoning_levels?: string[] }>;
    };
    assert.deepEqual(capabilitiesPayload.data?.[0]?.supported_reasoning_levels, [
      "none",
      "low",
      "medium",
      "high",
      "xhigh",
    ]);

    let recordedBody: Record<string, unknown> | null = null;
    const chatResponse = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              messages: [{ role: "user", content: "ping" }],
              reasoning_effort: "none",
            }),
          }),
        ),
    );

    assert.equal(chatResponse.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded.reasoning, { effort: "none" });
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
  }
});

Deno.test("openai: models returns stored Codex snapshot without upstream fetch", async () => {
  const response = await withFetchMock(
    () => {
      throw new Error("handleModels should not fetch upstream models");
    },
    () => handleModels(),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as { data?: Array<Record<string, unknown> & { id?: string }> };
  assert.ok(Array.isArray(payload.data));
  const model = payload.data.find((entry) => entry.id === DEFAULT_TEST_MODEL);
  assert.ok(model);
  assert.deepEqual(Object.keys(model).sort(), ["created", "id", "object", "owned_by"]);
  assert.equal(model.object, "model");
  assert.equal(typeof model.created, "number");
  assert.equal(Object.prototype.hasOwnProperty.call(model, "supported_reasoning_levels"), false);
  assert.equal(Object.prototype.hasOwnProperty.call(model, "display_name"), false);
});

Deno.test("openai: model capabilities are exposed outside /v1 model objects", async () => {
  const response = await withFetchMock(
    () => {
      throw new Error("handleModelCapabilities should not fetch upstream models");
    },
    () => handleModelCapabilities(),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as {
    object?: string;
    data?: Array<{
      id?: string;
      object?: string;
      upstream_provider?: string;
      supported_endpoints?: string[];
      supported_reasoning_levels?: string[];
      default_reasoning_effort?: string | null;
      context_window_tokens?: number | null;
      max_context_window_tokens?: number | null;
      auto_compact_token_limit_tokens?: number | null;
    }>;
  };
  assert.equal(payload.object, "list");
  assert.ok(Array.isArray(payload.data));
  const model = payload.data.find((entry) => entry.id === DEFAULT_TEST_MODEL);
  assert.ok(model);
  assert.equal(model.object, "uos.model_capabilities");
  assert.equal(model.upstream_provider, "codex_chatgpt");
  assert.deepEqual(model.supported_reasoning_levels, ["none", "low", "medium", "high", "xhigh"]);
  assert.equal(model.default_reasoning_effort, "medium");
  assert.equal(model.context_window_tokens, 272000);
  assert.equal(model.max_context_window_tokens, 1000000);
  assert.equal(model.auto_compact_token_limit_tokens, null);
  assert.ok(model.supported_endpoints?.includes("/v1/chat/completions"));
  assert.ok(model.supported_endpoints?.includes("/v1/responses"));
});

Deno.test("openai: models returns an empty list when no snapshot is stored", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  kvStore.delete(snapshotKey);

  try {
    const response = await withFetchMock(
      () => {
        throw new Error("handleModels should not fetch upstream models");
      },
      () => handleModels(),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as { object?: string; data?: unknown[] };
    assert.equal(payload.object, "list");
    assert.deepEqual(payload.data, []);
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
  }
});

Deno.test("openai: unsupported snapshot model is rejected before upstream fetch", async () => {
  const response = await withFetchMock(
    () => {
      throw new Error("unsupported model requests should not fetch upstream");
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "gpt-5-chat-latest",
            messages: [{ role: "user", content: "ping" }],
          }),
        }),
      ),
  );

  assert.equal(response.status, 404);
  const payload = await response.json() as { error?: { message?: string; code?: string; param?: string | null } };
  assert.equal(payload.error?.code, "model_not_found");
  assert.equal(payload.error?.param, "model");
  assert.match(payload.error?.message ?? "", /Use \/v1\/models/);
});

Deno.test("openai: reasoning validation returns OpenAI-style parameter errors", async () => {
  const response = await withFetchMock(
    () => {
      throw new Error("unsupported reasoning requests should not fetch upstream");
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            messages: [{ role: "user", content: "ping" }],
            reasoning_effort: "minimal",
          }),
        }),
      ),
  );

  assert.equal(response.status, 400);
  const payload = await response.json() as { error?: { code?: string; param?: string | null; type?: string } };
  assert.equal(payload.error?.type, "invalid_request_error");
  assert.equal(payload.error?.code, "invalid_request_error");
  assert.equal(payload.error?.param, "reasoning_effort");
});

Deno.test("openai: upstream detail errors are normalized to OpenAI-style envelopes", async () => {
  const response = await withFetchMock(
    () =>
      new Response(
        JSON.stringify({
          detail: "The 'gpt-5-chat-latest' model is not supported when using Codex with a ChatGPT account.",
        }),
        { status: 400, headers: { "Content-Type": "application/json" } },
      ),
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            messages: [{ role: "user", content: "ping" }],
          }),
        }),
      ),
  );

  assert.equal(response.status, 400);
  assert.equal(response.headers.get("x-ubq-upstream"), "chatgpt_codex");
  const payload = await response.json() as { error?: { message?: string; code?: string; type?: string } };
  assert.equal(payload.error?.type, "invalid_request_error");
  assert.equal(payload.error?.code, "upstream_error");
  assert.match(payload.error?.message ?? "", /not supported when using Codex/);
});

Deno.test("http: CORS wrapper exposes a gateway request id", () => {
  const response = withCors(new Response("{}", { headers: { "Content-Type": "application/json" } }));
  assert.ok(response.headers.get("x-uos-request-id"));
  assert.match(response.headers.get("Access-Control-Expose-Headers") ?? "", /x-uos-request-id/);
  assert.match(response.headers.get("Access-Control-Expose-Headers") ?? "", /x-ubq-upstream/);
});

Deno.test("openai: normalize function-style tools for codex compatibility", async (t) => {
  await t.step("chat completions flattens tools and tool_choice", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              messages: [{ role: "user", content: "get weather" }],
              tools: [
                {
                  type: "function",
                  function: {
                    name: "fetch_weather",
                    description: "Fetch weather for a city.",
                    parameters: { type: "object", properties: { city: { type: "string" } } },
                  },
                },
                {
                  type: "function",
                  name: "legacy_tool",
                  description: "Already top-level tool name.",
                  parameters: { type: "object", properties: {} },
                  function: { strict: true },
                },
              ],
              tool_choice: {
                type: "function",
                name: "forced_choice",
                function: { name: "fetch_weather", strict: true },
              },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    const recordedTools = recorded["tools"] as Array<Record<string, unknown>> | undefined;
    assert.ok(Array.isArray(recordedTools));
    assert.equal(recordedTools.length, 2);
    assert.equal(recordedTools[0]?.name, "fetch_weather");
    assert.equal(recordedTools[1]?.name, "legacy_tool");
    assert.equal(recordedTools[0]?.description, "Fetch weather for a city.");
    assert.deepEqual(recordedTools[0]?.parameters, {
      type: "object",
      properties: { city: { type: "string" } },
    });
    assert.equal(recordedTools[1]?.strict, true);
    assert.equal(Object.prototype.hasOwnProperty.call(recordedTools[0], "function"), false);
    assert.equal(Object.prototype.hasOwnProperty.call(recordedTools[1], "function"), false);
    const recordedToolChoice = recorded["tool_choice"] as Record<string, unknown> | undefined;
    assert.ok(recordedToolChoice);
    assert.equal(recordedToolChoice.type, "function");
    assert.equal(recordedToolChoice["name"], "forced_choice");
    assert.equal(Object.prototype.hasOwnProperty.call(recordedToolChoice, "strict"), false);
    assert.equal(Object.prototype.hasOwnProperty.call(recordedToolChoice, "function"), false);
  });

  await t.step("responses flattens tools", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              input: "get weather",
              tools: [
                {
                  type: "function",
                  function: {
                    name: "fetch_weather",
                    description: "Fetch weather for a city.",
                    parameters: { type: "object", properties: { city: { type: "string" } } },
                  },
                },
              ],
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    const recordedTools = recorded["tools"] as Array<Record<string, unknown>> | undefined;
    assert.ok(Array.isArray(recordedTools));
    assert.equal(recordedTools.length, 1);
    assert.equal(recordedTools[0]?.name, "fetch_weather");
    assert.equal(recordedTools[0]?.description, "Fetch weather for a city.");
    assert.deepEqual(recordedTools[0]?.parameters, { type: "object", properties: { city: { type: "string" } } });
    assert.equal(Object.prototype.hasOwnProperty.call(recordedTools[0], "function"), false);
  });
});

Deno.test("openai: chat completions accept system-only messages", async () => {
  let recordedBody: Record<string, unknown> | null = null;

  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            messages: [{ role: "system", content: "Only system." }],
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  const recorded = recordedBody as Record<string, unknown>;
  assert.equal(recorded["instructions"], "Only system.");
  const input = recorded["input"];
  assert.ok(Array.isArray(input));
  assert.ok(input.length > 0);
  const first = input[0] as Record<string, unknown>;
  assert.equal(first["type"], "message");
  assert.equal(first["role"], "user");
  const content = first["content"];
  assert.ok(Array.isArray(content));
  const firstContent = (content as Record<string, unknown>[])[0] ?? null;
  assert.equal(firstContent?.["type"], "input_text");
});

Deno.test("openai: responses accept non-message input items", async () => {
  let recordedBody: Record<string, unknown> | null = null;

  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            input: [
              {
                type: "message",
                role: "user",
                content: [{ type: "input_text", text: "ping" }],
              },
              { type: "reasoning", summary: "thinking..." },
              { type: "function_call", name: "test", call_id: "call_1", arguments: "{}" },
              { type: "function_call_output", call_id: "call_1", output: "ok" },
            ],
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);

  const input = recordedBody["input"];
  assert.ok(Array.isArray(input));

  const types = (input as Array<Record<string, unknown>>)
    .map((item) => (item && typeof item === "object") ? item["type"] : null)
    .filter((value): value is string => typeof value === "string");

  assert.ok(types.includes("reasoning"));
  assert.ok(types.includes("function_call"));
  assert.ok(types.includes("function_call_output"));
});

Deno.test("openai: responses preserve image detail on normalized input images", async () => {
  let recordedBody: Record<string, unknown> | null = null;

  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            reasoning: { effort: "low" },
            input: [
              {
                role: "user",
                content: [
                  { type: "input_text", text: "Read this." },
                  {
                    type: "input_image",
                    image_url: "data:image/jpeg;base64,/9j/4AAQ",
                    detail: "high",
                  },
                ],
              },
            ],
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  const input = recordedBody["input"];
  assert.ok(Array.isArray(input));
  const message = (input as Record<string, unknown>[])[0];
  const content = message?.content;
  assert.ok(Array.isArray(content));
  const image = (content as Record<string, unknown>[]).find((part) => part.type === "input_image");
  assert.equal(image?.image_url, "data:image/jpeg;base64,/9j/4AAQ");
  assert.equal(image?.detail, "high");
});

Deno.test("auth: kernel attestation tokens are reusable within TTL", async () => {
  const keyPair = await crypto.subtle.generateKey(
    {
      name: "RSASSA-PKCS1-v1_5",
      modulusLength: 2048,
      publicExponent: new Uint8Array([0x01, 0x00, 0x01]),
      hash: "SHA-256",
    },
    true,
    ["sign", "verify"],
  );

  const spki = new Uint8Array(await crypto.subtle.exportKey("spki", keyPair.publicKey));
  const publicPem = toPublicKeyPem(spki);
  kvStore.set(keyToString(["uos_ai", "kernel_pubkeys"]), [{ pem: publicPem }]);

  const bearerToken = "ghs_test_token";
  const nowSeconds = Math.floor(Date.now() / 1000);
  const payload = {
    iss: "ubiquity-os-kernel",
    aud: "ai.ubq.fi",
    iat: nowSeconds,
    exp: nowSeconds + 600,
    jti: `jti_${crypto.randomUUID()}`,
    owner: "acme",
    repo: "demo",
    installation_id: null,
    auth_token_sha256: await sha256Base64Url(bearerToken),
    state_id: "state_test",
  };

  const header = { alg: "RS256", typ: "JWT" };
  const headerB64 = encodeJsonBase64Url(header);
  const payloadB64 = encodeJsonBase64Url(payload);
  const signingInput = `${headerB64}.${payloadB64}`;
  const signature = new Uint8Array(
    await crypto.subtle.sign("RSASSA-PKCS1-v1_5", keyPair.privateKey, TEXT_ENCODER.encode(signingInput)),
  );
  const kernelToken = `${signingInput}.${encodeBase64Url(signature)}`;

  const { getKernelAttestationContext } = await import("../src/auth.ts");

  const req = new Request("https://ai.ubq.fi/v1/responses", {
    method: "POST",
    headers: { "X-Ubiquity-Kernel-Token": kernelToken },
    body: "{}",
  });

  const first = await getKernelAttestationContext(req, bearerToken);
  const second = await getKernelAttestationContext(req, bearerToken);
  assert.ok(first);
  assert.ok(second);
  assert.deepEqual(second, first);
});

addEventListener("unload", () => {
  (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = originalOpenKv;
});
