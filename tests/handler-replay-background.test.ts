import assert from "node:assert/strict";
import { withTerminalRequestLog } from "../src/handler.ts";

type TerminalLogInput = Parameters<typeof withTerminalRequestLog>[1];
type ReplayInput = NonNullable<TerminalLogInput["sentinelReplayInput"]>;
type ReplayPersistence = NonNullable<TerminalLogInput["persistSentinelReplay"]>;
type RecordTelemetry = NonNullable<TerminalLogInput["recordTelemetry"]>;
type RecordAnalytics = NonNullable<TerminalLogInput["recordCacheAnalytics"]>;
type RecordAdminError = NonNullable<TerminalLogInput["recordAdminError"]>;

const acceptedInput = (): ReplayInput => ({
  endpoint: "/v1/responses",
  method: "POST",
  body: new TextEncoder().encode('{"model":"fixture"}'),
  content_type: "application/json",
  compatibility_headers: {},
  request_id: "handler-replay-background",
  git_sha: "fixture-git-sha",
  deno_revision: "fixture-deno-revision",
});

const ignoredTelemetry: RecordTelemetry = () =>
  Promise.resolve({
    status: "ignored" as const,
    reason: "unknown_release" as const,
    release: null,
    provider: null,
    route: null,
    model_hash: null,
  });

const ignoredAnalytics: RecordAnalytics = () =>
  Promise.resolve({
    status: "ignored" as const,
    reason: "unknown_release" as const,
    bucket_start_at_ms: null,
  });

Deno.test("SSE lifecycle watchdog cancels a hanging upstream and settles quota once", async () => {
  const delivery = Promise.withResolvers<void>();
  const upstreamReadStarted = Promise.withResolvers<void>();
  const terminalObserved = Promise.withResolvers<void>();
  const settlements: { outcome: string; reason?: string }[] = [];
  const terminalErrors: Parameters<RecordAdminError>[0][] = [];
  let upstreamCancelled = 0;
  const upstream = new ReadableStream<Uint8Array>({
    pull() {
      upstreamReadStarted.resolve();
      return new Promise<void>(() => {});
    },
    cancel() {
      upstreamCancelled += 1;
    },
  });
  const response = await withTerminalRequestLog(
    new Response(upstream, { status: 200, headers: { "Content-Type": "text/event-stream" } }),
    {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "lifecycle-watchdog-hanging-body",
      onTerminal: (outcome, reason) => {
        settlements.push({ outcome, reason });
      },
      deliveryCompleted: delivery.promise,
      deliverySignal: new AbortController().signal,
      streamLifecycleDeadlineMs: 25,
      recordTelemetry: ignoredTelemetry,
      recordCacheAnalytics: ignoredAnalytics,
      recordAdminError: (error) => {
        terminalErrors.push(error);
        terminalObserved.resolve();
        return Promise.resolve();
      },
    },
  );
  const reader = response.body!.getReader();
  const pendingRead = reader.read();
  await upstreamReadStarted.promise;
  await Promise.race([
    terminalObserved.promise,
    new Promise<never>((_, reject) => setTimeout(() => reject(new Error("lifecycle watchdog did not fire")), 500)),
  ]);
  assert.equal(upstreamCancelled, 1);
  assert.deepEqual(settlements, [{ outcome: "incomplete", reason: "stream_lifecycle_timeout" }]);
  assert.equal(terminalErrors.length, 1);
  assert.equal(terminalErrors[0]?.terminal_type, "deadline");
  assert.equal(terminalErrors[0]?.failure_kind, "stream_lifecycle_timeout");
  await reader.cancel("test cleanup").catch(() => {});
  await pendingRead.catch(() => {});
  delivery.resolve();
  await new Promise<void>((resolve) => setTimeout(resolve, 0));
  assert.equal(terminalErrors.length, 1);
  assert.equal(settlements.length, 1);
});

