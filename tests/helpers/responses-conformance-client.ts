import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { request, type IncomingHttpHeaders } from "node:http";
import { spawn, execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { mkdir, mkdtemp, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";

export const CONFORMANCE_LIMITS = Object.freeze({ bytes: 262_144, events: 256, deadlineMs: 5_000 });
export const CONFORMANCE_CALL_ID = "call_fixture_echo_1";
export const CONFORMANCE_ARGUMENTS = '{"text":"fixture-argument-ok"}';
export const CONFORMANCE_RESULT = "fixture-tool-ok";
export const CONFORMANCE_FINAL = "fixture-final-ok";
export type ConformanceEvent = Record<string, unknown>;
export type ConformanceMarker = Readonly<{ kind: string; atMs: number; detail: ConformanceEvent }>;

export const conformanceRecord = (value: unknown): ConformanceEvent => {
  if (!value || typeof value !== "object" || Array.isArray(value)) throw new Error("Expected a fixture record");
  return value as ConformanceEvent;
};

/** Event receipts, rather than a timer cadence, release the controlled producer. */
export const createConformanceJournal = () => {
  const entries: ConformanceMarker[] = [];
  const emitter = new EventEmitter();
  const add = (kind: string, detail: ConformanceEvent = {}): void => {
    if (entries.length >= CONFORMANCE_LIMITS.events) throw new Error("Fixture chronology exceeded its bound");
    const entry = { kind, atMs: performance.now(), detail };
    entries.push(entry);
    emitter.emit("entry", entry);
  };
  const wait = (predicate: (entry: ConformanceMarker) => boolean, timeoutMs = CONFORMANCE_LIMITS.deadlineMs): Promise<ConformanceMarker> => {
    const existing = entries.find(predicate);
    if (existing) return Promise.resolve(existing);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        emitter.off("entry", observe);
        reject(new Error("Fixture event deadline exceeded"));
      }, timeoutMs);
      const observe = (entry: ConformanceMarker): void => {
        if (!predicate(entry)) return;
        clearTimeout(timer);
        emitter.off("entry", observe);
        resolve(entry);
      };
      emitter.on("entry", observe);
    });
  };
  return { entries, add, wait };
};

/** Independent SSE framing retains data order across UTF-8 and CR/LF read boundaries. */
const createSseDecoder = (emit: (value: ConformanceEvent) => void) => {
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let pending = "";
  let event = "";
  let data: string[] = [];
  const line = (value: string): void => {
    if (!value) {
      if (data.length) {
        const payload = conformanceRecord(JSON.parse(data.join("\n")) as unknown);
        if (event) assert.equal(payload.type, event, "named SSE event must match its payload");
        emit(payload);
      }
      event = "";
      data = [];
      return;
    }
    if (value.startsWith(":")) return;
    const colon = value.indexOf(":");
    const field = colon < 0 ? value : value.slice(0, colon);
    const content = colon < 0 ? "" : value.slice(colon + 1).replace(/^ /, "");
    if (field === "event") event = content;
    if (field === "data") data.push(content);
  };
  const drain = (final: boolean): void => {
    for (;;) {
      const index = pending.search(/[\r\n]/);
      if (index < 0 || (!final && pending[index] === "\r" && index === pending.length - 1)) break;
      const width = pending[index] === "\r" && pending[index + 1] === "\n" ? 2 : 1;
      line(pending.slice(0, index));
      pending = pending.slice(index + width);
    }
    if (final && (pending || data.length)) throw new Error("SSE ended inside an unterminated event");
  };
  return {
    push: (chunk: Uint8Array): void => {
      pending += decoder.decode(chunk, { stream: true });
      drain(false);
    },
    finish: (): void => {
      pending += decoder.decode();
      drain(true);
    },
  };
};

export type ConformanceWireCapture = {
  request: { path: string; body: string };
  status: number;
  headers: Partial<Record<string, string>>;
  chunksBase64: string[];
  events: ConformanceEvent[];
  outcome: "open" | "delivered" | "interrupted" | "failed";
  error: string | null;
};

