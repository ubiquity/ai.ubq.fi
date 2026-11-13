import { test } from "bun:test";
import assert from "node:assert/strict";
import { mkdtemp } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import {
  CONFORMANCE_ARGUMENTS,
  CONFORMANCE_CALL_ID,
  CONFORMANCE_FINAL,
  completedConformanceResponse,
  conformanceContinuation,
  conformanceRecord,
  conformanceRequest,
  openConformanceResponse,
  runConformanceRoundTrip,
  type ConformanceEvent,
  type ConformanceWireCapture,
} from "./helpers/responses-conformance-client.ts";
import { startConformanceMock, startResponsesConformanceFixture } from "./helpers/responses-conformance-fixture.ts";

type Fixture = Awaited<ReturnType<typeof startResponsesConformanceFixture>>;

const fixtureDirectory = async (): Promise<string> => {
  const path = await mkdtemp(join(tmpdir(), "uos-responses-conformance-"));
  console.warn(`Synthetic Responses captures: ${path}`);
  return path;
};

const terminalFor = (fixture: Fixture, capture: ConformanceWireCapture) => {
  const requestId = capture.headers["x-uos-request-id"];
  assert.ok(requestId, "The real handler must assign a request identity");
  return fixture.journal.wait((entry) => entry.kind === "request_terminal" && entry.detail.request_id === requestId).then((entry) => entry.detail);
};

const assertOneDispatchCharge = async (fixture: Fixture, capture: ConformanceWireCapture, committed: number): Promise<void> => {
  const requestId = capture.headers["x-uos-request-id"];
  assert.ok(requestId);
  const rows = await fixture.ledger(requestId);
  assert.equal(rows.request?.state, "dispatched");
  assert.equal(rows.window?.committed_requests, committed);
  assert.equal(rows.window.reserved_requests, 0);
  assert.equal(rows.paidFallback, "not_applicable");
  const writes = fixture.journal.entries
    .filter((entry) => entry.kind === "quota_commit")
    .flatMap((entry) => {
      const operations = entry.detail.writes;
      assert.ok(Array.isArray(operations));
      return operations.map(conformanceRecord);
    });
  const dispatched = writes.filter((write) => {
    const key = write.key;
    return Array.isArray(key) && key.at(-1) === requestId && conformanceRecord(write.value).state === "dispatched";
  });
  assert.equal(dispatched.length, 1, "One successful native CAS dispatch transition must charge the request once");
  assert.equal(fixture.terminals.filter((terminal) => terminal.request_id === requestId).length, 1);
};

const assertSequence = (capture: ConformanceWireCapture): void => {
  const sequence = capture.events.map((event) => event.sequence_number);
  assert.ok(sequence.every((number) => typeof number === "number"));
  for (let index = 1; index < sequence.length; index++) assert.equal(sequence[index], sequence[index - 1] + 1);
};

const assertDurableReopen = (fixture: Fixture, captures: ConformanceWireCapture[], dispatches: number): void => {
  assert.equal(fixture.storage.mode, "file");
  assert.equal(fixture.storage.codec, "bun-jsc-v1");
  const proof = fixture.verifyReopenedLedger();
  assert.equal(proof.outcome, "verified");
  assert.equal(proof.dispatchesBefore, dispatches);
  assert.equal(proof.dispatchesAfter, dispatches);
  assert.equal(proof.quotaCommitsBefore, proof.quotaCommitsAfter);
  assert.deepEqual(proof.after, proof.before);
  assert.ok(Array.isArray(proof.after));
  assert.deepEqual(
    proof.after.map((row) => conformanceRecord(row).requestId),
    captures.map((capture) => capture.headers["x-uos-request-id"])
  );
  for (const row of proof.after.map(conformanceRecord)) {
    assert.equal(conformanceRecord(row.request).state, "dispatched");
    assert.equal(conformanceRecord(row.window).committed_requests, dispatches);
    assert.equal(conformanceRecord(row.window).reserved_requests, 0);
    assert.equal(typeof row.requestVersionstamp, "string");
    assert.equal(typeof row.windowVersionstamp, "string");
  }
  assert.deepEqual(fixture.verifyReopenedLedger(), proof, "Repeated verification must be a read-only receipt lookup");
  const events = fixture.journal.entries.map((entry) => entry.kind);
  assert.ok(events.indexOf("transport_drained") < events.indexOf("kv_closed"));
  assert.ok(events.indexOf("kv_closed") < events.indexOf("kv_reopened_verified"));
};

