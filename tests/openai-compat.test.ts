import assert from "node:assert/strict";
import { DEFAULT_MODEL_KEY, DEFAULT_REASONING_EFFORT_KEY } from "../src/defaults.ts";
import { sha256Base64Url } from "../src/utils.ts";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const DEFAULT_TEST_MODEL = "gpt-5-fixture-default";
const TEST_CODEX_MODELS_KEY = ["ubq_ai", "codex_models"] as const;

const kvStore = new Map<string, unknown>();
type OpenAiAtomicOp = { type: "set" | "delete"; key: Deno.KvKey; value?: unknown };
let atomicCommitFailure: ((ops: readonly OpenAiAtomicOp[]) => Error | null) | null = null;
let exposePaidFallbackLedgerEntries = false;
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
    supported_reasoning_levels: ["none", "low", "medium", "high", "xhigh", "max", "ultra"],
    reasoning_effort_wire_map: { ultra: "max" },
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
  list: async function* (selector: Deno.KvListSelector, _options?: Deno.KvListOptions) {
    if (!exposePaidFallbackLedgerEntries || !("prefix" in selector)) return;
    for (const [encoded, value] of kvStore) {
      const key = JSON.parse(encoded) as Deno.KvKey;
      if (!selector.prefix.every((part, index) => key[index] === part)) continue;
      yield { key, value, versionstamp: "00000000000000000001" } as Deno.KvEntry<unknown>;
    }
  },
  atomic: () => {
    const ops: OpenAiAtomicOp[] = [];
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
        const failure = atomicCommitFailure?.(ops) ?? null;
        if (failure) return Promise.reject(failure);
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

const { getResponseTelemetry, handleChatCompletions, handleModelCapabilities, handleModels, handleResponses } =
  await import(
    "../src/openai.ts"
  );
const { withCors } = await import("../src/http.ts");
const { resetRuntimeConfigCacheForTest } = await import("../src/runtime_config.ts");
const { resetCodexRateLimitCacheForTest } = await import("../src/codex_rate_limit.ts");

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

const seedPaidFallbackKey = (
  id: string,
  options: {
    enabled?: boolean;
    limitMicrocredits?: number;
    spentMicrocredits?: number;
    reservedMicrocredits?: number;
    reservationRequestId?: string | null;
    modelIds?: readonly string[];
  } = {},
): void => {
  const hash = `hash-${id}`;
  const common = {
    paid_fallback_enabled: options.enabled ?? true,
    paid_fallback_limit_microcredits: options.limitMicrocredits ?? 1_000_000,
    paid_fallback_spent_microcredits: options.spentMicrocredits ?? 0,
    paid_fallback_reserved_microcredits: options.reservedMicrocredits ?? 0,
    paid_fallback_reservation_request_id: options.reservationRequestId ?? null,
  };
  kvStore.set(keyToString(["ubq_ai", "api_keys", "id", id]), {
    id,
    name: `Key ${id}`,
    prefix: "u_test",
    hash,
    created_at_ms: Date.now(),
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: -1,
    usage_requests: 0,
    usage_reset_at_ms: Date.now() + 60_000,
    window_ms: 60_000,
    ...common,
    paid_fallback_model_ids: options.modelIds ?? [DEFAULT_TEST_MODEL],
    paid_fallback_quota_per_credit: 500_000,
    paid_fallback_pricing_checked_at_ms: Date.now(),
  });
  kvStore.set(keyToString(["ubq_ai", "api_keys", "hash", hash]), {
    id,
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: -1,
    usage_requests: 0,
    usage_reset_at_ms: Date.now() + 60_000,
    window_ms: 60_000,
    ...common,
  });
};

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
  handler: (url: string, bodyText: string | null, init?: RequestInit) => Response | Promise<Response>,
  fn: () => Promise<T>,
): Promise<T> => {
  const prev = fetchMockQueue.chain;
  let release = () => {};
  fetchMockQueue.chain = new Promise<void>((resolve) => {
    release = () => resolve(undefined);
  });
  await prev;

  const snapshot = kvStore.get(keyToString(TEST_CODEX_MODELS_KEY)) as
    | { models?: Array<Record<string, unknown>>; source?: string; updated_at_ms?: number; client_version?: string }
    | undefined;
  if (snapshot?.models?.length) {
    const explicitDefault = kvStore.get(keyToString(DEFAULT_MODEL_KEY));
    kvStore.set(keyToString(["uos_ai", "runtime_config", "v2"]), {
      version: 2,
      default_model: typeof explicitDefault === "string"
        ? explicitDefault
        : String(snapshot.models[0]?.slug ?? DEFAULT_TEST_MODEL),
      default_reasoning_effort: String(kvStore.get(keyToString(DEFAULT_REASONING_EFFORT_KEY)) ?? "low"),
      codex_models: snapshot,
      updated_at_ms: Date.now(),
    });
  } else {
    kvStore.delete(keyToString(["uos_ai", "runtime_config", "v2"]));
  }
  resetRuntimeConfigCacheForTest();
  kvStore.delete(keyToString(["uos_ai", "codex_rate_limit"]));
  resetCodexRateLimitCacheForTest();

  const originalFetch = globalThis.fetch;
  globalThis.fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    const bodyText = typeof init?.body === "string" ? init.body : null;
    return await handler(url, bodyText, init);
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
              moderation: { model: "omni-moderation-latest" },
              prompt_cache_options: { mode: "implicit", ttl: "30m" },
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
    assert.ok(warnings.includes("moderation_ignored"));
    assert.ok(warnings.includes("prompt_cache_options_ignored"));
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal(recorded["model"], DEFAULT_TEST_MODEL);
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
    assert.equal("temperature" in recorded, false);
    assert.equal("max_output_tokens" in recorded, false);
    assert.equal("moderation" in recorded, false);
    assert.equal("prompt_cache_options" in recorded, false);
  });

  await t.step("chat preserves none reasoning effort upstream", async () => {
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
    assert.deepEqual(recorded["reasoning"], { effort: "none" });
  });

  await t.step("chat accepts null reasoning effort as unspecified", async () => {
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
              reasoning_effort: null,
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
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
              moderation: { model: "omni-moderation-latest" },
              prompt_cache_options: { mode: "implicit", ttl: "30m" },
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
    assert.ok(warnings.includes("moderation_ignored"));
    assert.ok(warnings.includes("prompt_cache_options_ignored"));
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal(recorded["model"], DEFAULT_TEST_MODEL);
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
    assert.equal("temperature" in recorded, false);
    assert.equal("max_output_tokens" in recorded, false);
    assert.equal("moderation" in recorded, false);
    assert.equal("prompt_cache_options" in recorded, false);
  });

  await t.step("responses accepts and strips Codex CLI client metadata", async () => {
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
              input: "ping",
              client_metadata: {
                session_id: "session_test",
                thread_id: "thread_test",
                request_kind: "turn",
              },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.deepEqual(parseWarnings(response.headers.get("x-uos-warning")), []);
    assert.ok(recordedBody);
    assert.equal("client_metadata" in recordedBody, false);
  });

  await t.step("responses rejects malformed Codex CLI client metadata", async () => {
    const response = await handleResponses(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          model: DEFAULT_TEST_MODEL,
          input: "ping",
          client_metadata: { session_id: 123 },
        }),
      }),
    );

    assert.equal(response.status, 400);
    const payload = await response.json() as { error?: { param?: string } };
    assert.equal(payload.error?.param, "client_metadata");
  });

  await t.step("responses rejects array-valued Codex CLI client metadata", async () => {
    const response = await handleResponses(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          model: DEFAULT_TEST_MODEL,
          input: "ping",
          client_metadata: ["session_test"],
        }),
      }),
    );

    assert.equal(response.status, 400);
    const payload = await response.json() as { error?: { param?: string } };
    assert.equal(payload.error?.param, "client_metadata");
  });

  await t.step("responses preserves none reasoning upstream", async () => {
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
              reasoning: { effort: "none" },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded["reasoning"], { effort: "none" });
  });

  await t.step("responses accepts null reasoning as unspecified", async () => {
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
              reasoning: null,
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
  });

  await t.step("responses accepts null reasoning fields as unspecified", async () => {
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
              reasoning: {
                effort: null,
                summary: null,
                generate_summary: null,
              },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
  });

  await t.step("responses accepts official context_management parameter", async () => {
    let recordedBody: Record<string, unknown> | null = null;
    const contextManagement = [{ type: "compaction", compact_threshold: 2000 }];

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
              context_management: contextManagement,
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded["context_management"], contextManagement);
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
    assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "none" });
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

