import assert from "node:assert/strict";
import fixture from "./fixtures/failover-sanitized-continuation.json" with { type: "json" };
import {
  buildOpenRouterRequestProjection,
  hasFailoverWarningAfterLatestUserMessage,
  projectOpenRouterResponsesIterator,
} from "../src/openrouter_projection.ts";
import { buildOpenRouterResponsesRequest } from "../src/openrouter.ts";
import { responseEventFromValue, responsesEventSemanticKind } from "../src/responses_failover_stream.ts";
import type { ResponsesStreamEvent, ResponsesStreamIterator } from "../src/responses_stream.ts";

const event = (value: Record<string, unknown>): ResponsesStreamEvent => responseEventFromValue(value);

const iterator = (events: readonly ResponsesStreamEvent[]): ResponsesStreamIterator =>
  (async function* () {
    yield* events;
  })();

const collect = async (events: ResponsesStreamIterator): Promise<ResponsesStreamEvent[]> => {
  const output: ResponsesStreamEvent[] = [];
  for await (const item of events) output.push(item);
  return output;
};

const customTool = { type: "custom", name: "exec", description: "Run", format: { type: "text" } };

Deno.test("sanitized incident shape fails at the reduced reasoning-content index before projection", () => {
  const input = fixture.input as Array<Record<string, unknown>>;
  assert.equal(fixture.incident_index, "input[101].content");
  assert.equal(fixture.reduced_fixture_index, "input[9].content");

  const legacyProviderValidation = (): never => {
    const item = input[9]!;
    if (item.type === "reasoning" && Array.isArray(item.content) && item.content.length > 0) {
      throw new Error("Invalid request: input[9].content maximum length is zero; received one item.");
    }
    throw new Error("fixture no longer reproduces the recorded validation shape");
  };
  assert.throws(legacyProviderValidation, /input\[9\]\.content/);

  const canonicalInput = structuredClone(input);
  const projection = buildOpenRouterRequestProjection({
    input: canonicalInput,
    stream: true,
    tools: fixture.tools,
  });
  const projectedInput = projection.input as readonly Record<string, unknown>[];
  const projectedReasoning = projectedInput.find((item) => item.type === "reasoning")!;
  assert.deepEqual(projectedReasoning.summary, [{
    type: "summary_text",
    text: "Retain this summary across the route.",
  }]);
  assert.equal("content" in projectedReasoning, false);
  assert.equal("encrypted_content" in projectedReasoning, false);
  assert.equal(
    projectedInput.some((item) =>
      item.type === "message" && item.role === "assistant" &&
      JSON.stringify(item).includes("Failover active")
    ),
    false,
  );
  assert.equal(canonicalInput[9]!.content instanceof Array, true);
  const projectedCall = projectedInput.find((item) => item.type === "function_call")!;
  const projectedOutput = projectedInput.find((item) => item.type === "function_call_output")!;
  assert.equal(projectedCall.call_id, "call_fixture");
  assert.equal(projectedOutput.call_id, "call_fixture");
  assert.equal(JSON.parse(projectedCall.arguments as string).input, "printf fixture-nonce");
});

Deno.test("tool projection is deterministic, collision-free, and preserves compatible functions", () => {
  const canonical = {
    input: "hello",
    stream: true,
    tools: [
      { type: "function", name: "exec", parameters: { type: "object" } },
      customTool,
      { type: "function", name: "not a valid OpenRouter name", parameters: { type: "object" } },
    ],
  };
  const first = buildOpenRouterRequestProjection(canonical);
  const second = buildOpenRouterRequestProjection(canonical);
  assert.deepEqual(first.tools, second.tools);
  const projectedTools = first.tools as readonly Record<string, unknown>[];
  const names = projectedTools.map((tool) => tool.name as string);
  assert.equal(new Set(names).size, names.length);
  assert.equal(names[0], "exec");
  assert.match(names[1]!, /^uos_custom_exec_[0-9a-f]{8}$/);
  assert.match(names[2]!, /^uos_function_not_a_valid_OpenRouter_name_[0-9a-f]{8}$/);
  assert.ok(names.every((name) => name.length <= 64 && /^[A-Za-z0-9_-]+$/.test(name)));
});