test("source Responses real sockets: split tool arguments, same call result and progressive final text", async () => {
  const fixture = await startResponsesConformanceFixture({ captureDirectory: await fixtureDirectory() });
  const captures: ConformanceWireCapture[] = [];
  try {
    const firstBody = conformanceRequest({ model: "deepseek-v4-flash" });
    const first = await openConformanceResponse(fixture.endpoint, fixture.token, firstBody);
    const argumentsDelta = await first.waitFor("response.function_call_arguments.delta");
    assert.equal(argumentsDelta.delta, '{"text":"fixture-');
    assert.equal(
      fixture.journal.entries.some((entry) => entry.kind === "provider_resumed"),
      false
    );
    assert.equal(
      first.capture.events.some((event) => event.type === "response.completed"),
      false
    );
    fixture.acknowledge(1, argumentsDelta);
    captures.push(await first.finished);
    const firstTerminal = completedConformanceResponse(captures[0]);
    const output = firstTerminal.output;
    assert.ok(Array.isArray(output));
    const call = output.map(conformanceRecord).find((item) => item.type === "function_call");
    assert.ok(call);
    assert.equal(call.call_id, CONFORMANCE_CALL_ID);
    assert.equal(call.arguments, CONFORMANCE_ARGUMENTS);
    assert.equal(call.status, "completed");
    assert.equal(firstTerminal.model, "deepseek-v4-flash", "Downstream preserves the requested alias");
    assert.deepEqual(conformanceRecord(firstTerminal.usage).input_tokens_details, { cached_tokens: 0 });
    assert.equal((await terminalFor(fixture, captures[0])).delivery_outcome, "delivered");
    await assertOneDispatchCharge(fixture, captures[0], 1);

    const second = await openConformanceResponse(fixture.endpoint, fixture.token, conformanceContinuation(firstBody, captures[0]));
    const textDelta = await second.waitFor("response.output_text.delta");
    assert.equal(textDelta.delta, "fixture-");
    assert.equal(
      second.capture.events.some((event) => event.type === "response.completed"),
      false
    );
    fixture.acknowledge(2, textDelta);
    captures.push(await second.finished);
    const secondTerminal = completedConformanceResponse(captures[1]);
    assert.ok(Array.isArray(secondTerminal.output));
    const content = conformanceRecord(secondTerminal.output[0]).content;
    assert.ok(Array.isArray(content));
    assert.equal(conformanceRecord(content[0]).text, CONFORMANCE_FINAL);
    assert.equal(Object.hasOwn(conformanceRecord(secondTerminal.usage), "input_tokens_details"), false, "Missing cache usage stays missing");
    assert.equal((await terminalFor(fixture, captures[1])).delivery_outcome, "delivered");
    await assertOneDispatchCharge(fixture, captures[1], 2);
    assert.equal(fixture.calls.length, 2);
    for (const capture of captures) assertSequence(capture);
    const forwarded = conformanceRecord(JSON.parse(fixture.calls[1].body) as unknown);
    assert.equal(forwarded.model, "deepseek-flash");
    assert.equal(Object.hasOwn(forwarded, "previous_response_id"), false, "The deployed Chat translator is stateless");
    assert.ok(Array.isArray(forwarded.messages));
    const messages = forwarded.messages.map(conformanceRecord);
    const assistant = messages.find((message) => message.role === "assistant");
    assert.ok(assistant && Array.isArray(assistant.tool_calls));
    assert.equal(conformanceRecord(assistant.tool_calls[0]).id, CONFORMANCE_CALL_ID);
    assert.equal(conformanceRecord(conformanceRecord(assistant.tool_calls[0]).function).arguments, CONFORMANCE_ARGUMENTS);
    assert.deepEqual(
      messages.find((message) => message.role === "tool"),
      { role: "tool", tool_call_id: CONFORMANCE_CALL_ID, content: "fixture-tool-ok" }
    );
    assert.ok(messages.some((message) => message.role === "system" && String(message.content).includes("Keep the exact fixture values.")));
    assert.equal(fixture.controller.snapshot().active, 0);
  } finally {
    await fixture.save(captures);
    await fixture.stop();
  }
  assertDurableReopen(fixture, captures, 2);
}, 30_000);

