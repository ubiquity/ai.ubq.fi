import assert from "node:assert/strict";
import { BOUNDED_RESPONSE_BODY_MAX_BYTES, readBoundedResponseBody } from "../src/bounded_response_body.ts";

const encoder = new TextEncoder();
const decoder = new TextDecoder();

Deno.test("bounded response body reads fragmented complete bodies", async () => {
  const response = new Response(
    new ReadableStream<Uint8Array>({
      start(controller) {
        controller.enqueue(encoder.encode("frag"));
        controller.enqueue(encoder.encode("mented"));
        controller.close();
      },
    }),
  );

  const result = await readBoundedResponseBody(response);
  assert.equal(result.complete, true);
  assert.equal(decoder.decode(result.bytes), "fragmented");
});

Deno.test("bounded response body truncates and cancels oversized bodies", async () => {
  let cancellations = 0;
  const response = new Response(
    new ReadableStream<Uint8Array>({
      start(controller) {
        controller.enqueue(new Uint8Array(BOUNDED_RESPONSE_BODY_MAX_BYTES + 1));
      },
      cancel() {
        cancellations += 1;
      },
    }),
  );

  const result = await readBoundedResponseBody(response);
  assert.equal(result.complete, false);
  assert.equal(result.bytes.byteLength, BOUNDED_RESPONSE_BODY_MAX_BYTES);
  await new Promise<void>((resolve) => queueMicrotask(() => resolve()));
  assert.equal(cancellations, 1);
});

Deno.test("bounded response body respects timeout and caller abort", async (t) => {
  const pendingResponse = (onCancel: () => void): Response =>
    new Response(
      new ReadableStream<Uint8Array>({
        pull: () => new Promise<void>(() => {}),
        cancel: onCancel,
      }),
    );

  await t.step("timeout", async () => {
    let cancellations = 0;
    const startedAt = performance.now();
    const result = await readBoundedResponseBody(
      pendingResponse(() => {
        cancellations += 1;
      }),
      { timeoutMs: 10 },
    );
    assert.equal(result.complete, false);
    assert.ok(performance.now() - startedAt < 500);
    await new Promise<void>((resolve) => queueMicrotask(() => resolve()));
    assert.equal(cancellations, 1);
  });

  await t.step("caller abort", async () => {
    let cancellations = 0;
    const controller = new AbortController();
    queueMicrotask(() => controller.abort("caller cancelled"));
    const result = await readBoundedResponseBody(
      pendingResponse(() => {
        cancellations += 1;
      }),
      {
        signal: controller.signal,
        timeoutMs: 1_000,
      },
    );
    assert.equal(result.complete, false);
    await new Promise<void>((resolve) => queueMicrotask(() => resolve()));
    assert.equal(cancellations, 1);
  });
});

Deno.test("bounded response body releases an incomplete reader exactly once", async () => {
  let releases = 0;
  let cancellations = 0;
  const reader = {
    read: () => Promise.resolve({ done: false, value: new Uint8Array(2) }),
    cancel: () => {
      cancellations += 1;
      return Promise.resolve();
    },
    releaseLock: () => releases += 1,
  };
  const response = { body: { getReader: () => reader } } as unknown as Response;

  const result = await readBoundedResponseBody(response, { maxBytes: 1 });
  assert.equal(result.complete, false);
  await new Promise<void>((resolve) => queueMicrotask(() => resolve()));
  assert.equal(cancellations, 1);
  assert.equal(releases, 1);
});
