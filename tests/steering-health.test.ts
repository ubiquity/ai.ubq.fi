import { test } from "bun:test";
import assert from "node:assert/strict";
import { createServer, type ServerResponse, type IncomingMessage } from "node:http";
import { chmod, mkdtemp, readFile, readdir, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { runSteeringProbe, validateSteering } from "../scripts/steering-health/probe.ts";
import { LIMITS, MODEL, record, startProbeProxy, type RequestSample } from "../scripts/steering-health/proxy.ts";

const assertNoRetainedToken = async (directory: string, token: string): Promise<void> => {
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) await assertNoRetainedToken(path, token);
    else assert.ok(!(await readFile(path)).includes(Buffer.from(token)), `Credential retained in ${entry.name}`);
  }
};

const sendAnswer = (response: ServerResponse, text: string): void => {
  const item = { id: "fixture-message", type: "message", role: "assistant", status: "completed", content: [{ type: "output_text", text, annotations: [] }] };
  for (const event of [
    { type: "response.created", response: { id: "fixture-response", object: "response", status: "in_progress", output: [] } },
    { type: "response.output_item.done", output_index: 0, item },
    { type: "response.completed", response: { id: "fixture-response", object: "response", status: "completed", output: [item] } },
  ])
    response.write(`event: ${event.type}\ndata: ${JSON.stringify(event)}\n\n`);
  response.end();
};

