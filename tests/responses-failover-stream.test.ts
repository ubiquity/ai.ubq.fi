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
  responsesEventReportsProgress,
  responsesEventSemanticKind,
  rewriteResponsesEventForWarning,
} from "../src/responses_failover_stream.ts";
import {
  readResponsesStream,
  type ResponsesStreamEvent,
  type ResponsesStreamIterator,
} from "../src/responses_stream.ts";

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

Deno.test("Responses progress detector accepts valid reasoning events without making them semantic", () => {
  const progressEvents = [
    event({ type: "response.reasoning_summary_text.delta", summary_index: 0, delta: "considering" }),
    event({ type: "response.reasoning_summary_text.done", summary_index: 0, text: "considered" }),
    event({ type: "response.reasoning_text.delta", content_index: 0, delta: "private reasoning" }),
    event({ type: "response.reasoning_text.done", content_index: 0, text: "private reasoning complete" }),
    event({
      type: "response.reasoning_summary_part.added",
      summary_index: 0,
    }),
    event({
      type: "response.reasoning_summary_part.done",
      summary_index: 0,
    }),
    event({
      type: "response.output_item.added",
      item: {
        id: "rs_1",
        type: "reasoning",
        summary: [],
      },
    }),
    event({
      type: "response.output_item.done",
      item: {
        id: "rs_1",
        type: "reasoning",
        content: [{ type: "reasoning_text", text: "work complete" }],
      },
    }),
  ];
  for (const progressEvent of progressEvents) {
    assert.equal(responsesEventReportsProgress(progressEvent), true, progressEvent.type);
    assert.equal(responsesEventSemanticKind(progressEvent), null, progressEvent.type);
  }

  const nonProgressEvents = [
    event({ type: "response.reasoning_summary_text.delta", summary_index: 0, delta: "" }),
    event({ type: "response.reasoning_summary_text.done", summary_index: -1, text: "invalid index" }),
    event({ type: "response.reasoning_text.done", content_index: 0, text: null }),
    event({
      type: "response.reasoning_summary_part.added",
      summary_index: -1,
    }),
    event({
      type: "response.reasoning_summary_part.added",
      part: { type: "summary_text", text: "missing summary index" },
    }),
    event({
      type: "response.output_item.added",
      item: { id: "", type: "reasoning", summary: [] },
    }),
    event({
      type: "response.output_item.done",
      item: { id: "rs_1", type: "reasoning" },
    }),
    event({ type: "response.output_text.delta", delta: "visible" }),
  ];
  for (const nonProgressEvent of nonProgressEvents) {
    assert.equal(responsesEventReportsProgress(nonProgressEvent), false, nonProgressEvent.type);
  }
});

