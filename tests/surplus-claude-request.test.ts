import { test } from "bun:test";
import assert from "node:assert/strict";
import { buildSurplusClaudeRequest } from "../src/provider/surplus-claude-request.ts";
import { buildClaudeCodeRequest } from "../src/provider/claude-code.ts";

const base = (): Record<string, unknown> => ({ model: "claude-opus-5.5", input: "hello", max_output_tokens: 128, stream: true, store: false });

test("Surplus Claude native request keeps instructions and caller input unchanged", () => {
  const body = {
    ...base(),
    instructions: "global instructions",
    input: [
      { type: "message", role: "developer", content: [{ type: "input_text", text: "developer instructions" }] },
      { type: "message", role: "user", content: [{ type: "input_text", text: "question" }] },
    ],
    prompt_cache_key: "caller-affinity-key",
  };
  const before = JSON.stringify(body);
  const result = buildSurplusClaudeRequest(body);
  assert.ok(result);
  assert.deepEqual(result.body, {
    model: "claude-opus-5.5",
    max_tokens: 128,
    messages: [{ role: "user", content: [{ type: "text", text: "question", cache_control: { type: "ephemeral" } }] }],
    stream: true,
    system: [
      { type: "text", text: "global instructions" },
      { type: "text", text: "developer instructions" },
    ],
    prompt_cache_key: "caller-affinity-key",
  });
  assert.ok(result);
  assert.equal(result.promptCacheMode, "implicit");
  assert.equal(result.explicitBreakpointCount, 0);
  assert.equal(JSON.stringify(body), before);
});

test("Surplus Claude replay preserves function names, call IDs, arguments, result and order", () => {
  const result = buildSurplusClaudeRequest({
    ...base(),
    input: [
      { type: "message", role: "user", content: "Read the file" },
      { type: "message", role: "assistant", content: [{ type: "output_text", text: "Reading" }] },
      { type: "function_call", call_id: "call-1", name: "read_file", arguments: '{"path":"a.txt","lines":3}' },
      { type: "function_call_output", call_id: "call-1", output: "file contents" },
    ],
    tools: [{ type: "function", name: "read_file", description: "Read a file", parameters: { type: "object", properties: { path: { type: "string" } } } }],
    parallel_tool_calls: false,
  });
  assert.ok(result);
  assert.deepEqual(result.body.tools, [
    { name: "read_file", description: "Read a file", input_schema: { type: "object", properties: { path: { type: "string" } } } },
  ]);
  assert.ok(result);
  assert.deepEqual(result.body.messages, [
    { role: "user", content: [{ type: "text", text: "Read the file" }] },
    {
      role: "assistant",
      content: [
        { type: "text", text: "Reading" },
        { type: "tool_use", id: "call-1", name: "read_file", input: { path: "a.txt", lines: 3 } },
      ],
    },
    { role: "user", content: [{ type: "tool_result", tool_use_id: "call-1", content: "file contents", cache_control: { type: "ephemeral" } }] },
  ]);
  assert.deepEqual(result.body.tool_choice, { type: "auto", disable_parallel_tool_use: true });
});

test("Surplus Claude flattens namespace tools and omits disabled hosted web search", () => {
  const result = buildSurplusClaudeRequest({
    ...base(),
    input: [
      { role: "user", content: "What time is it?" },
      { type: "function_call", name: "curr_time", namespace: "clock", call_id: "call-1", arguments: "{}" },
      { type: "function_call_output", call_id: "call-1", output: "2026-11-04T22:00:00Z" },
    ],
    tools: [
      {
        type: "namespace",
        name: "clock",
        description: "Tools for reading time.",
        tools: [
          { type: "function", name: "curr_time", description: "Current time", parameters: { type: "object", properties: {}, additionalProperties: false } },
        ],
      },
      { type: "function", name: "read_file", parameters: { type: "object", properties: { path: { type: "string" } } } },
      { type: "web_search", external_web_access: false, user_location: { type: "approximate", country: "US" } },
    ],
    tool_choice: { type: "function", name: "curr_time", namespace: "clock" },
    parallel_tool_calls: false,
  });
  assert.ok(result);
  assert.deepEqual(
    (result.body.tools as { name: string }[]).map((tool) => tool.name),
    ["clock_curr_time", "read_file"]
  );
  assert.deepEqual(result.toolNames.get("clock_curr_time"), { name: "curr_time", namespace: "clock" });
  assert.deepEqual(result.toolNames.get("read_file"), { name: "read_file", namespace: null });
  assert.deepEqual(result.body.tool_choice, { type: "tool", name: "clock_curr_time", disable_parallel_tool_use: true });
  const messages = result.body.messages as { content: Record<string, unknown>[] }[];
  assert.equal(messages[1].content[0].name, "clock_curr_time");
});

