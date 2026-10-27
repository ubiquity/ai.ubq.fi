import { test } from "bun:test";
import assert from "node:assert/strict";
import { chatResponsesProjectionFailure, isLocalContextAdmissionFailure } from "../src/deepseek/context-admission-failure.ts";
import { aggregateContextOverflow, failure } from "../src/deepseek/responses.ts";
import { handleDeepSeekResponses } from "../src/deepseek/handlers.ts";
import { handleLithosResponses } from "../src/provider/lithos-handlers.ts";
import { handleCerebrasResponses } from "../src/provider/cerebras-responses.ts";
import { handleDeepSeekWaterfallResponses } from "../src/deepseek/waterfall-handler.ts";
import { createResponseTelemetryState, getResponseTelemetry, type UsageContext } from "../src/openai-telemetry.ts";
import { createFetchMock } from "./helpers/fetch-mock.ts";

const metadata = { session_id: "session", thread_id: "thread", turn_id: "turn" };
const request = (value: unknown = metadata): Request =>
  new Request("http://127.0.0.1:7999/v1/responses", {
    method: "POST",
    headers: value === undefined ? {} : { "x-codex-turn-metadata": typeof value === "string" ? value : JSON.stringify(value) },
  });
const overflow = aggregateContextOverflow("Fixture aggregate prompt exceeds the verified provider budget.");
const values = (text: string): Record<string, unknown>[] =>
  [...text.matchAll(/^data: (.+)$/gm)].map((match) => JSON.parse(match[1]) as Record<string, unknown>);
const context = (): { usage: UsageContext; observations: { completed: boolean; usage: unknown }[]; dispatches: number[] } => {
  const observations: { completed: boolean; usage: unknown }[] = [];
  const dispatches: number[] = [];
  return {
    usage: {
      keyId: null,
      kernelRepo: null,
      kernelOrg: null,
      responseTelemetry: createResponseTelemetryState(),
      beforeProviderDispatch: () => {
        dispatches.push(1);
        return Promise.resolve(undefined);
      },
      onTerminalUsage: (usage, completed) => {
        observations.push({ usage, completed });
      },
    },
    observations,
    dispatches,
  };
};

test("Codex aggregate failure emits one failed terminal with truthful missing usage", async () => {
  const spy = context();
  const response = chatResponsesProjectionFailure(request(), { stream: true }, overflow, spy.usage);
  assert.equal(response.status, 200);
  assert.equal(isLocalContextAdmissionFailure(response), true);
  const events = values(await response.text());
  assert.deepEqual(
    events.map((event) => event.type),
    ["response.created", "response.failed"]
  );
  const created = events[0].response as Record<string, unknown>;
  const failed = events[1].response as Record<string, unknown>;
  assert.equal(created.id, failed.id);
  assert.equal(failed.status, "failed");
  assert.deepEqual(failed.error, { code: "context_length_exceeded", message: overflow.message, param: "input" });
  assert.deepEqual(failed.output, []);
  assert.equal(failed.usage, null);
  assert.deepEqual(spy.observations, [{ usage: null, completed: false }]);
  assert.deepEqual(spy.dispatches, []);
  const telemetry = getResponseTelemetry(response);
  assert.ok(telemetry);
  assert.equal(telemetry.completed, false);
  assert.equal(telemetry.streamTerminalType, "response.failed");
  assert.equal(telemetry.inputTokens, null);
  assert.equal(telemetry.totalTokens, null);
  assert.equal(telemetry.usageObserved, false);
  assert.equal(telemetry.firstProviderDispatchMs, null);
});

for (const value of [null, "not-json", [], {}, { ...metadata, session_id: " " }, { ...metadata, thread_id: 1 }, { ...metadata, turn_id: "" }]) {
  test(`Malformed or missing native identity retains HTTP400: ${JSON.stringify(value)}`, async () => {
    const response = chatResponsesProjectionFailure(request(value), { stream: true }, overflow);
    assert.equal(response.status, 400);
    assert.equal(isLocalContextAdmissionFailure(response), false);
    assert.equal(((await response.json()) as { error: { code: string } }).error.code, "context_length_exceeded");
  });
}

test("Plain OpenAI, nonstream and other projection failures retain HTTP400", () => {
  const plain = new Request("http://127.0.0.1:7999/v1/responses", { method: "POST" });
  assert.equal(chatResponsesProjectionFailure(plain, { stream: true }, overflow).status, 400);
  for (const stream of [undefined, false, "true"]) assert.equal(chatResponsesProjectionFailure(request(), { stream }, overflow).status, 400);
  for (const error of [failure("input", "Other context refusal", "context_length_exceeded"), failure("input", "Invalid input")]) {
    assert.equal(chatResponsesProjectionFailure(request(), { stream: true }, error).status, 400);
  }
});

