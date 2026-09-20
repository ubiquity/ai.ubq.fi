import assert from "node:assert/strict";

import {
  createDeepSeekResponsesStreamTranslator,
  encodeResponsesEvent,
  type DeepSeekResponsesEcho,
  toDeepSeekChatMessages,
  toDeepSeekResponsesChatBody,
  toDeepSeekResponsesPayload,
  toResponsesUsage,
} from "../src/deepseek_responses.ts";

const echo: DeepSeekResponsesEcho = { tools: undefined, tool_choice: undefined, parallel_tool_calls: true, instructions: null };

const chatCompletion = (message: Record<string, unknown>, usage: Record<string, unknown> = { prompt_tokens: 10, completion_tokens: 4, total_tokens: 14 }) => ({
  id: "chatcmpl-1",
  object: "chat.completion",
  created: 1_780_000_000,
  model: "deepseek-flash",
  choices: [{ index: 0, message, finish_reason: "stop" }],
  usage,
});

const chatChunk = (delta: Record<string, unknown>, extra: Record<string, unknown> = {}): Record<string, unknown> => ({
  id: "chatcmpl-stream",
  object: "chat.completion.chunk",
  created: 1_780_000_001,
  model: "deepseek-flash",
  choices: [{ index: 0, delta, finish_reason: null, ...extra }],
});

const eventTypes = (events: readonly Record<string, unknown>[]): string[] => events.map((event) => String(event.type));

Deno.test("deepseek responses: maps Responses input onto Chat messages", () => {
  const result = toDeepSeekChatMessages(
    [
      { type: "message", role: "user", content: [{ type: "input_text", text: "hello" }] },
      { type: "message", role: "assistant", content: [{ type: "output_text", text: "hi" }] },
      { type: "message", role: "developer", content: [{ type: "input_text", text: "be terse" }] },
      { type: "reasoning", summary: [] },
      { type: "function_call", name: "get_date", arguments: "{}", call_id: "call_1" },
      { type: "function_call", name: "get_weather", arguments: '{"city":"x"}', call_id: "call_2" },
      { type: "function_call_output", call_id: "call_1", output: "2026-09-16" },
      { type: "function_call_output", call_id: "call_2", output: { temp: 7 } },
    ],
    "system prompt"
  );
  assert.equal(result.ok, true);
  assert.deepEqual(result.value, [
    { role: "system", content: "system prompt" },
    { role: "user", content: "hello" },
    { role: "assistant", content: "hi" },
    // DeepSeek, like OpenAI Chat, has no developer role.
    { role: "system", content: "be terse" },
    // Consecutive calls collapse into one assistant turn.
    {
      role: "assistant",
      content: null,
      tool_calls: [
        { id: "call_1", type: "function", function: { name: "get_date", arguments: "{}" } },
        { id: "call_2", type: "function", function: { name: "get_weather", arguments: '{"city":"x"}' } },
      ],
    },
    { role: "tool", tool_call_id: "call_1", content: "2026-09-16" },
    { role: "tool", tool_call_id: "call_2", content: '{"temp":7}' },
  ]);
});

Deno.test("deepseek responses: rejects input shapes it cannot translate", () => {
  const badRole = toDeepSeekChatMessages([{ type: "message", role: "system", content: "x" }], null);
  assert.equal(badRole.ok, false);
  const badType = toDeepSeekChatMessages([{ type: "computer_call", call_id: "c" }], null);
  assert.equal(badType.ok, false);
  const badContent = toDeepSeekChatMessages([{ type: "message", role: "user", content: [{ type: "input_audio" }] }], null);
  assert.equal(badContent.ok, false);
});

