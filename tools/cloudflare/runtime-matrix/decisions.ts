import assert from "node:assert/strict";
import { MATRIX_MODELS } from "../fixtures/provider-matrix.mjs";
import { createClient, dispatches, object, post, responseObject, selectProviders, settled, type MatrixContext } from "./contracts.ts";

export async function decisionsCases(context: MatrixContext): Promise<void> {
  await selectProviders(context, ["openrouter"]);
  const key = await createClient(context);
  const client = { ...context, clientToken: key.token };
  const marker = "fixture:matrix:decisions:success";
  const response = await post(client, "/v1/decisions", {
    model: MATRIX_MODELS.decision,
    state: marker,
    questions: { ready: { type: "noul", instructions: "Ready?" } },
    trace: { id: "controlled-decision" },
  });
  const result = await responseObject(response);
  assert.deepEqual(result.answers, { ready: 0.9 });
  assert.deepEqual(result.trace, { id: "controlled-decision" });
  assert.equal(response.headers.get("x-uos-upstream"), "openrouter");
  const state = await settled(client, response);
  assert.equal(state.finished, true);
  const requestId = response.headers.get("x-uos-request-id");
  assert.equal(object(state.context).request_id, requestId);
  assert.equal(state.extras_family, "Decisions");
  assert.equal(state.extras_child_count, 1);
  assert.deepEqual(state.attempts, [], "Decisions must not enter the ordinary or paid provider waterfall");
  const dispatch = object(state.extras_dispatch);
  assert.equal(dispatch.provider, "openrouter");
  assert.equal(dispatch.transport_started, true);
  assert(Array.isArray(state.children) && state.children.length === 1);
  const child = object(state.children[0]);
  assert.equal(child.request_id, requestId);
  assert.equal(child.transport_started, true);
  assert.equal(child.settled, true);
  assert.equal(object(state.terminal_intent).semantic_terminal, "response.completed");
  assert.deepEqual(
    dispatches(await context.events(), marker).map((call) => call.wire),
    ["decisions"]
  );
  const invalidText = await post(client, "/v1/decisions", {
    model: MATRIX_MODELS.openrouter,
    state: marker,
    questions: { ready: { type: "noul", instructions: "Ready?" } },
  });
  await responseObject(invalidText, 400);
  assert.equal((await settled(client, invalidText)).finished, true);
  for (const path of ["/v1/responses", "/v1/chat/completions"]) {
    const invalid = await post(client, path, {
      model: MATRIX_MODELS.decision,
      ...(path.endsWith("responses") ? { input: marker } : { messages: [{ role: "user", content: marker }] }),
    });
    await responseObject(invalid, 404);
  }
  assert.equal(dispatches(await context.events(), marker).length, 1);
  context.cases.push("matrix_decisions_cold_catalog_fixed_wire_and_text_model_separation");
}