Deno.test("Codex additional_tools namespaces flatten into the request-scoped registry", () => {
  const projection = buildOpenRouterRequestProjection({
    input: [{
      type: "additional_tools",
      role: "developer",
      tools: [{
        type: "namespace",
        name: "functions",
        tools: [
          { type: "custom", name: "exec", description: "Run a command", format: { type: "text" } },
          { type: "function", name: "wait", parameters: { type: "object", properties: {} } },
        ],
      }],
    }, { type: "message", role: "user", content: [{ type: "input_text", text: "hello" }] }],
    stream: true,
  });
  assert.equal(
    (projection.input as readonly Record<string, unknown>[]).some((item) => item.type === "additional_tools"),
    false,
  );
  assert.deepEqual(projection.registry.entries.map((entry) => [entry.originalType, entry.originalName]), [
    ["custom", "exec"],
    ["function", "wait"],
  ]);
  assert.equal(projection.tools?.length, 2);
  assert.equal((projection.tools?.[0] as Record<string, unknown>).type, "function");
});

Deno.test("failover warning episode detection resets after a new user message", () => {
  const warning = {
    type: "message",
    role: "assistant",
    content: [{
      type: "output_text",
      text:
        "⚠ Failover active: your request was rerouted to `openrouter:fixture/model` because the primary Codex service was unavailable.",
    }],
  };
  const user = { type: "message", role: "user", content: [{ type: "input_text", text: "hello" }] };
  assert.equal(hasFailoverWarningAfterLatestUserMessage([user, warning]), true);
  assert.equal(
    hasFailoverWarningAfterLatestUserMessage([user, warning, {
      ...user,
      content: [{ type: "input_text", text: "again" }],
    }]),
    false,
  );
});

Deno.test("reasoning content null is treated as absent during projection", () => {
  const projection = buildOpenRouterRequestProjection({
    input: [{ type: "reasoning", content: null, summary: [] }],
    stream: true,
  });
  assert.deepEqual(projection.input, [{ type: "reasoning", summary: [] }]);
});

Deno.test("malformed, orphaned, and kind-mismatched continuation items fail before dispatch", () => {
  const base = { stream: true, tools: [customTool] };
  for (
    const input of [
      [{ type: "custom_tool_call_output", call_id: "missing", output: "x" }],
      [{ type: "function_call", call_id: "call_1", name: "exec", arguments: "{}" }],
      [{ type: "unknown_call", call_id: "call_1" }],
    ]
  ) {
    assert.throws(
      () => buildOpenRouterResponsesRequest({ ...base, input }),
      (error: unknown) =>
        error instanceof Error &&
        (error as Error & { status?: number; code?: string }).status === 400 &&
        (error as Error & { status?: number; code?: string }).code === "openrouter_translation_invalid",
    );
  }
});

Deno.test("parallel projected custom calls reverse-project with isolated arguments and one terminal", async () => {
  const projection = buildOpenRouterRequestProjection({
    input: "start",
    stream: true,
    tools: [
      { type: "custom", name: "exec", description: "Run", format: { type: "text" } },
      { type: "custom", name: "patch", description: "Patch", format: { type: "text" } },
    ],
  });
  const names = projection.tools!.map((tool) => tool.name as string);
  const values = await collect(projectOpenRouterResponsesIterator(
    iterator([
      event({ type: "response.created", response: { id: "resp_parallel" } }),
      event({
        type: "response.output_item.added",
        response_id: "resp_parallel",
        output_index: 0,
        item: {
          id: "item_a",
          type: "function_call",
          status: "in_progress",
          call_id: "call_a",
          name: names[0],
          arguments: "",
        },
      }),
      event({
        type: "response.output_item.added",
        response_id: "resp_parallel",
        output_index: 1,
        item: {
          id: "item_b",
          type: "function_call",
          status: "in_progress",
          call_id: "call_b",
          name: names[1],
          arguments: "",
        },
      }),
      event({ type: "response.function_call_arguments.delta", item_id: "item_b", delta: '{"input":"beta"}' }),
      event({ type: "response.function_call_arguments.delta", item_id: "item_a", delta: '{"input":"alpha"}' }),
      event({
        type: "response.function_call_arguments.done",
        item_id: "item_b",
        arguments: '{"input":"beta"}',
      }),
      event({
        type: "response.function_call_arguments.done",
        item_id: "item_a",
        arguments: '{"input":"alpha"}',
      }),
      event({
        type: "response.completed",
        response: {
          id: "resp_parallel",
          status: "completed",
          output: [
            {
              id: "item_a",
              type: "function_call",
              status: "completed",
              call_id: "call_a",
              name: names[0],
              arguments: '{"input":"alpha"}',
            },
            {
              id: "item_b",
              type: "function_call",
              status: "completed",
              call_id: "call_b",
              name: names[1],
              arguments: '{"input":"beta"}',
            },
          ],
        },
      }),
    ]),
    projection.registry,
  ));
  const doneItems = values.filter((item) => item.type === "response.output_item.done");
  assert.deepEqual(doneItems.map((item) => (item.value.item as Record<string, unknown>).input), ["beta", "alpha"]);
  assert.deepEqual(doneItems.map((item) => (item.value.item as Record<string, unknown>).call_id), ["call_b", "call_a"]);
  const terminal = values.filter((item) => item.terminal);
  assert.equal(terminal.length, 1);
  const terminalOutput = (terminal[0]!.value.response as Record<string, unknown>).output as Record<string, unknown>[];
  assert.deepEqual(terminalOutput.map((item) => item.input), ["alpha", "beta"]);
  assert.equal(terminalOutput.every((item) => item.type === "custom_tool_call"), true);
});