Deno.test("deepseek responses: flattens namespaced tools and drops what the API cannot serve", () => {
  const result = toDeepSeekResponsesChatBody(
    {
      input: "hi",
      max_output_tokens: 256,
      reasoning: { effort: "ultra" },
      text: { format: { type: "json_object" } },
      tool_choice: { type: "function", name: "now" },
      parallel_tool_calls: false,
      tools: [
        { type: "web_search" },
        { type: "function", name: "now", description: "top level", parameters: { type: "object" } },
        {
          type: "namespace",
          name: "clock",
          tools: [
            { type: "function", name: "now", description: "current time", parameters: { type: "object" }, strict: false },
            { type: "function", name: "sleep", parameters: { type: "object" } },
          ],
        },
      ],
    },
    "deepseek-v4-flash",
    false
  );
  assert.equal(result.ok, true);
  const { body, toolNames } = result.value;
  assert.equal(body.model, "deepseek-flash");
  assert.equal(body.max_tokens, 256);
  // The Codex `ultra` preset is the documented `max` tier upstream.
  assert.equal(body.reasoning_effort, "max");
  assert.deepEqual(body.response_format, { type: "json_object" });
  assert.equal(body.parallel_tool_calls, false);
  assert.deepEqual(body.messages, [{ role: "user", content: "hi" }]);
  const tools = body.tools as { function: { name: string } }[];
  assert.deepEqual(
    tools.map((tool) => tool.function.name),
    ["now", "clock_now", "sleep"]
  );
  // A disambiguated name maps back to the name the client asked for.
  assert.equal(toolNames.get("clock_now"), "now");
  assert.deepEqual(body.tool_choice, { type: "function", function: { name: "now" } });
});

Deno.test("deepseek responses: replays reasoning on tool turns because the provider requires it", () => {
  // A tool-bearing request whose historical assistant turn omits
  // `reasoning_content` is rejected by the provider with HTTP 400:
  // "The `reasoning_content` in the thinking mode must be passed back to the API."
  const withoutReasoning = toDeepSeekResponsesChatBody(
    {
      input: [
        { type: "message", role: "user", content: [{ type: "input_text", text: "run echo hi" }] },
        { type: "function_call", name: "shell", arguments: '{"cmd":"echo hi"}', call_id: "call_1" },
        { type: "function_call_output", call_id: "call_1", output: "hi" },
      ],
      tools: [{ type: "function", name: "shell", parameters: { type: "object" } }],
    },
    "deepseek-flash",
    false
  );
  assert.equal(withoutReasoning.ok, true);
  assert.deepEqual(withoutReasoning.value.body.messages, [
    { role: "user", content: "run echo hi" },
    {
      role: "assistant",
      content: null,
      reasoning_content: "",
      tool_calls: [{ id: "call_1", type: "function", function: { name: "shell", arguments: '{"cmd":"echo hi"}' } }],
    },
    { role: "tool", tool_call_id: "call_1", content: "hi" },
  ]);
});

Deno.test("deepseek responses: fills reasoning on every assistant turn after the last user message", () => {
  // Reproduces the production failure: Codex echoes the assistant's text
  // message ahead of its `function_call`, so continuing a tool call sends a
  // plain assistant message in the tail. The provider rejected that request
  // with HTTP 400 until every trailing assistant turn carried the field.
  const body = toDeepSeekResponsesChatBody(
    {
      input: [
        { type: "message", role: "user", content: [{ type: "input_text", text: "list files" }] },
        { type: "message", role: "assistant", content: [{ type: "output_text", text: "Let me look." }] },
        { type: "function_call", name: "shell", arguments: '{"cmd":"ls"}', call_id: "call_1" },
        { type: "function_call_output", call_id: "call_1", output: "a.ts" },
      ],
      tools: [{ type: "function", name: "shell", parameters: { type: "object" } }],
    },
    "deepseek-flash",
    false
  );
  assert.equal(body.ok, true);
  const messages = body.value.body.messages as Record<string, unknown>[];
  assert.deepEqual(messages, [
    { role: "user", content: "list files" },
    { role: "assistant", content: "Let me look.", reasoning_content: "" },
    {
      role: "assistant",
      content: null,
      reasoning_content: "",
      tool_calls: [{ id: "call_1", type: "function", function: { name: "shell", arguments: '{"cmd":"ls"}' } }],
    },
    { role: "tool", tool_call_id: "call_1", content: "a.ts" },
  ]);
});

