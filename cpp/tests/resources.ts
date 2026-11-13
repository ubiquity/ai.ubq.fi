import assert from "node:assert/strict";
import { createServer, request } from "node:http";
import { mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { toDeepSeekResponsesChatBody } from "../../src/deepseek/chat-projection.ts";
import { COMPACTION_SUMMARY_POLICY } from "../../src/deepseek/forwarded-payload-policy.ts";
import { startCandidate, SYNTHETIC_TOKEN } from "./candidate-process.ts";

const executable = process.argv[2];
assert.ok(executable);
const parsedPrefix: unknown = JSON.parse(process.argv[3] ?? "[]");
assert.ok(Array.isArray(parsedPrefix) && parsedPrefix.every((value): value is string => typeof value === "string"));
const prefix: string[] = parsedPrefix;
const directory = await mkdtemp(join(tmpdir(), "uos-cpp-resources-"));
const forwarded: unknown[] = [];
const server = createServer((incoming, outgoing) => {
  void (async () => {
    const chunks: Buffer[] = [];
    let bytes = 0;
    for await (const chunk of incoming) {
      const buffer = Buffer.isBuffer(chunk) ? chunk : Buffer.from(String(chunk));
      bytes += buffer.length;
      assert.ok(bytes <= 6 * 1024 * 1024);
      chunks.push(buffer);
    }
    forwarded.push(JSON.parse(Buffer.concat(chunks).toString()) as unknown);
    const chunk = {
      id: "resource-provider",
      object: "chat.completion.chunk",
      created: 1780000001,
      model: "deepseek-flash",
      choices: [{ index: 0, delta: { content: "resource-ok" }, finish_reason: "stop" }],
      usage: { prompt_tokens: 100, completion_tokens: 2, total_tokens: 102 },
    };
    outgoing.writeHead(200, { "content-type": "text/event-stream", "x-request-id": "resource-provider" });
    outgoing.end(`data: ${JSON.stringify(chunk)}\n\ndata: [DONE]\n\n`);
  })().catch((error: unknown) => outgoing.destroy(error instanceof Error ? error : new Error(String(error))));
});
await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
const address = server.address();
assert.ok(address && typeof address !== "string");
const candidate = await startCandidate({
  executable,
  prefix,
  directory: join(directory, "candidate"),
  stateDirectory: join(directory, "state"),
  providerOrigin: `http://127.0.0.1:${address.port}`,
});
const send = (body: unknown, declaredBytes?: number) =>
  new Promise<{ status: number; text: string }>((resolve, reject) => {
    const data = JSON.stringify(body);
    const outgoing = request(
      `${candidate.endpoint}/v1/responses`,
      {
        method: "POST",
        agent: false,
        headers: { authorization: `Bearer ${SYNTHETIC_TOKEN}`, "content-type": "application/json", "content-length": declaredBytes ?? Buffer.byteLength(data) },
      },
      (response) => {
        let text = "";
        response.setEncoding("utf8");
        response.on("data", (value: string) => {
          text += value;
          assert.ok(text.length < 262144);
        });
        response.once("error", reject);
        response.once("end", () => {
          resolve({ status: response.statusCode ?? 0, text });
          outgoing.destroy();
        });
      }
    );
    outgoing.once("error", reject);
    outgoing.setTimeout(10000, () => outgoing.destroy(new Error("Resource scenario deadline exceeded")));
    if (declaredBytes) outgoing.flushHeaders();
    else outgoing.end(data);
  });
const receipts: unknown[] = [];
try {
  const base =
    "alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november oscar papa quebec romeo sierra tango uniform victor whiskey xray yankee zulu ";
  const part = base.repeat(Math.ceil(65536 / base.length)).slice(0, 65536);
  const normal = {
    model: "deepseek-flash",
    stream: true,
    input: Array.from({ length: 24 }, () => ({ role: "user", content: [{ type: "input_text", text: part }] })),
  };
  const head = `${COMPACTION_SUMMARY_POLICY.prefix}\n`;
  const summary = {
    model: "deepseek-flash",
    stream: true,
    truncation: "disabled",
    input: [{ role: "user", content: [{ type: "input_text", text: head + "s".repeat(720 * 1024 - Buffer.byteLength(head)) }] }],
  };
  for (const body of [normal, summary]) {
    const oracle = toDeepSeekResponsesChatBody(body, "deepseek-flash", true);
    assert.equal(oracle.ok, true);
    const response = await send(body);
    assert.equal(response.status, 200);
    assert.match(response.text, /event: response.completed/);
    assert.deepEqual(forwarded.at(-1), oracle.value.body, "Large input must preserve the actual source projection");
    receipts.push({ requestBytes: Buffer.byteLength(JSON.stringify(body)), sourceProjectionMatches: true, response });
  }
  const oversize = await send({}, 4 * 1024 * 1024 + 1);
  assert.equal(oversize.status, 413);
  let nested: unknown = 0;
  for (let index = 0; index < 65; index++) nested = [nested];
  assert.equal((await send({ model: "deepseek-flash", stream: true, input: "ok", metadata: nested })).status, 400);
  assert.equal((await send({ model: "deepseek-flash", stream: true, input: "ok", metadata: Array.from({ length: 65537 }, () => 0) })).status, 400);
  assert.equal(forwarded.length, 2);
  const state = await candidate.control.send("READ_STATE");
  assert.ok(typeof state.state === "object" && state.state !== null && "quota_transactions" in state.state);
  assert.equal(state.state.quota_transactions, 8, "Two dispatches and two post-reservation bound refusals");
  receipts.push({ oversize, state, providerPosts: forwarded.length });
} finally {
  assert.equal((await candidate.finish("SIGTERM")).code, 0);
  await new Promise<void>((resolve, reject) => {
    server.close((error) => {
      if (error) reject(error);
      else resolve();
    });
  });
  await writeFile(join(directory, "resources.json"), JSON.stringify(receipts, null, 2));
}
console.log(JSON.stringify({ status: "passed", directory, providerPosts: forwarded.length, largeInputs: 2 }));
