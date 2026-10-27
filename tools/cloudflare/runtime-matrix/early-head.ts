import assert from "node:assert/strict";
import { MATRIX_MODELS } from "../fixtures/provider-matrix.mjs";
import {
  createClient,
  dispatches,
  object,
  post,
  responseObject,
  selectProviders,
  settled,
  stringKey,
  type MatrixContext,
  type MatrixHarness,
} from "./contracts.ts";

export const EARLY_HEAD_CASE_NAMES = [
  "matrix_v6_mixed400_becomes_truthful_failed_sse_with_original_http_log",
  "matrix_v6_mixed503_becomes_truthful_failed_sse_with_original_http_log",
  "matrix_v6_fallback_answer_has_common_headers_and_true_internal_provider",
  "matrix_v6_no200_chain_retains_original_http_error",
  "matrix_v6_json_head_never_publishes_provisional_sse",
  "matrix_v6_direct_stream_retains_selected_headers_and_no_synthetic_chain",
  "matrix_v6_nonstream_retains_json_and_selected_headers",
  "matrix_v6_chat_retains_selected_headers_and_chat_error_shape",
];
function commonOnly(response: Response): void {
  for (const name of ["x-uos-upstream", "x-uos-provider-request-id", "x-uos-attempted-providers", "retry-after", "ratelimit", "ratelimit-policy"]) {
    assert.equal(response.headers.get(name), null, `Provisional ${name} must not name an unknown final provider`);
  }
  assert(response.headers.get("x-uos-request-id"));
}
async function requestLog(context: MatrixContext, keyId: string, requestId: string): Promise<Record<string, unknown>> {
  const deadline = Date.now() + 3000;
  do {
    const page = await context.rpc({
      operation: "query",
      selector: { kind: "prefix", prefix: stringKey("uos_ai", "paid_fallback", "ledger", keyId), start: null, end: null },
      reverse: false,
      limit: 10,
      cursor: null,
      now_ms: Date.now(),
    });
    assert(Array.isArray(page.entries));
    for (const entry of page.entries.map(object)) {
      const row = object(object(entry.record).value);
      if (row.id === requestId && typeof row.completed_at_ms === "number") return row;
    }
    await Bun.sleep(10);
  } while (Date.now() < deadline);
  throw new Error("The actual terminal request log did not complete");
}
async function synthetic(context: MatrixContext, mode: "mixed400" | "mixed503" | "success" | "no200" | "jsonhead"): Promise<void> {
  await selectProviders(context, ["ubiquity", "lithos", "openrouter"]);
  const key = await createClient(context);
  const client = { ...context, clientToken: key.token };
  const marker = `fixture:matrix:early:${mode}`;
  const response = await post(client, "/v1/responses", { model: "ubiquity/deepseek-flash", input: marker, stream: true });
  if (mode === "no200") {
    assert.equal(response.status, 400);
    assert(response.headers.get("content-type")?.startsWith("application/json"));
    assert.equal(response.headers.get("x-uos-upstream"), "openrouter");
    await response.arrayBuffer();
    assert.equal((await settled(client, response)).finished, true);
    context.cases.push(EARLY_HEAD_CASE_NAMES[3]);
    return;
  }
  assert.equal(response.status, 200);
  assert(response.headers.get("content-type")?.startsWith("text/event-stream"));
  const text = await response.text();
  const events = [...text.matchAll(/^data: (.+)$/gmu)].map((match) => object(JSON.parse(match[1])));
  const state = await settled(client, response);
  assert.equal(state.finished, true);
  assert(Array.isArray(state.attempts));
  assert(state.attempts.map(object).every((attempt) => attempt.paid === null && attempt.transport_started === true));
  if (mode === "jsonhead") {
    assert.equal(response.headers.get("x-uos-upstream"), "lithos");
    assert(text.includes("response.completed"));
    assert.deepEqual(
      dispatches(await context.events(), marker).map((call) => call.provider),
      ["lithos"]
    );
    context.cases.push(EARLY_HEAD_CASE_NAMES[4]);
    return;
  }
  commonOnly(response);
  assert.deepEqual(
    dispatches(await context.events(), marker).map((call) => call.provider),
    ["lithos", "openrouter"]
  );
  const id = response.headers.get("x-uos-request-id");
  assert(id);
  const log = await requestLog(context, key.id, id);
  assert.equal(log.provider, "openrouter");
  assert.equal(log.status_code, 200);
  if (mode === "success") {
    assert(text.includes("matrix answer") && text.includes("response.completed"));
    assert(!text.includes("chatcmpl_early_head"));
    context.cases.push(EARLY_HEAD_CASE_NAMES[2]);
    return;
  }
  assert.deepEqual(
    events.map((event) => event.type),
    ["response.created", "response.failed"]
  );
  assert.deepEqual(
    events.map((event) => event.sequence_number),
    [0, 1]
  );
  const created = object(events[0].response);
  const failed = object(events[1].response);
  assert.equal(failed.id, created.id);
  assert.equal(failed.status, "failed");
  assert.equal(failed.usage, null);
  assert.deepEqual(failed.output, []);
  assert.deepEqual(failed.error, { code: "matrix_head_refusal", message: mode });
  const original = object(log.native_http_failure);
  assert.equal(original.status, mode === "mixed400" ? 400 : 503);
  assert.equal(original.body_complete, true);
  assert.equal(object(original.headers)["retry-after"], "17");
  assert.equal(object(original.error).code, "matrix_head_refusal");
  assert(!state.semantic_committed);
  context.cases.push(EARLY_HEAD_CASE_NAMES[mode === "mixed400" ? 0 : 1]);
}
async function unchanged(context: MatrixContext, mode: "direct" | "nonstream" | "chat"): Promise<void> {
  await selectProviders(context, ["ubiquity", "lithos", "openrouter"]);
  const marker = `fixture:matrix:early:${mode}`;
  const body =
    mode === "chat"
      ? { model: MATRIX_MODELS.lithos, messages: [{ role: "user", content: marker }], stream: true }
      : { model: mode === "direct" ? MATRIX_MODELS.lithos : "ubiquity/deepseek-flash", input: marker, stream: mode !== "nonstream" };
  const response = await post(context, mode === "chat" ? "/v1/chat/completions" : "/v1/responses", body);
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("x-uos-upstream"), "lithos");
  if (mode === "nonstream") assert.equal((await responseObject(response)).status, "completed");
  else {
    const text = await response.text();
    assert(!text.includes("response.completed"));
    if (mode === "chat") assert(!text.includes("response.failed") && text.includes('"error"'));
  }
  assert.equal((await settled(context, response)).finished, true);
  assert.deepEqual(
    dispatches(await context.events(), marker).map((call) => call.provider),
    ["lithos"]
  );
  context.cases.push(EARLY_HEAD_CASE_NAMES[mode === "direct" ? 5 : mode === "nonstream" ? 6 : 7]);
}
export async function runEarlyHeadWireCases(harness: MatrixHarness): Promise<void> {
  for (const mode of ["mixed400", "mixed503", "success", "no200", "jsonhead"] as const)
    await harness.withScenario("precommit", (context) => synthetic(context, mode));
  for (const mode of ["direct", "nonstream", "chat"] as const) await harness.withScenario("precommit", (context) => unchanged(context, mode));
}