Deno.test("deepseek responses: an assistant turn before the last user message keeps its reasoning absent", () => {
  // The measured provider rule exempts history before the last user message, so
  // the fill must not rewrite replayed turns the provider never validated.
  const body = toDeepSeekResponsesChatBody(
    {
      input: [
        { type: "message", role: "user", content: "list files" },
        { type: "message", role: "assistant", content: "Let me look." },
        { type: "message", role: "user", content: "now summarize" },
      ],
      tools: [{ type: "function", name: "shell", parameters: { type: "object" } }],
    },
    "deepseek-flash",
    false
  );
  assert.equal(body.ok, true);
  const messages = body.value.body.messages as Record<string, unknown>[];
  assert.equal("reasoning_content" in messages[1], false);
});

Deno.test("deepseek responses: carries the client's echoed reasoning onto its assistant turn", () => {
  const withReasoning = toDeepSeekResponsesChatBody(
    {
      input: [
        { type: "message", role: "user", content: [{ type: "input_text", text: "run echo hi" }] },
        {
          type: "reasoning",
          id: "rs_1",
          summary: [
            { type: "summary_text", text: "I should call " },
            { type: "summary_text", text: "the shell tool." },
          ],
        },
        { type: "function_call", name: "shell", arguments: '{"cmd":"echo hi"}', call_id: "call_1" },
        { type: "function_call_output", call_id: "call_1", output: "hi" },
      ],
      tools: [{ type: "function", name: "shell", parameters: { type: "object" } }],
    },
    "deepseek-flash",
    false
  );
  assert.equal(withReasoning.ok, true);
  const messages = withReasoning.value.body.messages as Record<string, unknown>[];
  assert.equal(messages[1].reasoning_content, "I should call the shell tool.");
  assert.deepEqual(messages[1].tool_calls, [{ id: "call_1", type: "function", function: { name: "shell", arguments: '{"cmd":"echo hi"}' } }]);
});

Deno.test("deepseek responses: a reasoning item also rides a plain assistant message", () => {
  const withMessage = toDeepSeekResponsesChatBody(
    {
      input: [
        { type: "message", role: "user", content: "hi" },
        { type: "reasoning", content: [{ type: "reasoning_text", text: "Because." }] },
        { type: "message", role: "assistant", content: [{ type: "output_text", text: "hello" }] },
        { type: "message", role: "user", content: "again" },
      ],
    },
    "deepseek-flash",
    false
  );
  assert.equal(withMessage.ok, true);
  const messages = withMessage.value.body.messages as Record<string, unknown>[];
  assert.equal(messages[1].reasoning_content, "Because.");
});

Deno.test("deepseek responses: reasoning is never invented without tools", () => {
  const withoutTools = toDeepSeekResponsesChatBody(
    {
      input: [
        { type: "message", role: "user", content: "hi" },
        { type: "function_call", name: "shell", arguments: "{}", call_id: "call_1" },
        { type: "function_call_output", call_id: "call_1", output: "hi" },
      ],
    },
    "deepseek-flash",
    false
  );
  assert.equal(withoutTools.ok, true);
  const messages = withoutTools.value.body.messages as Record<string, unknown>[];
  assert.equal("reasoning_content" in messages[1], false);
});

