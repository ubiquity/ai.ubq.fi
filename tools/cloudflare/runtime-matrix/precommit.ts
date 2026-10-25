import assert from "node:assert/strict";
import { DEEPSEEK_WATERFALL_MODEL_ID } from "../../../src/deepseek/waterfall.ts";
import { MATRIX_MODELS } from "../fixtures/provider-matrix.mjs";
import { createClient, dispatches, object, post, responseObject, selectProviders, settled, stringKey, type MatrixContext } from "./contracts.ts";

export const PRECOMMIT_CASE_NAMES = [
  "matrix_synthetic_presemantic_eof_advances_one_original_admission",
  "matrix_synthetic_presemantic_failed_advances_one_original_admission",
  "matrix_synthetic_presemantic_malformed_advances_one_original_admission",
  "matrix_synthetic_semantic_output_prevents_next_hop",
  "matrix_direct_presemantic_eof_does_not_enter_synthetic_chain",
  "matrix_synthetic_presemantic_caller_abort_never_advances",
  "matrix_synthetic_direct_failure_preserves_existing_surplus_admission",
  "matrix_synthetic_authoritative_paid_capacity_allows_openlux",
  "matrix_synthetic_paid_presemantic_eof_stays_unknown_without_openlux",
];

async function apiRows(context: MatrixContext, keyId: string, family: string): Promise<Record<string, unknown>[]> {
  const result = await context.rpc({
    operation: "query",
    selector: { kind: "prefix", prefix: stringKey("uos_ai", "api_key_usage", "v3", family, keyId), start: null, end: null },
    reverse: false,
    limit: 10,
    cursor: null,
    now_ms: Date.now(),
  });
  assert(Array.isArray(result.entries));
  return result.entries.map((entry) => object(object(object(entry).record).value));
}

async function oneOriginal(
  context: MatrixContext,
  response: Response | string,
  keyId: string,
  providers: string[],
  allowPaid = false
): Promise<Record<string, unknown>> {
  const id = typeof response === "string" ? response : response.headers.get("x-uos-request-id");
  assert(id);
  const state = typeof response === "string" ? await context.lifecycle(id) : await settled(context, response);
  assert.equal(object(state.context).request_id, id);
  assert.equal(state.finished, true);
  assert(Array.isArray(state.attempts));
  assert.deepEqual(
    state.attempts.map((attempt) => object(attempt).provider),
    providers
  );
  assert(state.attempts.every((attempt) => object(attempt).transport_started === true));
  if (!allowPaid) assert(state.attempts.every((attempt) => object(attempt).paid === null));
  assert.equal(state.kernel, null, "This API-key fixture must not create a kernel admission");
  assert.notEqual(object(state.terminal_intent).delivery, "delivered", "Workers EOF is not a native delivery receipt");
  const requests = await apiRows(context, keyId, "request");
  assert.equal(requests.length, 1, "All synthetic hops must share one original API admission");
  assert.equal(requests[0]?.request_id, id);
  assert.equal(requests[0]?.state, "dispatched");
  const windows = await apiRows(context, keyId, "window");
  assert.equal(windows.length, 1);
  assert.equal(windows[0]?.committed_requests, 1);
  assert.equal(windows[0]?.reserved_requests, 0);
  return state;
}

async function failedBeforeOutput(context: MatrixContext, kind: "eof" | "failed" | "malformed"): Promise<void> {
  const key = await createClient(context);
  const client = { ...context, clientToken: key.token };
  const marker = `fixture:matrix:precommit:${kind}`;
  const response = await post(client, "/v1/responses", { model: DEEPSEEK_WATERFALL_MODEL_ID, input: marker, stream: true });
  assert.equal(response.status, 200);
  const text = await response.text();
  const calls = dispatches(await context.events(), marker);
  assert.deepEqual(
    calls.map((call) => call.provider),
    ["lithos", "openrouter"],
    "A metadata-only failed synthetic hop must advance exactly once"
  );
  assert.deepEqual(
    calls.map((call) => call.wire),
    ["chat", "responses"]
  );
  assert(text.includes("matrix answer") && text.includes("response.completed"));
  assert(!text.includes("chatcmpl_precommit_lithos") && !text.includes("response.failed"), "The abandoned hop must not leak its precommit prefix");
  assert.equal(response.headers.get("x-uos-provider-request-id"), `matrix-openrouter-${kind}`);
  await oneOriginal(client, response, key.id, ["lithos", "openrouter"]);
  context.cases.push(`matrix_synthetic_presemantic_${kind}_advances_one_original_admission`);
}