export const conformanceHeaders = (headers: IncomingHttpHeaders): Record<string, string> =>
  Object.fromEntries(
    Object.entries(headers)
      .filter(([name]) => /^(?:content-type|cache-control|retry-after|x-request-id|x-uos-.+)$/.test(name))
      .map(([name, value]) => [name, Array.isArray(value) ? value.join(", ") : (value ?? "")])
  );

export const conformanceEndpoint = (endpoint: string, path = "/v1/responses"): URL => {
  const url = new URL(path, endpoint);
  if (url.protocol !== "http:" || !["127.0.0.1", "localhost", "[::1]"].includes(url.hostname)) {
    throw new Error("Synthetic conformance targets must use loopback HTTP");
  }
  return url;
};

/** A real HTTP socket client; destroy() is an actual downstream disconnect. */
export const openConformanceResponse = async (endpoint: string, token: string, body: ConformanceEvent) => {
  const url = conformanceEndpoint(endpoint);
  const serialized = JSON.stringify(body);
  if (Buffer.byteLength(serialized) > CONFORMANCE_LIMITS.bytes) throw new Error("Fixture request exceeded its byte bound");
  const journal = createConformanceJournal();
  const finished = Promise.withResolvers<ConformanceWireCapture>();
  const opened = Promise.withResolvers<ReturnType<typeof onResponse>>();
  let disconnected = false;
  function onResponse(response: import("node:http").IncomingMessage) {
    const capture: ConformanceWireCapture = {
      request: { path: url.pathname, body: serialized },
      status: response.statusCode ?? 0,
      headers: conformanceHeaders(response.headers),
      chunksBase64: [],
      events: [],
      outcome: "open",
      error: null,
    };
    let bytes = 0;
    const decoder = createSseDecoder((event) => {
      if (capture.events.length >= CONFORMANCE_LIMITS.events) throw new Error("Fixture response exceeded its event bound");
      capture.events.push(event);
      journal.add("event", event);
    });
    response.on("data", (chunk: Buffer) => {
      try {
        bytes += chunk.byteLength;
        if (bytes > CONFORMANCE_LIMITS.bytes) throw new Error("Fixture response exceeded its byte bound");
        capture.chunksBase64.push(chunk.toString("base64"));
        if (capture.headers["content-type"]?.includes("text/event-stream")) decoder.push(chunk);
      } catch (error) {
        response.destroy(error instanceof Error ? error : new Error(String(error)));
      }
    });
    response.once("error", (error) => {
      capture.error = error.message;
    });
    response.once("end", () => {
      try {
        if (capture.headers["content-type"]?.includes("text/event-stream")) decoder.finish();
        capture.outcome = "delivered";
      } catch (error) {
        capture.error = error instanceof Error ? error.message : String(error);
        capture.outcome = "failed";
      }
    });
    response.once("close", () => {
      if (capture.outcome === "open") capture.outcome = disconnected ? "interrupted" : "failed";
      journal.add("closed", { outcome: capture.outcome });
      finished.resolve(capture);
    });
    return {
      capture,
      finished: finished.promise,
      waitFor: (type: string) => journal.wait((entry) => entry.kind === "event" && entry.detail.type === type).then((entry) => entry.detail),
      disconnect: (): void => {
        disconnected = true;
        response.destroy();
      },
    };
  }
  const outgoing = request(url, {
    method: "POST",
    agent: false,
    headers: { authorization: `Bearer ${token}`, "content-type": "application/json", "content-length": Buffer.byteLength(serialized) },
  });
  outgoing.once("response", (response) => {
    opened.resolve(onResponse(response));
  });
  outgoing.once("error", opened.reject);
  outgoing.setTimeout(CONFORMANCE_LIMITS.deadlineMs, () => {
    outgoing.destroy(new Error("Fixture HTTP deadline exceeded"));
  });
  outgoing.end(serialized);
  return await opened.promise;
};

export const conformanceRequest = (options: { model?: string; namespace?: string } = {}): ConformanceEvent => ({
  model: options.model ?? "deepseek-flash",
  stream: true,
  store: false,
  reasoning: { effort: "none" },
  instructions: "Use the fixture tool and preserve its result.",
  input: [
    { role: "developer", content: "Keep the exact fixture values." },
    { role: "user", content: "Run the fixture tool." },
  ],
  tools: [
    { type: "function", name: "fixture_echo", parameters: { type: "object", properties: { text: { type: "string" } }, required: ["text"] } },
    ...(options.namespace
      ? [{ type: "namespace", name: options.namespace, tools: [{ type: "function", name: "fixture_echo", parameters: { type: "object" } }] }]
      : []),
  ],
});