Deno.test("deepseek responses: freeform tools round-trip through the function-only provider contract", () => {
  // Codex's freeform tools (`apply_patch`, code-mode `exec`) have no Chat
  // Completions shape, so they are advertised as one freeform string parameter
  // and their calls come back as `custom_tool_call` items.
  const body = toDeepSeekResponsesChatBody(
    {
      instructions: "You are a coding agent.",
      input: [
        { type: "message", role: "user", content: [{ type: "input_text", text: "patch it" }] },
        { type: "custom_tool_call", call_id: "call_1", name: "apply_patch", input: "*** Begin Patch\n*** End Patch" },
        { type: "custom_tool_call_output", call_id: "call_1", output: "Done!" },
      ],
      tools: [{ type: "custom", name: "apply_patch", description: "Apply a patch." }],
      reasoning: { effort: "max" },
    },
    "deepseek-v4-flash",
    false
  );
  assert.equal(body.ok, true);
  const tools = body.value.body.tools as { type: string; function: { name: string; parameters: unknown } }[];
  assert.deepEqual(tools, [
    {
      type: "function",
      function: {
        name: "apply_patch",
        description: "Apply a patch.",
        parameters: {
          type: "object",
          properties: { input: { type: "string", description: "Freeform input for the tool." } },
          required: ["input"],
          additionalProperties: false,
        },
      },
    },
  ]);
  // The replayed freeform text travels as the single Chat argument.
  const messages = body.value.body.messages as Record<string, unknown>[];
  assert.equal(messages[0].role, "system");
  assert.deepEqual(messages[2], {
    role: "assistant",
    content: null,
    reasoning_content: "",
    tool_calls: [{ id: "call_1", type: "function", function: { name: "apply_patch", arguments: '{"input":"*** Begin Patch\\n*** End Patch"}' } }],
  });
  assert.deepEqual(messages[3], { role: "tool", tool_call_id: "call_1", content: "Done!" });
  assert.deepEqual([...body.value.customToolNames], ["apply_patch"]);

  // The provider answers with JSON arguments; the client gets its text back.
  const payload = toDeepSeekResponsesPayload(
    chatCompletion({
      content: "",
      tool_calls: [{ id: "call_1", type: "function", function: { name: "apply_patch", arguments: '{"input":"*** Begin Patch\\n*** End Patch"}' } }],
    }),
    "deepseek-v4-flash",
    "resp_1",
    echo,
    body.value.toolNames,
    body.value.customToolNames
  );
  assert.deepEqual((payload.output as Record<string, unknown>[])[0], {
    id: "resp_1_ctc_0_0",
    type: "custom_tool_call",
    status: "completed",
    call_id: "call_1",
    name: "apply_patch",
    input: "*** Begin Patch\n*** End Patch",
  });
});

Deno.test("deepseek responses: a streamed freeform call emits custom tool events", () => {
  const body = toDeepSeekResponsesChatBody(
    {
      input: [{ type: "message", role: "user", content: [{ type: "input_text", text: "run code" }] }],
      tools: [{ type: "custom", name: "exec", description: "Run code." }],
    },
    "deepseek-v4-flash",
    true
  );
  assert.equal(body.ok, true);
  const translator = createDeepSeekResponsesStreamTranslator(
    "deepseek-v4-flash",
    "resp_1",
    echo,
    1_780_000_000,
    body.value.toolNames,
    body.value.customToolNames
  );
  translator.open();
  const events = [
    ...translator.push({
      choices: [{ index: 0, delta: { tool_calls: [{ index: 0, id: "call_1", function: { name: "exec", arguments: '{"input":"text(' } }] } }],
    }),
    ...translator.push({ choices: [{ index: 0, delta: { tool_calls: [{ index: 0, function: { arguments: 'hi);"}' } }] } }] }),
    ...translator.finish(),
  ];
  const added = events.find((event) => event.type === "response.output_item.added") as { item: Record<string, unknown> };
  assert.equal(added.item.type, "custom_tool_call");
  assert.equal(added.item.input, "");
  // No function-argument deltas: a freeform call carries its input on the item.
  assert.equal(
    events.some((event) => event.type === "response.function_call_arguments.delta"),
    false
  );
  const deltas = events.filter((event) => event.type === "response.custom_tool_call_input.delta");
  assert.equal(deltas.length, 1);
  assert.equal(deltas[0].delta, "text(hi);");
  const completed = events.filter((event) => event.type === "response.completed");
  assert.equal(completed.length, 1);
  assert.deepEqual((completed[0].response as { output: Record<string, unknown>[] }).output, [
    { id: "resp_1_fc_0", type: "custom_tool_call", status: "completed", call_id: "call_1", name: "exec", input: "text(hi);" },
  ]);
});

Deno.test("deepseek responses: rejects unsupported wire requests instead of approximating them", () => {
  const schema = toDeepSeekResponsesChatBody({ input: "hi", text: { format: { type: "json_schema", name: "x" } } }, "deepseek-flash", false);
  assert.equal(schema.ok, false);
  assert.equal(schema.param, "text.format.type");
  const tokens = toDeepSeekResponsesChatBody({ input: "hi", max_output_tokens: 0 }, "deepseek-flash", false);
  assert.equal(tokens.ok, false);
  assert.equal(tokens.param, "max_output_tokens");
  const model = toDeepSeekResponsesChatBody({ input: "hi" }, "gpt-5.6-sol", false);
  assert.equal(model.ok, false);
  assert.equal(model.param, "model");
  const empty = toDeepSeekResponsesChatBody({ input: [] }, "deepseek-flash", false);
  assert.equal(empty.ok, false);
});