Deno.test("SSE lifecycle watchdog preserves terminal bytes before stalled delivery finalization", async () => {
  const delivery = Promise.withResolvers<void>();
  const terminalObserved = Promise.withResolvers<void>();
  const settlements: { outcome: string; reason?: string }[] = [];
  const terminalErrors: Parameters<RecordAdminError>[0][] = [];
  const terminalFrame = 'data: {"type":"response.completed","response":{"status":"completed"}}\n\n';
  const response = await withTerminalRequestLog(
    new Response(
      new ReadableStream<Uint8Array>({
        start(controller) {
          controller.enqueue(new TextEncoder().encode(terminalFrame));
          controller.close();
        },
      }),
      { status: 200, headers: { "Content-Type": "text/event-stream" } },
    ),
    {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "lifecycle-watchdog-stalled-delivery",
      onTerminal: (outcome, reason) => {
        settlements.push({ outcome, reason });
      },
      deliveryCompleted: delivery.promise,
      deliverySignal: new AbortController().signal,
      streamLifecycleDeadlineMs: 25,
      recordTelemetry: ignoredTelemetry,
      recordCacheAnalytics: ignoredAnalytics,
      recordAdminError: (error) => {
        terminalErrors.push(error);
        terminalObserved.resolve();
        return Promise.resolve();
      },
    },
  );
  const bodyStartedAt = performance.now();
  assert.equal(await response.text(), terminalFrame);
  assert.ok(performance.now() - bodyStartedAt < 250, "normal stream bytes were delayed by the watchdog");
  assert.equal(terminalErrors.length, 0);
  await terminalObserved.promise;
  assert.equal(terminalErrors.length, 1);
  assert.equal(settlements.length, 1);
  delivery.resolve();
  await new Promise<void>((resolve) => setTimeout(resolve, 0));
  assert.equal(terminalErrors.length, 1);
  assert.equal(settlements.length, 1);
});
Deno.test("terminal logging reports admission busy failures to the admin error ledger", async () => {
  const recorded: Parameters<RecordAdminError>[0][] = [];
  const response = await withTerminalRequestLog(
    new Response(
      JSON.stringify({
        error: {
          message: "All Codex accounts are busy",
          type: "server_error",
          code: "codex_admission_busy",
        },
      }),
      { status: 503, headers: { "Content-Type": "application/json" } },
    ),
    {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "admission-busy-admin-error",
      recordTelemetry: ignoredTelemetry,
      recordCacheAnalytics: ignoredAnalytics,
      recordAdminError: (error) => {
        recorded.push(error);
        return Promise.resolve();
      },
    },
  );

  assert.equal(response.status, 503);
  assert.equal(recorded.length, 1);
  assert.equal(recorded[0].request_id, "admission-busy-admin-error");
  assert.equal(recorded[0].status, 503);
  assert.equal(recorded[0].terminal_type, "http.error");
  assert.equal(recorded[0].failure_kind, "codex_admission_busy");
});

type TestGlobals = typeof globalThis & {
  EdgeRuntime?: Readonly<{ waitUntil?: (task: Promise<unknown>) => void }>;
};

Deno.test("EdgeRuntime replay registration returns failure responses before deferred persistence settles", async () => {
  const globals = globalThis as TestGlobals;
  const previousEdgeRuntime = globals.EdgeRuntime;
  const registeredTasks: Promise<unknown>[] = [];
  const persistence = Promise.withResolvers<Awaited<ReturnType<ReplayPersistence>>>();
  const persistenceStarted = Promise.withResolvers<void>();
  let persistenceSettled = false;
  let persistedSnapshot: ReplayInput | null = null;
  const capture = acceptedInput();
  const originalBytes = [...capture.body];

  globals.EdgeRuntime = {
    waitUntil(task) {
      registeredTasks.push(task);
    },
  };

  try {
    const response = await withTerminalRequestLog(
      new Response('{"error":{"code":"provider_transport"}}', {
        status: 502,
        headers: { "Content-Type": "application/json" },
      }),
      {
        route: "responses",
        startedAtMonotonicMs: performance.now(),
        requestId: "handler-replay-background",
        sentinelReplayInput: capture,
        persistSentinelReplay: (snapshot) => {
          persistedSnapshot = snapshot;
          persistenceStarted.resolve();
          return persistence.promise;
        },
        recordTelemetry: ignoredTelemetry,
        recordCacheAnalytics: ignoredAnalytics,
      },
    );

    assert.equal(response.status, 502);
    await persistenceStarted.promise;
    assert.equal(registeredTasks.length, 1);
    const snapshot = persistedSnapshot as ReplayInput | null;
    if (snapshot === null) throw new Error("replay persistence did not receive a snapshot");
    assert.notEqual(snapshot.body, capture.body);
    assert.deepEqual([...snapshot.body], originalBytes);
    assert.ok(capture.body.every((byte) => byte === 0));
    assert.equal(persistenceSettled, false);

    persistenceSettled = true;
    persistence.reject(new Error("fixture persistence failure"));
    await registeredTasks[0];
    assert.equal(response.status, 502);
    assert.ok(capture.body.every((byte) => byte === 0));
  } finally {
    if (!persistenceSettled) {
      persistenceSettled = true;
      persistence.resolve({ status: "disabled", reason: "kv_unavailable" });
    }
    await Promise.allSettled(registeredTasks);
    if (previousEdgeRuntime === undefined) delete globals.EdgeRuntime;
    else globals.EdgeRuntime = previousEdgeRuntime;
  }
});