async function committedOrDirect(context: MatrixContext, synthetic: boolean): Promise<void> {
  const key = await createClient(context);
  const client = { ...context, clientToken: key.token };
  const marker = `fixture:matrix:precommit:${synthetic ? "after-output" : "direct-eof"}`;
  const response = await post(client, "/v1/responses", {
    model: synthetic ? DEEPSEEK_WATERFALL_MODEL_ID : MATRIX_MODELS.lithos,
    input: marker,
    stream: true,
  });
  assert.equal(response.status, 200);
  const text = await response.text();
  assert(text.includes("response.failed") && !text.includes("response.completed"));
  if (synthetic) assert(text.includes("committed Lithos output"));
  assert.deepEqual(
    dispatches(await context.events(), marker).map((call) => call.provider),
    ["lithos"]
  );
  await oneOriginal(client, response, key.id, ["lithos"]);
  context.cases.push(synthetic ? "matrix_synthetic_semantic_output_prevents_next_hop" : "matrix_direct_presemantic_eof_does_not_enter_synthetic_chain");
}

async function abortBeforeOutput(context: MatrixContext): Promise<void> {
  const key = await createClient(context);
  const client = { ...context, clientToken: key.token };
  const marker = "fixture:matrix:precommit:abort";
  const controller = new AbortController();
  const request = post(client, "/v1/responses", { model: DEEPSEEK_WATERFALL_MODEL_ID, input: marker, stream: true }, controller.signal);
  // Observe dispatch before cancelling; the corrected gate intentionally withholds headers.
  const outcome = request.then(
    async (response) => {
      try {
        await response.text();
      } catch {
        /* The real downstream reader may observe the caller abort. */
      }
    },
    () => {}
  );
  const deadline = Date.now() + 3000;
  let cancelled = false;
  while (Date.now() < deadline) {
    const events = await context.events();
    if (events.some((event) => object(event).kind === "matrix_precommit_ready" && object(event).marker === marker)) controller.abort();
    cancelled = events.some((event) => object(event).kind === "matrix_cancelled" && object(event).marker === marker);
    if (cancelled) break;
    await Bun.sleep(10);
  }
  controller.abort();
  await outcome;
  assert(cancelled, "Caller cancellation before output must reach the actual controlled upstream");
  assert.deepEqual(
    dispatches(await context.events(), marker).map((call) => call.provider),
    ["lithos"]
  );
  const requests = await apiRows(client, key.id, "request");
  assert.equal(requests.length, 1);
  assert(typeof requests[0]?.request_id === "string");
  const state = await oneOriginal(client, requests[0].request_id, key.id, ["lithos"]);
  assert.notEqual(object(state.terminal_intent).semantic_terminal, "response.completed");
  context.cases.push("matrix_synthetic_presemantic_caller_abort_never_advances");
}

export async function precommitCases(context: MatrixContext): Promise<void> {
  await selectProviders(context, ["ubiquity", "lithos", "openrouter"]);
  for (const kind of ["eof", "failed", "malformed"] as const) await failedBeforeOutput(context, kind);
  await committedOrDirect(context, true);
  await committedOrDirect(context, false);
  await abortBeforeOutput(context);
}

async function paidRows(context: MatrixContext, state: Record<string, unknown>): Promise<Record<string, unknown>[]> {
  assert(Array.isArray(state.attempts));
  const rows = [];
  for (const attempt of state.attempts.map(object)) {
    if (attempt.paid === null) continue;
    const reservation = object(attempt.paid);
    assert(typeof reservation.key_id === "string" && typeof reservation.attempt_id === "string");
    const row = await context.read(stringKey("uos_ai", "paid_fallback", "v3", "request", reservation.key_id, reservation.attempt_id));
    assert(row);
    rows.push(object(row.value));
  }
  return rows;
}