Deno.test("deepseek responses: streaming requests ask the provider for usage", () => {
  const result = toDeepSeekResponsesChatBody({ input: "hi" }, "deepseek-flash", true);
  assert.equal(result.ok, true);
  assert.equal(result.value.body.stream, true);
  assert.deepEqual(result.value.body.stream_options, { include_usage: true });
});

Deno.test("deepseek responses: builds a completed Responses object from a Chat completion", () => {
  const payload = toDeepSeekResponsesPayload(
    chatCompletion({
      role: "assistant",
      content: "42",
      reasoning_content: "because",
      tool_calls: [{ id: "call_9", type: "function", function: { name: "clock_now", arguments: "{}" } }],
    }),
    "deepseek-v4-flash",
    "resp_test",
    echo,
    new Map([["clock_now", "now"]])
  );
  assert.equal(payload.object, "response");
  assert.equal(payload.status, "completed");
  assert.equal(payload.model, "deepseek-v4-flash");
  const output = payload.output as Record<string, unknown>[];
  assert.deepEqual(
    output.map((item) => item.type),
    ["reasoning", "message", "function_call"]
  );
  assert.deepEqual(output[1].content, [{ type: "output_text", text: "42", annotations: [] }]);
  assert.equal(output[2].name, "now");
  assert.equal(output[2].call_id, "call_9");
  assert.deepEqual(payload.usage, {
    input_tokens: 10,
    output_tokens: 4,
    total_tokens: 14,
  });
});

Deno.test("deepseek responses: usage translation reports measured counters and never invents an unmeasured one", () => {
  assert.equal(toResponsesUsage({ prompt_tokens: 1 }), null);
  assert.equal(toResponsesUsage(null), null);
  // An upstream that reports no counter leaves the detail object absent: a
  // missing measurement must not be published as a measured zero.
  assert.deepEqual(toResponsesUsage({ prompt_tokens: 1, completion_tokens: 2, total_tokens: 3 }), {
    input_tokens: 1,
    output_tokens: 2,
    total_tokens: 3,
  });
  // The provider's own spelling and the official nested spelling both map onto
  // the field Codex reads.
  assert.deepEqual(toResponsesUsage({ prompt_tokens: 100, completion_tokens: 2, total_tokens: 102, prompt_cache_hit_tokens: 96 }), {
    input_tokens: 100,
    input_tokens_details: { cached_tokens: 96 },
    output_tokens: 2,
    total_tokens: 102,
  });
  assert.deepEqual(toResponsesUsage({ prompt_tokens: 100, completion_tokens: 2, total_tokens: 102, prompt_tokens_details: { cached_tokens: 96 } }), {
    input_tokens: 100,
    input_tokens_details: { cached_tokens: 96 },
    output_tokens: 2,
    total_tokens: 102,
  });
  // Thinking mode's reasoning count is a second measured counter on the same
  // response, so both details travel together when the provider reports both.
  assert.deepEqual(
    toResponsesUsage({
      prompt_tokens: 100,
      completion_tokens: 40,
      total_tokens: 140,
      prompt_cache_hit_tokens: 96,
      completion_tokens_details: { reasoning_tokens: 31 },
    }),
    {
      input_tokens: 100,
      input_tokens_details: { cached_tokens: 96 },
      output_tokens: 40,
      output_tokens_details: { reasoning_tokens: 31 },
      total_tokens: 140,
    }
  );
  // A counter cannot exceed the total it is a subset of, so an impossible value
  // is reported as unknown rather than relayed.
  assert.deepEqual(toResponsesUsage({ prompt_tokens: 10, completion_tokens: 2, total_tokens: 12, prompt_cache_hit_tokens: 11 }), {
    input_tokens: 10,
    output_tokens: 2,
    total_tokens: 12,
  });
  assert.deepEqual(toResponsesUsage({ prompt_tokens: 10, completion_tokens: 2, total_tokens: 12, completion_tokens_details: { reasoning_tokens: 3 } }), {
    input_tokens: 10,
    output_tokens: 2,
    total_tokens: 12,
  });
});

