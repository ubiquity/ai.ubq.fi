import assert from "node:assert/strict";
import {
  fetchSurplusModels,
  fetchSurplusResponses,
  resetSurplusModelsCacheForTest,
  type SurplusFetch,
} from "../src/surplus.ts";

const jsonResponse = (body: unknown, status = 200, headers: HeadersInit = {}): Response =>
  new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json", ...headers },
  });

Deno.test("fetchSurplusModels preserves exact IDs and exposes text-capable routes only", async () => {
  resetSurplusModelsCacheForTest();
  const calls: Array<{ url: string; init?: RequestInit }> = [];
  const fetcher: SurplusFetch = (input, init) => {
    calls.push({ url: input.toString(), init });
    return Promise.resolve(jsonResponse({
      object: "list",
      data: [
        {
          id: "gpt-5.6-sol",
          created: 1_735_000_000,
          provider: "openai",
          architecture: { modality: "text->text" },
          pricing: { prompt: "0.000001", completion: "0.000003", cache_read: "0.0000001" },
        },
        {
          id: "image-model-that-must-not-route",
          architecture: { output_modalities: ["image"] },
        },
        {
          id: "claude-opus-5",
          provider: "anthropic",
          architecture: { input_modalities: ["text"], output_modalities: ["text"] },
          description: "test model",
        },
      ],
    }));
  };

  const snapshot = await fetchSurplusModels({
    apiKey: "inf_test",
    fetcher,
    force: true,
  });

  assert.deepEqual(snapshot?.models.map((model) => model.id), ["gpt-5.6-sol", "claude-opus-5"]);
  assert.equal(snapshot?.models[0].owned_by, "openai");
  assert.equal(snapshot?.models[0].input_price_per_token, 0.000001);
  assert.deepEqual(snapshot?.models[0].supported_endpoint_types, ["openai", "openai-response"]);
  assert.equal(snapshot?.models[1].description, "test model");
  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, "https://api.surplusintelligence.ai/v1/models");
  assert.equal(new Headers(calls[0].init?.headers).get("Accept"), "application/json");
  assert.equal(new Headers(calls[0].init?.headers).has("Authorization"), false);
});

Deno.test("fetchSurplusResponses forwards the canonical body and provider request ID", async () => {
  const body = {
    model: "claude-opus-5",
    input: [{ role: "user", content: [{ type: "input_text", text: "hello" }] }],
    stream: true,
  };
  const calls: Array<{ url: string; init?: RequestInit }> = [];
  const fetcher: SurplusFetch = (input, init) => {
    calls.push({ url: input.toString(), init });
    return Promise.resolve(
      new Response("data: [DONE]\n\n", {
        status: 200,
        headers: { "X-Request-Id": " surplus-request-1 " },
      }),
    );
  };

  const result = await fetchSurplusResponses(body, {
    apiKey: "inf_test",
    fetcher,
  });

  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, "https://api.surplusintelligence.ai/v1/responses");
  assert.equal(calls[0].init?.method, "POST");
  assert.deepEqual(JSON.parse(String(calls[0].init?.body)), body);
  const headers = new Headers(calls[0].init?.headers);
  assert.equal(headers.get("Authorization"), "Bearer inf_test");
  assert.equal(headers.get("Accept"), "text/event-stream");
  assert.equal(headers.get("Content-Type"), "application/json");
  assert.equal(result.request_id, "surplus-request-1");
});

Deno.test("fetchSurplusResponses translates Codex ultra reasoning to the upstream max preset", async () => {
  let forwarded: Record<string, unknown> | null = null;
  const fetcher: SurplusFetch = (_input, init) => {
    forwarded = JSON.parse(String(init?.body)) as Record<string, unknown>;
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  await fetchSurplusResponses(
    { model: "gpt-5.6-sol", input: "hello", reasoning: { effort: "ultra" } },
    { apiKey: "inf_test", fetcher },
  );

  assert.deepEqual(forwarded, {
    model: "gpt-5.6-sol",
    input: "hello",
    reasoning: { effort: "max" },
  });
});

Deno.test("fetchSurplusResponses runs the quota hook immediately before transport", async () => {
  const events: string[] = [];
  const fetcher: SurplusFetch = (_input, init) => {
    events.push("fetch");
    assert.equal(init?.signal?.aborted, false);
    return Promise.resolve(new Response("{}", { status: 200 }));
  };
  await fetchSurplusResponses(
    { model: "gpt-5.6-sol", input: "hello" },
    {
      apiKey: "inf_test",
      fetcher,
      beforeDispatch: () => {
        events.push("before-dispatch");
        return Promise.resolve({
          markTransportStarted: () => {
            events.push("started");
          },
          cancelBeforeTransport: () =>
            Promise.resolve().then(() => {
              events.push("cancelled");
            }),
        });
      },
    },
  );
  assert.deepEqual(events, ["before-dispatch", "started", "fetch"]);
});