Deno.test("supported local-shell calls project and reverse-project without losing action fields", async () => {
  const canonical = {
    input: [{
      type: "local_shell_call",
      call_id: "shell_call",
      action: {
        type: "exec",
        command: ["printf", "shell-nonce"],
        timeout_ms: 5000,
        working_directory: "/tmp/fixture",
        env: { FIXTURE_MODE: "test" },
        user: "fixture-user",
        with_escalated_permissions: false,
        justification: "bounded fixture command",
      },
    }],
    stream: true,
    tools: [{ type: "local_shell" }],
  };
  const projection = buildOpenRouterRequestProjection(canonical);
  const projectedCall = (projection.input as readonly Record<string, unknown>[])[0]!;
  assert.equal(projectedCall.type, "function_call");
  const projectedArguments = JSON.parse(projectedCall.arguments as string) as Record<string, unknown>;
  assert.deepEqual(projectedArguments.command, ["printf", "shell-nonce"]);
  assert.equal(projectedArguments.workdir, "/tmp/fixture");
  assert.deepEqual(projectedArguments.env, { FIXTURE_MODE: "test" });
  assert.equal(projectedArguments.with_escalated_permissions, false);
  assert.equal(projectedArguments.justification, "bounded fixture command");

  const values = await collect(projectOpenRouterResponsesIterator(
    iterator([
      event({ type: "response.created", response: { id: "resp_shell" } }),
      event({
        type: "response.output_item.added",
        response_id: "resp_shell",
        output_index: 0,
        item: {
          id: "shell_item",
          type: "function_call",
          status: "in_progress",
          call_id: "shell_call",
          name: (projection.tools![0] as Record<string, unknown>).name,
          arguments: "",
        },
      }),
      event({
        type: "response.function_call_arguments.done",
        response_id: "resp_shell",
        item_id: "shell_item",
        arguments: JSON.stringify(projectedArguments),
      }),
      event({
        type: "response.completed",
        response: {
          id: "resp_shell",
          output: [{
            id: "shell_item",
            type: "function_call",
            status: "completed",
            call_id: "shell_call",
            name: (projection.tools![0] as Record<string, unknown>).name,
            arguments: JSON.stringify(projectedArguments),
          }],
        },
      }),
    ]),
    projection.registry,
  ));
  const item = values.find((value) => value.type === "response.output_item.done")?.value.item as Record<
    string,
    unknown
  >;
  assert.equal(item.type, "local_shell_call");
  assert.deepEqual(item.action, {
    type: "exec",
    command: ["printf", "shell-nonce"],
    working_directory: "/tmp/fixture",
    timeout_ms: 5000,
    env: { FIXTURE_MODE: "test" },
    user: "fixture-user",
    with_escalated_permissions: false,
    justification: "bounded fixture command",
  });
});