const fixtureGateway = async (mode: "healthy" | "blocked" | "identity" | "auth" | "reply" | "changed") => {
  let calls = 0;
  const serve = async (request: IncomingMessage, response: ServerResponse): Promise<void> => {
    if (request.url === "/health") {
      const sha = "a".repeat(40);
      response.writeHead(200, {
        "content-type": "application/json",
        "x-uos-git-sha": mode === "identity" ? "b".repeat(40) : sha,
        "x-uos-deployment-id": "fixture",
      });
      response.end(JSON.stringify({ release: { git_sha: sha, deployment_id: "fixture" } }));
      return;
    }
    if (request.url?.startsWith("/v1/models")) {
      response.end(JSON.stringify({ models: [{ slug: MODEL, supported_reasoning_levels: [{ effort: "low" }] }] }));
      return;
    }
    let body = "";
    for await (const chunk of request) body += String(chunk);
    response.setHeader("x-uos-git-sha", mode === "changed" ? "b".repeat(40) : "a".repeat(40));
    response.setHeader("x-uos-deployment-id", "fixture");
    calls++;
    if (mode === "auth") {
      response.writeHead(401);
      response.end(JSON.stringify({ error: { code: "invalid_api_key", message: request.headers.authorization } }));
      return;
    }
    if (calls === 1 && mode !== "reply") {
      if (mode === "blocked") return;
      response.writeHead(200, { "content-type": "text/event-stream" });
      response.flushHeaders();
      const timer = setInterval(() => response.write(":\n\n"), 25);
      response.once("close", () => {
        clearInterval(timer);
      });
      return;
    }
    response.writeHead(200, { "content-type": "text/event-stream" });
    sendAnswer(response, /STEER_ACK_[a-f0-9-]+/.exec(body)?.[0] ?? "missing");
  };
  const server = createServer((request, response) => {
    void serve(request, response);
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  assert.ok(address && typeof address !== "string");
  return {
    endpoint: new URL(`http://127.0.0.1:${address.port}`),
    calls: () => calls,
    stop: async () => {
      server.closeAllConnections();
      await new Promise<void>((resolve) =>
        server.close(() => {
          resolve();
        })
      );
    },
  };
};

test("steering rejects ACK-only, duplicate, reordered, wrong-turn and wrong-answer receipts", () => {
  const identity = { thread_id: "thread", session_id: "thread", turn_id: "turn" };
  const first: RequestSample = { started: 0, bytes: 1, markerCount: 0, ordered: false, identity, cancelled: true };
  const second: RequestSample = { started: 1, bytes: 1, markerCount: 1, ordered: true, identity };
  assert.equal(validateSteering([first, second], "thread", "turn", "marker", "marker"), null);
  for (const bad of [
    { ...second, markerCount: 0 },
    { ...second, markerCount: 2 },
    { ...second, ordered: false },
    { ...second, identity: { ...identity, turn_id: "other" } },
  ]) {
    assert.ok(validateSteering([first, bad], "thread", "turn", "marker", "marker"));
  }
  assert.ok(validateSteering([first], "thread", "turn", "marker", "marker"));
  assert.ok(validateSteering([first, second], "thread", "turn", "unchanged", "marker"));
  assert.ok(validateSteering([{ ...first, cancelled: false }, second], "thread", "turn", "marker", "marker"));
});

test("identity refusal spends no inference and cleans up", async () => {
  const gateway = await fixtureGateway("identity");
  try {
    const result = await runSteeringProbe({ endpoint: gateway.endpoint, token: "private-secret-canary", executable: "missing-codex", deadlineMs: 2000 });
    assert.equal(result.status, "failed");
    assert.equal(gateway.calls(), 0);
    assert.equal(record(result.cleanup).childSettled, true);
    assert.equal(record(result.cleanup).proxyClosed, true);
    assert.ok(!JSON.stringify(result).includes("private-secret-canary"));
  } finally {
    await gateway.stop();
  }
});

const codex = Bun.which("codex");
if (!codex) console.warn("Actual-native tests skipped: installed codex unavailable");
if (codex)
  for (const mode of ["healthy", "blocked", "auth", "changed"] as const) {
    test(`real installed Codex: ${mode} steering and cleanup`, async () => {
      assert.ok(codex);
      const gateway = await fixtureGateway(mode);
      try {
        const token = "private-secret-canary";
        const result = await runSteeringProbe({ endpoint: gateway.endpoint, token, executable: codex, deadlineMs: mode === "blocked" ? 1200 : 5000 });
        assert.equal(result.status, mode === "healthy" ? "passed" : "failed");
        assert.equal(record(result.cleanup).childSettled, true);
        assert.equal(record(result.cleanup).proxyClosed, true);
        assert.equal(gateway.calls(), mode === "healthy" ? 2 : 1);
        for (const name of ["receipt.json", "native-events.json", "native-stderr.txt"]) {
          const artifact = await readFile(join(String(result.artifacts), name), "utf8");
          assert.ok(!artifact.includes(token));
        }
        await assertNoRetainedToken(String(result.artifacts), token);
        assert.equal(record(result.cleanup).nativeStateRemoved, true);
        if (mode === "healthy") assert.equal(record(result.cleanup).nativeExitCode, 0);
      } finally {
        await gateway.stop();
      }
    }, 10_000);
  }

test("proxy refuses a third request and growth beyond its serialized byte budget", async () => {
  const gateway = await fixtureGateway("reply");
  const signal = AbortSignal.timeout(3000);
  const proxy = await startProbeProxy({ endpoint: gateway.endpoint, token: "fixture", marker: "marker", initial: "initial", signal });
  const body = { model: MODEL, reasoning: { effort: "low" }, input: "initial marker" };
  try {
    for (let index = 0; index < 2; index++) {
      const response = await fetch(proxy.url + "/responses", { method: "POST", body: JSON.stringify(body), signal });
      await response.text();
    }
    const refused = await fetch(proxy.url + "/responses", { method: "POST", body: JSON.stringify(body), signal });
    assert.equal(refused.status, 400);
    await refused.text();
    assert.equal(gateway.calls(), 2);
  } finally {
    await proxy.stop();
    await gateway.stop();
  }
  const secondGateway = await fixtureGateway("reply");
  const secondProxy = await startProbeProxy({ endpoint: secondGateway.endpoint, token: "fixture", marker: "marker", initial: "initial", signal });
  try {
    const base = { model: MODEL, reasoning: { effort: "low" }, input: "" };
    base.input = "x".repeat(LIMITS.requestBytes - Buffer.byteLength(JSON.stringify(base)));
    const response = await fetch(secondProxy.url + "/responses", { method: "POST", body: JSON.stringify(base), signal });
    assert.equal(response.status, 400);
    await response.text();
    assert.equal(secondGateway.calls(), 0);
  } finally {
    await secondProxy.stop();
    await secondGateway.stop();
  }
});

test("a native crash is a failed JSON receipt with its real exit status", async () => {
  const directory = await mkdtemp(join(tmpdir(), "uos-steering-exit-test-"));
  const executable = join(directory, "codex");
  await writeFile(executable, '#!/bin/sh\nif [ "$1" = "--version" ]; then echo "codex-cli 0.164.0"; exit 0; fi\nexit 7\n');
  await chmod(executable, 0o700);
  const gateway = await fixtureGateway("reply");
  try {
    const result = await runSteeringProbe({ endpoint: gateway.endpoint, token: "fixture", executable, deadlineMs: 2000 });
    assert.equal(result.status, "failed");
    assert.equal(record(result.cleanup).nativeExitCode, 7);
    assert.equal(record(result.cleanup).childSettled, true);
    assert.equal(gateway.calls(), 0);
  } finally {
    await gateway.stop();
  }
});