test("Surplus Claude preserves late developer messages in the native system channel", () => {
  const result = buildSurplusClaudeRequest({
    ...base(),
    input: [
      { role: "developer", content: "first instruction" },
      { role: "user", content: "question" },
      { role: "developer", content: "later reminder" },
    ],
  });
  assert.ok(result);
  assert.deepEqual(result.body.system, [
    { type: "text", text: "first instruction" },
    { type: "text", text: "later reminder" },
  ]);
  assert.deepEqual(result.body.messages, [{ role: "user", content: [{ type: "text", text: "question", cache_control: { type: "ephemeral" } }] }]);
});

test("Surplus Claude images retain base64 media type and remote URL without fetching", () => {
  const result = buildSurplusClaudeRequest({
    ...base(),
    input: [
      {
        role: "user",
        content: [
          { type: "input_image", image_url: "data:image/png;base64,aGVsbG8=" },
          { type: "input_image", image_url: "https://example.test/a.png" },
        ],
      },
    ],
  });
  assert.ok(result);
  assert.deepEqual(result.body.messages, [
    {
      role: "user",
      content: [
        { type: "image", source: { type: "base64", media_type: "image/png", data: "aGVsbG8=" } },
        { type: "image", source: { type: "url", url: "https://example.test/a.png" }, cache_control: { type: "ephemeral" } },
      ],
    },
  ]);
});

test("Surplus Claude explicit block policies suppress automatic markers and preserve lifetime", () => {
  const result = buildSurplusClaudeRequest({
    ...base(),
    input: [
      { role: "user", content: [{ type: "input_text", text: "stable", cache_control: { type: "ephemeral", ttl: "1h" } }] },
      { role: "user", content: [{ type: "input_text", text: "changing" }] },
    ],
  });
  assert.ok(result);
  assert.equal(result.promptCacheMode, "explicit");
  assert.equal(result.explicitBreakpointCount, 1);
  assert.ok(result);
  assert.deepEqual(result.body.messages, [
    {
      role: "user",
      content: [
        { type: "text", text: "stable", cache_control: { type: "ephemeral", ttl: "1h" } },
        { type: "text", text: "changing" },
      ],
    },
  ]);
});

test("Surplus Claude translates an explicit Responses breakpoint in a tool result", () => {
  const result = buildSurplusClaudeRequest({
    ...base(),
    input: [
      { role: "user", content: "run" },
      { type: "function_call", name: "run", call_id: "c", arguments: "{}" },
      { type: "function_call_output", call_id: "c", output: [{ type: "input_text", text: "result", prompt_cache_breakpoint: { mode: "explicit" } }] },
    ],
  });
  assert.ok(result);
  assert.equal(result.promptCacheMode, "explicit");
  assert.equal(result.explicitBreakpointCount, 1);
  const messages = result.body.messages as { content: Record<string, unknown>[] }[];
  assert.deepEqual(messages[2].content, [
    { type: "tool_result", tool_use_id: "c", content: [{ type: "text", text: "result", cache_control: { type: "ephemeral" } }] },
  ]);
});