Deno.test("openai: none remains a gateway special case when snapshot levels omit it", async () => {
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
    assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "none" });
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
      reasoning_effort_wire_map?: Record<string, string>;
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
  assert.deepEqual(model.supported_reasoning_levels, ["none", "low", "medium", "high", "xhigh", "max", "ultra"]);
  assert.equal(model.default_reasoning_effort, "medium");
  assert.deepEqual(model.reasoning_effort_wire_map, { ultra: "max" });
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

Deno.test("openai: unlisted reasoning tiers pass through for upstream validation", async () => {
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
            reasoning_effort: "minimal",
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "minimal" });
});

Deno.test("openai: max reasoning is forwarded for models that support it", async () => {
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
            reasoning_effort: "max",
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "max" });
});

Deno.test("openai: catalog wire metadata maps Codex CLI ultra to upstream max", async () => {
  let recordedBody: Record<string, unknown> | null = null;
  let recordedUserAgent: string | null = null;
  const response = await withFetchMock(
    (_url, bodyText, init) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      recordedUserAgent = new Headers(init?.headers).get("user-agent");
      return sseResponse(baseSseChunks());
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            messages: [{ role: "user", content: "ping" }],
            reasoning_effort: "ultra",
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "max" });
  assert.equal(recordedUserAgent, "codex_cli_rs/0.125.0 (ai.ubq.fi)");
});