async function nonExhaustedPaidWindow(context: MatrixContext, keyId: string): Promise<void> {
  const page = await context.rpc({
    operation: "query",
    selector: { kind: "prefix", prefix: stringKey("uos_ai", "paid_fallback", "v3", "window", keyId), start: null, end: null },
    reverse: false,
    limit: 2,
    cursor: null,
    now_ms: Date.now(),
  });
  assert(Array.isArray(page.entries) && page.entries.length === 1 && page.next_cursor === null);
  const window = object(object(object(page.entries[0]).record).value);
  assert(typeof window.settled_microcredits === "number" && typeof window.reserved_microcredits === "number" && typeof window.limit_microcredits === "number");
  assert(
    window.settled_microcredits + window.reserved_microcredits < window.limit_microcredits,
    "A local spending limit must not explain the missing next tier"
  );
}

async function paidBoundary(context: MatrixContext, kind: "surplus" | "capacity" | "paid-eof"): Promise<void> {
  const key = await createClient(context, true);
  const client = { ...context, clientToken: key.token };
  const marker = `fixture:matrix:precommit-paid:${kind}`;
  const response = await post(client, "/v1/responses", { model: DEEPSEEK_WATERFALL_MODEL_ID, input: marker, stream: true });
  assert.equal(response.status, 200);
  const text = await response.text();
  const providers = ["lithos", "openrouter", "surplus", ...(kind === "capacity" ? ["openlux"] : [])];
  assert.deepEqual(
    dispatches(await context.events(), marker).map((call) => call.provider),
    providers
  );
  const state = await oneOriginal(
    client,
    response,
    key.id,
    providers.map((provider) => (provider === "openlux" ? "metered" : provider)),
    true
  );
  const rows = await paidRows(client, state);
  await nonExhaustedPaidWindow(client, key.id);
  assert(rows.every((row) => object(row.reservation).request_id === response.headers.get("x-uos-request-id")));
  if (kind === "paid-eof") {
    assert(text.includes("response.failed") && !text.includes("response.completed"));
    assert.equal(rows.length, 1);
    assert.equal(rows[0]?.terminal_state, "failed");
    assert.notEqual(rows[0]?.billing_state, "settled");
    assert.notEqual(rows[0]?.billing_state, "not_billed");
    assert.equal(rows[0]?.spend_microcredits, null, "A truncated paid stream retains unknown spend, never a refund");
    assert.equal(rows[0]?.transport_started, true);
    context.cases.push("matrix_synthetic_paid_presemantic_eof_stays_unknown_without_openlux");
    return;
  }
  assert(text.includes("response.completed") && !text.includes("response.failed"));
  const provider = kind === "capacity" ? "metered" : "surplus";
  const completed = rows.find((row) => object(row.reservation).provider === provider);
  assert(completed);
  assert.equal(completed.billing_state, "settled");
  assert.equal(completed.spend_microcredits, kind === "capacity" ? 40 : 5);
  assert.equal(completed.terminal_state, "completed");
  context.cases.push(
    kind === "capacity" ? "matrix_synthetic_authoritative_paid_capacity_allows_openlux" : "matrix_synthetic_direct_failure_preserves_existing_surplus_admission"
  );
}

export async function precommitPaidCases(context: MatrixContext): Promise<void> {
  await selectProviders(context, ["ubiquity", "lithos", "openrouter"]);
  const headers = { authorization: `Bearer ${context.clientToken ?? "fixture-access"}` };
  await responseObject(await fetch(context.url("/v1/models?client_version=0.163.0"), { headers }));
  await responseObject(await fetch(context.url("/v1/models"), { headers }));
  await paidBoundary(context, "surplus");
  await paidBoundary(context, "capacity");
  await paidBoundary(context, "paid-eof");
}
