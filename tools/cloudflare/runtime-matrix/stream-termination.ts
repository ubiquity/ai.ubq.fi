import assert from "node:assert/strict";
import { MATRIX_MODELS } from "../fixtures/provider-matrix.mjs";
import { assertSingleAdmission, dispatches, object, post, responseObject, selectProviders, settled, type MatrixContext } from "./contracts.ts";

export async function streamTerminationCases(context: MatrixContext): Promise<void> {
  await selectProviders(context, ["codex"]);
  const marker = "fixture:matrix:codex:terminal-hang";
  const response = await post(context, "/v1/responses", { model: MATRIX_MODELS.codex, input: marker, stream: true });
  assert.equal(response.status, 200);
  const text = await response.text();
  assert.equal([...text.matchAll(/"type":"response.completed"/gu)].length, 1);
  assert(!text.includes("response.failed"));
  const state = await settled(context, response);
  assertSingleAdmission(state, "chatgpt_codex");
  assert.deepEqual(object(state.terminal_intent), {
    semantic_terminal: "response.completed",
    reader_termination: "eof",
    body: "drained",
    delivery: "unobserved",
  });
  const deadline = Date.now() + 2000;
  let cancelled: unknown[];
  do {
    cancelled = (await context.events()).filter((event) => object(event).kind === "matrix_cancelled" && object(event).marker === marker);
    if (cancelled.length === 0) await Bun.sleep(10);
  } while (cancelled.length === 0 && Date.now() < deadline);
  assert.equal(cancelled.length, 1, "The still-open upstream must be cancelled exactly once after its first terminal");
  assert.equal(dispatches(await context.events(), marker).length, 1);
  const next = await post(context, "/v1/responses", { model: MATRIX_MODELS.codex, input: "fixture:matrix:codex:after-terminal-hang", stream: false });
  assert.equal((await responseObject(next)).status, "completed");
  assertSingleAdmission(await settled(context, next), "chatgpt_codex");
  assert.deepEqual((await settled(context, response)).terminal_intent, state.terminal_intent);
  context.cases.push("matrix_native_first_terminal_closes_and_cancels_hanging_upstream_once");
}
