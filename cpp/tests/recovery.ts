import assert from "node:assert/strict";
import { mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { conformanceRequest, openConformanceResponse, type ConformanceEvent } from "../../tests/helpers/responses-conformance-client.ts";
import { startConformanceMock, closeMockEnvironment } from "./mock.ts";
import { startCandidate, SYNTHETIC_TOKEN } from "./candidate-process.ts";

const executable = process.argv[2];
assert.ok(executable);
const value: unknown = JSON.parse(process.argv[3] ?? "[]");
assert.ok(Array.isArray(value) && value.every((part): part is string => typeof part === "string"));
const prefix: string[] = value;
const directory = await mkdtemp(join(tmpdir(), "uos-cpp-recovery-"));
const phases = ["after_reserve_fsync", "after_dispatch_intent_fsync", "after_terminal_intent_fsync", "after_settlement_fsync"];
const receipts: unknown[] = [];
try {
  for (const phase of phases) {
    const root = join(directory, phase);
    const stateDirectory = join(root, "state");
    const mock = await startConformanceMock();
    let candidate = await startCandidate({ executable, prefix, directory: join(root, "original"), stateDirectory, providerOrigin: mock.origin });
    try {
      const armed = await candidate.control.send("ARM_FENCE", { request_ordinal: 1, phase });
      assert.equal(armed.kind, "FENCE_ARMED");
      const flow = (async () => {
        const response = await openConformanceResponse(candidate.endpoint, SYNTHETIC_TOKEN, conformanceRequest());
        const delta = await response.waitFor("response.function_call_arguments.delta");
        mock.receipt(1, delta);
        return await response.finished;
      })().then(
        (capture) => ({ capture }),
        (error: unknown) => ({ error: error instanceof Error ? error.message : String(error) })
      );
      const reached = await candidate.control.wait("FENCE_REACHED");
      const before = await candidate.control.send("READ_STATE", { request_id: reached.request_id });
      const abrupt = await candidate.finish("SIGKILL");
      assert.equal(abrupt.signal, "SIGKILL");
      const client = await flow;
      const postsAtCrash = mock.calls.length;
      candidate = await startCandidate({ executable, prefix, directory: join(root, "restart-one"), stateDirectory, providerOrigin: mock.origin });
      const after = await candidate.control.send("READ_STATE", { request_id: reached.request_id });
      const state = after.state as ConformanceEvent;
      assert.equal(mock.calls.length, postsAtCrash, "Startup cannot issue any provider request");
      assert.equal(state.quota_transactions, phase === "after_reserve_fsync" ? 1 : 2);
      assert.equal(state.committed_requests, phase === "after_reserve_fsync" ? 0 : 1);
      assert.equal(state.reserved_requests, phase === "after_reserve_fsync" ? 1 : 0);
      const row = state.request as ConformanceEvent;
      assert.equal(row.state, phase === "after_reserve_fsync" ? "reserved" : "dispatched");
      if (phase === "after_terminal_intent_fsync") {
        assert.equal(row.terminal, "response.completed");
        assert.equal(row.delivery, "unobserved");
        assert.equal(row.settled, true);
      }
      if (phase === "after_settlement_fsync") {
        assert.equal(row.settled, true);
        assert.equal(row.delivery, "delivered");
      }
      assert.equal((await candidate.finish("SIGTERM")).code, 0);
      candidate = await startCandidate({ executable, prefix, directory: join(root, "restart-two"), stateDirectory, providerOrigin: mock.origin });
      const repeated = await candidate.control.send("READ_STATE", { request_id: reached.request_id });
      assert.deepEqual(repeated.state, after.state, "A second restart must not apply settlement twice");
      assert.equal(mock.calls.length, postsAtCrash);
      receipts.push({ phase, reached, before, abrupt, client, after, repeated, postsAtCrash, upstream: mock.calls, chronology: mock.journal.entries });
    } finally {
      assert.equal((await candidate.finish("SIGTERM")).code, 0);
      await mock.stop();
    }
  }
} finally {
  await closeMockEnvironment();
  await writeFile(join(directory, "recovery.json"), JSON.stringify(receipts, null, 2));
}
console.log(JSON.stringify({ status: "passed", directory, phases, actualProcessRestarts: phases.length * 2, startupReplays: 0 }));
