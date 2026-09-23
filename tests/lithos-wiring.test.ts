import assert from "node:assert/strict";

import { LITHOS_CHAT_COMPLETIONS_URL, LITHOS_MODEL_IDS, LITHOS_RATE_LIMIT_HEADERS } from "../src/lithos.ts";
import { setKvForTest } from "../src/kv.ts";
import {
  buildModelCatalogSnapshot,
  getResponseTelemetry,
  handleChatCompletions,
  handleModelCapabilities,
  handleModels,
  handleResponses,
} from "../src/openai.ts";

// The catalog builder reads discovery credentials from the environment. Clearing
// them keeps this suite on the credential-gated providers it owns, and keeps the
// discovery fetches from reaching a network the test task does not allow.
Deno.env.delete("METERED_API_KEY");
Deno.env.delete("SURPLUS_API_KEY");

const LITHOS_API_KEY_ENV = "LITHOSAI_API_KEY";
const LITHOS_API_KEY = "lith_sk_fixture";
const LITHOS_MODEL = "deepseek-ai/DeepSeek-V4.1-Flash-ultra";
const KIMI_MODEL = "moonshotai/Kimi-K3";

const keyOf = (key: Deno.KvKey): string => JSON.stringify(key);

/**
 * Minimal Deno.Kv stand-in. Every path these handlers read (the provider
 * selection, the model snapshot, the whitelist, provider health) only reads and
 * writes single keys, so an in-memory map is enough and no local KV file is
 * opened.
 */
const kvStore = new Map<string, unknown>();
type KvOp = { type: "set" | "delete"; key: Deno.KvKey; value?: unknown };
const kvStub = {
  get: (key: Deno.KvKey) =>
    Promise.resolve({
      key,
      value: kvStore.get(keyOf(key)) ?? null,
      versionstamp: kvStore.has(keyOf(key)) ? "00000000000000000001" : null,
    } as Deno.KvEntryMaybe<unknown>),
  getMany: (keys: readonly Deno.KvKey[]) =>
    Promise.resolve(
      keys.map((key) => ({
        key,
        value: kvStore.get(keyOf(key)) ?? null,
        versionstamp: kvStore.has(keyOf(key)) ? "00000000000000000001" : null,
      }))
    ),
  set: (key: Deno.KvKey, value: unknown) => {
    kvStore.set(keyOf(key), value);
    return Promise.resolve({ ok: true } as const);
  },
  delete: (key: Deno.KvKey) => {
    kvStore.delete(keyOf(key));
    return Promise.resolve();
  },
  list: function* () {},
  atomic: () => {
    const ops: KvOp[] = [];
    const chain = {
      check: () => chain,
      set: (key: Deno.KvKey, value: unknown) => {
        ops.push({ type: "set", key, value });
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        ops.push({ type: "delete", key });
        return chain;
      },
      commit: () => {
        for (const op of ops) {
          if (op.type === "set") kvStore.set(keyOf(op.key), op.value);
          else kvStore.delete(keyOf(op.key));
        }
        return Promise.resolve({ ok: true } as const);
      },
    };
    return chain;
  },
  close: () => {},
} as unknown as Deno.Kv;

setKvForTest(kvStub);

type UpstreamCall = Readonly<{ url: string; body: Record<string, unknown>; headers: Headers }>;
type UpstreamResult<T> = Readonly<{ result: T; calls: UpstreamCall[] }>;

/**
 * Runs `run` with a recorded fetch. Any URL this suite does not explicitly
 * answer (paid discovery, enrichment) gets a refusal, so no fixture can reach a
 * real network.
 */
const withUpstream = async <T>(
  handler: (call: UpstreamCall, calls: readonly UpstreamCall[]) => Response | Promise<Response>,
  run: () => Promise<T>
): Promise<UpstreamResult<T>> => {
  const calls: UpstreamCall[] = [];
  const originalFetch = globalThis.fetch;
  globalThis.fetch = (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    const call: UpstreamCall = {
      url,
      body: typeof init?.body === "string" ? (JSON.parse(init.body) as Record<string, unknown>) : {},
      headers: new Headers(init?.headers),
    };
    calls.push(call);
    return Promise.resolve(handler(call, calls));
  };
  try {
    return { result: await run(), calls };
  } finally {
    globalThis.fetch = originalFetch;
  }
};

