import assert from "node:assert/strict";
import {
  BUFFERED_INFERENCE_DEADLINE_MS,
  createInferenceSignal,
  createStreamFirstEventDeadline,
  createStreamSemanticDeadline,
  INFERENCE_DEADLINE_MS,
  OPENAI_DEFAULT_REQUEST_TIMEOUT_MS,
  OPENAI_FLEX_REQUEST_TIMEOUT_MS,
  STREAM_FAILOVER_RESERVE_MS,
  STREAM_FIRST_EVENT_DEADLINE_MS,
  STREAM_INACTIVITY_DEADLINE_MS,
  setBufferedInferenceDeadlineMsForTest,
} from "../src/inference-deadline.ts";
import { dispatchDeepSeekUpstream, streamDeepSeekChatCompletion } from "../src/deepseek/handlers.ts";

/** The wall-clock budget one inference attempt owns, in milliseconds. */
const INFERENCE_BUDGET_MS = 30 * 60_000;

Deno.test("inference deadlines pin the documented per-attempt budget", () => {
  assert.equal(OPENAI_DEFAULT_REQUEST_TIMEOUT_MS, 10 * 60_000);
  assert.equal(OPENAI_FLEX_REQUEST_TIMEOUT_MS, 15 * 60_000);
  assert.equal(STREAM_FIRST_EVENT_DEADLINE_MS, INFERENCE_BUDGET_MS);
  assert.equal(STREAM_INACTIVITY_DEADLINE_MS, INFERENCE_BUDGET_MS);
  assert.equal(INFERENCE_DEADLINE_MS, STREAM_FIRST_EVENT_DEADLINE_MS);
  assert.equal(BUFFERED_INFERENCE_DEADLINE_MS, STREAM_FIRST_EVENT_DEADLINE_MS);
  // The failover reserve is subtracted from this budget before a fallback
  // attempt starts, so it must stay far below it.
  assert.equal(STREAM_FAILOVER_RESERVE_MS, 15_000);
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

  await new Promise<void>((resolve) => {
    signal.addEventListener(
      "abort",
      () => {
        resolve();
      },
      { once: true }
    );
  });

  assert.equal(signal.aborted, true);
  assert.equal(signal.reason?.name, "TimeoutError");
});

Deno.test("failover attempts share one pre-header deadline", async () => {
  const request = new AbortController();
  const shared = createStreamFirstEventDeadline(request.signal, 80);
  const primary = createStreamSemanticDeadline(shared.signal, 30);
  await new Promise<void>((resolve) => {
    primary.signal.addEventListener(
      "abort",
      () => {
        resolve();
      },
      { once: true }
    );
  });
  assert.equal(primary.signal.reason?.name, "TimeoutError");
  assert.equal(shared.signal.aborted, false);
  const remainingAtFallback = shared.remainingMs();
  assert.ok(remainingAtFallback > 0 && remainingAtFallback < 80);
  const fallback = createStreamSemanticDeadline(shared.signal, Math.ceil(remainingAtFallback) + 20);
  await new Promise<void>((resolve) => {
    fallback.signal.addEventListener(
      "abort",
      () => {
        resolve();
      },
      { once: true }
    );
  });
  assert.equal(shared.signal.aborted, true);
  assert.equal(fallback.signal.reason?.name, "TimeoutError");
  primary.clear();
  fallback.clear();
  shared.clear();
});

Deno.test("clearing a selected attempt leaves it available after the shared deadline", async () => {
  const request = new AbortController();
  const shared = createStreamFirstEventDeadline(request.signal, 20);
  const selected = createStreamSemanticDeadline(shared.signal, Math.ceil(shared.remainingMs()));
  selected.clear();
  shared.clear();
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.equal(selected.signal.aborted, false);
  assert.equal(shared.signal.aborted, false);
});

