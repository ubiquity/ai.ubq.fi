import { test } from "bun:test";
import assert from "node:assert/strict";
import { request as httpRequest, type IncomingMessage } from "node:http";
import { serveHttp } from "#http";
import { earlySseHeadersCommitted, registerEarlySseHeaders } from "../src/runtime/http-early-sse.ts";
import { nativeFailureAfterEarlyHeaders, nativeHeaderUsageContext, originalNativeHttpFailure } from "../src/responses-native-headers.ts";
import { attachResponseTelemetry, createResponseTelemetryState, type UsageContext } from "../src/openai-telemetry.ts";
import { observeRawBodyOnce, readJsonBody } from "../src/request.ts";
import { apiKeyHashKey } from "../src/api-keys.ts";
import { apiKeyPolicyFromHashRecord, apiKeyUsageV3WindowKey, reserveApiKeyUsageV3, type ApiKeyProviderDispatch } from "../src/api-key-policy.ts";
import type { Kv } from "#kv";
import type { ApiKeyHashRecord, ApiKeyUsageWindowV3 } from "../src/types.ts";
import { CountingKv } from "./helpers/counting-kv.ts";
import { withCors } from "../src/http.ts";
import { withTerminalRequestLog } from "../src/handler/terminal-log.ts";

const nativeHeaders = { "x-codex-turn-metadata": JSON.stringify({ session_id: "session", thread_id: "thread", turn_id: "turn" }) };
const raw = { model: "deepseek-ai/DeepSeek-V4.1-Flash", input: "canary", stream: true };
const usage = (beforeProviderDispatch?: UsageContext["beforeProviderDispatch"]): UsageContext => ({
  keyId: null,
  kernelRepo: null,
  kernelOrg: null,
  requestId: "native-readiness-fixture",
  beforeProviderDispatch,
  responseTelemetry: createResponseTelemetryState(),
});
const errorResponse = (status: number, code: string): Response =>
  new Response(JSON.stringify({ error: { code, message: `original ${code}`, param: "input", type: "original_type" } }), {
    status,
    headers: { "content-type": "application/json", "retry-after": "17" },
  });
const deadline = async <T>(promise: Promise<T>, label = "fixture boundary"): Promise<T> =>
  await Promise.race([
    promise,
    Bun.sleep(1000).then(() => {
      throw new Error(`${label} timed out`);
    }),
  ]);

test("native keyless dispatch preserves original body observation and aborts the same pending handler on disconnect", async () => {
  let observedBody = "";
  let observedRequest: Request | undefined;
  let resolveAborted = () => {};
  const aborted = new Promise<void>((resolve) => {
    resolveAborted = resolve;
  });
  const server = serveHttp({ hostname: "127.0.0.1", port: 0 }, async (req) => {
    observedRequest = req;
    observeRawBodyOnce(
      req,
      (bytes) => {
        observedBody = new TextDecoder().decode(bytes);
      },
      () => {}
    );
    await readJsonBody(req);
    const context = nativeHeaderUsageContext(req, raw, usage());
    await context?.beforeProviderDispatch?.("lithos");
    assert.equal(earlySseHeadersCommitted(req), true);
    await new Promise<void>((resolve) => {
      req.signal.addEventListener(
        "abort",
        () => {
          resolveAborted();
          resolve();
        },
        { once: true }
      );
    });
    return new Response(null);
  });
  let client: ReturnType<typeof httpRequest> | undefined;
  try {
    const address = await server.ready;
    const response = await deadline(
      new Promise<IncomingMessage>((resolve, reject) => {
        client = httpRequest({ hostname: "127.0.0.1", port: address.port, path: "/v1/responses", method: "POST", headers: nativeHeaders }, resolve);
        client.once("error", reject);
        client.end(JSON.stringify(raw));
      }),
      "native headers"
    );
    assert.equal(response.statusCode, 200);
    assert.equal(response.headers["x-uos-request-id"], "native-readiness-fixture");
    assert.equal(observedBody, JSON.stringify(raw));
    client?.destroy();
    response.destroy();
    await deadline(aborted, "original Request abort");
    assert.equal(observedRequest?.signal.aborted, true);
  } finally {
    client?.destroy();
    await server.shutdown();
  }
});

