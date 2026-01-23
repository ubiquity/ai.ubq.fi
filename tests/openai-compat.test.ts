import assert from "node:assert/strict";
import { DEFAULT_MODEL_KEY, DEFAULT_REASONING_EFFORT_KEY } from "../src/defaults.ts";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);

const kvStore = new Map<string, unknown>();
kvStore.set(keyToString(DEFAULT_MODEL_KEY), "gpt-5.2");
kvStore.set(keyToString(DEFAULT_REASONING_EFFORT_KEY), "low");
kvStore.set(keyToString(["ubq_ai", "codex_auth"]), {
  access_token: "access",
  refresh_token: "refresh",
  account_id: "acct",
  updated_at_ms: Date.now(),
});

const originalOpenKv = (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv;

const kvStub = {
  get: async (key: Deno.KvKey) =>
    ({ key, value: kvStore.get(keyToString(key)) ?? null }) as Deno.KvEntryMaybe<unknown>,
  set: async (key: Deno.KvKey, value: unknown) => {
    kvStore.set(keyToString(key), value);
    return { ok: true } as const;
  },
  delete: async (key: Deno.KvKey) => {
    kvStore.delete(keyToString(key));
  },
  list: async function* (_selector: Deno.KvListSelector, _options?: Deno.KvListOptions) {
    return;
  },
  atomic: () => {
    const chain = {
      check: () => chain,
      set: () => chain,
      delete: () => chain,
      commit: async () => ({ ok: true } as const),
    };
    return chain;
  },
  close: () => {},
} as unknown as Deno.Kv;

(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = async () => kvStub;

const { handleChatCompletions, handleResponses } = await import("../src/openai.ts");

const TEXT_ENCODER = new TextEncoder();

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
  `data: ${JSON.stringify({
    type: "response.completed",
    response: { model: "gpt-5.2", usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 } },
  })}\n\n`,
];

const withFetchMock = async <T>(
  handler: (url: string, bodyText: string | null) => Response | Promise<Response>,
  fn: () => Promise<T>,
): Promise<T> => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === "string"
      ? input
      : input instanceof URL
      ? input.toString()
      : input.url;
    const bodyText = typeof init?.body === "string" ? init.body : null;
    return await handler(url, bodyText);
  };
  try {
    return await fn();
  } finally {
    globalThis.fetch = originalFetch;
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
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as { model?: string };
    assert.equal(payload.model, "gpt-5.2");
    assert.equal(response.headers.get("x-uos-warning"), "temperature_ignored");
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal(recorded["model"], "gpt-5.2");
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
    assert.equal("temperature" in recorded, false);
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
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as { model?: string; reasoning?: unknown };
    assert.equal(payload.model, "gpt-5.2");
    assert.equal(response.headers.get("x-uos-warning"), "temperature_ignored");
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal(recorded["model"], "gpt-5.2");
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
    assert.equal("temperature" in recorded, false);
  });
});

if (originalOpenKv) {
  Deno.test("openai: restore openKv", () => {
    (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = originalOpenKv;
  });
}