const chatRequest = (body: Record<string, unknown>): Request =>
  new Request("https://ai.ubq.fi/v1/chat/completions", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

const responsesRequest = (body: Record<string, unknown>): Request =>
  new Request("https://ai.ubq.fi/v1/responses", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

const usageContext = (requestId: string) => ({
  keyId: null,
  kernelRepo: null,
  kernelOrg: null,
  requestId,
  startedAtMs: Date.now(),
  startedAtMonotonicMs: performance.now(),
});

/** One recorded buffered Chat completion, shaped like the vendor's wire. */
const lithosCompletion = (message: Record<string, unknown>): Record<string, unknown> => ({
  id: "chatcmpl-lithos-1",
  object: "chat.completion",
  created: 1_790_160_326,
  model: LITHOS_MODEL,
  choices: [{ index: 0, message, finish_reason: "stop" }],
  usage: {
    prompt_tokens: 91,
    completion_tokens: 113,
    total_tokens: 204,
    prompt_tokens_details: { cached_tokens: 64 },
    completion_tokens_details: { reasoning_tokens: 99 },
  },
  // Provider-only diagnostics must never reach the client.
  system_fingerprint: "fp_lithos_provider_only",
});

const withLithosKey = async (run: () => Promise<void>): Promise<void> => {
  // The credential gate can only be exercised through the environment. A test
  // task whose `--allow-env` allowlist omits this variable gets an explicit
  // remediation instead of a bare NotCapable from the first `Deno.env` call.
  try {
    Deno.env.get(LITHOS_API_KEY_ENV);
  } catch {
    throw new Error(`the test task's --allow-env allowlist must include ${LITHOS_API_KEY_ENV} for the LithosAI credential-gate assertions`);
  }
  const previous = Deno.env.get(LITHOS_API_KEY_ENV);
  Deno.env.set(LITHOS_API_KEY_ENV, LITHOS_API_KEY);
  try {
    await run();
  } finally {
    if (previous === undefined) Deno.env.delete(LITHOS_API_KEY_ENV);
    else Deno.env.set(LITHOS_API_KEY_ENV, previous);
  }
};

const withoutLithosKey = async (run: () => Promise<void>): Promise<void> => {
  try {
    Deno.env.get(LITHOS_API_KEY_ENV);
  } catch {
    throw new Error(`the test task's --allow-env allowlist must include ${LITHOS_API_KEY_ENV} for the LithosAI credential-gate assertions`);
  }
  const previous = Deno.env.get(LITHOS_API_KEY_ENV);
  Deno.env.delete(LITHOS_API_KEY_ENV);
  try {
    await run();
  } finally {
    if (previous !== undefined) Deno.env.set(LITHOS_API_KEY_ENV, previous);
  }
};

Deno.test("lithos wiring: dispatches Chat Completions to the vendor with the bearer key and a verbatim reasoning_effort", async () => {
  await withLithosKey(async () => {
    const messages = [{ role: "user", content: "Say OK." }];
    const { result: response, calls } = await withUpstream(
      () =>
        Response.json(lithosCompletion({ role: "assistant", content: "OK", reasoning_content: "Considered the request.", tool_calls: null }), {
          headers: { "Content-Type": "application/json", "x-request-id": "lithos-middlebox-injected" },
        }),
      () =>
        handleChatCompletions(
          chatRequest({
            model: LITHOS_MODEL,
            messages,
            reasoning_effort: "xhigh",
            max_completion_tokens: 512,
            stream: false,
          }),
          usageContext("lithos-chat-buffered")
        )
    );

    assert.equal(calls.length, 1, "a direct provider dispatch must not fan out");
    assert.equal(calls[0].url, LITHOS_CHAT_COMPLETIONS_URL);
    assert.equal(calls[0].headers.get("authorization"), `Bearer ${LITHOS_API_KEY}`);
    assert.equal(calls[0].headers.get("content-type"), "application/json");
    // The tier is forwarded verbatim and the OpenAI output cap becomes the
    // field this vendor documents (`max_tokens`).
    assert.deepEqual(calls[0].body, { model: LITHOS_MODEL, messages, reasoning_effort: "xhigh", max_tokens: 512, stream: false });

    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "lithos");
    // This vendor sends no request-id header at all, so the gateway claims no
    // correlation id even when a middlebox injects one.
    assert.equal(response.headers.get("x-uos-provider-request-id"), null);
    const payload = (await response.json()) as Record<string, unknown>;
    assert.equal(payload.model, LITHOS_MODEL);
    assert.deepEqual(payload.usage, {
      prompt_tokens: 91,
      completion_tokens: 113,
      total_tokens: 204,
      prompt_tokens_details: { cached_tokens: 64 },
      completion_tokens_details: { reasoning_tokens: 99 },
    });
    assert.doesNotMatch(JSON.stringify(payload), /fp_lithos_provider_only/);

    const telemetry = getResponseTelemetry(response);
    assert.equal(telemetry?.provider, "lithos");
    assert.equal(telemetry?.reasoning, "xhigh");
    assert.deepEqual(telemetry?.attemptedProviders, ["lithos"]);
    assert.equal(telemetry?.completed, true);
  });
});

Deno.test("lithos wiring: relays the vendor's Chat stream frames and its unconditional usage", async () => {
  await withLithosKey(async () => {
    const chunk = (delta: Record<string, unknown>, choices?: Record<string, unknown>[]) => ({
      id: "chatcmpl-lithos-stream",
      object: "chat.completion.chunk",
      created: 1_790_160_327,
      model: KIMI_MODEL,
      choices: choices ?? [{ index: 0, delta, finish_reason: null }],
    });
    const frames = [
      chunk({ role: "assistant", content: "" }),
      chunk({ reasoning_content: "Let me think" }),
      chunk({ content: "Paris" }),
      chunk({}, [{ index: 0, delta: {}, finish_reason: "stop" }]),
      // The authoritative totals ride a chunk with an EMPTY choices array and
      // are relayed without any `stream_options.include_usage` request flag.
      {
        id: "chatcmpl-lithos-stream",
        object: "chat.completion.chunk",
        created: 1_790_160_328,
        model: KIMI_MODEL,
        choices: [],
        usage: {
          prompt_tokens: 11,
          completion_tokens: 7,
          total_tokens: 18,
          prompt_tokens_details: null,
          completion_tokens_details: { reasoning_tokens: 5 },
        },
      },
    ];
    const body = `${frames.map((frame) => `data: ${JSON.stringify(frame)}\n\n`).join("")}data: [DONE]\n\n`;
    const { result: response, calls } = await withUpstream(
      () => new Response(body, { status: 200, headers: { "Content-Type": "text/event-stream" } }),
      () =>
        handleChatCompletions(chatRequest({ model: KIMI_MODEL, messages: [{ role: "user", content: "hi" }], stream: true }), usageContext("lithos-chat-stream"))
    );

    assert.equal(response.status, 200);
    assert.equal(response.headers.get("Content-Type"), "text/event-stream");
    assert.equal(response.headers.get("x-uos-upstream"), "lithos");
    // Nothing injects DeepSeek's usage flag on this wire.
    assert.deepEqual(calls[0].body, { model: KIMI_MODEL, messages: [{ role: "user", content: "hi" }], reasoning_effort: "medium", stream: true });

    const text = await response.text();
    const dataFrames = text
      .split("\n\n")
      .filter((frame) => frame.startsWith("data: "))
      .map((frame) => frame.slice(6));
    assert.equal(dataFrames.at(-1), "[DONE]");
    const relayed = dataFrames.slice(0, -1).map((frame) => JSON.parse(frame) as Record<string, unknown>);
    assert.equal(relayed.length, frames.length);
    assert.deepEqual(relayed[1]?.choices, [{ index: 0, delta: { reasoning_content: "Let me think" }, finish_reason: null }]);
    assert.deepEqual(relayed[4]?.choices, []);
    assert.deepEqual(relayed[4]?.usage, {
      prompt_tokens: 11,
      completion_tokens: 7,
      total_tokens: 18,
      completion_tokens_details: { reasoning_tokens: 5 },
    });

    const telemetry = getResponseTelemetry(response);
    assert.equal(telemetry?.provider, "lithos");
    assert.equal(telemetry?.stream, true);
    assert.equal(telemetry?.streamTerminalType, "response.completed");
    assert.equal(telemetry?.inputTokens, 11);
    assert.equal(telemetry?.outputTokens, 7);
    assert.equal(telemetry?.completed, true);
  });
});

Deno.test("lithos wiring: streams the translated Responses event sequence", async () => {
  await withLithosKey(async () => {
    const frames = [
      {
        id: "chatcmpl-lithos-stream",
        object: "chat.completion.chunk",
        created: 1_790_160_329,
        model: LITHOS_MODEL,
        choices: [{ index: 0, delta: { role: "assistant", content: "" }, finish_reason: null }],
      },
      {
        id: "chatcmpl-lithos-stream",
        object: "chat.completion.chunk",
        created: 1_790_160_329,
        model: LITHOS_MODEL,
        choices: [{ index: 0, delta: { reasoning_content: "Thinking" }, finish_reason: null }],
      },
      {
        id: "chatcmpl-lithos-stream",
        object: "chat.completion.chunk",
        created: 1_790_160_329,
        model: LITHOS_MODEL,
        choices: [{ index: 0, delta: { content: "Paris" }, finish_reason: null }],
      },
      {
        id: "chatcmpl-lithos-stream",
        object: "chat.completion.chunk",
        created: 1_790_160_329,
        model: LITHOS_MODEL,
        choices: [{ index: 0, delta: {}, finish_reason: "stop" }],
      },
      {
        id: "chatcmpl-lithos-stream",
        object: "chat.completion.chunk",
        created: 1_790_160_330,
        model: LITHOS_MODEL,
        choices: [],
        usage: {
          prompt_tokens: 11,
          completion_tokens: 7,
          total_tokens: 18,
          prompt_tokens_details: null,
          completion_tokens_details: { reasoning_tokens: 5 },
        },
      },
    ];
    const body = `${frames.map((frame) => `data: ${JSON.stringify(frame)}\n\n`).join("")}data: [DONE]\n\n`;
    const { result: response, calls } = await withUpstream(
      () => new Response(body, { status: 200, headers: { "Content-Type": "text/event-stream" } }),
      () => handleResponses(responsesRequest({ model: LITHOS_MODEL, input: "Capital of France?", stream: true }), usageContext("lithos-responses-stream"))
    );

    assert.equal(response.status, 200);
    assert.equal(response.headers.get("Content-Type"), "text/event-stream");
    assert.equal(response.headers.get("x-uos-upstream"), "lithos");
    assert.equal(calls[0].body.stream, true);
    assert.equal(Object.hasOwn(calls[0].body, "stream_options"), false, "this provider's usage is unconditional");

    const events = (await response.text())
      .split("\n\n")
      .filter((frame) => frame.startsWith("event: "))
      .map((frame) => JSON.parse(frame.split("\ndata: ")[1]) as Record<string, unknown>);
    const types = events.map((event) => event.type);
    assert.equal(types[0], "response.created");
    assert.ok(types.includes("response.output_item.added"));
    assert.ok(types.includes("response.reasoning_summary_text.delta"));
    assert.ok(types.includes("response.output_text.delta"));
    // Official Responses events carry a monotonic per-response sequence number.
    assert.deepEqual(
      events.map((event) => event.sequence_number),
      events.map((_, index) => index)
    );

    const terminal = events.find((event) => event.type === "response.completed");
    assert.ok(terminal, "a stopped stream must end with a completed terminal");
    const terminalResponse = terminal.response as Record<string, unknown>;
    assert.equal(terminalResponse.status, "completed");
    assert.equal(terminalResponse.model, LITHOS_MODEL);
    const output = terminalResponse.output as Record<string, unknown>[];
    assert.equal(output[0]?.type, "reasoning");
    assert.deepEqual(output[1]?.content, [{ type: "output_text", text: "Paris", annotations: [] }]);
    assert.deepEqual(terminalResponse.usage, {
      input_tokens: 11,
      output_tokens: 7,
      output_tokens_details: { reasoning_tokens: 5 },
      total_tokens: 18,
    });

    const telemetry = getResponseTelemetry(response);
    assert.equal(telemetry?.provider, "lithos");
    assert.equal(telemetry?.streamTerminalType, "response.completed");
    assert.equal(telemetry?.completed, true);
  });
});

Deno.test("lithos wiring: serves /v1/responses through the shared profile translation", async () => {
  await withLithosKey(async () => {
    const { result: response, calls } = await withUpstream(
      () => Response.json(lithosCompletion({ role: "assistant", content: "Paris", reasoning_content: "Considered it.", tool_calls: null })),
      () =>
        handleResponses(
          responsesRequest({
            model: LITHOS_MODEL,
            input: "Capital of France?",
            reasoning: { effort: "high" },
            max_output_tokens: 256,
            stream: false,
          }),
          usageContext("lithos-responses-buffered")
        )
    );

    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, LITHOS_CHAT_COMPLETIONS_URL);
    // The translated body is a Chat Completions body, and this provider is not
    // sent `stream_options` because it reports usage unconditionally.
    assert.deepEqual(calls[0].body, {
      model: LITHOS_MODEL,
      messages: [{ role: "user", content: "Capital of France?" }],
      stream: false,
      max_tokens: 256,
      reasoning_effort: "high",
    });

    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "lithos");
    const payload = (await response.json()) as Record<string, unknown>;
    assert.equal(payload.object, "response");
    assert.equal(payload.status, "completed");
    assert.equal(payload.model, LITHOS_MODEL);
    const output = payload.output as Record<string, unknown>[];
    assert.equal(output[0]?.type, "reasoning");
    assert.deepEqual(output[1]?.content, [{ type: "output_text", text: "Paris", annotations: [] }]);
    // The vendor's own counters land on the Responses detail fields.
    assert.deepEqual(payload.usage, {
      input_tokens: 91,
      input_tokens_details: { cached_tokens: 64 },
      output_tokens: 113,
      output_tokens_details: { reasoning_tokens: 99 },
      total_tokens: 204,
    });

    const telemetry = getResponseTelemetry(response);
    assert.equal(telemetry?.provider, "lithos");
    assert.equal(telemetry?.reasoning, "high");
    assert.equal(telemetry?.outputTokenAllowance, 256);

    // A tier this vendor refuses fails closed at the boundary: the Codex `ultra`
    // preset has no mapping on this wire, so no upstream dispatch happens.
    const rejected = await withUpstream(
      () => {
        throw new Error("a refused tier must never reach the vendor");
      },
      () =>
        handleResponses(
          responsesRequest({ model: LITHOS_MODEL, input: "hi", reasoning: { effort: "ultra" }, stream: false }),
          usageContext("lithos-responses-ultra")
        )
    );
    assert.equal(rejected.calls.length, 0);
    assert.equal(rejected.result.status, 400);
    const rejectedBody = (await rejected.result.json()) as { error?: { message?: string; type?: string; param?: string } };
    assert.equal(rejectedBody.error?.type, "invalid_request_error");
    assert.equal(rejectedBody.error?.param, "reasoning.effort");
    assert.match(rejectedBody.error?.message ?? "", /not supported by LithosAI/);
  });
});