Deno.test("Responses semantic detector commits on visible text, refusal, and completed tools", () => {
  assert.equal(responsesEventSemanticKind(event({ type: "response.output_text.delta", delta: "x" })), "text");
  assert.equal(
    responsesEventSemanticKind(event({ type: "response.reasoning_summary_text.delta", delta: "thinking" })),
    null,
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
      type: "response.output",
      output: [{ type: "mcp_call", status: "completed", id: "mcp_top_level" }],
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
  assert.equal(responsesEventSemanticKind(event({ type: "response.refusal.delta", delta: "blocked" })), "text");
  assert.equal(
    responsesEventSemanticKind(event({ type: "response.refusal.done", refusal: "I cannot help with that." })),
    "text",
  );
  assert.equal(
    responsesEventSemanticKind(event({
      type: "response.content_part.done",
      part: { type: "refusal", refusal: "I cannot help with that." },
    })),
    "text",
  );
  assert.equal(
    responsesEventSemanticKind(event({
      type: "response.output_item.done",
      item: { type: "web_search_call", status: "completed", id: "ws_1" },
    })),
    "tool_call",
  );
  assert.equal(responsesEventSemanticKind(event({ type: "response.mcp_call.completed" })), "tool_call");
  assert.equal(responsesEventSemanticKind(event({ type: "response.mcp_call.in_progress" })), "tool_call");
  assert.equal(responsesEventSemanticKind(event({ type: "response.mcp_call.failed" })), "tool_call");
  assert.equal(
    responsesEventSemanticKind(event({
      type: "response.failed",
      response: { output: [{ type: "mcp_call", status: "failed", id: "mcp_1" }] },
    })),
    "tool_call",
  );
  assert.equal(
    responsesEventSemanticKind(event({
      type: "response.failed",
      response: {
        output: [{
          type: "message",
          content: [{ type: "refusal", refusal: "I cannot help with that." }],
        }],
      },
    })),
    "text",
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

Deno.test("Responses precommit can release on hidden-reasoning progress without marking semantic output", async () => {
  const created = event({ type: "response.created", response: { id: "resp_reasoning" } });
  const reasoning = event({
    type: "response.reasoning_summary_text.delta",
    summary_index: 0,
    delta: "active reasoning",
  });
  const visible = event({ type: "response.output_text.delta", delta: "visible" });
  const source = iterator([created, reasoning, visible]);
  const progressTypes: string[] = [];
  const prepared = await prepareResponsesStreamForCommit(source, {
    releaseOnProgress: true,
    onProgress: (progressEvent) => progressTypes.push(progressEvent.type),
  });
  assert.deepEqual(prepared.buffered, [created, reasoning]);
  assert.deepEqual(progressTypes, ["response.reasoning_summary_text.delta"]);
  assert.equal(prepared.semantic, null);
  assert.equal(prepared.semanticKind, null);
  assert.equal(prepared.terminal, null);
  assert.equal((await prepared.iterator.next()).value, visible);
  await prepared.iterator.return("test complete").catch(() => {});
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

Deno.test("Responses precommit keeps reasoning-only completion nonsemantic for the empty-output check", async () => {
  const completed = event({
    type: "response.completed",
    response: { id: "resp_empty", object: "response", status: "completed", output: [] },
  });
  const progressTypes: string[] = [];
  const summaryPart = event({ type: "response.reasoning_summary_part.added", summary_index: 0 });
  const reasoningItem = event({
    type: "response.output_item.added",
    item: { id: "rs_empty", type: "reasoning", summary: [] },
  });
  const prepared = await prepareResponsesStreamForCommit(
    iterator([
      event({ type: "response.created", response: { id: "resp_empty" } }),
      summaryPart,
      summaryPart,
      reasoningItem,
      reasoningItem,
      event({ type: "response.reasoning_summary_text.delta", summary_index: 0, delta: "hidden reasoning" }),
      event({ type: "response.reasoning_text.done", content_index: 0, text: "hidden reasoning" }),
      completed,
    ]),
    { onProgress: (progressEvent) => progressTypes.push(progressEvent.type) },
  );
  assert.deepEqual(progressTypes, [
    "response.reasoning_summary_part.added",
    "response.output_item.added",
    "response.reasoning_summary_text.delta",
    "response.reasoning_text.done",
  ]);
  assert.equal(prepared.semantic, null);
  assert.equal(prepared.terminal, completed);
});

Deno.test("Owned stream rewrites a post-release reasoning-only completion as empty", async () => {
  let observedFailure: {
    failureKind: string;
    semanticCommitmentObserved: boolean;
    syntheticTerminalType: string | null;
  } | null = null;
  let observedUpstreamTerminal: ResponsesStreamEvent | null = null;
  const completed = event({
    type: "response.completed",
    sequence_number: 2,
    response: {
      id: "resp_reasoning_empty",
      status: "completed",
      output: [{
        id: "rs_empty",
        type: "reasoning",
        summary: [{ type: "summary_text", text: "hidden reasoning" }],
      }],
    },
  });
  const body = createOwnedResponsesStream({
    initial: [
      event({ type: "response.created", response: { id: "resp_reasoning_empty" } }),
      event({ type: "response.reasoning_summary_text.delta", summary_index: 0, delta: "hidden reasoning" }),
    ],
    iterator: iterator([completed]),
    responseId: "resp_reasoning_empty",
    onFailure: (error, details) => {
      observedFailure = {
        failureKind: error instanceof Error && "kind" in error ? String(error.kind) : "unknown",
        semanticCommitmentObserved: details.semanticCommitmentObserved,
        syntheticTerminalType: details.syntheticTerminalType,
      };
      observedUpstreamTerminal = details.upstreamTerminal;
    },
  });
  const values = [...(await new Response(body).text()).matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  for (let attempt = 0; attempt < 10 && observedFailure === null; attempt += 1) {
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  assert.deepEqual(values.map((value) => value.type), [
    "response.created",
    "response.reasoning_summary_text.delta",
    "error",
  ]);
  assert.equal(values.at(-1)?.code, "empty_upstream_completion");
  assert.equal(values.some((value) => value.type === "response.completed"), false);
  assert.deepEqual(observedFailure, {
    failureKind: "empty_upstream_completion",
    semanticCommitmentObserved: false,
    syntheticTerminalType: "error",
  });
  assert.equal(observedUpstreamTerminal, completed);
});

Deno.test("Owned stream forwards a completion whose terminal contains visible output", async () => {
  let failureCount = 0;
  const body = createOwnedResponsesStream({
    initial: [
      event({ type: "response.created", response: { id: "resp_visible_terminal" } }),
      event({ type: "response.reasoning_summary_part.added", summary_index: 0 }),
    ],
    iterator: iterator([
      event({
        type: "response.completed",
        response: {
          id: "resp_visible_terminal",
          status: "completed",
          output: [{
            id: "msg_visible_terminal",
            type: "message",
            role: "assistant",
            content: [{ type: "output_text", text: "Visible answer." }],
          }],
        },
      }),
    ]),
    responseId: "resp_visible_terminal",
    onFailure: () => {
      failureCount += 1;
    },
  });
  const values = [...(await new Response(body).text()).matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  assert.equal(values.at(-1)?.type, "response.completed");
  assert.equal(values.some((value) => value.type === "error"), false);
  assert.equal(failureCount, 0);
});

Deno.test("Failover warning output does not hide an empty provider completion", async () => {
  let semanticCommitmentObserved: boolean | null = null;
  let observedUpstreamTerminal: ResponsesStreamEvent | null = null;
  const completed = event({
    type: "response.completed",
    sequence_number: 1,
    response: { id: "resp_warning_empty", status: "completed", output: [] },
  });
  const body = createOwnedResponsesStream({
    initial: [
      event({
        type: "response.created",
        sequence_number: 0,
        response: { id: "resp_warning_empty", model: "google/gemini" },
      }),
    ],
    iterator: iterator([completed]),
    responseId: "resp_warning_empty",
    warning: { model: "google/gemini" },
    onFailure: (_error, details) => {
      semanticCommitmentObserved = details.semanticCommitmentObserved;
      observedUpstreamTerminal = details.upstreamTerminal;
    },
  });
  const values = [...(await new Response(body).text()).matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  for (let attempt = 0; attempt < 10 && semanticCommitmentObserved === null; attempt += 1) {
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  assert.equal(values.some((value) => value.type === "response.output_text.delta"), true);
  assert.equal(values.some((value) => value.type === "response.completed"), false);
  assert.equal(values.at(-1)?.type, "error");
  assert.equal(values.at(-1)?.code, "empty_upstream_completion");
  assert.equal(semanticCommitmentObserved, false);
  assert.equal(observedUpstreamTerminal, completed);
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

Deno.test("Owned stream forwards a committed upstream error terminal", async () => {
  const upstreamError = event({
    type: "error",
    sequence_number: 2,
    code: "provider_stream_error",
    message: "Provider stopped generation.",
    param: null,
  });
  const body = createOwnedResponsesStream({
    initial: [
      event({
        type: "response.created",
        sequence_number: 0,
        response: { id: "resp_error", object: "response", status: "in_progress", output: [] },
      }),
      event({
        type: "response.output_text.delta",
        sequence_number: 1,
        response_id: "resp_error",
        item_id: "msg_error",
        output_index: 0,
        content_index: 0,
        delta: "partial",
      }),
    ],
    iterator: iterator([upstreamError]),
    responseId: "resp_error",
  });
  const values = [...(await new Response(body).text()).matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  assert.equal(values.at(-1)?.type, "error");
  assert.equal(values.at(-1)?.code, "provider_stream_error");
  assert.equal(values.at(-1)?.message, "Provider stopped generation.");
  assert.equal(values.filter((value) => value.type === "response.failed").length, 0);
});

Deno.test("Owned stream synthetic failure preserves template, text, tools, and sequence order", async () => {
  const responseTemplate = {
    id: "resp_broken",
    object: "response",
    created_at: 42,
    model: "google/gemini",
    service_tier: "default",
    usage: null,
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
  assert.equal("usage" in response, false);
  assert.equal(response.status, "failed");
  assert.deepEqual(response.error, {
    code: "server_error",
    message: "The upstream stream ended unexpectedly.",
  });
  assert.match(JSON.stringify(response.output), /kept text/);
  assert.match(JSON.stringify(response.output), /ctc_broken/);
  const output = response.output as Record<string, unknown>[];
  assert.equal(output.find((item) => item.id === "msg_broken")?.status, "incomplete");
  assert.equal(output.find((item) => item.id === "ctc_broken")?.status, "completed");
});

Deno.test("Owned stream synthesizes a Codex failure after semantic output without response.created", async () => {
  let observedFailure: {
    failureKind: string;
    responseCreatedObserved: boolean;
    semanticCommitmentObserved: boolean;
    syntheticTerminalType: string | null;
  } | null = null;
  const body = createOwnedResponsesStream({
    initial: [
      event({
        type: "response.output_text.delta",
        response_id: "resp_without_created",
        item_id: "msg_without_created",
        output_index: 0,
        delta: "partial",
      }),
    ],
    iterator: (async function* (): ResponsesStreamIterator {
      yield* [];
      throw new Error("provider socket reset");
    })(),
    responseId: null,
    onFailure: (_error, details) => {
      observedFailure = details;
    },
  });
  const values = [...(await new Response(body).text()).matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  assert.deepEqual(values.map((value) => value.type), ["response.output_text.delta", "response.failed"]);
  assert.equal(values.filter((value) => value.type === "error").length, 0);
  const terminal = values.at(-1)!;
  assert.equal((terminal.response as Record<string, unknown>).id, "resp_without_created");
  assert.deepEqual(observedFailure, {
    failureKind: "read_error",
    responseCreatedObserved: false,
    semanticCommitmentObserved: true,
    syntheticTerminalType: "response.failed",
    upstreamTerminal: null,
  });
});

Deno.test("Owned stream closes its iterator after a post-commit validation failure", async () => {
  let returned = false;
  const rest = (async function* (): ResponsesStreamIterator {
    try {
      yield event({
        type: "response.output_text.delta",
        response_id: "resp_validation",
        delta: "invalid",
      });
    } finally {
      returned = true;
    }
    return undefined;
  })();
  let validations = 0;
  const body = createOwnedResponsesStream({
    initial: [
      event({
        type: "response.created",
        response: { id: "resp_validation", object: "response", status: "in_progress", output: [] },
      }),
      event({ type: "response.output_text.delta", response_id: "resp_validation", delta: "kept" }),
    ],
    iterator: rest,
    responseId: "resp_validation",
    validateEvent: () => {
      validations += 1;
      if (validations > 2) throw new Error("invalid provider event");
    },
  });
  const text = await new Response(body).text();
  assert.match(text, /response.failed/);
  assert.equal(returned, true);
});

Deno.test("Owned stream preserves refusal text in synthetic failure output", async () => {
  const body = createOwnedResponsesStream({
    initial: [
      event({
        type: "response.created",
        response: { id: "resp_refusal", object: "response", status: "in_progress", output: [] },
      }),
      event({
        type: "response.refusal.done",
        response_id: "resp_refusal",
        item_id: "msg_refusal",
        output_index: 0,
        content_index: 0,
        refusal: "I cannot help with that.",
      }),
    ],
    iterator: iterator([]),
    responseId: "resp_refusal",
  });
  const values = [...(await new Response(body).text()).matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  assert.match(JSON.stringify(values.at(-1)?.response), /"type":"refusal"/);
  assert.match(JSON.stringify(values.at(-1)?.response), /I cannot help with that/);
});

Deno.test("Owned stream marks recovered text completed only after a done event", async () => {
  const responseTemplate = {
    id: "resp_done_text",
    object: "response",
    created_at: 43,
    model: "google/gemini",
    status: "in_progress",
    output: [],
  };
  const body = createOwnedResponsesStream({
    initial: [
      event({ type: "response.created", sequence_number: 0, response: responseTemplate }),
      event({
        type: "response.output_text.delta",
        sequence_number: 1,
        response_id: "resp_done_text",
        item_id: "msg_done_text",
        output_index: 0,
        content_index: 0,
        delta: "complete text",
      }),
      event({
        type: "response.output_text.done",
        sequence_number: 2,
        response_id: "resp_done_text",
        item_id: "msg_done_text",
        output_index: 0,
        content_index: 0,
        text: "complete text",
      }),
    ],
    iterator: iterator([]),
    responseId: "resp_done_text",
  });
  const values = [...(await new Response(body).text()).matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  const response = values.at(-1)?.response as Record<string, unknown>;
  const output = response.output as Record<string, unknown>[];
  assert.equal(output.find((item) => item.id === "msg_done_text")?.status, "completed");
});

Deno.test("Owned stream emits one synthetic failure after a post-commit inactivity timeout", async () => {
  let cancelCount = 0;
  let observedFailure: Record<string, unknown> | null = null;
  const encoder = new TextEncoder();
  const frame = (value: Record<string, unknown>): string =>
    "data: " + JSON.stringify(value) + String.fromCharCode(10, 10);
  const source = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(encoder.encode(
        frame({ type: "response.created", response: { id: "resp_stalled", status: "in_progress", output: [] } }) +
          frame({
            type: "response.output_text.delta",
            response_id: "resp_stalled",
            item_id: "msg_stalled",
            output_index: 0,
            delta: "partial",
          }),
      ));
    },
    cancel() {
      cancelCount += 1;
    },
  });
  const upstream = readResponsesStream(source, undefined, { firstEventTimeoutMs: 100, inactivityTimeoutMs: 12 });
  const first = await upstream.next();
  const second = await upstream.next();
  assert.ok(first.value);
  assert.ok(second.value);
  const firstEvent = first.value as ResponsesStreamEvent;
  const secondEvent = second.value as ResponsesStreamEvent;
  const body = createOwnedResponsesStream({
    initial: [firstEvent, secondEvent],
    iterator: upstream,
    responseId: "resp_stalled",
    onFailure: (_error, details) => {
      observedFailure = {
        failureKind: details.failureKind,
        semanticCommitmentObserved: details.semanticCommitmentObserved,
        syntheticTerminalType: details.syntheticTerminalType,
      };
    },
  });
  const output = await new Response(body).text();
  const values = output.split(String.fromCharCode(10)).filter((line) => line.startsWith("data: ")).map((line) =>
    JSON.parse(line.slice(6)) as Record<string, unknown>
  );
  assert.deepEqual(values.map((value) => value.type), [
    "response.created",
    "response.output_text.delta",
    "response.failed",
  ]);
  assert.equal(values.filter((value) => value.type === "response.failed").length, 1);
  assert.equal(values.filter((value) => value.delta === "partial").length, 1);
  const terminalResponse = values.at(-1)?.response as Record<string, unknown>;
  assert.equal(terminalResponse.status, "failed");
  assert.deepEqual(terminalResponse.error, {
    code: "server_error",
    message: "The upstream stream ended unexpectedly.",
  });
  assert.equal("usage" in terminalResponse, false);
  assert.deepEqual(observedFailure, {
    failureKind: "inactivity_timeout",
    semanticCommitmentObserved: true,
    syntheticTerminalType: "response.failed",
  });
  assert.equal(cancelCount, 1);
  assert.equal(source.locked, false);
});