test("failed dispatch admission and ineligible native requests keep original HTTP failures", async () => {
  let admissions = 0;
  const server = serveHttp({ hostname: "127.0.0.1", port: 0 }, async (req) => {
    await req.text();
    const context = usage(() => {
      admissions++;
      return Promise.reject(new Error("admission refused"));
    });
    const wrapped = nativeHeaderUsageContext(req, raw, context);
    await assert.rejects(wrapped?.beforeProviderDispatch?.("lithos") ?? Promise.resolve(), /admission refused/);
    assert.equal(earlySseHeadersCommitted(req), false);
    const failure = errorResponse(429, "rate_limit_exceeded");
    assert.equal(await nativeFailureAfterEarlyHeaders(req, failure, context), failure);
    return failure;
  });
  try {
    const address = await server.ready;
    const response = await fetch(`http://127.0.0.1:${address.port}`, { method: "POST", headers: nativeHeaders, body: "{}" });
    assert.equal(response.status, 429);
    assert.equal(response.headers.get("retry-after"), "17");
    await response.text();
    assert.equal(admissions, 1);
  } finally {
    await server.shutdown();
  }
  const context = usage();
  for (const [headers, body] of [
    [{}, raw],
    [{ "x-codex-turn-metadata": "{}" }, raw],
    [nativeHeaders, { ...raw, stream: false }],
    [nativeHeaders, { ...raw, model: "gpt-oss-120b" }],
  ] as const) {
    const req = new Request("http://localhost/v1/responses", { method: "POST", headers });
    assert.equal(nativeHeaderUsageContext(req, body, context), context);
  }
});

for (const [status, code] of [
  [401, "authentication_error"],
  [429, "rate_limit_exceeded"],
  [503, "server_error"],
] as const) {
  test(`selected native HTTP${status} failure keeps internal identity and emits only truthful standard failure events`, async () => {
    let original: ReturnType<typeof originalNativeHttpFailure>;
    let admissions = 0;
    const context = usage(() => {
      admissions++;
      return Promise.resolve(undefined);
    });
    const server = serveHttp({ hostname: "127.0.0.1", port: 0 }, async (req) => {
      await req.text();
      await nativeHeaderUsageContext(req, raw, context)?.beforeProviderDispatch?.("lithos");
      const result = await nativeFailureAfterEarlyHeaders(req, errorResponse(status, code), context);
      original = originalNativeHttpFailure(req);
      return result;
    });
    try {
      const address = await server.ready;
      const response = await fetch(`http://127.0.0.1:${address.port}`, { method: "POST", headers: nativeHeaders, body: "{}" });
      assert.equal(response.status, 200);
      const events = [...(await response.text()).matchAll(/^data: (.+)$/gm)].map(
        (match) => JSON.parse(match[1]) as { type: string; response: { id: string; error: unknown; usage: unknown; output: unknown[] } }
      );
      assert.deepEqual(
        events.map((event) => event.type),
        ["response.created", "response.failed"]
      );
      assert.equal(events[0].response.id, events[1].response.id);
      assert.deepEqual(events[1].response.error, { code, message: `original ${code}` });
      assert.equal(events[1].response.usage, null);
      assert.deepEqual(events[1].response.output, []);
      assert.ok(original);
      assert.equal(original.status, status);
      assert.equal(original.headers["retry-after"], "17");
      assert.deepEqual(original.error, { code, message: `original ${code}`, param: "input", type: "original_type" });
      const telemetry = context.responseTelemetry;
      assert.ok(telemetry);
      assert.equal(telemetry.completed, false);
      assert.equal(telemetry.semanticOutputObserved, false);
      assert.equal(telemetry.usageObserved, false);
      assert.equal(admissions, 1);
    } finally {
      await server.shutdown();
    }
  });
}

test("native readiness retains one existing quota commitment and the transport's original compensation receipt", async () => {
  const now = Date.now();
  const record: ApiKeyHashRecord = {
    id: "readiness-ledger",
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: 3,
    usage_requests: 0,
    usage_reset_at_ms: now + 3600000,
    window_ms: 3600000,
    usage_quota_version: 3,
    paid_fallback_enabled: false,
    paid_fallback_limit_microcredits: 0,
    paid_fallback_spent_microcredits: 0,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
  };
  const policy = apiKeyPolicyFromHashRecord("readiness-ledger", record, now);
  assert.ok(policy);
  const kv = new CountingKv();
  kv.seed(apiKeyHashKey("readiness-ledger"), record);
  const admission = await reserveApiKeyUsageV3(policy, "request", "responses", { kv: kv as unknown as Kv, nowMs: now });
  assert.equal(admission.ok, true);
  let originalReceipt: ApiKeyProviderDispatch | undefined;
  const context = usage(async (provider) => {
    originalReceipt = await admission.reservation.beforeProviderDispatch(provider);
    return originalReceipt;
  });
  const req = new Request("http://localhost/v1/responses", { method: "POST", headers: nativeHeaders });
  const wrapped = nativeHeaderUsageContext(req, raw, context);
  const receipt = await wrapped?.beforeProviderDispatch?.("lithos");
  assert.equal(receipt, originalReceipt);
  assert.ok(receipt);
  receipt.markTransportStarted();
  assert.equal(await wrapped?.beforeProviderDispatch?.("deepseek"), undefined);
  await admission.reservation.release();
  await receipt.cancelBeforeTransport();
  const window = kv.entries.get(JSON.stringify(apiKeyUsageV3WindowKey(policy)))?.value as ApiKeyUsageWindowV3;
  assert.equal(window.committed_requests, 1);
  assert.equal(window.reserved_requests, 0);
});