Deno.test("lithos wiring: maps the vendor's refusal semantics distinctly", async () => {
  await withLithosKey(async () => {
    const message = [{ role: "user", content: "hi" }];

    // 402 out of credit: terminal, its own type and code, and explicitly
    // non-retryable. It must not be reported as a transient rate limit.
    const insufficientQuota = await withUpstream(
      () =>
        Response.json(
          { error: { message: "Your organization has no credit remaining.", type: "insufficient_quota", code: "insufficient_quota" } },
          { status: 402, headers: { "Content-Type": "application/json", "x-should-retry": "false" } }
        ),
      () => handleChatCompletions(chatRequest({ model: LITHOS_MODEL, messages: message, stream: false }), usageContext("lithos-402"))
    );
    assert.equal(insufficientQuota.result.status, 402);
    assert.equal(insufficientQuota.result.headers.get("x-uos-upstream"), "lithos");
    assert.equal(insufficientQuota.result.headers.get("x-should-retry"), "false");
    const quotaBody = (await insufficientQuota.result.json()) as { error?: { message?: string; type?: string; code?: string } };
    assert.equal(quotaBody.error?.type, "insufficient_quota");
    assert.equal(quotaBody.error?.code, "insufficient_quota");
    assert.equal(quotaBody.error?.message, "Your organization has no credit remaining.");
    assert.equal(getResponseTelemetry(insufficientQuota.result)?.failureKind, "upstream_http_error");

    // 429 budget refusal: a rate limit, with the provider's capacity headers and
    // both retry hints forwarded.
    const budgetRefusal = await withUpstream(
      () =>
        Response.json(
          { error: { message: "Rate limit exceeded for input_tokens.", type: "input_tokens", code: "rate_limit_exceeded" } },
          {
            status: 429,
            headers: {
              "Content-Type": "application/json",
              "x-ratelimit-limit-requests": "600",
              "x-ratelimit-remaining-requests": "0",
              "x-ratelimit-reset-requests": "12s",
              "x-ratelimit-limit-tokens": "200000",
              "x-ratelimit-remaining-tokens": "0",
              "x-ratelimit-reset-tokens": "45s",
              "retry-after-ms": "900",
              "Retry-After": "17",
              "x-uos-provider-only": "must-not-be-relayed",
            },
          }
        ),
      () => handleChatCompletions(chatRequest({ model: LITHOS_MODEL, messages: message, stream: false }), usageContext("lithos-429-budget"))
    );
    assert.equal(budgetRefusal.result.status, 429);
    assert.equal(budgetRefusal.result.headers.get("x-should-retry"), null, "a per-minute budget refusal stays retryable");
    for (const header of LITHOS_RATE_LIMIT_HEADERS) assert.notEqual(budgetRefusal.result.headers.get(header), null, `${header} must be forwarded`);
    assert.equal(budgetRefusal.result.headers.get("Retry-After"), "17");
    assert.equal(budgetRefusal.result.headers.get("retry-after-ms"), "900");
    assert.equal(budgetRefusal.result.headers.get("x-uos-provider-only"), null);
    const budgetBody = (await budgetRefusal.result.json()) as { error?: { type?: string; code?: string } };
    assert.equal(budgetBody.error?.type, "rate_limit_error");
    assert.equal(budgetBody.error?.code, "rate_limit_exceeded");

    // 429 model at capacity: still a 429, but its own code so the two causes are
    // never collapsed into one.
    const overloaded = await withUpstream(
      () => Response.json({ error: { message: "The model is at capacity.", type: "provider_overloaded", code: "provider_overloaded" } }, { status: 429 }),
      () => handleChatCompletions(chatRequest({ model: LITHOS_MODEL, messages: message, stream: false }), usageContext("lithos-429-overloaded"))
    );
    assert.equal(overloaded.result.status, 429);
    const overloadedBody = (await overloaded.result.json()) as { error?: { code?: string } };
    assert.equal(overloadedBody.error?.code, "provider_overloaded");

    // 404 unknown model: terminal model failure, not a gateway fault.
    const unknownModel = await withUpstream(
      () => Response.json({ error: { message: "The model does not exist.", type: "invalid_request_error", code: "model_not_found" } }, { status: 404 }),
      () => handleChatCompletions(chatRequest({ model: LITHOS_MODEL, messages: message, stream: false }), usageContext("lithos-404"))
    );
    assert.equal(unknownModel.result.status, 404);
    assert.equal(unknownModel.result.headers.get("x-should-retry"), "false");
    assert.equal(((await unknownModel.result.json()) as { error?: { code?: string } }).error?.code, "model_not_found");

    // The engine-parameter envelope carries an INTEGER code and no `error`
    // object; both envelopes are accepted, and neither throws.
    const engineViolation = await withUpstream(
      () =>
        Response.json(
          { object: "error", message: "Invalid value for 'n': must be 1", type: "BadRequestError", code: 400 },
          { status: 400, headers: { "Content-Type": "application/json" } }
        ),
      () => handleChatCompletions(chatRequest({ model: LITHOS_MODEL, messages: message, stream: false }), usageContext("lithos-400-engine"))
    );
    assert.equal(engineViolation.result.status, 400);
    assert.equal(engineViolation.result.headers.get("x-should-retry"), "false");
    const engineBody = (await engineViolation.result.json()) as { error?: { message?: string; type?: string; code?: string } };
    assert.equal(engineBody.error?.message, "Invalid value for 'n': must be 1");
    assert.equal(engineBody.error?.type, "invalid_request_error");
    assert.equal(engineBody.error?.code, "400");

    // A non-JSON body is tolerated: the gateway answers with its own bounded
    // message instead of throwing while rendering the failure.
    const nonJson = await withUpstream(
      () => new Response("<html>provider-only-body</html>", { status: 500, headers: { "Content-Type": "text/html" } }),
      () => handleChatCompletions(chatRequest({ model: LITHOS_MODEL, messages: message, stream: false }), usageContext("lithos-500"))
    );
    assert.equal(nonJson.result.status, 500);
    const nonJsonBody = (await nonJson.result.json()) as { error?: { message?: string; type?: string; code?: string } };
    assert.equal(nonJsonBody.error?.message, "LithosAI upstream returned an error.");
    assert.equal(nonJsonBody.error?.type, "server_error");
    assert.equal(nonJsonBody.error?.code, "lithos_upstream_error");
    assert.doesNotMatch(JSON.stringify(nonJsonBody), /provider-only-body/);
  });
});