Deno.test("deepseek responses: stream translator emits the Responses event sequence", () => {
  const translator = createDeepSeekResponsesStreamTranslator("deepseek-flash", "resp_stream", echo, 1_780_000_000);
  const events: Record<string, unknown>[] = [];
  events.push(...translator.push(chatChunk({ role: "assistant", reasoning_content: "think " })));
  events.push(...translator.push(chatChunk({ content: "Hel" })));
  events.push(...translator.push(chatChunk({ content: "lo" }, { finish_reason: "stop" })));
  events.push(
    ...translator.push({
      ...chatChunk({}),
      usage: {
        prompt_tokens: 7,
        completion_tokens: 3,
        total_tokens: 10,
        prompt_cache_hit_tokens: 6,
        completion_tokens_details: { reasoning_tokens: 2 },
      },
    })
  );
  events.push(...translator.finish());

  assert.deepEqual(eventTypes(events), [
    "response.created",
    "response.in_progress",
    "response.output_item.added",
    "response.content_part.added",
    "response.output_text.delta",
    "response.output_text.delta",
    "response.output_text.done",
    "response.content_part.done",
    "response.output_item.done",
    "response.completed",
  ]);
  const completed = events.at(-1) as { response: Record<string, unknown> };
  assert.equal(completed.response.status, "completed");
  assert.deepEqual(completed.response.usage, {
    input_tokens: 7,
    input_tokens_details: { cached_tokens: 6 },
    output_tokens: 3,
    output_tokens_details: { reasoning_tokens: 2 },
    total_tokens: 10,
  });
  const output = completed.response.output as Record<string, unknown>[];
  assert.deepEqual(
    output.map((item) => item.type),
    ["reasoning", "message"]
  );
});

Deno.test("deepseek responses: stream translator accumulates fragmented tool calls", () => {
  const translator = createDeepSeekResponsesStreamTranslator("deepseek-flash", "resp_tools", echo, 1_780_000_000);
  const events: Record<string, unknown>[] = [];
  events.push(
    ...translator.push(chatChunk({ role: "assistant", tool_calls: [{ index: 0, id: "call_1", type: "function", function: { name: "shell", arguments: "" } }] }))
  );
  events.push(...translator.push(chatChunk({ tool_calls: [{ index: 0, function: { arguments: '{"cmd":' } }] })));
  events.push(...translator.push(chatChunk({ tool_calls: [{ index: 0, function: { arguments: '"ls"}' } }] }, { finish_reason: "tool_calls" })));
  events.push(...translator.finish());

  assert.deepEqual(eventTypes(events), [
    "response.created",
    "response.in_progress",
    "response.output_item.added",
    "response.function_call_arguments.delta",
    "response.function_call_arguments.delta",
    "response.function_call_arguments.done",
    "response.output_item.done",
    "response.completed",
  ]);
  const done = events.find((event) => event.type === "response.output_item.done") as { item: Record<string, unknown> };
  assert.deepEqual(done.item, {
    id: "resp_tools_fc_0",
    type: "function_call",
    status: "completed",
    call_id: "call_1",
    name: "shell",
    arguments: '{"cmd":"ls"}',
  });
});

Deno.test("deepseek responses: stream translator is idempotent at the terminal", () => {
  const translator = createDeepSeekResponsesStreamTranslator("deepseek-flash", "resp_once", echo, 1_780_000_000);
  assert.deepEqual(eventTypes(translator.finish()), ["response.created", "response.in_progress", "response.completed"]);
  assert.deepEqual(translator.finish(), []);
});

Deno.test("deepseek responses: encodes named SSE frames", () => {
  assert.equal(
    encodeResponsesEvent({ type: "response.completed", response: { id: "resp_1" } }),
    'event: response.completed\ndata: {"type":"response.completed","response":{"id":"resp_1"}}\n\n'
  );
});