test("Surplus Claude keeps explicit cache breakpoints on tool call and result blocks", () => {
  const result = buildSurplusClaudeRequest({
    ...base(),
    input: [
      { role: "user", content: "run" },
      { type: "function_call", name: "run", call_id: "c", arguments: "{}", prompt_cache_breakpoint: { mode: "explicit" } },
      { type: "function_call_output", call_id: "c", output: "result", cache_control: { type: "ephemeral", ttl: "1h" } },
    ],
  });
  assert.ok(result);
  assert.equal(result.promptCacheMode, "explicit");
  assert.equal(result.explicitBreakpointCount, 2);
  const messages = result.body.messages as { content: Record<string, unknown>[] }[];
  assert.deepEqual(messages[1].content[0].cache_control, { type: "ephemeral" });
  assert.deepEqual(messages[2].content[0].cache_control, { type: "ephemeral", ttl: "1h" });
});

test("Surplus Claude explicit mode with no breakpoints stays without automatic caching", () => {
  const result = buildSurplusClaudeRequest({ ...base(), prompt_cache_options: { mode: "explicit" } });
  assert.ok(result);
  assert.equal(result.promptCacheMode, "explicit");
  assert.ok(result);
  assert.deepEqual(result.body.messages, [{ role: "user", content: [{ type: "text", text: "hello" }] }]);
});

test("Surplus Claude reasoning preserves requested effort and the Codex ultra translation", () => {
  assert.equal(buildSurplusClaudeRequest({ ...base(), reasoning: { effort: "max" } }), null);
  for (const [requested, expected] of [
    ["low", "low"],
    ["medium", "medium"],
    ["high", "high"],
    ["xhigh", "xhigh"],
    ["max", "max"],
    ["ultra", "max"],
  ]) {
    const result = buildSurplusClaudeRequest({ ...base(), reasoning: { effort: requested, summary: "none" } }, undefined, {
      supportsNativeReasoningEffort: true,
    });
    assert.ok(result);
    assert.deepEqual(result.body.thinking, { type: "adaptive" });
    assert.deepEqual(result.body.output_config, { effort: expected });
  }
});

test("Surplus Claude carries JSON-schema structured output with adaptive reasoning", () => {
  const schema = {
    type: "object",
    properties: { title: { type: "string", minLength: 1, maxLength: 36 } },
    required: ["title"],
    additionalProperties: false,
  };
  const result = buildSurplusClaudeRequest(
    {
      ...base(),
      reasoning: { effort: "high", summary: "concise" },
      text: { format: { type: "json_schema", strict: true, schema, name: "codex_output_schema" } },
    },
    undefined,
    { supportsNativeReasoningEffort: true }
  );
  assert.ok(result);
  assert.deepEqual(result.body.output_config, {
    effort: "high",
    format: { type: "json_schema", schema },
  });
});

test("Surplus Claude treats the default OpenAI text format as no structured output", () => {
  const options = { supportsNativeReasoningEffort: true };
  const withFormat = buildSurplusClaudeRequest({ ...base(), reasoning: { effort: "high" }, text: { format: { type: "text" } } }, undefined, options);
  const withoutText = buildSurplusClaudeRequest({ ...base(), reasoning: { effort: "high" } }, undefined, options);
  assert.ok(withFormat);
  assert.deepEqual(withFormat.body.output_config, { effort: "high" });
  assert.deepEqual(withFormat.body, withoutText?.body);
  for (const text of [{ format: { type: "json_object" } }, { format: { type: "grammar" } }, { format: { type: "text", schema: { type: "object" } } }]) {
    assert.equal(buildSurplusClaudeRequest({ ...base(), text }, undefined, options), null, JSON.stringify(text));
  }
});

test("the direct Claude Code lane accepts the default text format with effort-only output config", () => {
  const body = { model: "claude-opus-5-5", input: "hello", reasoning: { effort: "high" }, text: { format: { type: "text" } } };
  const built = buildClaudeCodeRequest(body, { nativeReasoningEfforts: ["low", "medium", "high"] });
  assert.ok(built);
  assert.deepEqual(built.body.output_config, { effort: "high" });
  assert.equal(buildClaudeCodeRequest({ ...body, text: { format: { type: "unknown_format" } } }, { nativeReasoningEfforts: ["high"] }), null);
});

test("Surplus Claude accepts the Codex concise reasoning summary preference", () => {
  const result = buildSurplusClaudeRequest({ ...base(), reasoning: { effort: "high", summary: "concise" } }, undefined, {
    supportsNativeReasoningEffort: true,
  });
  assert.ok(result);
  assert.deepEqual(result.body.thinking, { type: "adaptive" });
  assert.deepEqual(result.body.output_config, { effort: "high" });
});

