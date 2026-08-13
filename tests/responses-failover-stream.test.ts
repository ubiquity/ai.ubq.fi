import assert from "node:assert/strict";
import {
  appendResponsesPrecommitEvent,
  buildFailoverWarningEvents,
  createOwnedResponsesStream,
  failureEventAfterCommit,
  MAX_RESPONSES_PRECOMMIT_CHARS,
  MAX_RESPONSES_PRECOMMIT_EVENTS,
  prepareResponsesStreamForCommit,
  responseEventFromValue,
  responsesEventSemanticKind,
  rewriteResponsesEventForWarning,
} from "../src/responses_failover_stream.ts";
import type { ResponsesStreamEvent, ResponsesStreamIterator } from "../src/responses_stream.ts";

const event = (value: Record<string, unknown>): ResponsesStreamEvent => responseEventFromValue(value);

const iterator = (events: ResponsesStreamEvent[]): ResponsesStreamIterator =>
  (async function* () {
    yield* events;
  })();

Deno.test("Responses semantic detector ignores setup and empty deltas", () => {
  assert.equal(responsesEventSemanticKind(event({ type: "response.created", response: { id: "resp_1" } })), null);
  assert.equal(responsesEventSemanticKind(event({ type: "response.output_text.delta", delta: "" })), null);
  assert.equal(responsesEventSemanticKind(event({ type: "response.reasoning_summary_text.delta", delta: "" })), null);
});

Deno.test("Responses semantic detector commits on text, reasoning, and completed executable tools", () => {
  assert.equal(responsesEventSemanticKind(event({ type: "response.output_text.delta", delta: "x" })), "text");
  assert.equal(
    responsesEventSemanticKind(event({ type: "response.reasoning_summary_text.delta", delta: "thinking" })),
    "reasoning",
  );
  assert.equal(
    responsesEventSemanticKind(event({
      type: "response.output_item.done",
      item: { type: "function_call", call_id: "call_1", name: "lookup", arguments: "{}" },
    })),
    "tool_call",
  );
  assert.equal(
    responsesEventSemanticKind(event({
      type: "response.output_item.done",
      item: { type: "custom_tool_call", call_id: "call_2", name: "exec", input: "pwd" },
    })),
    "tool_call",
  );
});

Deno.test("Responses precommit preparation holds setup until semantic output", async () => {
  const created = event({ type: "response.created", response: { id: "resp_1" } });
  const empty = event({ type: "response.output_text.delta", delta: "" });
  const semantic = event({ type: "response.output_text.delta", delta: "hello" });
  const prepared = await prepareResponsesStreamForCommit(iterator([created, empty, semantic]));
  assert.deepEqual(prepared.buffered.map((item) => item.type), [
    "response.created",
    "response.output_text.delta",
    "response.output_text.delta",
  ]);
  assert.equal(prepared.semantic, semantic);
  assert.equal(prepared.semanticKind, "text");
});

Deno.test("Responses precommit bounds cover delayed discovery events and characters", () => {
  const buffered = Array.from(
    { length: MAX_RESPONSES_PRECOMMIT_EVENTS },
    (_, index) => event({ type: "response.in_progress", sequence_number: index }),
  );
  assert.throws(
    () => appendResponsesPrecommitEvent(buffered, event({ type: "response.in_progress" }), 0),
    /precommit buffer exceeded/,
  );
  assert.throws(
    () =>
      appendResponsesPrecommitEvent(
        [],
        event({ type: "response.in_progress" }),
        MAX_RESPONSES_PRECOMMIT_CHARS,
      ),
    /precommit buffer exceeded/,
  );
});

Deno.test("Failover warning is a valid assistant item at zero and shifts later output indices", () => {
  const warning = buildFailoverWarningEvents("google/gemini-2.5-pro", "resp_1", 10);
  assert.equal(warning.events.length, 6);
  assert.equal(warning.events[0]?.value.output_index, 0);
  assert.equal(warning.events.at(-1)?.type, "response.output_item.done");
  assert.match(JSON.stringify(warning.item), /Failover active/);
  const rewritten = rewriteResponsesEventForWarning(
    event({
      type: "response.custom_tool_call_input.delta",
      output_index: 0,
      item_id: "ctc_1",
      sequence_number: 12,
      delta: "pw",
    }),
    warning.item,
    18,
  );
  assert.equal(rewritten.value.output_index, 1);
  assert.equal(rewritten.value.sequence_number, 18);
  assert.equal(rewritten.value.item_id, "ctc_1");
  assert.equal(rewritten.value.delta, "pw");
});

Deno.test("Failover terminal rewrite prefixes warning output and owns one failed terminal", () => {
  const warning = buildFailoverWarningEvents("google/gemini-2.5-pro", "resp_1");
  const completed = rewriteResponsesEventForWarning(
    event({
      type: "response.completed",
      response: { id: "resp_1", output: [{ id: "ctc_1", type: "custom_tool_call", call_id: "call_1" }] },
    }),
    warning.item,
    warning.events.length,
  );
  assert.equal((completed.value.response as { output?: unknown[] }).output?.length, 2);
  const failed = failureEventAfterCommit("resp_1", 99);
  assert.equal(failed.type, "response.failed");
  assert.equal(failed.terminal, true);
});