Deno.test("openai: responses applies catalog reasoning wire metadata", async () => {
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
            reasoning: { effort: "ultra" },
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "max" });
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

Deno.test("openai: YunWu paid fallback routing matrix", async (t) => {
  const originalApiKey = Deno.env.get("YUNWU_API_KEY");
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  try {
    await t.step("already-loaded disabled policy bypasses paid fallback reservation", async () => {
      const keyId = "fallback-policy-bypass";
      seedPaidFallbackKey(keyId, { enabled: true });
      let calls = 0;
      const response = await withFetchMock(
        () => {
          calls += 1;
          return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
            status: 429,
            headers: { "Content-Type": "application/json" },
          });
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: false,
              requestId: "request-fallback-policy-bypass",
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.status, 429);
      assert.equal(calls, 1);
      const stored = kvStore.get(keyToString(["ubq_ai", "api_keys", "id", keyId])) as {
        paid_fallback_reservation_request_id?: string | null;
      };
      assert.equal(stored.paid_fallback_reservation_request_id, null);
    });

    await t.step("disabled, unpriced, and exhausted keys retain the primary 429", async () => {
      const cases = [
        {
          id: "fallback-disabled",
          options: { enabled: false },
          expectedCode: "upstream_error",
        },
        {
          id: "fallback-unpriced",
          options: { modelIds: ["some-other-model"] },
          expectedCode: "upstream_error",
        },
        {
          id: "fallback-exhausted",
          options: { limitMicrocredits: 100, spentMicrocredits: 100 },
          expectedCode: "paid_fallback_limit_exceeded",
        },
      ] as const;

      for (const testCase of cases) {
        seedPaidFallbackKey(testCase.id, testCase.options);
        let calls = 0;
        const response = await withFetchMock(
          () => {
            calls += 1;
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: { "Content-Type": "application/json" },
            });
          },
          () =>
            handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              }),
              {
                keyId: testCase.id,
                kernelRepo: null,
                kernelOrg: null,
                requestId: `request-${testCase.id}`,
                startedAtMs: Date.now(),
              },
            ),
        );
        assert.equal(response.status, 429);
        assert.equal(calls, 1);
        const payload = await response.json() as { error?: { code?: string } };
        assert.equal(payload.error?.code, testCase.expectedCode);
      }
    });

    await t.step("primary errors and network failures other than 429 never dispatch YunWu", async () => {
      for (const scenario of ["http_500", "network"] as const) {
        const keyId = `fallback-${scenario}`;
        seedPaidFallbackKey(keyId);
        let calls = 0;
        const response = await withFetchMock(
          () => {
            calls += 1;
            if (scenario === "network") throw new TypeError("primary network unavailable");
            return new Response(JSON.stringify({ error: { message: "Primary failed" } }), {
              status: 500,
              headers: { "Content-Type": "application/json" },
            });
          },
          () =>
            handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              }),
              {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId: `request-${keyId}`,
                startedAtMs: Date.now(),
              },
            ),
        );
        assert.equal(response.status, scenario === "http_500" ? 500 : 502);
        assert.equal(calls, 1);
      }
    });

    await t.step("Responses sends the same canonical payload to YunWu exactly once", async () => {
      const keyId = "fallback-responses-success";
      seedPaidFallbackKey(keyId);
      const bodies: Record<string, unknown>[] = [];
      const urls: string[] = [];
      const response = await withFetchMock(
        (url, bodyText, init) => {
          urls.push(url);
          if (bodyText) bodies.push(JSON.parse(bodyText) as Record<string, unknown>);
          if (url === "https://yunwu.ai/v1/responses") {
            assert.equal(new Headers(init?.headers).get("Authorization"), "Bearer yunwu-test-key");
            return new Response(sseResponse(baseSseChunks()).body, {
              status: 200,
              headers: {
                "Content-Type": "text/event-stream",
                "X-Oneapi-Request-Id": "yunwu-responses-request",
              },
            });
          }
          return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
            status: 429,
            headers: { "Content-Type": "application/json" },
          });
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                input: "ping",
                reasoning: { effort: "ultra" },
              }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId: "request-fallback-responses-success",
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.status, 200);
      assert.equal(response.headers.get("x-ubq-upstream"), "yunwu");
      assert.equal(getResponseTelemetry(response)?.quotaUsedPercent, 100);
      assert.equal(getResponseTelemetry(response)?.fallbackReason, "primary_429");
      assert.deepEqual(urls, [
        "https://chatgpt.com/backend-api/codex/responses",
        "https://yunwu.ai/v1/responses",
      ]);
      assert.equal(bodies.length, 2);
      assert.deepEqual(bodies[1], bodies[0]);
      assert.deepEqual(bodies[1].reasoning, { effort: "max" });
    });

    await t.step("Chat Completions also falls back through YunWu Responses once", async () => {
      const keyId = "fallback-chat-success";
      seedPaidFallbackKey(keyId);
      const urls: string[] = [];
      const response = await withFetchMock(
        (url) => {
          urls.push(url);
          if (url === "https://yunwu.ai/v1/responses") {
            return new Response(sseResponse(baseSseChunks()).body, {
              status: 200,
              headers: {
                "Content-Type": "text/event-stream",
                "X-Oneapi-Request-Id": "yunwu-chat-request",
              },
            });
          }
          return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
            status: 429,
            headers: { "Content-Type": "application/json" },
          });
        },
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                messages: [{ role: "user", content: "ping" }],
              }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId: "request-fallback-chat-success",
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.status, 200);
      assert.equal(response.headers.get("x-ubq-upstream"), "yunwu");
      assert.deepEqual(urls, [
        "https://chatgpt.com/backend-api/codex/responses",
        "https://yunwu.ai/v1/responses",
      ]);
    });

    await t.step("a YunWu error is attempted once and remains pending when it has a provider request id", async () => {
      const keyId = "fallback-yunwu-error";
      seedPaidFallbackKey(keyId);
      let yunwuCalls = 0;
      const response = await withFetchMock(
        (url) => {
          if (url === "https://yunwu.ai/v1/responses") {
            yunwuCalls += 1;
            return new Response(JSON.stringify({ error: { message: "YunWu busy" } }), {
              status: 503,
              headers: {
                "Content-Type": "application/json",
                "X-Oneapi-Request-Id": "yunwu-error-request",
              },
            });
          }
          if (url === "https://yunwu.ai/api/log/token") {
            return new Response(JSON.stringify({ success: true, data: [] }), {
              status: 200,
              headers: { "Content-Type": "application/json" },
            });
          }
          return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
            status: 429,
            headers: { "Content-Type": "application/json" },
          });
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId: "request-fallback-yunwu-error",
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.status, 503);
      assert.equal(response.headers.get("x-ubq-upstream"), "yunwu");
      assert.equal(yunwuCalls, 1);
      const stored = kvStore.get(keyToString(["ubq_ai", "api_keys", "id", keyId])) as {
        paid_fallback_reserved_microcredits?: number;
      };
      assert.equal(stored.paid_fallback_reserved_microcredits, 1_000_000);
    });

    await t.step("a ledger write failure after YunWu accepts preserves the usable response", async () => {
      const keyId = "fallback-ledger-write-failure";
      const requestId = "request-fallback-ledger-write-failure";
      const providerRequestId = "yunwu-ledger-write-failure";
      seedPaidFallbackKey(keyId);
      atomicCommitFailure = (ops) =>
        ops.some((op) => {
            const value = op.value as { provider_request_id?: unknown } | undefined;
            return op.type === "set" && op.key[0] === "uos_ai" && op.key[1] === "paid_fallback" &&
              op.key[2] === "ledger" && value?.provider_request_id === providerRequestId;
          })
          ? new Error("injected paid fallback ledger failure")
          : null;
      try {
        const response = await withFetchMock(
          (url) => {
            if (url === "https://yunwu.ai/v1/responses") {
              return new Response(sseResponse(baseSseChunks()).body, {
                status: 200,
                headers: {
                  "Content-Type": "text/event-stream",
                  "X-Oneapi-Request-Id": providerRequestId,
                },
              });
            }
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: { "Content-Type": "application/json" },
            });
          },
          () =>
            handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              }),
              {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId,
                startedAtMs: Date.now(),
              },
            ),
        );
        assert.equal(response.status, 200);
        assert.match(await response.text(), /pong/);
        assert.equal(getResponseTelemetry(response)?.completed, true);
        const stored = kvStore.get(keyToString(["ubq_ai", "api_keys", "id", keyId])) as {
          paid_fallback_reservation_request_id?: string | null;
        };
        assert.equal(stored.paid_fallback_reservation_request_id, requestId);
      } finally {
        atomicCommitFailure = null;
      }
    });

    await t.step("reconciliation failures preserve chat and Responses results across stream modes", async () => {
      const cases = [
        { route: "responses", stream: false },
        { route: "responses", stream: true },
        { route: "chat", stream: false },
        { route: "chat", stream: true },
      ] as const;
      exposePaidFallbackLedgerEntries = true;
      atomicCommitFailure = (ops) =>
        ops.some((op) => {
            const value = op.value as { billing_status?: unknown } | undefined;
            return op.type === "set" && op.key[0] === "uos_ai" && op.key[1] === "paid_fallback" &&
              op.key[2] === "ledger" && value?.billing_status === "reconciled";
          })
          ? new Error("injected paid fallback reconciliation failure")
          : null;
      try {
        for (const testCase of cases) {
          const suffix = `${testCase.route}-${testCase.stream ? "stream" : "nonstream"}`;
          const keyId = `fallback-reconcile-${suffix}`;
          const requestId = `request-fallback-reconcile-${suffix}`;
          const providerRequestId = `yunwu-reconcile-${suffix}`;
          seedPaidFallbackKey(keyId);
          const result = await withFetchMock(
            (url) => {
              if (url === "https://yunwu.ai/v1/responses") {
                return new Response(sseResponse(baseSseChunks()).body, {
                  status: 200,
                  headers: {
                    "Content-Type": "text/event-stream",
                    "X-Oneapi-Request-Id": providerRequestId,
                  },
                });
              }
              if (url === "https://yunwu.ai/api/log/token") {
                return new Response(
                  JSON.stringify({
                    success: true,
                    data: [{
                      request_id: providerRequestId,
                      quota: 100,
                      prompt_tokens: 1,
                      completion_tokens: 1,
                      model_name: DEFAULT_TEST_MODEL,
                      created_at: Math.floor(Date.now() / 1000),
                    }],
                  }),
                  { status: 200, headers: { "Content-Type": "application/json" } },
                );
              }
              return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
                status: 429,
                headers: { "Content-Type": "application/json" },
              });
            },
            async () => {
              const context = {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId,
                startedAtMs: Date.now(),
              };
              const response = await (testCase.route === "responses"
                ? handleResponses(
                  new Request("https://ai.ubq.fi/v1/responses", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      input: "ping",
                      stream: testCase.stream,
                    }),
                  }),
                  context,
                )
                : handleChatCompletions(
                  new Request("https://ai.ubq.fi/v1/chat/completions", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      messages: [{ role: "user", content: "ping" }],
                      stream: testCase.stream,
                    }),
                  }),
                  context,
                ));
              const completedBeforeConsumption = getResponseTelemetry(response)?.completed;
              const text = await response.text();
              return { response, text, completedBeforeConsumption };
            },
          );
          const { response, text, completedBeforeConsumption } = result;
          assert.equal(response.status, 200, suffix);
          if (testCase.stream) assert.equal(completedBeforeConsumption, false, suffix);
          assert.match(text, /pong/, suffix);
          assert.equal(getResponseTelemetry(response)?.completed, true, suffix);
          const stored = kvStore.get(keyToString(["ubq_ai", "api_keys", "id", keyId])) as {
            paid_fallback_reservation_request_id?: string | null;
          };
          assert.equal(stored.paid_fallback_reservation_request_id, requestId, suffix);
        }
      } finally {
        atomicCommitFailure = null;
        exposePaidFallbackLedgerEntries = false;
      }
    });

    await t.step("reconciliation failure does not replace the original YunWu error", async () => {
      const keyId = "fallback-error-reconcile-failure";
      const requestId = "request-fallback-error-reconcile-failure";
      const providerRequestId = "yunwu-error-reconcile-failure";
      seedPaidFallbackKey(keyId);
      exposePaidFallbackLedgerEntries = true;
      atomicCommitFailure = (ops) =>
        ops.some((op) => (op.value as { billing_status?: unknown } | undefined)?.billing_status === "reconciled")
          ? new Error("injected upstream error reconciliation failure")
          : null;
      try {
        const response = await withFetchMock(
          (url) => {
            if (url === "https://yunwu.ai/v1/responses") {
              return new Response(JSON.stringify({ error: { message: "YunWu original error" } }), {
                status: 503,
                headers: {
                  "Content-Type": "application/json",
                  "X-Oneapi-Request-Id": providerRequestId,
                },
              });
            }
            if (url === "https://yunwu.ai/api/log/token") {
              return new Response(
                JSON.stringify({
                  success: true,
                  data: [{
                    request_id: providerRequestId,
                    quota: 100,
                    prompt_tokens: 1,
                    completion_tokens: 0,
                    model_name: DEFAULT_TEST_MODEL,
                    created_at: Math.floor(Date.now() / 1000),
                  }],
                }),
                { status: 200, headers: { "Content-Type": "application/json" } },
              );
            }
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: { "Content-Type": "application/json" },
            });
          },
          () =>
            handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              }),
              {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId,
                startedAtMs: Date.now(),
              },
            ),
        );
        assert.equal(response.status, 503);
        const payload = await response.json() as { error?: { message?: string } };
        assert.equal(payload.error?.message, "YunWu original error");
        const stored = kvStore.get(keyToString(["ubq_ai", "api_keys", "id", keyId])) as {
          paid_fallback_reservation_request_id?: string | null;
        };
        assert.equal(stored.paid_fallback_reservation_request_id, requestId);
      } finally {
        atomicCommitFailure = null;
        exposePaidFallbackLedgerEntries = false;
      }
    });
  } finally {
    atomicCommitFailure = null;
    exposePaidFallbackLedgerEntries = false;
    if (originalApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalApiKey);
  }
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

Deno.test("openai: buffered responses preserve function calls emitted as output items", async () => {
  const functionCall = {
    id: "fc_test",
    type: "function_call",
    status: "completed",
    name: "assistant_exports_download",
    call_id: "call_export",
    arguments: '{"format":"csv"}',
  };
  const response = await withFetchMock(
    () =>
      sseResponse([
        `data: ${
          JSON.stringify({
            type: "response.output_item.done",
            output_index: 0,
            item: functionCall,
          })
        }\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.completed",
            response: {
              model: DEFAULT_TEST_MODEL,
              output: [],
              usage: { input_tokens: 10, output_tokens: 20, total_tokens: 30 },
            },
          })
        }\n\n`,
      ]),
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            input: "export csv",
            tools: [{
              type: "function",
              name: "assistant_exports_download",
              description: "Download the selected records.",
              parameters: {
                type: "object",
                properties: { format: { type: "string", enum: ["csv", "json"] } },
                required: ["format"],
                additionalProperties: false,
              },
              strict: true,
            }],
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as { output?: unknown[] };
  assert.deepEqual(payload.output, [functionCall]);
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
