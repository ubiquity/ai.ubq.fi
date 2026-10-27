import assert from "node:assert/strict";
import { MATRIX_KEYS, MATRIX_MODELS } from "../fixtures/provider-matrix.mjs";
import { assertSingleAdmission, createClient, dispatches, object, post, responseObject, selectProviders, settled, type MatrixContext } from "./contracts.ts";

const tools = [{ type: "function", name: "read_file", parameters: { type: "object", properties: { path: { type: "string" } } } }];

async function firstOutput(response: Response, stream: boolean): Promise<unknown[]> {
  assert.equal(response.status, 200);
  let body: Record<string, unknown>;
  if (stream) {
    const text = await response.text();
    const events = [...text.matchAll(/^data: (.+)$/gmu)].map((match) => object(JSON.parse(match[1])));
    const completed = events.filter((event) => event.type === "response.completed");
    assert.equal(completed.length, 1);
    body = object(object(completed[0]).response);
  } else body = await responseObject(response);
  assert(Array.isArray(body.output));
  assert(body.output.some((item: unknown) => object(item).type === "reasoning"));
  assert(body.output.some((item: unknown) => object(item).type === "function_call"));
  assert(!JSON.stringify(body).includes("private-fixture-signature"));
  return body.output;
}

async function replayTurn(context: MatrixContext, stream: boolean): Promise<void> {
  const mode = stream ? "stream" : "json";
  const producer = `fixture:matrix:replay:claude-producer-${mode}`;
  const first = await post(context, "/v1/responses", { model: MATRIX_MODELS.claude, input: producer, tools, stream });
  const output = await firstOutput(first, stream);
  const firstState = await settled(context, first);
  assertSingleAdmission(firstState, "claude_code");
  const consumer = `fixture:matrix:replay:claude-consumer-${mode}`;
  const body = {
    model: MATRIX_MODELS.claude,
    input: [
      { type: "message", role: "user", content: [{ type: "input_text", text: consumer }] },
      ...output,
      { type: "function_call_output", call_id: "call_1", output: [{ type: "input_text", text: "file" }] },
      { type: "message", role: "user", content: [{ type: "input_text", text: "continue" }] },
    ],
    tools,
    stream: false,
  };
  const second = await post(context, "/v1/responses", body);
  assert.equal((await responseObject(second)).status, "completed");
  const secondState = await settled(context, second);
  assertSingleAdmission(secondState, "claude_code");
  assert.notEqual(first.headers.get("x-uos-request-id"), second.headers.get("x-uos-request-id"));
  assert(Array.isArray(firstState.attempts) && Array.isArray(secondState.attempts));
  const firstFence = object(object(object(firstState.attempts[0]).fence).Claude);
  const secondFence = object(object(object(secondState.attempts[0]).fence).Claude);
  assert.equal(secondFence.account_id, firstFence.account_id);
  assert.equal(secondFence.cohort_id, firstFence.cohort_id);
  const calls = dispatches(await context.events(), consumer);
  assert.equal(calls.length, 1);
  const call = object(calls[0]);
  assert.equal(call.wire, "anthropic");
  assert(Array.isArray(call.context_messages));
  const messages = call.context_messages.map(object);
  const assistant = messages.find((message) => message.role === "assistant");
  assert(assistant && Array.isArray(assistant.content));
  const blocks = assistant.content.map(object);
  assert(blocks.some((block) => block.type === "thinking" && block.thinking === "thought" && block.signature === "private-fixture-signature"));
  assert(blocks.some((block) => block.type === "text" && block.text === "answer"));
  assert(blocks.some((block) => block.type === "tool_use" && block.id === "call_1" && block.name === "read_file" && object(block.input).path === "a.txt"));
  assert(
    messages.some(
      (message) =>
        message.role === "user" &&
        Array.isArray(message.content) &&
        message.content.some((value: unknown) => {
          const block = object(value);
          return block.type === "tool_result" && block.tool_use_id === "call_1" && JSON.stringify(block.content).includes("file");
        })
    )
  );
  const foreign = await createClient(context);
  await responseObject(
    await context.post(
      "/admin/providers/claude-code/access",
      { allowed_key_ids: [object(secondState.api).key_id, foreign.id] },
      context.adminToken ?? "fixture-admin"
    )
  );
  const marker = `fixture:matrix:replay:claude-foreign-${mode}`;
  const refused = await post({ ...context, clientToken: foreign.token }, "/v1/responses", {
    ...body,
    input: [{ type: "message", role: "user", content: [{ type: "input_text", text: marker }] }, ...body.input.slice(1)],
  });
  assert.equal(refused.status, 400);
  await refused.arrayBuffer();
  assert.equal(dispatches(await context.events(), marker).length, 0);
  context.cases.push(`matrix_claude_${mode}_native_replay_restored_only_for_original_principal_cohort`);
}

export async function claudeReplayCases(context: MatrixContext): Promise<void> {
  await selectProviders(context, ["claude_code"]);
  const key = await createClient(context);
  await responseObject(
    await context.post(
      "/admin/providers/claude-code/auth",
      { credential: MATRIX_KEYS.claude, create_new: true, activate: true, label: "controlled replay" },
      context.adminToken ?? "fixture-admin"
    )
  );
  await responseObject(await context.post("/admin/providers/claude-code/access", { allowed_key_ids: [key.id] }, context.adminToken ?? "fixture-admin"));
  const client = { ...context, clientToken: key.token };
  await replayTurn(client, false);
  await replayTurn(client, true);
}