test("source Responses real socket disconnect cancels held upstream and settles exactly once", async () => {
  const fixture = await startResponsesConformanceFixture({ captureDirectory: await fixtureDirectory() });
  let capture: ConformanceWireCapture | null = null;
  try {
    const response = await openConformanceResponse(fixture.endpoint, fixture.token, conformanceRequest());
    await response.waitFor("response.function_call_arguments.delta");
    assert.equal(
      fixture.journal.entries.some((entry) => entry.kind === "provider_resumed"),
      false
    );
    fixture.journal.add("client_socket_destroyed");
    response.disconnect();
    capture = await response.finished;
    assert.equal(capture.outcome, "interrupted");
    await fixture.journal.wait((entry) => entry.kind === "provider_cancelled");
    const terminal = await terminalFor(fixture, capture);
    assert.equal(terminal.delivery_outcome, "interrupted");
    assert.equal(fixture.calls.length, 1);
    assert.equal(
      fixture.journal.entries.some((entry) => entry.kind === "provider_resumed"),
      false
    );
    assert.equal(
      capture.events.some((event) => event.type === "response.completed"),
      false
    );
    await assertOneDispatchCharge(fixture, capture, 1);
    assert.equal(fixture.controller.snapshot().active, 0);
    assert.equal(fixture.controller.snapshot().waiting, 0);
    const chronology = fixture.journal.entries.map((entry) => entry.kind);
    assert.ok(chronology.indexOf("client_socket_destroyed") < chronology.indexOf("provider_cancelled"));
  } finally {
    await fixture.save(capture);
    await fixture.stop();
  }
  assert.ok(capture);
  assertDurableReopen(fixture, [capture], 1);
}, 15_000);

test("source accepts a 240KiB synthetic history without altering its forwarded fields", async () => {
  const fixture = await startResponsesConformanceFixture({ captureDirectory: await fixtureDirectory() });
  const captures: ConformanceWireCapture[] = [];
  try {
    const input = Array.from({ length: 4 }, (_, index) => ({ role: "user", content: `history-${index}: ${"x".repeat(60 * 1_024)}` }));
    const body = { ...conformanceRequest(), input };
    captures.push(...(await runConformanceRoundTrip({ endpoint: fixture.endpoint, token: fixture.token, request: body, acknowledge: fixture.acknowledge })));
    const upstream = conformanceRecord(JSON.parse(fixture.calls[0].body) as unknown);
    assert.ok(Array.isArray(upstream.messages));
    const history = upstream.messages.map(conformanceRecord).filter((message) => message.role === "user");
    assert.deepEqual(
      history.map((message) => message.content),
      input.map((message) => message.content)
    );
    assert.ok(Buffer.byteLength(fixture.calls[0].body) > 240 * 1_024);
    for (const capture of captures) await terminalFor(fixture, capture);
  } finally {
    await fixture.save(captures);
    await fixture.stop();
  }
}, 15_000);

const rejected = async (fixture: Fixture, body: ConformanceEvent, token = fixture.token): Promise<ConformanceWireCapture> => {
  const response = await openConformanceResponse(fixture.endpoint, token, body);
  const capture = await response.finished;
  assert.ok(capture.status >= 400 && capture.status < 500);
  assert.equal(fixture.calls.length, 0, "Authentication, validation and quota refusals must precede provider dispatch");
  return capture;
};

test("source synthetic auth, validation, revocation and exhausted quota refuse before dispatch", async () => {
  const fixture = await startResponsesConformanceFixture({ captureDirectory: await fixtureDirectory() });
  const captures: ConformanceWireCapture[] = [];
  try {
    captures.push(await rejected(fixture, conformanceRequest(), "invalid-fixture-key"));
    captures.push(await rejected(fixture, { ...conformanceRequest(), max_output_tokens: 0 }));
    await fixture.revoke();
    captures.push(await rejected(fixture, conformanceRequest()));
    assert.deepEqual(
      captures.map((capture) => capture.status),
      [401, 400, 401]
    );
  } finally {
    await fixture.save(captures);
    await fixture.stop();
  }
  const exhausted = await startResponsesConformanceFixture({ usageLimit: 0, captureDirectory: await fixtureDirectory() });
  try {
    await exhausted.save(await rejected(exhausted, conformanceRequest()));
  } finally {
    await exhausted.stop();
  }
}, 15_000);

