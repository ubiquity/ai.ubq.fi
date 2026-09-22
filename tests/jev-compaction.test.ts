import assert from "node:assert/strict";
import {
  buildCompactionResponse,
  CompactionUnavailable,
  handleJevResponsesCompaction,
  isJevCompactionRequest,
  setJevCompactionAskerForTest,
} from "../src/jev_compaction/compaction.ts";
import { MAX_SUMMARY_CHARS, parseCodexInput, renderSummary, SUMMARY_MARKER } from "../lib/jev_compaction/codex_items.ts";
import { collectToolCalls } from "../lib/jev_compaction/state.ts";
import type { JevAsker, JevQuestions, JevResponse } from "../lib/jev_compaction/types.ts";
import { getResponseTelemetry } from "../src/openai.ts";

/**
 * Focused regression coverage for the gateway Jev compaction slice: the
 * header-only predicate, the verbatim renderer, and every fail-closed path.
 * No network, no credential: every asker is injected.
 */

const COMPACTION_METADATA = JSON.stringify({ request_kind: "compaction", compaction: { implementation: "responses" } });
const COMPACTION_PROMPT = "You are performing a CONTEXT CHECKPOINT COMPACTION. Create a handoff summary for another LLM.";
const KEPT_MARKER = "KEEP_MARKER_release_notes_artifact";

const text = (role: string, value: string) => ({ type: "message", role, content: [{ type: "input_text", text: value }] });
const call = (callId: string, command: string) => ({ type: "function_call", call_id: callId, name: "shell", arguments: JSON.stringify({ command }) });
const output = (callId: string, value: string) => ({ type: "function_call_output", call_id: callId, output: value });

/** Three obsolete unpinned call/result pairs, a pinned keep marker and the injected prompt. */
const transcript = (): unknown[] => [
  text("user", `Goal: finish the release notes and keep ${KEPT_MARKER} verbatim.`),
  text("assistant", "Starting with the old build logs."),
  call("old_a", "tail -n 400 /tmp/old/build.log"),
  output("old_a", `RESULT_A obsolete build log line. `.repeat(60)),
  call("old_b", "grep -c deprecated /tmp/old/metrics.csv"),
  output("old_b", `RESULT_B obsolete metric line. `.repeat(60)),
  call("old_c", "ls -la /tmp/old/cache"),
  output("old_c", `RESULT_C obsolete cache line. `.repeat(60)),
  text("assistant", "The old investigation is finished; those logs are obsolete."),
  text("user", "Recent follow-up one: draft the release notes outline."),
  text("assistant", "Drafted the outline."),
  text("user", "Recent follow-up two: check the artifact wording."),
  text("assistant", "Checked."),
  text("user", `Final goal: ${KEPT_MARKER} stays verbatim.`),
  text("user", COMPACTION_PROMPT),
];

const body = (input: unknown[], stream = true): Record<string, unknown> => ({ model: "deepseek-flash", stream, input });

type CallVerdict = Readonly<{ keepCall: number; keepResult: number }>;

/** Answers every `call_/result_` question from the verdict map. */
const decisionAsker = (verdicts: Readonly<Record<string, CallVerdict | undefined>>, options: Readonly<{ partial?: boolean }> = {}): JevAsker => ({
  ask(_state, questions: JevQuestions): Promise<JevResponse> {
    const answers: Record<string, { noul: number }> = {};
    for (const name of Object.keys(questions)) {
      if (options.partial && name.startsWith("result_")) continue;
      const match = /^(?<kind>call|result)_(?<id>t\d+)$/.exec(name) as { groups: { kind: string; id: string } } | null;
      if (!match) continue;
      const { id, kind } = match.groups;
      const lookup = verdicts[id];
      const verdict = lookup ?? { keepCall: 1, keepResult: 1 };
      answers[name] = { noul: kind === "call" ? verdict.keepCall : verdict.keepResult };
    }
    return Promise.resolve({ answers });
  },
});

const throwAsker = (message: string): JevAsker => ({
  ask(): Promise<JevResponse> {
    return Promise.reject(new Error(message));
  },
});

const compactionRequest = (input: unknown[], stream = true, extraHeaders: Record<string, string> = {}): Request =>
  new Request("http://127.0.0.1:7999/v1/responses", {
    method: "POST",
    headers: { "content-type": "application/json", "x-codex-turn-metadata": COMPACTION_METADATA, ...extraHeaders },
    body: JSON.stringify(body(input, stream)),
  });

