import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtemp, mkdir, readFile, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createInterface } from "node:readline";
import { createFixtureControl } from "./fixture-control.ts";
import {
  conformanceRequest,
  openConformanceResponse,
  runConformanceRoundTrip,
  runCodexConformance,
  createConformanceJournal,
} from "../../tests/helpers/responses-conformance-client.ts";
import { initializeTestNativeKv } from "../../scripts/test-native-kv.ts";
import { setKvForTest } from "../../src/kv.ts";
import { openKv } from "../../src/storage/kv.ts";

initializeTestNativeKv({
  artifactDirectory: "/home/codex/repos/ubiquity/ai.ubq.fi/.data/releases/d94fef52ccc76e0d5aa019154f11e47d5e10fba6/.native/kv",
  expectedProvenanceSha256: "57d7405783c9f34343f3d4fe089b3733effc079d9521cd341a3dc5bc0c87c201",
  inputsManifest: new URL("../../tools/native-kv/inputs.json", import.meta.url).pathname,
});
const driverKv = await openKv(":memory:");
setKvForTest(driverKv);
const { startConformanceMock } = await import("../../tests/helpers/responses-conformance-fixture.ts");
const { shutdownOptionalTelemetry } = await import("../../serve.ts");

const executable = process.argv[2];
assert.ok(executable, "A candidate executable is required");
const prefixValue: unknown = JSON.parse(process.argv[3] ?? "[]");
assert.ok(Array.isArray(prefixValue) && prefixValue.every((value): value is string => typeof value === "string"));
const prefix: string[] = prefixValue;
const native = process.argv.includes("--native");
const disconnect = process.argv.includes("--disconnect");
const controlled = process.argv.includes("--control");
const control = controlled ? createFixtureControl() : null;
const candidateJournal = createConformanceJournal();
const directory = await mkdtemp(join(tmpdir(), "uos-cpp-conformance-"));
const token = `u_${"d".repeat(64)}`;
const mock = await startConformanceMock({ holdToolDelta: !native });
const stateDirectory = join(directory, "state");
await mkdir(stateDirectory, { mode: 0o700 });
const configPath = join(directory, "config.json");
await writeFile(
  configPath,
  JSON.stringify({
    fixture_mode: true,
    short_deadlines: true,
    token,
    upstream_key: "fixture-provider-secret",
    upstream_origin: mock.origin,
    state_directory: stateDirectory,
    ...control?.config,
  }),
  { mode: 0o600 }
);
const argv = [...prefix, executable, "--config", configPath];
const child = spawn(argv[0], argv.slice(1), {
  stdio: control ? ["ignore", "pipe", "pipe", control.commandRead, control.replyWrite] : ["ignore", "pipe", "pipe"],
  env: {
    PATH: "/usr/bin:/bin",
    HOME: process.env.HOME ?? "/home/codex",
    TMPDIR: directory,
    ASAN_OPTIONS: "detect_leaks=1:halt_on_error=1:abort_on_error=1",
    UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
  },
});
if (control)
  child.once("spawn", () => {
    control.started();
  });
