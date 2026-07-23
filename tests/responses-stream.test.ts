import assert from "node:assert/strict";
import { proxyResponsesStream, readResponsesStream, ResponsesStreamError } from "../src/responses_stream.ts";

const bytes = (value: string): Uint8Array => new TextEncoder().encode(value);

const chunked = (value: string, boundaries: number[], cancel?: () => void): ReadableStream<Uint8Array> => {
  let offset = 0;
  return new ReadableStream({
    pull(controller) {
      const end = boundaries.shift() ?? value.length;
      if (offset >= value.length) return controller.close();
      controller.enqueue(bytes(value.slice(offset, end)));
      offset = end;
      if (offset >= value.length) controller.close();
    },
    cancel,
  });
};

Deno.test("Responses SSE parser handles mixed separators, multiline data, and fragmented UTF-8", async () => {
  const payload = ': keepalive\r\ndata: {"type":"response.output_text.delta",\r\ndata: "delta":"hi 🌍"}\r\n\r\n' +
    'data: {"type":"response.completed","response":{"status":"completed"}}';
  const encoded = bytes(payload);
  for (let boundary = 1; boundary < encoded.length; boundary += 1) {
    const source = new ReadableStream<Uint8Array>({
      start(controller) {
        controller.enqueue(encoded.slice(0, boundary));
        controller.enqueue(encoded.slice(boundary));
        controller.close();
      },
    });
    const events = [];
    for await (const event of readResponsesStream(source)) events.push(event);
    assert.deepEqual(events.map((event) => event.type), ["response.output_text.delta", "response.completed"]);
  }
});

Deno.test("Responses SSE parser rejects malformed JSON and EOF before terminal", async () => {
  await assert.rejects(
    async () => {
      for await (const _ of readResponsesStream(chunked("data: nope\n\n", []))) {
        // consume
      }
    },
    ResponsesStreamError,
  );
  await assert.rejects(
    async () => {
      for await (
        const _ of readResponsesStream(chunked('data: {"type":"response.output_text.delta","delta":"x"}\n\n', []))
      ) {
        // consume
      }
    },
    /before a terminal event/,
  );
});

Deno.test("Responses proxy forwards first terminal only and cancels a hanging upstream", async () => {
  let cancelled = false;
  const source = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(bytes('data: {"type":"response.completed","response":{"status":"completed"}}\n\n'));
    },
    cancel() {
      cancelled = true;
    },
  });
  const output = await new Response(proxyResponsesStream(source)).text();
  assert.match(output, /response.completed/);
  assert.equal(cancelled, true);
});

Deno.test("Responses proxy emits an official error event after premature EOF", async () => {
  const source = chunked('data: {"type":"response.output_text.delta","delta":"x"}\n\n', []);
  const output = await new Response(proxyResponsesStream(source)).text();
  assert.match(output, /response.output_text.delta/);
  assert.match(output, /event: error/);
  assert.doesNotMatch(output, /response.completed/);
});
