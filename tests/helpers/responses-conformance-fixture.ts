import { serveHttp, type NetAddr } from "#http";
import { BUN_KV_CODEC, openKv, type Kv, type KvKey } from "#kv";
import assert from "node:assert/strict";
import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import { mkdir, mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { shutdownOptionalTelemetry } from "../../serve.ts";
import { PAID_FALLBACK_NO_LIMIT } from "../../src/api-keys.ts";
import {
  apiKeyPolicyFromHashRecord,
  apiKeyUsageV3RequestKey,
  apiKeyUsageV3WindowKey,
  resetApiKeyPolicyCacheForTest,
  type ApiKeyPolicy,
} from "../../src/api-key-policy.ts";
import { config } from "../../src/config.ts";
import { DEEPSEEK_CHAT_COMPLETIONS_URL } from "../../src/deepseek/index.ts";
import { setInferenceAdmissionControllerForTest } from "../../src/handler/admission.ts";
import handler from "../../src/handler/index.ts";
import { createServeHandler } from "../../src/handler/serve-handler.ts";
import { createInferenceAdmissionController } from "../../src/inference-admission.ts";
import { setKvForTest } from "../../src/kv.ts";
import type { ApiKeyHashRecord, ApiKeyRecord, ApiKeyUsageRequestV3, ApiKeyUsageWindowV3 } from "../../src/types.ts";
import { sha256Base64Url } from "../../src/utils.ts";
import {
  CONFORMANCE_ARGUMENTS,
  CONFORMANCE_CALL_ID,
  CONFORMANCE_FINAL,
  CONFORMANCE_LIMITS,
  conformanceHeaders,
  conformanceEndpoint,
  conformanceRecord,
  createConformanceJournal,
  normalizeConformanceValue,
  type ConformanceEvent,
} from "./responses-conformance-client.ts";

export const CONFORMANCE_SOURCE_SHA = "d94fef52ccc76e0d5aa019154f11e47d5e10fba6";
const TOKEN = `u_${"d".repeat(64)}`;
const KEY_ID = "responses-conformance-key";
const PROVIDER_KEYS = ["DEEPSEEK_API_KEY", "SURPLUS_API_KEY", "METERED_API_KEY", "LITHOSAI_API_KEY", "OPENROUTER_API_KEY", "CODEX_AUTH_JSON_B64"];
type AtomicWrite = Readonly<{ key: KvKey; value: unknown }>;
type RecordedRequest = Readonly<{
  path: string;
  body: string;
  headers: Record<string, string>;
  response?: { status: number; headers: Record<string, string>; chunksBase64: string[] };
}>;
type FixtureOptions = Readonly<{
  toolName?: string;
  holdToolDelta?: boolean;
  usageLimit?: number;
  captureDirectory?: string;
  providerOrigin?: string;
  maxDispatches?: number;
}>;

const observeQuotaCommits = (kv: Kv, journal: ReturnType<typeof createConformanceJournal>): void => {
  const originalAtomic = kv.atomic.bind(kv);
  kv.atomic = () => {
    const operation = originalAtomic();
    const originalSet = operation.set.bind(operation);
    const originalCommit = operation.commit.bind(operation);
    const writes: AtomicWrite[] = [];
    operation.set = (key, value, options) => {
      if (key[0] === "uos_ai" && key[1] === "api_key_usage" && key[2] === "v3") writes.push({ key, value: structuredClone(value) });
      originalSet(key, value, options);
      return operation;
    };
    operation.commit = async () => {
      const result = await originalCommit();
      if (result.ok && writes.length) journal.add("quota_commit", { writes });
      return result;
    };
    return operation;
  };
};

const seedKey = async (kv: Kv, usageLimit: number) => {
  const hash = await sha256Base64Url(TOKEN);
  const now = Date.now();
  const common = {
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: usageLimit,
    usage_requests: 0,
    usage_reset_at_ms: now + 3_600_000,
    window_ms: 3_600_000,
    usage_quota_version: 3,
    paid_fallback_enabled: false,
    paid_fallback_limit_microcredits: PAID_FALLBACK_NO_LIMIT,
    paid_fallback_spent_microcredits: 0,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
  } satisfies Omit<ApiKeyHashRecord, "id">;
  const hashRecord: ApiKeyHashRecord = { id: KEY_ID, ...common };
  const keyRecord: ApiKeyRecord = {
    id: KEY_ID,
    name: "Synthetic Responses conformance key",
    prefix: TOKEN.slice(0, 10),
    hash,
    created_at_ms: now,
    ...common,
    paid_fallback_model_ids: [],
    paid_fallback_quota_per_credit: 0,
    paid_fallback_max_exposure_microcredits: {},
    paid_fallback_pricing_checked_at_ms: now,
  };
  await kv.set(["ubq_ai", "api_keys", "id", KEY_ID], keyRecord);
  await kv.set(["ubq_ai", "api_keys", "hash", hash], hashRecord);
  const policy = apiKeyPolicyFromHashRecord(hash, hashRecord, now);
  assert.ok(policy, "Synthetic key must resolve through the actual API-key policy");
  return { policy, hashRecord, keyRecord, hash };
};

const chatFrame = (delta: ConformanceEvent, finishReason: string | null = null): string =>
  `data: ${JSON.stringify({
    id: "chatcmpl-fixture-1",
    object: "chat.completion.chunk",
    created: 1_780_000_001,
    model: "deepseek-flash",
    choices: [{ index: 0, delta, finish_reason: finishReason }],
  })}\n\n`;

const usageFrame = (turn: number): string =>
  `data: ${JSON.stringify({
    id: "chatcmpl-fixture-1",
    object: "chat.completion.chunk",
    created: 1_780_000_001,
    model: "deepseek-flash",
    choices: [],
    usage:
      turn === 1
        ? { prompt_tokens: 11, completion_tokens: 5, total_tokens: 16, prompt_cache_hit_tokens: 0 }
        : { prompt_tokens: 23, completion_tokens: 7, total_tokens: 30 },
  })}\n\n`;

const firstFrame = (turn: number, toolName: string): string =>
  turn === 1
    ? chatFrame({
        role: "assistant",
        tool_calls: [{ index: 0, id: CONFORMANCE_CALL_ID, type: "function", function: { name: toolName, arguments: '{"text":"fixture-' } }],
      })
    : chatFrame({ role: "assistant", content: "fixture-" });

const remainingFrames = (turn: number): string =>
  (turn === 1
    ? chatFrame({ tool_calls: [{ index: 0, function: { arguments: 'argument-ok"}' } }] }, "tool_calls")
    : chatFrame({ content: "final-ok" }, "stop")) +
  usageFrame(turn) +
  "data: [DONE]\n\n";

const readRequest = async (request: IncomingMessage): Promise<string> => {
  const chunks: Buffer[] = [];
  let bytes = 0;
  for await (const value of request) {
    const chunk = Buffer.isBuffer(value) ? value : Buffer.from(String(value));
    bytes += chunk.byteLength;
    if (bytes > CONFORMANCE_LIMITS.bytes) throw new Error("Mock upstream request exceeded its bound");
    chunks.push(chunk);
  }
  return Buffer.concat(chunks).toString("utf8");
};

const boundedMockDispatches = (value?: number): number => {
  if (value === undefined) return 4;
  if (!Number.isInteger(value) || value < 4 || value > 4_096) throw new Error("Mock maxDispatches must be an integer from 4 through 4096");
  return value;
};

export const startConformanceMock = async (options: FixtureOptions = {}, journal = createConformanceJournal()) => {
  const maxDispatches = boundedMockDispatches(options.maxDispatches);
  const calls: RecordedRequest[] = [];
  const pending = new Map<number, (reason: "receipt" | "cancel") => void>();
  const tasks = new Set<Promise<void>>();
  const serve = async (request: IncomingMessage, response: ServerResponse): Promise<void> => {
    const body = await readRequest(request);
    if (calls.length >= maxDispatches) throw new Error("Fixture mock dispatch bound exceeded");
    const responseHeaders = { "content-type": "text/event-stream", "cache-control": "no-cache", "x-request-id": `fixture-provider-${calls.length + 1}` };
    const frames: string[] = [];
    calls.push({
      path: request.url ?? "",
      body,
      headers: conformanceHeaders(request.headers),
      response: { status: 200, headers: responseHeaders, chunksBase64: frames },
    });
    const turn = calls.length;
    journal.add("provider_dispatch", { turn, body: JSON.parse(body) as unknown });
    const gate = Promise.withResolvers<"receipt" | "cancel">();
    pending.set(turn, gate.resolve);
    response.once("close", () => {
      if (!response.writableFinished) journal.add("provider_cancelled", { turn });
      gate.resolve("cancel");
    });
    response.writeHead(200, responseHeaders);
    const initial = firstFrame(turn, options.toolName ?? "fixture_echo");
    response.write(initial);
    frames.push(Buffer.from(initial).toString("base64"));
    journal.add("provider_first_delta", { turn });
    if (turn === 1 && options.holdToolDelta === false) gate.resolve("receipt");
    const deadline = setTimeout(() => response.destroy(new Error("Mock receipt deadline exceeded")), CONFORMANCE_LIMITS.deadlineMs);
    const reason = await gate.promise;
    clearTimeout(deadline);
    pending.delete(turn);
    if (reason === "cancel" || response.destroyed) return;
    journal.add("provider_resumed", { turn });
    const remaining = remainingFrames(turn);
    response.end(remaining);
    frames.push(Buffer.from(remaining).toString("base64"));
  };
  const server = createServer((request, response) => {
    const task = serve(request, response).catch((error: unknown) => {
      journal.add("mock_error", { message: error instanceof Error ? error.message : String(error) });
      response.destroy(error instanceof Error ? error : new Error(String(error)));
    });
    tasks.add(task);
    void task.finally(() => tasks.delete(task));
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  assert.ok(address && typeof address !== "string");
  return {
    calls,
    journal,
    origin: `http://127.0.0.1:${address.port}`,
    receipt: (turn: number, event: ConformanceEvent): void => {
      assert.ok(["response.function_call_arguments.delta", "response.output_text.delta"].includes(String(event.type)));
      assert.ok(typeof event.delta === "string" && event.delta.length > 0);
      const release = pending.get(turn);
      assert.ok(release, "A receipt must refer to the still-paused producer");
      journal.add("client_received_delta", { turn, event });
      release("receipt");
    },
    stop: async (): Promise<void> => {
      const closed = new Promise<void>((resolve, reject) =>
        server.close((error) => {
          if (error) reject(error);
          else resolve();
        })
      );
      server.closeAllConnections();
      await closed;
      await Promise.all(tasks);
    },
  };
};

const externalMock = (origin: string) => ({
  origin: conformanceEndpoint(origin, "/").origin,
  calls: [] as RecordedRequest[],
  receipt: (): void => {
    throw new Error("The external mock owner must acknowledge its own client receipt");
  },
  stop: (): Promise<void> => Promise.resolve(),
});

const fetchInputUrl = (input: RequestInfo | URL): string => {
  if (typeof input === "string") return input;
  if (input instanceof URL) return input.href;
  return input.url;
};

const redirectMockFetch = (origin: string): (() => void) => {
  const original = globalThis.fetch;
  globalThis.fetch = Object.assign(
    (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
      const url = fetchInputUrl(input);
      if (url !== DEEPSEEK_CHAT_COMPLETIONS_URL) return Promise.reject(new Error("Fixture refused an unmapped provider destination"));
      return original(`${origin}/chat/completions`, init);
    },
    { preconnect: original.preconnect }
  );
  return () => {
    globalThis.fetch = original;
  };
};

const privateDatabasePath = async (captureDirectory?: string): Promise<string> => {
  const parent = captureDirectory ?? tmpdir();
  if (captureDirectory) await mkdir(parent, { recursive: true, mode: 0o700 });
  return join(await mkdtemp(join(parent, "uos-state-")), "kv-bun.sqlite3");
};

const readLedger = async (database: Kv, policy: ApiKeyPolicy, requestId: string) => {
  const request = await database.get<ApiKeyUsageRequestV3>(apiKeyUsageV3RequestKey(policy, requestId), { consistency: "strong" });
  const window = await database.get<ApiKeyUsageWindowV3>(apiKeyUsageV3WindowKey(policy), { consistency: "strong" });
  return {
    request: request.value,
    requestVersionstamp: request.versionstamp,
    window: window.value,
    windowVersionstamp: window.versionstamp,
    paidFallback: "not_applicable" as const,
  };
};

/** The complete source handler, real delivery transport, native KV and synthetic authority. */
export const startResponsesConformanceFixture = async (options: FixtureOptions = {}) => {
  assert.equal(Bun.version, "1.4.0");
  boundedMockDispatches(options.maxDispatches);
  for (const name of PROVIDER_KEYS) assert.equal(process.env[name], undefined, `Credential-free profile must omit ${name}`);
  const journal = createConformanceJournal();
  const storage: { mode: "file"; databasePath: string; codec: string; reopen: ConformanceEvent | null } = {
    mode: "file",
    databasePath: await privateDatabasePath(options.captureDirectory),
    codec: BUN_KV_CODEC,
    reopen: null,
  };
  const kv = await openKv(storage.databasePath);
  const key = await seedKey(kv, options.usageLimit ?? 50);
  observeQuotaCommits(kv, journal);
  const upstream = options.providerOrigin ? externalMock(options.providerOrigin) : await startConformanceMock(options, journal);
  const restoreFetch = redirectMockFetch(upstream.origin);
  const originalInfo = console.info;
  const originalDeploy = config.isDeploy;
  const inbound: RecordedRequest[] = [];
  const terminals: ConformanceEvent[] = [];
  const controller = createInferenceAdmissionController({ maxActive: 2, maxWaiting: 1, maxQueueWaitMs: 200 });
  process.env.DEEPSEEK_API_KEY = "fixture-provider-secret";
  (config as { isDeploy: boolean }).isDeploy = true;
  setKvForTest(kv);
  resetApiKeyPolicyCacheForTest();
  setInferenceAdmissionControllerForTest(controller);
  console.info = (...args: unknown[]) => {
    if (args[0] !== "[ai.ubq.fi] request_terminal") return;
    const terminal = conformanceRecord(JSON.parse(String(args[1])) as unknown);
    terminals.push(terminal);
    journal.add("request_terminal", terminal);
  };
  const gateway = serveHttp(
    { hostname: "127.0.0.1", port: 0, onListen: () => {} },
    createServeHandler(async (request, delivery) => {
      const body = await request.clone().text();
      if (Buffer.byteLength(body) > CONFORMANCE_LIMITS.bytes) throw new Error("Fixture inbound request exceeded its bound");
      const headers = Object.fromEntries([...request.headers].map(([name, value]) => [name, name === "authorization" ? "[synthetic]" : value]));
      inbound.push({ path: new URL(request.url).pathname, body, headers });
      return await handler(request, delivery);
    })
  );
  await gateway.ready;
  const endpoint = `http://127.0.0.1:${(gateway.addr as NetAddr).port}`;
  let stopped = false;
  let kvClosed = false;
  let savedCaptures: unknown = null;
  const ledger = (requestId: string) => readLedger(kv, key.policy, requestId);
  const accountingFor = (database: Kv) =>
    Promise.all(
      terminals.map(async (terminal) => ({ requestId: terminal.request_id, ...(await readLedger(database, key.policy, String(terminal.request_id))) }))
    );
  let accounting: Awaited<ReturnType<typeof accountingFor>> = [];
  const persist = async (): Promise<void> => {
    if (!options.captureDirectory) return;
    await mkdir(options.captureDirectory, { recursive: true, mode: 0o700 });
    const receipt = {
      oracleSourceSha: CONFORMANCE_SOURCE_SHA,
      providerOwnership: options.providerOrigin ? "external" : "fixture",
      storage,
      inbound,
      upstream: upstream.calls,
      chronology: journal.entries,
      terminals,
      accounting,
      captures: savedCaptures,
    };
    await writeFile(join(options.captureDirectory, "raw.json"), JSON.stringify(receipt, null, 2) + "\n", { mode: 0o600 });
    await writeFile(join(options.captureDirectory, "normalized.json"), JSON.stringify(normalizeConformanceValue(receipt), null, 2) + "\n", { mode: 0o600 });
  };
  const save = async (captures: unknown): Promise<void> => {
    savedCaptures = captures;
    if (!kvClosed) accounting = await accountingFor(kv);
    await persist();
  };
  const quotaCommits = (): number => journal.entries.filter((entry) => entry.kind === "quota_commit").length;
  const reopen = async (): Promise<void> => {
    const before = accounting;
    const dispatchesBefore = upstream.calls.length;
    const quotaCommitsBefore = quotaCommits();
    const database = await openKv(storage.databasePath);
    try {
      observeQuotaCommits(database, journal);
      accounting = await accountingFor(database);
      assert.deepEqual(accounting, before, "Ledger rows and revisions must survive closing and reopening the real Bun database");
      assert.equal(upstream.calls.length, dispatchesBefore, "Reopening state cannot dispatch another generation");
      assert.equal(quotaCommits(), quotaCommitsBefore, "Reopening state cannot settle a ledger twice");
      storage.reopen = {
        outcome: "verified",
        before,
        after: accounting,
        dispatchesBefore,
        dispatchesAfter: upstream.calls.length,
        quotaCommitsBefore,
        quotaCommitsAfter: quotaCommits(),
      };
      journal.add("kv_reopened_verified");
    } finally {
      database.close();
    }
  };
  return {
    endpoint,
    token: TOKEN,
    journal,
    inbound,
    terminals,
    calls: upstream.calls,
    controller,
    storage,
    ledger,
    save,
    verifyReopenedLedger: (): ConformanceEvent => {
      assert.ok(stopped && storage.reopen?.outcome === "verified", "Stop and drain the fixture before durable readback");
      return storage.reopen;
    },
    acknowledge: upstream.receipt,
    expected: { callId: CONFORMANCE_CALL_ID, arguments: CONFORMANCE_ARGUMENTS, final: CONFORMANCE_FINAL },
    revoke: async (): Promise<void> => {
      await kv.set(["ubq_ai", "api_keys", "hash", key.hash], { ...key.hashRecord, revoked_at_ms: Date.now() });
      resetApiKeyPolicyCacheForTest();
    },
    stop: async (): Promise<void> => {
      if (stopped) return;
      stopped = true;
      try {
        const shutdown = await Promise.allSettled([upstream.stop(), gateway.shutdown()]);
        await shutdownOptionalTelemetry();
        const failure = shutdown.find((result) => result.status === "rejected");
        if (failure?.status === "rejected") throw failure.reason;
        journal.add("transport_drained");
        accounting = await accountingFor(kv);
        kv.close();
        kvClosed = true;
        journal.add("kv_closed");
        await reopen();
        await persist();
      } catch (error) {
        storage.reopen = { outcome: "failed", message: error instanceof Error ? error.message : String(error) };
        await persist();
        throw error;
      } finally {
        console.info = originalInfo;
        restoreFetch();
        delete process.env.DEEPSEEK_API_KEY;
        (config as { isDeploy: boolean }).isDeploy = originalDeploy;
        setInferenceAdmissionControllerForTest(null);
        setKvForTest(null);
        resetApiKeyPolicyCacheForTest();
        if (!kvClosed) kv.close();
      }
    },
  };
};