Deno.test("jev compaction predicate is header-only and exact", async () => {
  const unmarked = new Request("http://127.0.0.1:7999/v1/responses", { method: "POST", body: JSON.stringify(body(transcript())) });
  assert.equal(isJevCompactionRequest(unmarked), false);
  assert.equal(await unmarked.text(), JSON.stringify(body(transcript())), "an unmarked request body must stay unread");

  const malformed = compactionRequest(transcript(), true, { "x-codex-turn-metadata": "{not json" });
  assert.equal(isJevCompactionRequest(malformed), false);

  const v2 = compactionRequest(transcript(), true, {
    "x-codex-turn-metadata": JSON.stringify({ request_kind: "compaction", compaction: { implementation: "responses_compaction_v2" } }),
  });
  assert.equal(isJevCompactionRequest(v2), false);

  const ordinaryTurn = compactionRequest(transcript(), true, {
    "x-codex-turn-metadata": JSON.stringify({ request_kind: "turn", compaction: { implementation: "responses" } }),
  });
  assert.equal(isJevCompactionRequest(ordinaryTurn), false);

  const marked = compactionRequest(transcript());
  assert.equal(isJevCompactionRequest(marked), true);
  assert.equal(await marked.text(), JSON.stringify(body(transcript())), "the predicate must not consume the body");
});

Deno.test("renderer applies Jev decisions verbatim and drops selected content", async () => {
  const verdicts = {
    t1: { keepCall: 0.1, keepResult: 0.1 },
    t2: { keepCall: 0.9, keepResult: 0.1 },
    t3: { keepCall: 0.9, keepResult: 0.9 },
  };
  const outcome = await buildCompactionResponse(body(transcript()), decisionAsker(verdicts), { stream: true });
  assert.equal(outcome.status, 200);
  assert.equal(outcome.contentType, "text/event-stream");
  assert.match(outcome.headers["x-jev-compaction"] ?? "", /calls_dropped=1/);
  assert.match(outcome.headers["x-jev-compaction"] ?? "", /results_dropped=1/);

  const summary = outcome.body;
  assert.ok(summary.includes(SUMMARY_MARKER), "the summary marker must be present");
  assert.ok(summary.includes(KEPT_MARKER), "pinned text must be kept verbatim");
  assert.ok(summary.includes("[tool result old_c — kept verbatim]"), "kept results must be labelled verbatim");
  assert.ok(summary.includes("[tool result old_b — truncated, original was"), "dropped results must be truncated, not removed");
  assert.ok(!summary.includes("[tool result old_a"), "a dropped call's result must not survive");
  assert.ok(!summary.includes("RESULT_A"), "a dropped call's payload must not survive");
  assert.ok(!summary.includes("[tool call old_a]"), "the dropped call line must not survive");
  assert.ok(summary.includes("event: response.completed"), "a streamed compaction must terminate with response.completed");
  assert.ok(!summary.includes(COMPACTION_PROMPT.split(" ")[0] + " " + "are performing"), "the injected prompt must be excluded from the state");

  const buffered = await buildCompactionResponse(body(transcript()), decisionAsker(verdicts), { stream: false });
  assert.equal(buffered.contentType, "application/json");
  const parsed = JSON.parse(buffered.body) as { status?: string; output?: { content?: { text?: string }[] }[] };
  assert.equal(parsed.status, "completed");
  assert.ok((parsed.output?.[0]?.content?.[0]?.text ?? "").includes(KEPT_MARKER));
});

Deno.test("renderer marks kept calls and never rewrites kept text", () => {
  const parsed = parseCodexInput(transcript());
  assert.ok(parsed, "the synthetic transcript must parse");
  const calls = collectToolCalls(parsed.messages, 6);
  const decisions = calls.map((entry) => ({ id: entry.id, tool: entry.tool, keepCall: 1, keepResult: 1, action: "keep" as const, reason: "pinned" as const }));
  const summary = renderSummary(parsed, { decisions, callIds: new Map(calls.map((entry) => [entry.id, entry.tool_use_id])), headChars: 300 });
  assert.ok(!summary.includes("no decision (kept)"));
  assert.ok(summary.includes(KEPT_MARKER));
  assert.ok(summary.includes("[tool call old_c] shell — keep"));
});

Deno.test("compaction fails closed on malformed input, no candidates and no reduction", async () => {
  await assert.rejects(
    () => buildCompactionResponse({ model: "deepseek-flash" }, decisionAsker({}), { stream: true }),
    (error: unknown) => error instanceof CompactionUnavailable && error.kind === "missing-input"
  );
  await assert.rejects(
    () => buildCompactionResponse(body([text("user", "hello"), text("assistant", "hi")]), decisionAsker({}), { stream: true }),
    (error: unknown) => error instanceof CompactionUnavailable && error.kind === "no-candidates"
  );
  const keepEverything = decisionAsker({ t1: { keepCall: 1, keepResult: 1 }, t2: { keepCall: 1, keepResult: 1 }, t3: { keepCall: 1, keepResult: 1 } });
  await assert.rejects(
    () => buildCompactionResponse(body(transcript()), keepEverything, { stream: true }),
    (error: unknown) => error instanceof CompactionUnavailable && error.kind === "no-reduction"
  );
});