Deno.test("Responses precommit preparation accepts a completed response without output", async () => {
  const completed = event({
    type: "response.completed",
    response: { id: "resp_empty", object: "response", status: "completed", output: [] },
  });
  const prepared = await prepareResponsesStreamForCommit(iterator([
    event({ type: "response.created", response: { id: "resp_empty" } }),
    completed,
  ]));
  assert.equal(prepared.semantic, null);
  assert.equal(prepared.terminal, completed);
});

Deno.test("Owned failover stream emits warning first, shifts output, and emits one terminal", async () => {
  const initial = [
    event({ type: "response.created", sequence_number: 0, response: { id: "resp_1", model: "google/gemini" } }),
    event({
      type: "response.output_text.delta",
      sequence_number: 1,
      response_id: "resp_1",
      item_id: "msg_1",
      output_index: 0,
      content_index: 0,
      delta: "hello",
    }),
  ];
  const rest = iterator([
    event({
      type: "response.completed",
      sequence_number: 2,
      response: { id: "resp_1", object: "response", status: "completed", output: [] },
    }),
  ]);
  const body = createOwnedResponsesStream({
    initial,
    iterator: rest,
    responseId: "resp_1",
    warning: { model: "google/gemini" },
  });
  const text = await new Response(body).text();
  const values = [...text.matchAll(/^data: (.+)$/gm)].map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  assert.equal(values[0]?.type, "response.created");
  assert.match(JSON.stringify(values[3]), /Failover active/);
  const hello = values.find((value) => value.delta === "hello");
  assert.equal(hello?.output_index, 1);
  assert.deepEqual(values.map((value) => value.sequence_number), values.map((_, index) => index));
  assert.equal(values.filter((value) => value.type === "response.completed").length, 1);
  assert.equal(values.filter((value) => value.type === "response.failed").length, 0);
});

Deno.test("Owned failover stream preserves a legitimate incomplete terminal", async () => {
  const responseTemplate = {
    id: "resp_incomplete",
    object: "response",
    created_at: 17,
    model: "google/gemini",
    status: "in_progress",
    output: [],
  };
  const incomplete = {
    ...responseTemplate,
    status: "incomplete",
    incomplete_details: { reason: "max_output_tokens" },
    output: [{
      id: "msg_partial",
      type: "message",
      status: "incomplete",
      role: "assistant",
      content: [{ type: "output_text", text: "partial", annotations: [] }],
    }],
    usage: { input_tokens: 2, output_tokens: 3, total_tokens: 5 },
  };
  const body = createOwnedResponsesStream({
    initial: [
      event({ type: "response.created", sequence_number: 0, response: responseTemplate }),
      event({
        type: "response.output_text.delta",
        sequence_number: 1,
        response_id: "resp_incomplete",
        item_id: "msg_partial",
        output_index: 0,
        delta: "partial",
      }),
    ],
    iterator: iterator([event({ type: "response.incomplete", sequence_number: 2, response: incomplete })]),
    responseId: "resp_incomplete",
  });
  const values = [...(await new Response(body).text()).matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  assert.equal(values.filter((value) => value.type === "response.incomplete").length, 1);
  assert.equal(values.filter((value) => value.type === "response.failed").length, 0);
  const terminal = values.at(-1)?.response as Record<string, unknown>;
  assert.deepEqual(terminal.incomplete_details, { reason: "max_output_tokens" });
  assert.deepEqual(terminal.usage, { input_tokens: 2, output_tokens: 3, total_tokens: 5 });
});

Deno.test("Owned stream synthetic failure preserves template, text, tools, and sequence order", async () => {
  const responseTemplate = {
    id: "resp_broken",
    object: "response",
    created_at: 42,
    model: "google/gemini",
    service_tier: "default",
    status: "in_progress",
    output: [],
  };
  const toolItem = {
    id: "ctc_broken",
    type: "custom_tool_call",
    status: "completed",
    call_id: "call_broken",
    name: "exec",
    input: "pwd",
  };
  const body = createOwnedResponsesStream({
    initial: [
      event({ type: "response.created", sequence_number: 4, response: responseTemplate }),
      event({
        type: "response.output_text.delta",
        sequence_number: 5,
        response_id: "resp_broken",
        item_id: "msg_broken",
        output_index: 0,
        delta: "kept text",
      }),
      event({
        type: "response.output_item.done",
        sequence_number: 6,
        response_id: "resp_broken",
        output_index: 1,
        item: toolItem,
      }),
    ],
    iterator: iterator([]),
    responseId: "resp_broken",
  });
  const values = [...(await new Response(body).text()).matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  assert.deepEqual(values.map((value) => value.sequence_number), [4, 5, 6, 7]);
  const terminal = values.at(-1)!;
  assert.equal(terminal.type, "response.failed");
  const response = terminal.response as Record<string, unknown>;
  assert.equal(response.created_at, 42);
  assert.equal(response.model, "google/gemini");
  assert.equal(response.service_tier, "default");
  assert.equal(response.status, "failed");
  assert.match(JSON.stringify(response.output), /kept text/);
  assert.match(JSON.stringify(response.output), /ctc_broken/);
});
