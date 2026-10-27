import assert from "node:assert/strict";
import { MATRIX_MODELS } from "../fixtures/provider-matrix.mjs";
import { createClient, dispatches, object, post, responseObject, selectProviders, settled, stringKey, type MatrixContext } from "./contracts.ts";

async function paidRow(context: MatrixContext, attempt: Record<string, unknown>): Promise<Record<string, unknown>> {
  const reservation = object(attempt.paid);
  assert(typeof reservation.key_id === "string" && typeof reservation.attempt_id === "string");
  const row = await context.read(stringKey("uos_ai", "paid_fallback", "v3", "request", reservation.key_id, reservation.attempt_id));
  assert(row);
  return object(row.value);
}

async function apiCounts(context: MatrixContext, keyId: string): Promise<{ committed: number; reserved: number }> {
  const page = await context.rpc({
    operation: "query",
    selector: { kind: "prefix", prefix: stringKey("uos_ai", "api_key_usage", "v3", "window", keyId), start: null, end: null },
    reverse: false,
    limit: 2,
    cursor: null,
    now_ms: Date.now(),
  });
  assert(Array.isArray(page.entries) && page.entries.length <= 1 && page.next_cursor === null);
  if (page.entries.length === 0) return { committed: 0, reserved: 0 };
  const window = object(object(object(page.entries[0]).record).value);
  assert(typeof window.committed_requests === "number" && typeof window.reserved_requests === "number");
  return { committed: window.committed_requests, reserved: window.reserved_requests };
}

/** A fresh paid scenario establishes genuine full-pool refusal before the native SSE hop. */
export async function streamSettlementCases(context: MatrixContext): Promise<void> {
  await selectProviders(context, ["codex", "surplus", "openlux"]);
  const headers = { authorization: `Bearer ${context.clientToken ?? "fixture-access"}` };
  await responseObject(await fetch(context.url("/v1/models?client_version=0.163.0"), { headers }));
  await responseObject(await fetch(context.url("/v1/models"), { headers }));
  const key = await createClient(context, true);
  const before = await apiCounts(context, key.id);
  const marker = "fixture:matrix:paid:surplus";
  const client = { ...context, clientToken: key.token };
  const response = await post(client, "/v1/responses", { model: MATRIX_MODELS.codex, input: marker, stream: true });
  assert.equal(response.status, 200);
  assert(response.headers.get("content-type")?.startsWith("text/event-stream"));
  const text = await response.text();
  const events = [...text.matchAll(/^data: (.+)$/gmu)].map((match) => object(JSON.parse(match[1])));
  const completed = events.filter((event) => event.type === "response.completed");
  assert.equal(completed.length, 1);
  assert.deepEqual(object(object(completed[0]).response).usage, { input_tokens: 7, output_tokens: 3, total_tokens: 10, buyer_cost_micro: 5 });
  const state = await settled(client, response);
  assert.equal(state.finished, true);
  assert.equal(object(state.api).request_id, response.headers.get("x-uos-request-id"));
  assert(Array.isArray(state.attempts));
  const attempts = state.attempts.map(object);
  assert.equal(new Set(attempts.map((attempt) => attempt.id)).size, attempts.length);
  const paid = attempts.filter((attempt) => attempt.paid !== null);
  assert.equal(paid.length, 1);
  const row = await paidRow(context, object(paid[0]));
  assert.equal(row.billing_state, "settled");
  assert.equal(row.spend_microcredits, 5);
  assert.equal(row.terminal_state, "completed");
  assert.deepEqual(object(state.terminal_intent), {
    semantic_terminal: "response.completed",
    reader_termination: "eof",
    body: "drained",
    delivery: "unobserved",
  });
  const calls = dispatches(await context.events(), marker);
  assert.deepEqual(
    calls.map((call) => call.provider),
    ["codex", "codex", "surplus"]
  );
  const surplus = object(calls[2]);
  assert.equal(surplus.wire, "responses");
  assert.equal(surplus.stream, true);
  assert.equal(surplus.mask_secrets, "on");
  assert.deepEqual(await apiCounts(context, key.id), { committed: before.committed + 1, reserved: before.reserved });
  const repeated = await paidRow(context, object(paid[0]));
  assert.equal(repeated.billing_state, "settled");
  assert.equal(repeated.spend_microcredits, 5);
  assert.deepEqual((await settled(client, response)).api, state.api);
  assert(!(await context.events()).some((event) => object(event).kind === "matrix_billing_lookup"));
  context.cases.push("matrix_native_surplus_sse_usage_settles_exact_cost_once_without_delivery_claim");
}