test("shared mock and gateway have separate owners for external-target measurement", async () => {
  const mock = await startConformanceMock();
  const gateway = await startResponsesConformanceFixture({ providerOrigin: mock.origin });
  try {
    const response = await openConformanceResponse(gateway.endpoint, gateway.token, conformanceRequest());
    mock.receipt(1, await response.waitFor("response.function_call_arguments.delta"));
    const capture = await response.finished;
    completedConformanceResponse(capture);
    await terminalFor(gateway, capture);
    assert.equal(mock.calls.length, 1);
    assert.equal(gateway.calls.length, 0, "The gateway never creates or owns the external mock");
    await assertOneDispatchCharge(gateway, capture, 1);
  } finally {
    await gateway.stop();
    await mock.stop();
  }
}, 15_000);

test("source records the namespace collision replay name without silently repairing it", async () => {
  const fixture = await startResponsesConformanceFixture({ toolName: "clock_fixture_echo", captureDirectory: await fixtureDirectory() });
  const captures: ConformanceWireCapture[] = [];
  try {
    captures.push(
      ...(await runConformanceRoundTrip({
        endpoint: fixture.endpoint,
        token: fixture.token,
        request: conformanceRequest({ namespace: "clock" }),
        acknowledge: fixture.acknowledge,
      }))
    );
    const terminal = completedConformanceResponse(captures[0]);
    assert.ok(Array.isArray(terminal.output));
    const call = terminal.output.map(conformanceRecord).find((item) => item.type === "function_call");
    assert.equal(call?.name, "fixture_echo");
    assert.equal(call.namespace, "clock");
    const first = conformanceRecord(JSON.parse(fixture.calls[0].body) as unknown);
    assert.ok(Array.isArray(first.tools));
    assert.deepEqual(
      first.tools.map((tool) => conformanceRecord(conformanceRecord(tool).function).name),
      ["fixture_echo", "clock_fixture_echo"]
    );
    const second = conformanceRecord(JSON.parse(fixture.calls[1].body) as unknown);
    assert.ok(Array.isArray(second.messages));
    const assistant = second.messages.map(conformanceRecord).find((message) => message.role === "assistant");
    assert.ok(assistant && Array.isArray(assistant.tool_calls));
    // Deployed chatToolCallItem ignores the returned namespace on history replay.
    assert.equal(conformanceRecord(conformanceRecord(assistant.tool_calls[0]).function).name, "fixture_echo");
    for (const capture of captures) await terminalFor(fixture, capture);
  } finally {
    await fixture.save(captures);
    await fixture.stop();
  }
}, 15_000);

const exerciseMockDispatchBound = async (maxDispatches?: number): Promise<number> => {
  const fixture = await startResponsesConformanceFixture({ maxDispatches, captureDirectory: await fixtureDirectory() });
  const captures: ConformanceWireCapture[] = [];
  const maximum = maxDispatches ?? 4;
  try {
    for (let turn = 1; turn <= maximum; turn++) {
      const response = await openConformanceResponse(fixture.endpoint, fixture.token, conformanceRequest());
      const event = await response.waitFor(turn === 1 ? "response.function_call_arguments.delta" : "response.output_text.delta");
      fixture.acknowledge(turn, event);
      const capture = await response.finished;
      completedConformanceResponse(capture);
      await terminalFor(fixture, capture);
      captures.push(capture);
    }
    const refusal = await openConformanceResponse(fixture.endpoint, fixture.token, conformanceRequest());
    const refused = await refusal.finished;
    captures.push(refused);
    assert.ok(refused.status >= 500, "An exhausted mock must refuse the next actual transport request");
    assert.equal(fixture.calls.length, maximum);
    assert.equal(fixture.journal.entries.filter((entry) => entry.kind === "mock_error").length, 1);
    return captures.filter((capture) => capture.events.some((event) => event.type === "response.output_text.delta")).length;
  } finally {
    await fixture.save(captures);
    await fixture.stop();
  }
};

test("warm source fixture permits more than four text dispatches with a finite override and preserves the default bound", async () => {
  assert.equal(await exerciseMockDispatchBound(), 3);
  assert.equal(await exerciseMockDispatchBound(6), 5);
}, 15_000);