Deno.test("streamed dispatch does not abort a healthy stream that outlives BUFFERED_INFERENCE_DEADLINE_MS", async () => {
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();
  const makeChunk = (content: string): Uint8Array =>
    encoder.encode(
      `data: ${JSON.stringify({
        id: "chatcmpl-stream-deadline",
        object: "chat.completion.chunk",
        created: 1_780_000_000,
        model: "deepseek-flash",
        choices: [{ index: 0, delta: { content }, finish_reason: null }],
      })}\n\n`
    );
  const doneChunk = encoder.encode("data: [DONE]\n\n");

  // Set the buffered deadline to a short window (40 ms)
  setBufferedInferenceDeadlineMsForTest(40);

  try {
    // Upstream stream that emits frames every 20 ms across 80 ms (outliving 40 ms)
    const stream = new ReadableStream<Uint8Array>({
      start(controller) {
        const safeEnqueue = (data: Uint8Array): void => {
          try {
            controller.enqueue(data);
          } catch {
            // Already closed or cancelled
          }
        };
        const safeClose = (): void => {
          try {
            controller.close();
          } catch {
            // Already closed or cancelled
          }
        };
        setTimeout(() => safeEnqueue(makeChunk("frame-1")), 10);
        setTimeout(() => safeEnqueue(makeChunk("frame-2")), 30);
        setTimeout(() => safeEnqueue(makeChunk("frame-3")), 50);
        setTimeout(() => safeEnqueue(doneChunk), 70);
        setTimeout(() => safeClose(), 75);
      },
    });

    const req = new Request("https://ai.ubq.fi/v1/chat/completions", {
      method: "POST",
      headers: { "content-type": "application/json" },
    });
    const streamedBody = {
      model: "deepseek-flash",
      messages: [{ role: "user", content: "hello" }],
      stream: true,
    };

    const upstreamResponse = new Response(stream, {
      status: 200,
      headers: { "content-type": "text/event-stream" },
    });

    const dispatched = await dispatchDeepSeekUpstream(req, streamedBody, "deepseek-flash", undefined, {
      apiKey: "sk-test",
      fetcher: () => Promise.resolve(upstreamResponse),
    });
    assert.equal(dispatched.ok, true);

    const relay = streamDeepSeekChatCompletion(
      dispatched.upstream,
      dispatched.providerRequestId,
      undefined,
      dispatched.downstreamSignal,
      dispatched.requestSignal,
      "deepseek-flash"
    );
    assert.equal(relay.status, 200);

    const reader = relay.body?.getReader();
    assert.ok(reader);
    let collected = "";
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      collected += decoder.decode(value);
    }

    // Verify all frames delivered across the 75 ms generation despite 40 ms buffered deadline
    assert.ok(collected.includes("frame-1"));
    assert.ok(collected.includes("frame-2"));
    assert.ok(collected.includes("frame-3"));
    assert.ok(collected.includes("[DONE]"));

    // Verify contrast: buffered inference with a slow upstream stalls and aborts at the buffered deadline
    const bufferedBody = {
      model: "deepseek-flash",
      messages: [{ role: "user", content: "hello" }],
      stream: false,
    };
    const slowFetcher = (_url: unknown, init?: RequestInit) =>
      new Promise<Response>((resolve, reject) => {
        const timer = setTimeout(() => resolve(new Response("{}", { status: 200 })), 70);
        init?.signal?.addEventListener("abort", () => {
          clearTimeout(timer);
          reject(init.signal?.reason ?? new DOMException("The operation was aborted.", "TimeoutError"));
        });
      });
    const bufferedDispatched = await dispatchDeepSeekUpstream(req, bufferedBody, "deepseek-flash", undefined, {
      apiKey: "sk-test",
      fetcher: slowFetcher as typeof fetch,
    });
    assert.equal(bufferedDispatched.ok, false);
    assert.equal(bufferedDispatched.response.status, 504);
  } finally {
    setBufferedInferenceDeadlineMsForTest(null);
  }
});