assert.ok(child.stdout && child.stderr);
const candidateOut = child.stdout;
const candidateError = child.stderr;
let stderr = "";
let stderrTail = "";
candidateError.on("data", (value: Buffer) => {
  stderr += value.toString();
  stderrTail += value.toString();
  let boundary = stderrTail.indexOf("\n");
  while (boundary >= 0) {
    const line = stderrTail.slice(0, boundary);
    stderrTail = stderrTail.slice(boundary + 1);
    if (line.startsWith('{"event":"request_terminal"')) candidateJournal.add("terminal", JSON.parse(line) as Record<string, unknown>);
    boundary = stderrTail.indexOf("\n");
  }
  if (stderr.length > 2 * 1024 * 1024) child.kill("SIGTERM");
});
const exit = new Promise<{ code: number | null; signal: NodeJS.Signals | null }>((resolve) => {
  child.once("exit", (code, signal) => {
    resolve({ code, signal });
  });
});
const lines = createInterface({ input: candidateOut });
const ready = new Promise<string>((resolve, reject) => {
  const deadline = setTimeout(() => {
    reject(new Error("C++ readiness exceeded five seconds"));
  }, 5000);
  child.once("error", reject);
  child.once("exit", () => {
    reject(new Error("C++ exited before readiness"));
  });
  lines.on("line", (line) => {
    const value: unknown = JSON.parse(line);
    if (typeof value === "object" && value !== null && "endpoint" in value && typeof value.endpoint === "string") {
      clearTimeout(deadline);
      resolve(value.endpoint);
    }
  });
});
let failure: unknown = null;
let captures: unknown;
try {
  const endpoint = await ready;
  if (control) {
    const armed = await control.send("ARM_FENCE", { request_ordinal: 1, phase: "after_reserve_fsync" });
    assert.equal(armed.kind, "FENCE_ARMED");
    const opening = openConformanceResponse(endpoint, token, conformanceRequest());
    const reached = await control.wait("FENCE_REACHED");
    const observed = await control.send("READ_STATE", { request_id: reached.request_id });
    assert.ok(typeof observed.state === "object" && observed.state !== null && "reserved_requests" in observed.state && "committed_requests" in observed.state);
    assert.equal(observed.state.reserved_requests, 1);
    assert.equal(observed.state.committed_requests, 0);
    assert.equal(mock.calls.length, 0);
    const revoked = await control.send("REVOKE_KEY", { key_id: "responses-conformance-key" });
    assert.equal(revoked.kind, "REVOKED");
    await control.send("RELEASE_FENCE", { fence_id: armed.fence_id });
    const response = await opening;
    const first = await response.waitFor("response.function_call_arguments.delta");
    mock.receipt(1, first);
    const completed = await response.finished;
    assert.equal(completed.status, 200);
    assert.equal(completed.events.at(-1)?.type, "response.completed");
    const next = await openConformanceResponse(endpoint, token, conformanceRequest());
    const refused = await next.finished;
    assert.equal(refused.status, 401);
    assert.equal(mock.calls.length, 1);
    captures = { observed, completed, refused, pipeReports: control.reports, controls: control.entries };
  } else if (disconnect) {
    const response = await openConformanceResponse(endpoint, token, conformanceRequest());
    await response.waitFor("response.function_call_arguments.delta");
    response.disconnect();
    captures = await response.finished;
    await mock.journal.wait((entry) => entry.kind === "provider_cancelled");
    const terminal = await candidateJournal.wait((entry) => entry.kind === "terminal");
    assert.equal(terminal.detail.semantic_terminal, "cancelled");
    assert.equal(terminal.detail.delivery_outcome, "interrupted");
    assert.equal(mock.calls.length, 1);
    assert.equal(
      mock.journal.entries.some((entry) => entry.kind === "provider_resumed"),
      false
    );
  } else if (native) {
    captures = await runCodexConformance({
      executable: "/home/codex/.codex/packages/standalone/releases/0.167.2-aarch64-unknown-linux-musl/bin/codex",
      expectedSha256: "b581df3989da98e36fccd7d40cc980727a9ee87bcf0e6ef52ffb2544d6a5302f",
      expectedVersion: "0.167.2",
      endpoint,
      token,
      captureDirectory: join(directory, "native"),
      onTextDelta: (event) => {
        mock.receipt(2, event);
      },
    });
  } else {
    captures = await runConformanceRoundTrip({
      endpoint,
      token,
      acknowledge: (turn, event) => {
        mock.receipt(turn, event);
      },
    });
    assert.equal(mock.calls.length, 2);
    const continuation: unknown = JSON.parse(mock.calls[1].body);
    assert.ok(typeof continuation === "object" && continuation !== null && "messages" in continuation && Array.isArray(continuation.messages));
    assert.ok(
      continuation.messages.some(
        (message: unknown) =>
          typeof message === "object" &&
          message !== null &&
          "role" in message &&
          message.role === "tool" &&
          "content" in message &&
          message.content === "fixture-tool-ok"
      )
    );
    const invalid = await openConformanceResponse(endpoint, token, { ...conformanceRequest(), max_output_tokens: 0 });
    const refusal = await invalid.finished;
    assert.equal(refusal.status, 400);
    const missingStream = { ...conformanceRequest() };
    delete missingStream.stream;
    for (const unsupported of [{ ...conformanceRequest(), stream: false }, missingStream]) {
      const request = await openConformanceResponse(endpoint, token, unsupported);
      const rejected = await request.finished;
      assert.equal(rejected.status, 400);
      const payload: unknown = JSON.parse(Buffer.concat(rejected.chunksBase64.map((chunk) => Buffer.from(chunk, "base64"))).toString());
      assert.ok(
        typeof payload === "object" &&
          payload !== null &&
          "error" in payload &&
          typeof payload.error === "object" &&
          payload.error !== null &&
          "param" in payload.error
      );
      assert.equal(payload.error.param, "stream");
    }

    assert.equal(mock.calls.length, 2);
  }
} catch (error) {
  failure = error;
} finally {
  child.kill("SIGTERM");
  const stopped = await Promise.race([
    exit,
    new Promise<never>((_, reject) =>
      setTimeout(() => {
        child.kill("SIGKILL");
        reject(new Error("C++ failed to drain"));
      }, 5000)
    ),
  ]);
  control?.close();
  lines.close();
  await mock.stop();
  await shutdownOptionalTelemetry();
  driverKv.close();
  setKvForTest(null);
  await writeFile(join(directory, "candidate.stderr"), stderr);
  await writeFile(
    join(directory, "receipt.json"),
    JSON.stringify(
      { argv, stopped, captures, upstream: mock.calls, chronology: mock.journal.entries, failure: failure instanceof Error ? failure.message : failure },
      null,
      2
    )
  );
  if (!failure) assert.equal(stopped.code, 0, stderr);
}
assert.equal(failure, null, failure instanceof Error ? failure.message : "Conformance failed");
// Independent readback of the isolated versioned journal; never reads production KV.
const snapshot: unknown = JSON.parse(await readFile(join(stateDirectory, "snapshot.json"), "utf8"));
assert.ok(typeof snapshot === "object" && snapshot !== null);
const journal = await readFile(join(stateDirectory, "journal.bin"));
const transitions: string[] = [];
let offset = 0;
while (offset < journal.length) {
  const length = journal.readUInt32BE(offset);
  const payload = journal.subarray(offset + 4, offset + 4 + length - 64);
  const digest = journal.subarray(offset + 4 + length - 64, offset + 4 + length).toString();
  assert.equal(createHash("sha256").update(payload).digest("hex"), digest);
  const frame: unknown = JSON.parse(payload.toString());
  assert.ok(typeof frame === "object" && frame !== null && "value" in frame && typeof frame.value === "object" && frame.value !== null);
  if ("quota" in frame && frame.quota && "state" in frame.value) transitions.push(String(frame.value.state));
  offset += length + 4;
}
let expectedTransitions = ["reserved", "dispatched", "reserved", "dispatched", "reserved", "released", "reserved", "released", "reserved", "released"];
if (native) expectedTransitions = ["reserved", "dispatched", "reserved", "dispatched"];
if (disconnect || controlled) expectedTransitions = ["reserved", "dispatched"];
assert.deepEqual(transitions, expectedTransitions);
console.log(JSON.stringify({ status: "passed", directory, native, disconnect, controlled, providerPosts: mock.calls.length, quotaTransitions: transitions }));