test("native none requires independent disabled-thinking proof, never an effort or model-name guess", () => {
  const body = { ...base(), reasoning: { effort: "none", summary: "none" } };
  const before = JSON.stringify(body);
  assert.equal(buildSurplusClaudeRequest(body), null);
  assert.equal(buildSurplusClaudeRequest(body, undefined, { supportsNativeReasoningEffort: true }), null);
  assert.equal(buildSurplusClaudeRequest(body, undefined, { supportsNativeDisabledThinking: false }), null);
  const result = buildSurplusClaudeRequest(body, undefined, { supportsNativeDisabledThinking: true, supportsNativeReasoningEffort: false });
  assert.ok(result);
  assert.deepEqual(result.body.thinking, { type: "disabled" });
  assert.equal(result.body.output_config, undefined);
  assert.equal(JSON.stringify(body), before);
  assert.equal(buildSurplusClaudeRequest({ ...base(), reasoning: { effort: "high" } }, undefined, { supportsNativeDisabledThinking: true }), null);
});

test("Surplus Claude has no invented output cap when the caller omitted one", () => {
  const body = { ...base(), max_output_tokens: null };
  assert.equal(buildSurplusClaudeRequest(body), null);
  assert.equal(buildSurplusClaudeRequest(body, 128000)?.body.max_tokens, 128000);
  assert.equal(buildSurplusClaudeRequest(body, -1), null);
});

test("Surplus native adapter returns incompatible inputs intact to the existing Responses route", () => {
  const bodies = [
    { ...base(), model: "deepseek-v4.1-flash" },
    { ...base(), model: "gpt-6-sol" },
    { ...base(), input: [{ type: "reasoning", encrypted_content: "opaque" }] },
    { ...base(), input: [{ type: "reasoning", summary: [{ type: "summary_text", text: "summary" }] }] },
    { ...base(), input: [{ type: "function_call", name: "run", call_id: "c", arguments: "not-json" }] },
    { ...base(), tools: [{ type: "web_search" }] },
    { ...base(), tools: [{ type: "namespace", name: "functions", tools: [] }] },
    { ...base(), tools: [{ type: "custom", name: "apply_patch" }] },
    { ...base(), tools: [{ type: "function", name: "run", strict: true }] },
    { ...base(), prompt_cache_retention: "24h" },
    { ...base(), prompt_cache_options: { ttl: "30m" } },
    { ...base(), prompt_cache_options: { mode: "disabled" } },
    {
      ...base(),
      cache_control: { type: "unsupported" },
      input: [{ role: "user", content: [{ type: "input_text", text: "stable", cache_control: { type: "ephemeral" } }] }],
    },
    {
      ...base(),
      input: [{ role: "user", content: [{ type: "input_text", text: "stable", cache_control: { type: "ephemeral" }, prompt_cache_breakpoint: {} }] }],
    },
    { ...base(), text: { format: { type: "json_schema" } } },
    { ...base(), text: { format: "text" } },
    { ...base(), store: true },
    { ...base(), previous_response_id: "resp_other_provider" },
    { ...base(), input: [{ role: "user", content: [{ type: "input_file", file_id: "file_other_provider" }] }] },
    { ...base(), input: [{ role: "user", content: [{ type: "input_image", image_url: "https://example.test/a.png", detail: "original" }] }] },
  ];
  for (const body of bodies) {
    const before = JSON.stringify(body);
    assert.equal(buildSurplusClaudeRequest(body), null, before);
    assert.equal(JSON.stringify(body), before);
  }
});

test("Direct Claude Code omits enabled hosted web search while reseller adapters reject it", () => {
  const body = { ...base(), tools: [{ type: "web_search", external_web_access: true, user_location: { type: "approximate", country: "US" } }] };
  const direct = buildSurplusClaudeRequest(body, undefined, { provider: "claude_code", omitHostedWebSearch: true });
  assert.ok(direct);
  assert.equal(direct.body.tools, undefined);
  assert.equal(buildSurplusClaudeRequest(body), null);
});
