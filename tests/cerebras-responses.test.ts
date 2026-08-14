import assert from "node:assert/strict";
import {
  buildCerebrasResponsesTranslation,
  cerebrasResponseSse,
  chatCompletionToCerebrasResponse,
} from "../src/cerebras_responses.ts";

const expectOk = <T>(value: { ok: true; value: T } | { ok: false; message: string; param?: string }): T => {
  if (!value.ok) throw new Error(value.message);
  return value.value;
};

Deno.test("GPT-OSS Responses translation preserves text and tool turns", () => {
  const translated = buildCerebrasResponsesTranslation(
    [
      { type: "message", role: "system", content: [{ type: "input_text", text: "Follow policy." }] },
      { type: "message", role: "user", content: [{ type: "input_text", text: "look up status" }] },
      { type: "function_call", id: "fc_1", call_id: "call_1", name: "status", arguments: '{"id":"1"}' },
      { type: "function_call_output", call_id: "call_1", output: "ready" },
    ],
    "You are concise.",
    {
      tools: [{ type: "function", name: "status", description: "Read status", parameters: { type: "object" } }],
      tool_choice: { type: "function", name: "status" },
      parallel_tool_calls: false,
    },
  );
  const value = expectOk(translated);
  assert.deepEqual(value.messages, [
    { role: "developer", content: "You are concise." },
    { role: "developer", content: "Follow policy." },
    { role: "user", content: "look up status" },
    {
      role: "assistant",
      content: null,
      tool_calls: [{ id: "call_1", type: "function", function: { name: "status", arguments: '{"id":"1"}' } }],
    },
    { role: "tool", tool_call_id: "call_1", content: "ready" },
  ]);
  assert.deepEqual(value.tools, [{
    type: "function",
    function: { name: "status", description: "Read status", parameters: { type: "object" } },
  }]);
  assert.deepEqual(value.toolChoice, { type: "function", function: { name: "status" } });
});

Deno.test("GPT-OSS Responses translation rejects multimodal input and unsupported tools", () => {
  const image = buildCerebrasResponsesTranslation(
    [{ type: "message", role: "user", content: [{ type: "input_image", image_url: "data:image/png;base64,x" }] }],
    undefined,
    {},
  );
  assert.equal(image.ok, false);
  if (image.ok) throw new Error("expected image rejection");
  assert.equal(image.param, "input[0].content[0].type");

  const webSearch = buildCerebrasResponsesTranslation(
    [{ type: "message", role: "user", content: [{ type: "input_text", text: "search" }] }],
    undefined,
    { tools: [{ type: "web_search" }] },
  );
  assert.equal(webSearch.ok, false);
  if (webSearch.ok) throw new Error("expected web-search rejection");
  assert.equal(webSearch.param, "tools[0].type");

  const itemReference = buildCerebrasResponsesTranslation(
    [{ type: "item_reference", id: "item_1" }],
    undefined,
    {},
  );
  assert.equal(itemReference.ok, false);
  if (itemReference.ok) throw new Error("expected item-reference rejection");
  assert.equal(itemReference.param, "input[0].type");

  const nestedFunction = buildCerebrasResponsesTranslation(
    [{ type: "message", role: "user", content: [{ type: "input_text", text: "status" }] }],
    undefined,
    { tools: [{ type: "function", function: { name: "status", parameters: { type: "object" } } }] },
  );
  assert.equal(nestedFunction.ok, false);
  if (nestedFunction.ok) throw new Error("expected nested Chat tool rejection");
  assert.equal(nestedFunction.param, "tools[0].function");

  const nestedToolChoice = buildCerebrasResponsesTranslation(
    [{ type: "message", role: "user", content: [{ type: "input_text", text: "status" }] }],
    undefined,
    { tool_choice: { type: "function", function: { name: "status" } } },
  );
  assert.equal(nestedToolChoice.ok, false);
  if (nestedToolChoice.ok) throw new Error("expected nested Chat tool-choice rejection");
  assert.equal(nestedToolChoice.param, "tool_choice.function");
});