test("failed header callback cannot discard the transport compensation receipt or reencode existing SSE", async () => {
  const req = new Request("http://localhost/v1/responses", { method: "POST", headers: nativeHeaders });
  const receipt: ApiKeyProviderDispatch = { markTransportStarted: () => {}, cancelBeforeTransport: () => Promise.resolve() };
  const context = usage(() => Promise.resolve(receipt));
  const remove = registerEarlySseHeaders(req, () => {
    throw new Error("fixture disconnected at flush");
  });
  try {
    assert.equal(await nativeHeaderUsageContext(req, raw, context)?.beforeProviderDispatch?.("lithos"), receipt);
    assert.equal(earlySseHeadersCommitted(req), false);
  } finally {
    remove();
  }
  const removeReady = registerEarlySseHeaders(req, () => true);
  try {
    await nativeHeaderUsageContext(req, raw, context)?.beforeProviderDispatch?.("lithos");
    const source = new Response("event: response.failed\ndata: original\n\n", { status: 503, headers: { "content-type": "text/event-stream" } });
    assert.equal(await nativeFailureAfterEarlyHeaders(req, source, context), source);
    assert.equal(await source.text(), "event: response.failed\ndata: original\n\n");
  } finally {
    removeReady();
  }
});

test("bounded native error evidence reaches the real terminal log after CORS and Response cloning", async () => {
  const req = new Request("http://localhost/v1/responses", { method: "POST", headers: nativeHeaders });
  const remove = registerEarlySseHeaders(req, () => true);
  const context = usage();
  const recorded: Record<string, unknown>[] = [];
  const originalInfo = console.info;
  console.info = (label: unknown, payload?: unknown) => {
    if (label === "[ai.ubq.fi] request_terminal") recorded.push(JSON.parse(String(payload)) as Record<string, unknown>);
  };
  try {
    await nativeHeaderUsageContext(req, raw, context)?.beforeProviderDispatch?.("lithos");
    const failure = errorResponse(503, "server_error");
    failure.headers.set("set-cookie", "private-cookie");
    failure.headers.set("authorization", "private-auth");
    const encoded = await nativeFailureAfterEarlyHeaders(req, failure, context);
    const telemetry = context.responseTelemetry;
    assert.ok(telemetry);
    attachResponseTelemetry(encoded, telemetry);
    const cors = withCors(encoded, req);
    const cloned = cors.clone();
    const cancelledUnusedBody = cors.body?.cancel();
    const wrapped = await withTerminalRequestLog(cloned, {
      route: "responses",
      request: req,
      requestId: "native-error-log-fixture",
      telemetryResponse: encoded,
      startedAtMonotonicMs: performance.now(),
      recordTelemetry: () => Promise.resolve({ status: "ignored", reason: "unknown_release", release: null, provider: null, route: null, model_hash: null }),
      recordCacheAnalytics: () => Promise.resolve({ status: "ignored", reason: "unknown_release", bucket_start_at_ms: null }),
      recordUsageRollup: () => Promise.resolve(false),
      recordAdminError: () => Promise.resolve(),
    });
    await wrapped.text();
    await cancelledUnusedBody;
    assert.equal(recorded.length, 1);
    assert.equal(recorded[0].status, 200);
    assert.deepEqual(recorded[0].native_http_failure, {
      status: 503,
      headers: { "content-type": "application/json", "retry-after": "17" },
      headers_truncated: false,
      error: { code: "server_error", message: "original server_error", param: "input", type: "original_type" },
      body_complete: true,
    });
    assert.equal(JSON.stringify(recorded).includes("private-cookie"), false);
    assert.equal(JSON.stringify(recorded).includes("private-auth"), false);
  } finally {
    console.info = originalInfo;
    remove();
  }
});

test("oversized selected HTTP error body is cancelled and never presented as complete original evidence", async () => {
  const req = new Request("http://localhost/v1/responses", { method: "POST", headers: nativeHeaders });
  const remove = registerEarlySseHeaders(req, () => true);
  const context = usage();
  let cancellations = 0;
  try {
    await nativeHeaderUsageContext(req, raw, context)?.beforeProviderDispatch?.("lithos");
    const failure = new Response(
      new ReadableStream<Uint8Array>({
        start(controller) {
          controller.enqueue(new TextEncoder().encode(JSON.stringify({ error: { code: "not_complete", message: "x".repeat(65537) } })));
        },
        cancel() {
          cancellations++;
        },
      }),
      { status: 502, headers: { "content-type": "application/json" } }
    );
    const encoded = await nativeFailureAfterEarlyHeaders(req, failure, context);
    assert.equal(cancellations, 1);
    const evidence = originalNativeHttpFailure(req);
    assert.ok(evidence);
    assert.equal(evidence.status, 502);
    assert.equal(evidence.body_complete, false);
    assert.equal(evidence.error, null);
    assert.equal((await encoded.text()).includes("not_complete"), false);
  } finally {
    remove();
  }
});
