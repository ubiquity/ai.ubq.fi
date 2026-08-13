import assert from "node:assert/strict";
import {
  buildOpenRouterResponsesRequest,
  deriveOpenRouterSessionId,
  fetchOpenRouterResponses,
  isEligibleOpenRouterModel,
  OPENROUTER_EXCLUDED_MODELS,
  OPENROUTER_RESPONSES_URL,
  openRouterTaskTypeFromResponse,
  stripOpenRouterMetadata,
} from "../src/openrouter.ts";

Deno.test("OpenRouter request translation applies the fixed Auto policy and strips gateway fields", async () => {
  const sessionId = await deriveOpenRouterSessionId("api-key:key-1", { session_id: "raw-session" });
  const translated = buildOpenRouterResponsesRequest({
    model: "gpt-5.6-sol",
    input: [{ type: "custom_tool_call_output", call_id: "call_1", output: "done" }],
    instructions: "Continue.",
    reasoning: { effort: "ultra", summary: "auto" },
    tools: [{ type: "custom", name: "exec", description: "Run", format: { type: "text" } }],
    tool_choice: "auto",
    parallel_tool_calls: true,
    max_output_tokens: 512,
    context_management: [{ type: "compaction", compact_threshold: 1000 }],
    prompt_cache_key: "gateway-only",
    store: false,
    stream: true,
  }, sessionId);

  assert.equal(translated.model, "openrouter/auto");
  assert.deepEqual(translated.plugins, [{
    id: "auto-router",
    cost_tier: "max",
    excluded_models: [...OPENROUTER_EXCLUDED_MODELS],
  }]);
  assert.deepEqual(translated.reasoning, { effort: "max", summary: "auto" });
  assert.equal(translated.session_id, sessionId);
  assert.equal("prompt_cache_key" in translated, false);
  assert.equal("context_management" in translated, false);
  assert.equal("store" in translated, false);
  assert.ok(typeof sessionId === "string");
  assert.doesNotMatch(sessionId, /raw-session|key-1/);
  assert.equal(await deriveOpenRouterSessionId(null, { session_id: "raw-session" }), null);
  assert.equal(await deriveOpenRouterSessionId("api-key:key-1", {}), null);
});

Deno.test("OpenRouter request translation accepts a null output-token limit", () => {
  const translated = buildOpenRouterResponsesRequest({
    input: "hello",
    max_output_tokens: null,
    stream: true,
  });

  assert.equal(translated.max_output_tokens, null);
});

Deno.test("OpenRouter selected-model validation rejects disallowed publishers and token families", () => {
  for (
    const model of [
      "openai/gpt-5",
      "~openai/gpt-latest",
      "anthropic/claude-sonnet-4",
      "~anthropic/claude-opus-latest",
      "vendor/gpt-oss-120b",
      "vendor/claude-3",
      "openrouter/auto",
      "missing-separator",
      "vendor/",
      "",
      null,
    ]
  ) assert.equal(isEligibleOpenRouterModel(model), false, String(model));

  for (
    const model of [
      "google/gemini-2.5-pro",
      "deepseek/deepseek-v3.2",
      "qwen/qwen3-coder",
      "vendor/claudette-model",
      "vendor/gptransformer",
    ]
  ) assert.equal(isEligibleOpenRouterModel(model), true, model);
});

Deno.test("OpenRouter transport sends metadata opt-in without exposing raw session sources", async () => {
  const originalFetch = globalThis.fetch;
  let request: Request | null = null;
  globalThis.fetch = (input, init) => {
    request = new Request(input, init);
    return Promise.resolve(new Response("", { status: 200 }));
  };
  try {
    const result = await fetchOpenRouterResponses(
      { input: "hello", stream: true },
      { apiKey: "secret-key", sessionId: "uos_hashed" },
    );
    assert.equal(result.response.status, 200);
    const sentRequest = request as Request | null;
    assert.ok(sentRequest);
    assert.equal(sentRequest.url, OPENROUTER_RESPONSES_URL);
    assert.equal(sentRequest.headers.get("Authorization"), "Bearer secret-key");
    assert.equal(sentRequest.headers.get("X-OpenRouter-Metadata"), "enabled");
    const body = await sentRequest.json() as Record<string, unknown>;
    assert.equal(body.session_id, "uos_hashed");
    assert.equal(JSON.stringify(body).includes("secret-key"), false);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("OpenRouter metadata parsing extracts only bounded aggregate task type", () => {
  assert.equal(
    openRouterTaskTypeFromResponse({
      openrouter_metadata: {
        pipeline: [{ name: "auto-router", data: { task_type: "coding" } }],
      },
    }),
    "coding",
  );
  assert.equal(openRouterTaskTypeFromResponse({ openrouter_metadata: { pipeline: [] } }), null);
});

Deno.test("OpenRouter metadata is removed from client-visible response events", () => {
  assert.deepEqual(
    stripOpenRouterMetadata({
      type: "response.created",
      response: { id: "resp_1", model: "google/gemini", openrouter_metadata: { pipeline: [] } },
    }),
    { type: "response.created", response: { id: "resp_1", model: "google/gemini" } },
  );
});
