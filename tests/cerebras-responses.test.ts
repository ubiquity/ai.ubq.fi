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
});

Deno.test("GPT-OSS Chat output becomes a Responses body and complete SSE", () => {
  const translated = chatCompletionToCerebrasResponse(
    {
      id: "chatcmpl_fixture",
      created: 1_728_000_000,
      model: "gpt-oss-120b",
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
    "gpt-oss-120b",
  );
  const value = expectOk(translated);
  assert.equal(value.output_text, "Before the call");
  assert.deepEqual((value.output as Array<Record<string, unknown>>).map((item) => item.type), [
    "message",
    "function_call",
  ]);
  assert.deepEqual(value.usage, { input_tokens: 3, output_tokens: 4, total_tokens: 7 });
  const stream = cerebrasResponseSse(value);
  const events = [...stream.matchAll(/^data: (.+)$/gm)]
    .map((match) => match[1] === "[DONE]" ? null : JSON.parse(match[1]!) as Record<string, unknown>)
    .filter((event): event is Record<string, unknown> => event !== null);
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
  assert.equal((events[0]?.response as Record<string, unknown>).usage, null);
  assert.match(stream, /data: \[DONE\]/);
});

Deno.test("GPT-OSS length termination becomes an incomplete Responses terminal", () => {
  const translated = chatCompletionToCerebrasResponse(
    {
      id: "chatcmpl_truncated",
      created: 1_728_000_000,
      model: "gpt-oss-120b",
      choices: [{
        index: 0,
        message: { role: "assistant", content: "Partial output" },
        finish_reason: "length",
      }],
      usage: { prompt_tokens: 3, completion_tokens: 4, total_tokens: 7 },
    },
    "gpt-oss-120b",
  );
  const value = expectOk(translated);
  assert.equal(value.status, "incomplete");
  assert.deepEqual(value.incomplete_details, { reason: "max_output_tokens" });
  assert.equal((value.output as Array<Record<string, unknown>>)[0]?.status, "incomplete");
  const events = [...cerebrasResponseSse(value).matchAll(/^data: (.+)$/gm)]
    .map((match) => match[1] === "[DONE]" ? null : JSON.parse(match[1]!) as Record<string, unknown>)
    .filter((event): event is Record<string, unknown> => event !== null);
  assert.equal(events.at(-1)?.type, "response.incomplete");
  assert.equal(events.some((event) => event.type === "response.completed"), false);
});
