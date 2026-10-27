import assert from "node:assert/strict";
import { MATRIX_MODELS } from "../fixtures/provider-matrix.mjs";
import { dispatches, object, post, selectProviders, settled, type MatrixContext } from "./contracts.ts";

export async function lithosRefusalCases(context: MatrixContext): Promise<void> {
  await selectProviders(context, ["lithos"]);
  for (const chat of [false, true]) {
    const marker = `fixture:matrix:lithos:stream-refusal${chat ? "-chat" : ""}`;
    const body = chat
      ? { model: MATRIX_MODELS.lithos, messages: [{ role: "user", content: marker }], stream: true }
      : { model: MATRIX_MODELS.lithos, input: marker, stream: true };
    const response = await post(context, chat ? "/v1/chat/completions" : "/v1/responses", body);
    assert.equal(response.status, 200);
    assert(response.headers.get("content-type")?.startsWith("text/event-stream"));
    assert.equal(response.headers.get("x-should-retry"), "false");
    const text = await response.text();
    const events = [...text.matchAll(/^data: (.+)$/gmu)].map((match) => object(JSON.parse(match[1])));
    const failure = events.find((event) => (chat ? object(event.error).code === "tokens_exhausted" : event.type === "response.failed"));
    assert(failure);
    assert.equal(object(chat ? failure.error : object(failure.response).error).code, "tokens_exhausted");
    assert(!events.some((event) => event.type === "response.completed"));
    const state = await settled(context, response);
    assert.equal(state.finished, true);
    assert.equal(object(state.terminal_intent).delivery, "unobserved");
    assert(Array.isArray(state.attempts));
    const attempts = state.attempts.map(object);
    assert(attempts.every((attempt) => attempt.paid === null && attempt.provider === "lithos"));
    const calls = dispatches(await context.events(), marker);
    assert(calls.length > 0);
    assert(calls.every((call) => call.provider === "lithos" && call.wire === "chat" && call.stream === true));
    assert.equal(new Set(calls.map((call) => call.model)).size, calls.length, "Response prohibition must prevent retrying a refused rung");
    context.cases.push(`matrix_lithos_${chat ? "chat" : "responses"}_native_429_is_in_band_without_paid_fallback`);
  }
}
