import { test } from "bun:test";
import assert from "node:assert/strict";
import { ReadableStream as NodeReadableStream } from "node:stream/web";
import { createOwnedResponsesStream, responseEventFromValue } from "../src/responses-failover-stream.ts";
import type { ResponsesStreamEvent, ResponsesStreamIterator } from "../src/responses-stream.ts";

const error = { code: "context_length_exceeded", message: "ISOLATED_FIXTURE_CONTEXT_OVERFLOW", param: "input" };
const values = (text: string): Record<string, unknown>[] =>
  [...text.matchAll(/^data: (.+)$/gm)].map((match) => JSON.parse(match[1]) as Record<string, unknown>);
const iterator = (events: ResponsesStreamEvent[]): ResponsesStreamIterator =>
  (async function* () {
    yield* NodeReadableStream.from(events);
  })();

for (const nested of [false, true]) {
  for (const partial of [false, true]) {
    test(`Codex context error becomes one failed response: nested=${nested}, partial=${partial}`, async () => {
      const original = responseEventFromValue({
        type: "error",
        sequence_number: 2,
        ...(nested ? { error: { ...error, type: "invalid_request_error" } } : error),
      });
      const observed: ResponsesStreamEvent[] = [];
      const template = { id: "resp_context", object: "response", status: "in_progress", model: "fixture", output: [] };
      const initial = [responseEventFromValue({ type: "response.created", sequence_number: 0, response: template })];
      if (partial)
        initial.push(responseEventFromValue({ type: "response.output_text.delta", sequence_number: 1, item_id: "msg_context", delta: "preserved partial" }));
      const stream = createOwnedResponsesStream({
        codexContextErrorCompatibility: true,
        responseId: "resp_context",
        initial,
        iterator: iterator([original]),
        onEvent: (event) => {
          observed.push(event);
        },
      });
      const emitted = values(await new Response(stream).text());
      const failed = emitted.at(-1);
      assert.equal(failed?.type, "response.failed");
      assert.equal(emitted.filter((event) => event.type === "response.failed").length, 1);
      assert.equal(
        emitted.some((event) => event.type === "error" || event.type === "response.completed"),
        false
      );
      const response = failed.response as Record<string, unknown>;
      assert.equal(response.id, template.id);
      assert.equal(response.model, template.model);
      assert.equal(response.status, "failed");
      assert.deepEqual(response.error, nested ? { ...error, type: "invalid_request_error" } : error);
      assert.equal(JSON.stringify(response.output).includes("preserved partial"), partial);
      assert.equal(observed.at(-1), original);
      assert.equal(observed.filter((event) => event.terminal).length, 1);
    });
  }
}

for (const [compatibility, responseId, code] of [
  [false, "resp_context", "context_length_exceeded"],
  [true, null, "context_length_exceeded"],
  [true, "resp_context", "provider_stream_error"],
] as const) {
  test(`Unchanged terminal: compatibility=${compatibility}, identity=${responseId}, code=${code}`, async () => {
    const original = responseEventFromValue({ type: "error", sequence_number: 0, ...error, code });
    const stream = createOwnedResponsesStream({ codexContextErrorCompatibility: compatibility, responseId, initial: [], iterator: iterator([original]) });
    assert.equal(await new Response(stream).text(), original.raw);
  });
}
