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
    default_reasoning_level: "medium",
    supported_reasoning_levels: ["low", "medium", "high", "xhigh"],
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

const { handleChatCompletions, handleModels, handleResponses } = await import("../src/openai.ts");

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
      response: { model: DEFAULT_TEST_MODEL, usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 } },
    })
  }\n\n`,
];

const parseWarnings = (value: string | null): string[] =>
  value ? value.split(",").map((entry) => entry.trim()).filter(Boolean) : [];

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
    const payload = await response.json() as { model?: string; reasoning?: unknown };
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
});

Deno.test("openai: default model falls back to upstream models when snapshot is missing", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const defaultModelKey = keyToString(DEFAULT_MODEL_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  const previousDefault = kvStore.get(defaultModelKey);
  const dynamicDefaultModel = "gpt-5-upstream-default";
  kvStore.delete(snapshotKey);
  kvStore.delete(defaultModelKey);

  let requestedModelsUrl = "";
  let recordedResponseBody: Record<string, unknown> | null = null;

  try {
    const response = await withFetchMock(
      (url, bodyText) => {
        if (url.includes("/models")) {
          requestedModelsUrl = url;
          return new Response(
            JSON.stringify({
              models: [{
                slug: dynamicDefaultModel,
                display_name: "GPT-5 Upstream Default",
                default_reasoning_level: "medium",
                supported_reasoning_levels: ["low", "medium", "high", "xhigh"],
              }],
            }),
            { status: 200, headers: { "Content-Type": "application/json" } },
          );
        }

        recordedResponseBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse([
          `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_dynamic", created_at: 0 } })}\n\n`,
          `data: ${JSON.stringify({ type: "response.output_text.delta", delta: "pong" })}\n\n`,
          `data: ${
            JSON.stringify({
              type: "response.completed",
              response: { model: dynamicDefaultModel, usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 } },
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
    assert.ok(requestedModelsUrl.includes("/models"));
    const payload = await response.json() as { model?: string };
    assert.equal(payload.model, dynamicDefaultModel);
    assert.ok(recordedResponseBody);
    assert.equal((recordedResponseBody as Record<string, unknown>).model, dynamicDefaultModel);
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
    if (previousDefault === undefined) kvStore.delete(defaultModelKey);
    else kvStore.set(defaultModelKey, previousDefault);
  }
});

Deno.test("openai: models uses stored Codex client version", async () => {
  let requestedUrl = "";

  const response = await withFetchMock(
    (url) => {
      requestedUrl = url;
      return new Response(
        JSON.stringify({
          models: [{
            slug: DEFAULT_TEST_MODEL,
            display_name: "GPT-5 Fixture Default",
            default_reasoning_level: "medium",
            supported_reasoning_levels: ["low", "medium", "high", "xhigh"],
          }],
        }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      );
    },
    () => handleModels(),
  );

  assert.equal(response.status, 200);
  assert.equal(new URL(requestedUrl).searchParams.get("client_version"), "0.125.0");
  const payload = await response.json() as { data?: Array<{ id?: string }> };
  assert.ok(Array.isArray(payload.data));
  assert.ok(payload.data.some((model) => model.id === DEFAULT_TEST_MODEL));
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