export const completedConformanceResponse = (capture: ConformanceWireCapture): ConformanceEvent => {
  assert.equal(capture.status, 200);
  assert.equal(capture.outcome, "delivered");
  const terminals = capture.events.filter((event) => ["response.completed", "response.failed", "response.incomplete", "error"].includes(String(event.type)));
  assert.equal(terminals.length, 1, "each response has exactly one semantic terminal");
  assert.equal(terminals[0].type, "response.completed");
  return conformanceRecord(terminals[0].response);
};

export const conformanceContinuation = (first: ConformanceEvent, capture: ConformanceWireCapture, output: unknown = CONFORMANCE_RESULT): ConformanceEvent => {
  const terminal = completedConformanceResponse(capture);
  const input = Array.isArray(first.input) ? first.input : [];
  const result = Array.isArray(terminal.output) ? terminal.output : [];
  return { ...first, previous_response_id: terminal.id, input: [...input, ...result, { type: "function_call_output", call_id: CONFORMANCE_CALL_ID, output }] };
};

/** Shared HTTP scenario for independently launched TypeScript, Rust and C++ targets. */
export const runConformanceRoundTrip = async (options: {
  endpoint: string;
  token: string;
  request?: ConformanceEvent;
  acknowledge: (turn: number, event: ConformanceEvent) => void | Promise<void>;
}): Promise<ConformanceWireCapture[]> => {
  const body = options.request ?? conformanceRequest();
  const first = await openConformanceResponse(options.endpoint, options.token, body);
  try {
    await options.acknowledge(1, await first.waitFor("response.function_call_arguments.delta"));
    const tool = await first.finished;
    const second = await openConformanceResponse(options.endpoint, options.token, conformanceContinuation(body, tool));
    try {
      await options.acknowledge(2, await second.waitFor("response.output_text.delta"));
      const answer = await second.finished;
      completedConformanceResponse(answer);
      return [tool, answer];
    } finally {
      second.disconnect();
    }
  } finally {
    first.disconnect();
  }
};

type CodexConformanceOptions = Readonly<{
  executable: string;
  expectedSha256: string;
  expectedVersion: string;
  endpoint: string;
  token: string;
  captureDirectory: string;
  onTextDelta: (event: ConformanceEvent) => void;
}>;

type CodexExit = Readonly<{ code: number | null; signal: NodeJS.Signals | null; error?: string }>;

const pinCodexExecutable = async (options: CodexConformanceOptions): Promise<string> => {
  const digest = createHash("sha256");
  for await (const chunk of createReadStream(options.executable)) {
    assert.ok(Buffer.isBuffer(chunk));
    digest.update(chunk);
  }
  assert.equal(digest.digest("hex"), options.expectedSha256, "Use the exact independently pinned Codex executable");
  const result = await promisify(execFile)(options.executable, ["--version"], { timeout: CONFORMANCE_LIMITS.deadlineMs, encoding: "utf8" });
  assert.equal(result.stdout.trim(), `codex-cli ${options.expectedVersion}`);
  return result.stdout.trim();
};

const codexConfiguration = (endpoint: string): string => `model = "deepseek-flash"
model_provider = "fixture"
model_reasoning_effort = "none"
approval_policy = "never"
sandbox_mode = "read-only"
cli_auth_credentials_store = "file"
project_root_markers = []
web_search = "disabled"
[analytics]
enabled = false
[model_providers.fixture]
name = "Synthetic loopback fixture"
base_url = ${JSON.stringify(conformanceEndpoint(endpoint, "/v1").href)}
wire_api = "responses"
env_key = "CODEX_FIXTURE_API_KEY"
requires_openai_auth = false
supports_websockets = false
request_max_retries = 0
stream_max_retries = 0
stream_idle_timeout_ms = 5000
[features]
api_key_model_discovery = false
shell_tool = false
shell_snapshot = false
multi_agent = false
apps = false
plugins = false
plugin_discovery_prefetch = false
remote_plugin = false
hooks = false
skip_host_skill_discovery = true
code_mode_host = false
`;