const adapters = [
  { name: "deepseek", model: "deepseek-flash", handle: handleDeepSeekResponses },
  { name: "lithos", model: "deepseek-ai/DeepSeek-V4.1-Flash", handle: handleLithosResponses },
  { name: "cerebras", model: "gpt-oss-120b", handle: handleCerebrasResponses },
];
const largeInput = Array.from({ length: 110 }, (_, index) => ({
  type: "message",
  role: "user",
  content: [{ type: "input_text", text: `${index}: ${"x".repeat(60 * 1024)}` }],
}));
largeInput.push({ type: "message", role: "user", content: [{ type: "input_text", text: "PRESERVE_LATEST_INPUT_CANARY" }] });

for (const adapter of adapters) {
  test(`${adapter.name}: aggregate overflow is local, typed and byte-preserving`, async () => {
    const body = { model: adapter.model, input: largeInput, stream: true };
    const before = JSON.stringify(body);
    const spy = context();
    const previous = globalThis.fetch;
    let fetches = 0;
    globalThis.fetch = createFetchMock(() => {
      fetches++;
      throw new Error("Aggregate rejection must not dispatch");
    });
    try {
      const response = await adapter.handle(request(), body, adapter.model, spy.usage);
      assert.equal(response.status, 200);
      assert.equal(isLocalContextAdmissionFailure(response), true);
      const events = values(await response.text());
      assert.deepEqual(
        events.map((event) => event.type),
        ["response.created", "response.failed"]
      );
      assert.equal((events[1].response as { error: { code: string } }).error.code, "context_length_exceeded");
      assert.equal(JSON.stringify(body), before);
      assert.equal(fetches, 0);
      assert.deepEqual(spy.dispatches, []);
      assert.deepEqual(spy.observations, [{ usage: null, completed: false }]);
      for (const req of [request(), new Request("http://127.0.0.1:7999/v1/responses", { method: "POST" })]) {
        const ordinary = await adapter.handle(req, { ...body, stream: false }, adapter.model);
        assert.equal(ordinary.status, 400);
      }
      const plainStreaming = await adapter.handle(new Request("http://127.0.0.1:7999/v1/responses", { method: "POST" }), body, adapter.model);
      assert.equal(plainStreaming.status, 400);
    } finally {
      globalThis.fetch = previous;
    }
  });
  test(`${adapter.name}: disabled per-message refusal is not an aggregate recovery signal`, async () => {
    const body = {
      stream: true,
      truncation: "disabled",
      input: [{ type: "message", role: "user", content: [{ type: "input_text", text: "x".repeat(65537) }] }],
    };
    const response = await adapter.handle(request(), body, adapter.model);
    assert.equal(response.status, 400);
    assert.equal(isLocalContextAdmissionFailure(response), false);
  });
}

test("Synthetic waterfall stops local context failure without any provider or paid-tail dispatch", async () => {
  const spy = context();
  let fetches = 0;
  let paidCalls = 0;
  const previous = globalThis.fetch;
  globalThis.fetch = createFetchMock(() => {
    fetches++;
    throw new Error("No provider may be dispatched");
  });
  try {
    const response = await handleDeepSeekWaterfallResponses(request(), { model: "ubiquity/deepseek-flash", input: largeInput, stream: true }, {}, spy.usage, {
      selection: null,
      enabled: () => true,
      configured: () => true,
      paidTail: () => {
        paidCalls++;
        throw new Error("No paid fallback for local context failure");
      },
    });
    assert.equal(response.status, 200);
    assert.equal(isLocalContextAdmissionFailure(response), true);
    assert.equal(response.headers.get("x-uos-attempted-providers"), "lithos");
    assert.deepEqual(
      values(await response.text()).map((event) => event.type),
      ["response.created", "response.failed"]
    );
    assert.equal(fetches, 0);
    assert.equal(paidCalls, 0);
    assert.deepEqual(spy.dispatches, []);
    assert.deepEqual(spy.observations, [{ usage: null, completed: false }]);
    assert.equal(getResponseTelemetry(response)?.completed, false);
    assert.equal(spy.usage.responseTelemetry?.fallbackReason, null);
  } finally {
    globalThis.fetch = previous;
  }
});