Deno.test("compaction fails closed on Jev error or a partial decision set", async () => {
  await assert.rejects(
    () => buildCompactionResponse(body(transcript()), throwAsker("Jev request failed (500): upstream"), { stream: true }),
    (error: unknown) => error instanceof CompactionUnavailable && error.kind === "jev-failed"
  );
  await assert.rejects(
    () => buildCompactionResponse(body(transcript()), decisionAsker({ t1: { keepCall: 0.1, keepResult: 0.1 } }, { partial: true }), { stream: true }),
    (error: unknown) => error instanceof CompactionUnavailable && error.kind === "jev-failed"
  );
});

Deno.test("compaction fails closed on an oversized summary", async () => {
  const oversized = [
    text("user", `Goal: ${"x".repeat(MAX_SUMMARY_CHARS + 1_000)}`),
    call("old", "echo old"),
    output("old", `RESULT_OBSOLETE `.repeat(40)),
    text("user", "recent one"),
    text("assistant", "recent two"),
    text("user", "recent three"),
    text("assistant", "recent four"),
    text("user", "recent five"),
    text("assistant", "recent six"),
    text("user", COMPACTION_PROMPT),
  ];
  await assert.rejects(
    () => buildCompactionResponse(body(oversized), decisionAsker({ t1: { keepCall: 0.1, keepResult: 0.1 } }), { stream: true }),
    (error: unknown) => error instanceof CompactionUnavailable && error.kind === "summary-too-large"
  );
});

Deno.test("gateway handler fails closed without a credential and on caller cancellation", async () => {
  const missingKey = await handleJevResponsesCompaction(compactionRequest(transcript()), { apiKey: null });
  assert.equal(missingKey.status, 503);
  const failure = (await missingKey.json()) as { error: { code?: string; message?: string } };
  assert.equal(failure.error.code, "jev_compaction_unavailable");
  assert.match(failure.error.message ?? "", /missing-key/);

  const cancelled = await handleJevResponsesCompaction(compactionRequest(transcript()), { signal: AbortSignal.abort(), asker: decisionAsker({}) });
  assert.equal(cancelled.status, 499);
  const cancelledBody = (await cancelled.json()) as { error?: { code?: string } };
  assert.equal(cancelledBody.error?.code, "request_cancelled");
});

Deno.test("gateway handler answers a marked request through the injected asker", async () => {
  const verdicts = { t1: { keepCall: 0.1, keepResult: 0.1 }, t2: { keepCall: 0.1, keepResult: 0.1 }, t3: { keepCall: 0.1, keepResult: 0.1 } };
  const response = await handleJevResponsesCompaction(compactionRequest(transcript()), { apiKey: "test-key", asker: decisionAsker(verdicts) });
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("content-type"), "text/event-stream");
  assert.ok(response.headers.get("x-jev-compaction"));
  const textBody = await response.text();
  assert.ok(textBody.includes(SUMMARY_MARKER));
  assert.ok(textBody.includes(KEPT_MARKER));
});

Deno.test("completion telemetry marks a local success without inventing usage", async () => {
  const verdicts = { t1: { keepCall: 0.1, keepResult: 0.1 } };
  const response = await handleJevResponsesCompaction(compactionRequest(transcript()), { apiKey: "test-key", asker: decisionAsker(verdicts) });
  assert.equal(response.status, 200);
  const telemetry = getResponseTelemetry(response);
  assert.ok(telemetry, "a local success must carry completion telemetry for the terminal wrapper");
  assert.equal(telemetry.completed, true);
  assert.equal(telemetry.semanticOutputObserved, true);
  assert.equal(telemetry.stream, true);
  assert.equal(telemetry.streamTerminalType, "response.completed");
  assert.equal(telemetry.provider, "gateway");
  assert.equal(telemetry.failureKind, null);
  assert.equal(telemetry.usageObserved, false);
  assert.equal(telemetry.inputTokens, null);
  assert.equal(telemetry.outputTokens, null);
  assert.equal(telemetry.totalTokens, null);

  const refused = await handleJevResponsesCompaction(compactionRequest([text("user", "short"), text("assistant", "short answer")]), {
    apiKey: "test-key",
    asker: decisionAsker({}),
  });
  assert.ok(refused.status >= 400);
  assert.equal(getResponseTelemetry(refused), null, "a failed compaction must not claim completion");
});

Deno.test("test asker seam routes a handler compaction through the injected asker", async () => {
  const verdicts = {
    t1: { keepCall: 0.1, keepResult: 0.1 },
    t2: { keepCall: 0.1, keepResult: 0.1 },
    t3: { keepCall: 0.1, keepResult: 0.1 },
  };
  setJevCompactionAskerForTest(decisionAsker(verdicts));
  try {
    const response = await handleJevResponsesCompaction(compactionRequest(transcript()), { apiKey: "test-seam" });
    assert.equal(response.status, 200);
    assert.ok((await response.text()).includes(SUMMARY_MARKER));
  } finally {
    setJevCompactionAskerForTest(null);
  }
});