const finishCodexChild = async (child: ReturnType<typeof spawn>, closed: Promise<CodexExit>): Promise<CodexExit> => {
  child.stdin?.end();
  for (const signal of [null, "SIGTERM", "SIGKILL"] as const) {
    if (signal && child.pid) {
      try {
        process.kill(-child.pid, signal);
      } catch (error) {
        if (!(error instanceof Error && "code" in error && error.code === "ESRCH")) throw error;
      }
    }
    let timer: ReturnType<typeof setTimeout> | undefined;
    const result = await Promise.race([
      closed,
      new Promise<null>((resolve) => {
        timer = setTimeout(() => {
          resolve(null);
        }, 1_000);
      }),
    ]);
    clearTimeout(timer);
    if (result) return result;
  }
  throw new Error("Owned Codex process did not settle after bounded termination");
};

const startCodexClient = (options: CodexConformanceOptions, directory: string) => {
  const env: NodeJS.ProcessEnv = { PATH: process.env.PATH, CODEX_HOME: directory, TMPDIR: directory, CODEX_FIXTURE_API_KEY: options.token };
  if (process.env.HOME) env.HOME = process.env.HOME;
  const child = spawn(options.executable, ["--strict-config", "app-server", "--listen", "stdio://"], {
    cwd: directory,
    env,
    detached: true,
    stdio: ["pipe", "pipe", "pipe"],
  });
  const journal = createConformanceJournal();
  const closed = Promise.withResolvers<CodexExit>();
  const tools: ConformanceEvent[] = [];
  let stderr = "";
  let sequence = 0;
  let textReceipt = false;
  const lines = createInterface({ input: child.stdout });
  child.stderr.on("data", (chunk: Buffer) => {
    stderr = (stderr + chunk.toString()).slice(-32_768);
  });
  child.stdin.on("error", (error) => {
    journal.add("native_stdin_error", { message: error.message });
  });
  child.once("error", (error) => {
    closed.resolve({ error: error.message, code: null, signal: null });
  });
  child.once("close", (code, signal) => {
    lines.close();
    closed.resolve({ code, signal });
  });
  const send = (message: ConformanceEvent): void => {
    child.stdin.write(JSON.stringify(message) + "\n");
  };
  lines.on("line", (line) => {
    try {
      const message = conformanceRecord(JSON.parse(line) as unknown);
      journal.add("native_message", message);
      if (message.method === "item/tool/call") {
        const params = conformanceRecord(message.params);
        assert.equal(params.callId, CONFORMANCE_CALL_ID);
        assert.equal(params.tool, "fixture_echo");
        assert.deepEqual(params.arguments, { text: "fixture-argument-ok" });
        assert.equal(tools.length, 0, "The native turn must invoke the synthetic tool once");
        tools.push(params);
        send({ id: message.id, result: { contentItems: [{ type: "inputText", text: CONFORMANCE_RESULT }], success: true } });
      }
      if (message.method === "item/agentMessage/delta" && !textReceipt) {
        const params = conformanceRecord(message.params);
        if (typeof params.delta === "string" && params.delta) {
          textReceipt = true;
          options.onTextDelta({ type: "response.output_text.delta", delta: params.delta });
        }
      }
    } catch (error) {
      journal.add("native_protocol_error", { message: error instanceof Error ? error.message : String(error) });
      child.stdin.end();
    }
  });
  return {
    journal,
    tools,
    send,
    rpc: async (method: string, params: ConformanceEvent): Promise<ConformanceEvent> => {
      const id = ++sequence;
      send({ id, method, params });
      const reply = (await journal.wait((entry) => entry.kind === "native_message" && entry.detail.id === id && !entry.detail.method)).detail;
      if (reply.error) throw new Error(`Native ${method} failed: ${JSON.stringify(reply.error)}`);
      return conformanceRecord(reply.result);
    },
    stop: async (): Promise<CodexExit & { stderr: string }> => ({ ...(await finishCodexChild(child, closed.promise)), stderr }),
  };
};