Deno.test("GPT-OSS Chat output becomes a Responses body and complete SSE", () => {
  const translationStartedAt = Math.floor(Date.now() / 1000);
  const translated = chatCompletionToCerebrasResponse(
    {
      id: "chatcmpl_fixture",
      created: 1_728_000_000,
      model: "cerebras/gpt-oss-120b",
      choices: [{
        index: 0,
        message: {
          role: "assistant",
          content: "Before the call",
          tool_calls: [{
            id: "call_fixture",
            type: "function",
            function: { name: "status", arguments: '{"ok":true}' },
          }],
        },
        finish_reason: "tool_calls",
      }],
      usage: { prompt_tokens: 3, completion_tokens: 4, total_tokens: 7 },
    },
    "cerebras/gpt-oss-120b",
    {
      instructions: "Be concise.",
      maxOutputTokens: 512,
      parallelToolCalls: false,
      reasoningEffort: "medium",
      temperature: 0.5,
      toolChoice: "required",
      tools: [{ type: "function", name: "status" }],
      topP: 0.8,
      metadata: { request: "fixture" },
    },
  );
  const value = expectOk(translated);
  assert.equal(value.created_at, 1_728_000_000);
  assert.equal(typeof value.completed_at, "number");
  assert.ok((value.completed_at as number) >= translationStartedAt);
  assert.equal(value.output_text, "Before the call");
  assert.deepEqual((value.output as Array<Record<string, unknown>>).map((item) => item.type), [
    "message",
    "function_call",
  ]);
  assert.deepEqual(value.usage, {
    input_tokens: 3,
    input_tokens_details: { cached_tokens: 0, cache_write_tokens: 0 },
    output_tokens: 4,
    output_tokens_details: { reasoning_tokens: 0 },
    total_tokens: 7,
  });
  assert.deepEqual(
    {
      error: value.error,
      instructions: value.instructions,
      max_output_tokens: value.max_output_tokens,
      parallel_tool_calls: value.parallel_tool_calls,
      previous_response_id: value.previous_response_id,
      reasoning: value.reasoning,
      store: value.store,
      temperature: value.temperature,
      text: value.text,
      tool_choice: value.tool_choice,
      tools: value.tools,
      top_p: value.top_p,
      truncation: value.truncation,
      metadata: value.metadata,
    },
    {
      error: null,
      instructions: "Be concise.",
      max_output_tokens: 512,
      parallel_tool_calls: false,
      previous_response_id: null,
      reasoning: { effort: "medium", summary: null },
      store: false,
      temperature: 0.5,
      text: { format: { type: "text" } },
      tool_choice: "required",
      tools: [{ type: "function", name: "status" }],
      top_p: 0.8,
      truncation: "disabled",
      metadata: { request: "fixture" },
    },
  );
  const stream = cerebrasResponseSse(value);
  const events = [...stream.matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  assert.deepEqual(events.map((event) => event.sequence_number), events.map((_, index) => index));
  assert.deepEqual(events.map((event) => event.type), [
    "response.created",
    "response.output_item.added",
    "response.content_part.added",
    "response.output_text.delta",
    "response.output_text.done",
    "response.content_part.done",
    "response.output_item.done",
    "response.output_item.added",
    "response.function_call_arguments.delta",
    "response.function_call_arguments.done",
    "response.output_item.done",
    "response.completed",
  ]);
  assert.deepEqual((events[0]?.response as Record<string, unknown>).output, []);
  assert.equal((events[0]?.response as Record<string, unknown>).completed_at, null);
  assert.equal((events[0]?.response as Record<string, unknown>).usage, null);
  assert.doesNotMatch(stream, /data: \[DONE\]/);
});

Deno.test("GPT-OSS length termination becomes an incomplete Responses terminal", () => {
  const translated = chatCompletionToCerebrasResponse(
    {
      id: "chatcmpl_truncated",
      created: 1_728_000_000,
      model: "cerebras/gpt-oss-120b",
      choices: [{
        index: 0,
        message: { role: "assistant", content: "Partial output" },
        finish_reason: "length",
      }],
      usage: { prompt_tokens: 3, completion_tokens: 4, total_tokens: 7 },
    },
    "cerebras/gpt-oss-120b",
  );
  const value = expectOk(translated);
  assert.equal(value.status, "incomplete");
  assert.deepEqual(value.incomplete_details, { reason: "max_output_tokens" });
  assert.equal((value.output as Array<Record<string, unknown>>)[0]?.status, "incomplete");
  const events = [...cerebrasResponseSse(value).matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  assert.equal(events.at(-1)?.type, "response.incomplete");
  assert.equal(events.some((event) => event.type === "response.completed"), false);
});

Deno.test("GPT-OSS content filtering becomes an incomplete Responses terminal", () => {
  const translated = chatCompletionToCerebrasResponse(
    {
      id: "chatcmpl_filtered",
      created: 1_728_000_000,
      model: "cerebras/gpt-oss-120b",
      choices: [{
        index: 0,
        message: { role: "assistant", content: "Filtered output" },
        finish_reason: "content_filter",
      }],
      usage: { prompt_tokens: 3, completion_tokens: 2, total_tokens: 5 },
    },
    "cerebras/gpt-oss-120b",
  );
  const value = expectOk(translated);
  assert.equal(value.status, "incomplete");
  assert.deepEqual(value.incomplete_details, { reason: "content_filter" });
  assert.equal((value.output as Array<Record<string, unknown>>)[0]?.status, "incomplete");
  const events = [...cerebrasResponseSse(value).matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  assert.equal(events.at(-1)?.type, "response.incomplete");
  assert.equal(events.some((event) => event.type === "response.completed"), false);
});
