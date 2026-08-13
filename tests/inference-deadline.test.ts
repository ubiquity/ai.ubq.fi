import assert from "node:assert/strict";
import {
  createInferenceSignal,
  INFERENCE_DEADLINE_MS,
  OPENAI_DEFAULT_REQUEST_TIMEOUT_MS,
  OPENAI_FLEX_REQUEST_TIMEOUT_MS,
  STREAM_FIRST_EVENT_DEADLINE_MS,
  STREAM_INACTIVITY_DEADLINE_MS,
} from "../src/inference_deadline.ts";

Deno.test("inference deadlines cover the OpenAI SDK and Flex timeout guidance", () => {
  assert.equal(OPENAI_DEFAULT_REQUEST_TIMEOUT_MS, 10 * 60_000);
  assert.equal(OPENAI_FLEX_REQUEST_TIMEOUT_MS, 15 * 60_000);
  assert.equal(INFERENCE_DEADLINE_MS, OPENAI_FLEX_REQUEST_TIMEOUT_MS);
  assert.ok(STREAM_FIRST_EVENT_DEADLINE_MS < 125_000);
  assert.ok(STREAM_INACTIVITY_DEADLINE_MS < 400_000);
});

Deno.test("inference signal propagates downstream cancellation", () => {
  const controller = new AbortController();
  const signal = createInferenceSignal(controller.signal);
  const reason = new DOMException("client disconnected", "AbortError");

  controller.abort(reason);

  assert.equal(signal.aborted, true);
  assert.equal(signal.reason, reason);
});

Deno.test("inference signal enforces its deadline", async () => {
  const signal = createInferenceSignal(new AbortController().signal, 1);

  await new Promise<void>((resolve) => signal.addEventListener("abort", () => resolve(), { once: true }));

  assert.equal(signal.aborted, true);
  assert.equal(signal.reason?.name, "TimeoutError");
});