/** Explicit opt-in: ordinary CI never needs a host-installed Codex executable. */
export const runCodexConformance = async (options: CodexConformanceOptions): Promise<ConformanceEvent> => {
  const version = await pinCodexExecutable(options);
  await mkdir(options.captureDirectory, { recursive: true, mode: 0o700 });
  const directory = await mkdtemp(join(options.captureDirectory, "native-"));
  await writeFile(join(directory, "config.toml"), codexConfiguration(options.endpoint), { mode: 0o600, flag: "wx" });
  const native = startCodexClient(options, directory);
  let result: ConformanceEvent = { version, executableSha256: options.expectedSha256 };
  try {
    await native.rpc("initialize", { clientInfo: { name: "uos_conformance", version: "1" }, capabilities: { experimentalApi: true } });
    native.send({ method: "initialized" });
    const thread = await native.rpc("thread/start", {
      model: "deepseek-flash",
      modelProvider: "fixture",
      cwd: directory,
      approvalPolicy: "never",
      sandbox: "read-only",
      ephemeral: true,
      dynamicTools: [
        {
          type: "function",
          name: "fixture_echo",
          description: "Return one synthetic fixture result without host execution.",
          inputSchema: { type: "object", properties: { text: { type: "string" } }, required: ["text"], additionalProperties: false },
        },
      ],
    });
    assert.equal(thread.model, "deepseek-flash");
    assert.equal(thread.reasoningEffort, "none");
    const threadId = conformanceRecord(thread.thread).id;
    assert.equal(typeof threadId, "string");
    const turn = await native.rpc("turn/start", {
      threadId,
      input: [{ type: "text", text: "Call fixture_echo once with text fixture-argument-ok then reply fixture-final-ok." }],
    });
    const turnId = conformanceRecord(turn.turn).id;
    const terminal = await native.journal.wait(
      (entry) =>
        entry.kind === "native_message" &&
        entry.detail.method === "turn/completed" &&
        conformanceRecord(conformanceRecord(entry.detail.params).turn).id === turnId
    );
    const completedTurn = conformanceRecord(conformanceRecord(terminal.detail.params).turn);
    assert.equal(completedTurn.status, "completed");
    assert.equal(completedTurn.error, null);
    const items = completedTurn.items;
    assert.ok(Array.isArray(items));
    const answer = items.map(conformanceRecord).findLast((item) => item.type === "agentMessage");
    assert.equal(answer?.text, CONFORMANCE_FINAL);
    assert.equal(native.tools.length, 1);
    assert.equal(
      native.journal.entries.some((entry) => entry.kind === "native_protocol_error"),
      false
    );
    result = { ...result, thread, turnId, toolRequests: native.tools, finalText: answer.text };
  } finally {
    const cleanup = await native.stop();
    result.cleanup = cleanup;
    result.events = native.journal.entries;
    await writeFile(join(options.captureDirectory, "native-receipt.json"), JSON.stringify(result, null, 2) + "\n", { mode: 0o600 });
    assert.equal(cleanup.code, 0, "The owned native client must exit normally");
  }
  return result;
};

const conformanceIdPrefix = (value: string): string => {
  if (value.startsWith("fc_")) return "fc";
  if (value.startsWith("msg_")) return "msg";
  return "id";
};

/** Keep raw captures too: normalization changes only known nondeterministic metadata. */
export const normalizeConformanceValue = (value: unknown): unknown => {
  const ids = new Map<string, string>();
  const visit = (entry: unknown, key = ""): unknown => {
    if (Array.isArray(entry)) return entry.map((item) => visit(item));
    if (entry && typeof entry === "object") return Object.fromEntries(Object.entries(entry).map(([name, item]) => [name, visit(item, name)]));
    if (["created_at", "created_at_ms"].includes(key) && typeof entry === "number") return 0;
    if (["id", "item_id", "request_id"].includes(key) && typeof entry === "string") {
      if (!ids.has(entry)) ids.set(entry, `${conformanceIdPrefix(entry)}_${ids.size + 1}`);
      return ids.get(entry);
    }
    return entry;
  };
  return visit(value);
};