Deno.test("lithos wiring: the catalog advertises eight addressable ids only while the key is configured", async () => {
  await withLithosKey(async () => {
    const snapshot = await withUpstream(
      () => new Response("{}", { status: 503 }),
      () => buildModelCatalogSnapshot()
    );
    const rows = snapshot.result.models.filter((model) => model.providers.some((provider) => provider.id === "lithos"));
    assert.equal(rows.length, LITHOS_MODEL_IDS.length);
    assert.deepEqual(rows.map((model) => model.id).sort(), [...LITHOS_MODEL_IDS].sort());
    for (const row of rows) {
      // Every speed tier carries the same window and the route's endpoints:
      // `/v1/responses` is served by the gateway's translation.
      assert.equal(row.context_window_tokens, 1_048_576, `${row.id} context window`);
      assert.equal(row.max_context_window_tokens, 1_048_576, `${row.id} max context window`);
      assert.equal(row.effective_context_window_percent, 95, `${row.id} effective percent`);
      assert.deepEqual(row.providers[0]?.supported_endpoints, ["/v1/chat/completions", "/v1/responses"]);
      assert.deepEqual(row.supported_reasoning_levels, ["none", "minimal", "low", "medium", "high", "xhigh", "max"]);
      assert.equal(row.default_reasoning_effort, "medium");
    }
    assert.deepEqual(snapshot.result.sources.lithos, { status: "available", count: 8, updated_at_ms: null, configured: true });

    // `/v1/models` and `/uos/model_capabilities` expose the same eight ids.
    const list = await withUpstream(
      () => new Response("{}", { status: 503 }),
      () => handleModels()
    );
    const listed = (await list.result.json()) as { data: { id: string }[] };
    for (const id of LITHOS_MODEL_IDS)
      assert.ok(
        listed.data.some((model) => model.id === id),
        `${id} must be listed`
      );

    const capabilities = await withUpstream(
      () => new Response("{}", { status: 503 }),
      () => handleModelCapabilities()
    );
    const capabilityBody = (await capabilities.result.json()) as { data: Record<string, unknown>[] };
    const lithosCapabilities = capabilityBody.data.filter((model) => model.upstream_provider === "lithos");
    assert.equal(lithosCapabilities.length, LITHOS_MODEL_IDS.length);
    for (const model of lithosCapabilities) {
      // All seven tiers are accepted verbatim, so no Codex preset is translated.
      assert.deepEqual(model.reasoning_effort_wire_map, {});
      assert.equal(model.default_reasoning_effort, "medium");
      assert.deepEqual(model.supported_endpoints, ["/v1/chat/completions", "/v1/responses"]);
      assert.equal(model.context_window_tokens, 1_048_576);
    }
  });

  await withoutLithosKey(async () => {
    const snapshot = await withUpstream(
      () => new Response("{}", { status: 503 }),
      () => buildModelCatalogSnapshot()
    );
    assert.equal(snapshot.result.models.filter((model) => model.providers.some((provider) => provider.id === "lithos")).length, 0);
    assert.deepEqual(snapshot.result.sources.lithos, { status: "unavailable", count: 0, updated_at_ms: null, configured: false });

    const list = await withUpstream(
      () => new Response("{}", { status: 503 }),
      () => handleModels()
    );
    const listed = (await list.result.json()) as { data: { id: string }[] };
    for (const id of LITHOS_MODEL_IDS)
      assert.equal(
        listed.data.some((model) => model.id === id),
        false,
        `${id} must not be advertised without a key`
      );
  });
});
