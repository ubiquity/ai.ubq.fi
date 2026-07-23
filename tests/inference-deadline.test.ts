import assert from "node:assert/strict";
import { createInferenceSignal, INFERENCE_DEADLINE_MS } from "../src/inference_deadline.ts";

Deno.test("inference deadline stays inside the unchanged router timeout", () => {
  assert.equal(INFERENCE_DEADLINE_MS, 110_000);
  assert.ok(INFERENCE_DEADLINE_MS < 120_000);
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