Deno.test("complete calls found only in response.output or terminal output get native done events", async () => {
  const projection = buildOpenRouterRequestProjection({
    input: "hello",
    stream: true,
    tools: [{
      type: "function",
      name: "lookup",
      parameters: { type: "object", required: ["q"], properties: { q: { type: "string" } } },
    }],
  });
  const projectedName = projection.tools![0]!.name as string;
  const call = (id: string) => ({
    id,
    type: "function_call",
    status: "completed",
    call_id: id,
    name: projectedName,
    arguments: JSON.stringify({ q: id }),
  });
  for (
    const [label, events] of [
      ["response.output", [
        event({ type: "response.created", response: { id: "resp_output" } }),
        event({ type: "response.output", response_id: "resp_output", output: [call("call_output")] }),
        event({ type: "response.completed", response: { id: "resp_output", output: [call("call_output")] } }),
      ]],
      ["terminal output", [
        event({ type: "response.created", response: { id: "resp_terminal" } }),
        event({ type: "response.completed", response: { id: "resp_terminal", output: [call("call_terminal")] } }),
      ]],
    ] as const
  ) {
    const values = await collect(projectOpenRouterResponsesIterator(iterator(events), projection.registry));
    const done = values.filter((item) => item.type === "response.output_item.done");
    assert.equal(done.length, 1, label);
    assert.equal((done[0]!.value.item as Record<string, unknown>).type, "function_call", label);
    assert.equal(values.filter((item) => item.terminal).length, 1, label);
  }
});

Deno.test("invalid projected arguments do not become a semantic tool call", async () => {
  const projection = buildOpenRouterRequestProjection({
    input: "hello",
    stream: true,
    tools: [{ type: "custom", name: "exec", format: { type: "text" } }],
  });
  const projectedName = projection.tools![0]!.name as string;
  await assert.rejects(
    () =>
      collect(projectOpenRouterResponsesIterator(
        iterator([
          event({ type: "response.created", response: { id: "resp_invalid" } }),
          event({
            type: "response.output_item.added",
            response_id: "resp_invalid",
            output_index: 0,
            item: {
              id: "invalid_item",
              type: "function_call",
              status: "in_progress",
              call_id: "invalid_call",
              name: projectedName,
              arguments: "",
            },
          }),
          event({ type: "response.function_call_arguments.done", item_id: "invalid_item", arguments: "{}" }),
          event({ type: "response.completed", response: { id: "resp_invalid", output: [] } }),
        ]),
        projection.registry,
      )),
    /required|input/,
  );
});

Deno.test("OpenRouter raw reasoning is not a semantic commitment, while text remains one", () => {
  const options = { ignoreRawReasoning: true } as const;
  assert.equal(
    responsesEventSemanticKind(event({ type: "response.reasoning_summary_text.delta", delta: "summary" }), options),
    null,
  );
  assert.equal(
    responsesEventSemanticKind(event({ type: "response.reasoning_text.delta", delta: "raw" }), options),
    null,
  );
  assert.equal(
    responsesEventSemanticKind(
      event({
        type: "response.output",
        output: [{ type: "reasoning", summary: [{ type: "summary_text", text: "raw" }] }],
      }),
      options,
    ),
    null,
  );
  assert.equal(
    responsesEventSemanticKind(event({ type: "response.output_text.delta", delta: "visible" }), options),
    "text",
  );
});

Deno.test("synthetic warning history is removed, user quotes remain, and provider echoes are suppressed", async () => {
  const warning =
    "⚠ Failover active: your request was rerouted to `openrouter:fixture/model` because the primary Codex service was unavailable.";
  const projection = buildOpenRouterRequestProjection({
    input: [
      { type: "message", role: "user", content: [{ type: "input_text", text: warning }] },
      { type: "message", role: "assistant", content: [{ type: "output_text", text: warning }] },
    ],
    stream: true,
  });
  const projectedInput = projection.input as readonly Record<string, unknown>[];
  assert.equal(projectedInput.length, 1);
  assert.equal(projectedInput[0]!.role, "user");

  const values = await collect(projectOpenRouterResponsesIterator(
    iterator([
      event({ type: "response.created", response: { id: "resp_warning" } }),
      event({ type: "response.output_text.delta", delta: warning }),
      event({
        type: "response.output_item.done",
        item: {
          id: "msg_provider",
          type: "message",
          role: "assistant",
          content: [{ type: "output_text", text: warning }],
        },
      }),
      event({
        type: "response.completed",
        response: {
          id: "resp_warning",
          output: [{
            id: "msg_provider",
            type: "message",
            role: "assistant",
            content: [{ type: "output_text", text: warning }],
          }],
        },
      }),
    ]),
    projection.registry,
  ));
  assert.equal(JSON.stringify(values).includes(warning), false);
  assert.equal(values.filter((item) => item.terminal).length, 1);
});
