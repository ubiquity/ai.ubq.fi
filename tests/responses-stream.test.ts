import assert from "node:assert/strict";
import {
  preflightResponsesStream,
  proxyResponsesStream,
  readResponsesStream,
  ResponsesStreamError,
} from "../src/responses_stream.ts";

const bytes = (value: string): Uint8Array => new TextEncoder().encode(value);

const captureError = async (run: () => Promise<void>): Promise<unknown> => {
  let caught: unknown;
  try {
    await run();
  } catch (error) {
    caught = error;
  }
  if (caught === undefined) assert.fail("Expected operation to reject");
  return caught;
};

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
  const malformed = await captureError(async () => {
    for await (const _ of readResponsesStream(chunked("data: nope\n\n", []))) {
      // consume
    }
  });
  assert.ok(malformed instanceof ResponsesStreamError);
  assert.equal(malformed.kind, "malformed_event");
  const eof = await captureError(async () => {
    for await (
      const _ of readResponsesStream(chunked('data: {"type":"response.output_text.delta","delta":"x"}\n\n', []))
    ) {
      // consume
    }
  });
  assert.ok(eof instanceof ResponsesStreamError);
  assert.match(eof.message, /before a terminal event/);
  assert.equal(eof.kind, "premature_eof");
});

Deno.test("Responses parser rejects terminal events without their protocol payload", async () => {
  const error = await captureError(async () => {
    for await (
      const _ of readResponsesStream(chunked('data: {"type":"response.completed"}\n\n', []))
    ) {
      // consume
    }
  });
  assert.ok(error instanceof ResponsesStreamError);
  assert.equal(error.kind, "malformed_event");
});

Deno.test("Responses preflight surfaces immediate failures before a stream response is created", async () => {
  const malformed = await captureError(async () => {
    await preflightResponsesStream(chunked("data: nope\n\n", []));
  });
  assert.ok(malformed instanceof ResponsesStreamError);
  assert.equal(malformed.kind, "malformed_event");

  const eof = await captureError(async () => {
    await preflightResponsesStream(chunked("", []));
  });
  assert.ok(eof instanceof ResponsesStreamError);
  assert.equal(eof.kind, "premature_eof");
});

Deno.test("Responses parser wraps reader exceptions and releases its lock", async () => {
  const source = new ReadableStream<Uint8Array>({
    pull(controller) {
      controller.error(new Error("socket reset"));
    },
  });
  const error = await captureError(async () => {
    for await (const _ of readResponsesStream(source)) {
      // consume
    }
  });
  assert.ok(error instanceof ResponsesStreamError);
  assert.equal(error.kind, "read_error");
  assert.equal(source.locked, false);
});

Deno.test("Responses proxy forwards only the first terminal and cancels a hanging upstream once", async () => {
  let cancelCount = 0;
  const source = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(
        bytes(
          'data: {"type":"response.incomplete","response":{"status":"incomplete"}}\n\n' +
            'data: {"type":"response.completed","response":{"status":"completed"}}\n\n' +
            'data: {"type":"response.output_text.delta","delta":"post-terminal"}\n\n',
        ),
      );
    },
    cancel() {
      cancelCount += 1;
    },
  });
  const output = await new Response(proxyResponsesStream(source)).text();
  assert.match(output, /response.incomplete/);
  assert.doesNotMatch(output, /response.completed/);
  assert.doesNotMatch(output, /post-terminal/);
  assert.equal(cancelCount, 1);
  assert.equal(source.locked, false);
});

Deno.test("Responses proxy emits an official error event after premature EOF", async () => {
  const source = chunked('data: {"type":"response.output_text.delta","delta":"x"}\n\n', []);
  const output = await new Response(proxyResponsesStream(source)).text();
  assert.match(output, /response.output_text.delta/);
  assert.match(output, /event: error/);
  assert.doesNotMatch(output, /response.completed/);
});

Deno.test("Responses proxy does not wait for lifecycle callbacks before terminal delivery", async () => {
  const never = new Promise<void>(() => {});
  const source = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(bytes('data: {"type":"response.completed","response":{"status":"completed"}}\n\n'));
    },
  });
  const output = await Promise.race([
    new Response(proxyResponsesStream(source, { onEvent: () => never })).text(),
    new Promise<never>((_, reject) => setTimeout(() => reject(new Error("terminal delivery was delayed")), 100)),
  ]);
  assert.match(output, /response.completed/);
});

Deno.test("Responses proxy cancellation is forwarded once without waiting for bookkeeping", async () => {
  let upstreamCancelCount = 0;
  let downstreamCancelCount = 0;
  const never = new Promise<void>(() => {});
  const source = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(bytes('data: {"type":"response.output_text.delta","delta":"x"}\n\n'));
    },
    cancel() {
      upstreamCancelCount += 1;
    },
  });
  const reader = proxyResponsesStream(source, {
    onCancel: () => {
      downstreamCancelCount += 1;
      return never;
    },
  }).getReader();
  const first = await reader.read();
  assert.equal(first.done, false);
  await Promise.race([
    reader.cancel("downstream disconnected"),
    new Promise<never>((_, reject) => setTimeout(() => reject(new Error("cancel was delayed")), 100)),
  ]);
  await Promise.resolve();
  assert.equal(downstreamCancelCount, 1);
  assert.equal(upstreamCancelCount, 1);
  assert.equal(source.locked, false);
});
