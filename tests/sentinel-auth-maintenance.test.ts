import assert from "node:assert/strict";
import {
  type CodexAuthAppServerRequest,
  type CodexAuthAppServerResult,
  type CodexAuthAppServerRuntime,
  codexAuthMaintenanceArgs,
  type CodexAuthMaintenanceDependencies,
  codexAuthMaintenanceDue,
  type CodexAuthMaintenanceStage,
  maintainCodexAuthSlot,
  maintainCodexAuthSlots,
  runCodexAuthAppServerWithRuntime,
  SENTINEL_CODEX_AUTH_MAINTENANCE_INTERVAL_MS,
  SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS,
  SENTINEL_CODEX_AUTH_READINESS_MIN_VALIDITY_MS,
} from "../scripts/sentinel/auth-maintenance.ts";

const NOW = 1_800_000_000_000;
const MINUTE_MS = 60_000;
const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();
const fileSystemPermissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
]);
const fileSystemTestsUnavailable = fileSystemPermissions.some((permission) => permission.state !== "granted");

const base64Url = (value: string): string => btoa(value).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");

const jwt = (expiresAtMs: number, label: string): string =>
  `${base64Url(JSON.stringify({ alg: "none" }))}.${
    base64Url(JSON.stringify({ exp: Math.floor(expiresAtMs / 1_000), label }))
  }.signature`;

const authJson = (
  label: string,
  account: string,
  expiresAtMs: number,
  refreshedAtMs: number,
  includeUnknownField = true,
): string => {
  const value: Record<string, unknown> = {
    auth_mode: "chatgpt",
    tokens: {
      id_token: jwt(expiresAtMs, `id-${label}`),
      access_token: jwt(expiresAtMs, `access-${label}`),
      refresh_token: `refresh-${label}`,
      account_id: account,
    },
    last_refresh: new Date(refreshedAtMs).toISOString(),
  };
  if (includeUnknownField) value.preserved_unknown_field = { label };
  return JSON.stringify(value);
};

const appServerResult = (
  overrides: Partial<CodexAuthAppServerResult> = {},
): CodexAuthAppServerResult => ({
  code: overrides.code ?? 0,
  rpcSucceeded: overrides.rpcSucceeded ?? true,
  outputExceeded: overrides.outputExceeded ?? false,
  timedOut: overrides.timedOut ?? false,
  stdoutBytes: overrides.stdoutBytes ?? 0,
  stderrBytes: overrides.stderrBytes ?? 0,
  durationMs: overrides.durationMs ?? 10,
});

const memoryMaintenanceRuntime = (before: string) => {
  const durablePath = "/private/slot-1/auth.json";
  const stage: CodexAuthMaintenanceStage = {
    directory: "/private/slot-1/.sentinel-auth-maintenance-test",
    authPath: "/private/slot-1/.sentinel-auth-maintenance-test/auth.json",
  };
  const state = {
    files: new Map([[durablePath, before]]),
    staged: false,
    promoted: false,
    discarded: false,
  };
  const dependencies: CodexAuthMaintenanceDependencies = {
    now: () => NOW,
    readTextFile: (path) => {
      const value = state.files.get(path);
      if (value === undefined) return Promise.reject(new Error(`Missing test file ${path}`));
      return Promise.resolve(value);
    },
    stageAuth: (slotDirectory, rawJson) => {
      assert.equal(slotDirectory, "/private/slot-1");
      assert.equal(state.staged, false);
      assert.equal(rawJson, before);
      state.staged = true;
      state.files.set(stage.authPath, rawJson);
      return Promise.resolve(stage);
    },
    promoteAuth: (stagedAuthPath, durableAuthPath, expectedDurableRaw) => {
      assert.equal(stagedAuthPath, stage.authPath);
      assert.equal(durableAuthPath, durablePath);
      if (state.files.get(durablePath) !== expectedDurableRaw) {
        return Promise.reject(new Error("Sentinel Codex auth changed during maintenance"));
      }
      state.files.set(durablePath, state.files.get(stage.authPath)!);
      state.files.delete(stage.authPath);
      state.promoted = true;
      return Promise.resolve();
    },
    discardStage: (candidate) => {
      assert.deepEqual(candidate, stage);
      state.files.delete(stage.authPath);
      state.discarded = true;
      return Promise.resolve();
    },
  };
  return { dependencies, durablePath, stage, state };
};

const request = (overrides: Partial<CodexAuthAppServerRequest> = {}): CodexAuthAppServerRequest => ({
  executable: overrides.executable ?? "codex",
  args: overrides.args ?? codexAuthMaintenanceArgs(),
  cwd: overrides.cwd ?? "/private/workspace",
  env: overrides.env ?? { CODEX_HOME: "/private/stage", HOME: "/private/stage" },
  clearEnv: true,
  outputLimitBytes: overrides.outputLimitBytes ?? 4_096,
  timeoutMs: overrides.timeoutMs ?? 1_000,
});

type ScriptedRuntimeOptions = Readonly<{
  onMessage?: (
    message: Record<string, unknown>,
    emitJson: (value: unknown) => void,
    emitRaw: (value: string) => void,
  ) => void | Promise<void>;
  stderr?: string;
  statusCode?: number;
  fireTimer?: boolean;
}>;

const scriptedRuntime = (options: ScriptedRuntimeOptions = {}) => {
  const messages: Record<string, unknown>[] = [];
  const spawnedRequests: CodexAuthAppServerRequest[] = [];
  const killSignals: (Deno.Signal | undefined)[] = [];
  let stdoutController!: ReadableStreamDefaultController<Uint8Array>;
  let stdoutClosed = false;
  let statusSettled = false;
  let resolveStatus!: (status: Readonly<{ code: number }>) => void;
  let timerCleared = false;
  let currentNow = 100;
  const status = new Promise<Readonly<{ code: number }>>((resolve) => {
    resolveStatus = resolve;
  });
  const stdout = new ReadableStream<Uint8Array>({
    start(controller) {
      stdoutController = controller;
    },
  });
  const closeStdout = (): void => {
    if (stdoutClosed) return;
    stdoutClosed = true;
    stdoutController.close();
  };
  const settleStatus = (code: number): void => {
    if (statusSettled) return;
    statusSettled = true;
    resolveStatus({ code });
  };
  const emitRaw = (value: string): void => {
    if (stdoutClosed) return;
    stdoutController.enqueue(TEXT_ENCODER.encode(value));
  };
  const emitJson = (value: unknown): void => emitRaw(`${JSON.stringify(value)}\n`);
  const stdin = new WritableStream<Uint8Array>({
    async write(chunk) {
      const line = TEXT_DECODER.decode(chunk).trimEnd();
      const message: unknown = JSON.parse(line);
      assert.ok(message && typeof message === "object" && !Array.isArray(message));
      messages.push(message as Record<string, unknown>);
      await options.onMessage?.(message as Record<string, unknown>, emitJson, emitRaw);
    },
    close() {
      closeStdout();
      settleStatus(options.statusCode ?? 0);
    },
  });
  const stderr = new Blob([options.stderr ?? ""]).stream();
  const runtime: CodexAuthAppServerRuntime = {
    spawn(candidate) {
      spawnedRequests.push(candidate);
      return {
        status,
        stdout,
        stderr,
        stdin,
        kill(signal) {
          killSignals.push(signal);
          closeStdout();
          settleStatus(137);
        },
      };
    },
    now() {
      const value = currentNow;
      currentNow += 25;
      return value;
    },
    setTimer(callback, _delayMs) {
      if (options.fireTimer) queueMicrotask(callback);
      return 1 as unknown as ReturnType<typeof setTimeout>;
    },
    clearTimer() {
      timerCleared = true;
    },
  };
  return {
    emitJson,
    emitRaw,
    killSignals,
    messages,
    runtime,
    spawnedRequests,
    get timerCleared() {
      return timerCleared;
    },
  };
};

const initializeResult = (codexHome = "/private/stage") => ({
  id: 0,
  result: {
    userAgent: "codex_cli_rs/0.149.0",
    codexHome,
    platformFamily: "unix",
    platformOs: "linux",
  },
});

const accountResult = () => ({
  id: 1,
  result: {
    account: { type: "chatgpt", email: "sentinel@example.com", planType: "pro" },
    requiresOpenaiAuth: true,
  },
});

Deno.test("auth maintenance due policy covers refresh age and access lifetime", () => {
  assert.equal(
    codexAuthMaintenanceDue({
      lastRefresh: new Date(NOW - SENTINEL_CODEX_AUTH_MAINTENANCE_INTERVAL_MS + 1).toISOString(),
      accessTokenExpiresAtMs: NOW + SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS + 1,
    }, NOW),
    false,
  );
  assert.equal(
    codexAuthMaintenanceDue({
      lastRefresh: new Date(NOW - SENTINEL_CODEX_AUTH_MAINTENANCE_INTERVAL_MS).toISOString(),
      accessTokenExpiresAtMs: NOW + SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS + 1,
    }, NOW),
    true,
  );
  assert.equal(
    codexAuthMaintenanceDue({
      lastRefresh: new Date(NOW).toISOString(),
      accessTokenExpiresAtMs: NOW + SENTINEL_CODEX_AUTH_MAINTENANCE_MIN_VALIDITY_MS,
    }, NOW),
    true,
  );
});

Deno.test("auth maintenance uses the pinned app-server file-auth invocation", () => {
  assert.deepEqual(codexAuthMaintenanceArgs(), [
    "app-server",
    "--listen",
    "stdio://",
    "--strict-config",
    "-c",
    'cli_auth_credentials_store="file"',
  ]);
});

Deno.test("app-server waits for initialize before the exact initialized and refresh requests", async () => {
  let resolveInitializeWrite!: () => void;
  const initializeWritten = new Promise<void>((resolve) => {
    resolveInitializeWrite = resolve;
  });
  const scripted = scriptedRuntime({
    onMessage(message, emitJson) {
      if (message.method === "initialize") resolveInitializeWrite();
      if (message.method === "account/read") emitJson(accountResult());
    },
  });
  const candidate = request();
  const pending = runCodexAuthAppServerWithRuntime(candidate, scripted.runtime);
  await initializeWritten;
  assert.equal(scripted.messages.length, 1);
  assert.deepEqual(scripted.messages[0], {
    method: "initialize",
    id: 0,
    params: {
      clientInfo: {
        name: "provider_sentinel_auth_maintenance",
        title: "Provider Sentinel Auth Maintenance",
        version: "1.0.0",
      },
    },
  });
  scripted.emitJson({ method: "account/updated", params: { authMode: "chatgpt", planType: "pro" } });
  scripted.emitJson(initializeResult());
  const outcome = await pending;
  assert.deepEqual(scripted.messages, [
    scripted.messages[0],
    { method: "initialized" },
    { method: "account/read", id: 1, params: { refreshToken: true } },
  ]);
  assert.equal(outcome.rpcSucceeded, true);
  assert.equal(outcome.code, 0);
  assert.equal(scripted.spawnedRequests[0], candidate);
  assert.deepEqual(scripted.killSignals, []);
  assert.equal(scripted.timerCleared, true);
});

Deno.test("app-server rejects an initialize response for another Codex home", async () => {
  const scripted = scriptedRuntime({
    onMessage(message, emitJson) {
      if (message.method === "initialize") emitJson(initializeResult("/private/wrong-home"));
    },
  });
  const outcome = await runCodexAuthAppServerWithRuntime(request(), scripted.runtime);
  assert.equal(outcome.rpcSucceeded, false);
  assert.deepEqual(scripted.messages.map((message) => message.method), ["initialize"]);
});

Deno.test("app-server rejects malformed trailing protocol output", async () => {
  const scripted = scriptedRuntime({
    onMessage(message, emitJson, emitRaw) {
      if (message.method === "initialize") emitJson(initializeResult());
      if (message.method === "account/read") {
        emitJson(accountResult());
        emitRaw("not-json\n");
      }
    },
  });
  const outcome = await runCodexAuthAppServerWithRuntime(request(), scripted.runtime);
  assert.equal(outcome.rpcSucceeded, false);
});

Deno.test("app-server bounds private output and records its first stop reason", async () => {
  const scripted = scriptedRuntime({
    onMessage(message, _emitJson, emitRaw) {
      if (message.method === "initialize") emitRaw(`${"x".repeat(128)}\n`);
    },
  });
  const outcome = await runCodexAuthAppServerWithRuntime(
    request({ outputLimitBytes: 64 }),
    scripted.runtime,
  );
  assert.equal(outcome.outputExceeded, true);
  assert.equal(outcome.timedOut, false);
  assert.equal(outcome.rpcSucceeded, false);
  assert.ok(outcome.stdoutBytes > 64);
  assert.deepEqual(scripted.killSignals, ["SIGKILL"]);
});

Deno.test("app-server enforces one combined stdout and stderr output bound", async () => {
  const scripted = scriptedRuntime({
    stderr: "e".repeat(40),
    onMessage(message, _emitJson, emitRaw) {
      if (message.method === "initialize") emitRaw(`${"o".repeat(40)}\n`);
    },
  });
  const outcome = await runCodexAuthAppServerWithRuntime(
    request({ outputLimitBytes: 64 }),
    scripted.runtime,
  );
  assert.equal(outcome.outputExceeded, true);
  assert.equal(outcome.rpcSucceeded, false);
  assert.ok(outcome.stdoutBytes + outcome.stderrBytes > 64);
  assert.ok(outcome.stdoutBytes <= 64);
  assert.ok(outcome.stderrBytes <= 64);
  assert.deepEqual(scripted.killSignals, ["SIGKILL"]);
});

Deno.test("app-server kills a stalled protocol at its timeout", async () => {
  const scripted = scriptedRuntime({ fireTimer: true });
  const outcome = await runCodexAuthAppServerWithRuntime(request(), scripted.runtime);
  assert.equal(outcome.timedOut, true);
  assert.equal(outcome.outputExceeded, false);
  assert.equal(outcome.rpcSucceeded, false);
  assert.deepEqual(scripted.killSignals, ["SIGKILL"]);
});

Deno.test("an access JWT with exactly 30 minutes remaining invokes explicit refresh", async () => {
  const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
  const after = authJson("after", "stable-account", NOW + 51 * MINUTE_MS, NOW, false);
  const runtime = memoryMaintenanceRuntime(before);
  let capturedRequest: CodexAuthAppServerRequest | null = null;
  const disposition = await maintainCodexAuthSlot({
    slot: 1,
    slotDirectory: "/private/slot-1",
    workspace: "/private/empty",
  }, {
    ...runtime.dependencies,
    runAppServer: (candidate) => {
      capturedRequest = candidate;
      assert.equal(runtime.state.files.get(runtime.stage.authPath), before);
      runtime.state.files.set(runtime.stage.authPath, after);
      return Promise.resolve(appServerResult());
    },
  });
  assert.ok(capturedRequest);
  assert.deepEqual((capturedRequest as CodexAuthAppServerRequest).args, codexAuthMaintenanceArgs());
  assert.equal((capturedRequest as CodexAuthAppServerRequest).cwd, "/private/empty");
  assert.equal((capturedRequest as CodexAuthAppServerRequest).env.CODEX_HOME, runtime.stage.directory);
  assert.equal((capturedRequest as CodexAuthAppServerRequest).env.HOME, runtime.stage.directory);
  assert.equal((capturedRequest as CodexAuthAppServerRequest).clearEnv, true);
  assert.equal(disposition.due, true);
  assert.equal(disposition.invoked, true);
  assert.equal(disposition.rpcSucceeded, true);
  assert.equal(disposition.duplicateAccountSkipped, false);
  assert.equal(disposition.stateChanged, true);
  assert.equal(disposition.readyForMaintenanceWindow, true);
  assert.equal(runtime.state.files.get(runtime.durablePath), after);
  assert.equal("preserved_unknown_field" in JSON.parse(runtime.state.files.get(runtime.durablePath)!), false);
  assert.equal(runtime.state.promoted, true);
  assert.equal(runtime.state.discarded, true);
});

Deno.test("a nonzero exit and failed RPC still promote a valid token rotation", async () => {
  const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
  const after = authJson("after", "stable-account", NOW + 2 * 60 * MINUTE_MS, NOW, false);
  const runtime = memoryMaintenanceRuntime(before);
  const disposition = await maintainCodexAuthSlot({
    slot: 1,
    slotDirectory: "/private/slot-1",
    workspace: "/private/empty",
  }, {
    ...runtime.dependencies,
    runAppServer: () => {
      runtime.state.files.set(runtime.stage.authPath, after);
      return Promise.resolve(appServerResult({ code: 17, rpcSucceeded: false, stderrBytes: 73 }));
    },
  });
  assert.equal(disposition.commandCode, 17);
  assert.equal(disposition.rpcSucceeded, false);
  assert.equal(disposition.stderrBytes, 73);
  assert.equal(disposition.stateChanged, true);
  assert.equal(disposition.readyForMaintenanceWindow, true);
  assert.equal(runtime.state.files.get(runtime.durablePath), after);
});

Deno.test("rotated access at or below 50 minutes is promoted but is not ready", async () => {
  for (const remainingMs of [SENTINEL_CODEX_AUTH_READINESS_MIN_VALIDITY_MS, 49 * MINUTE_MS]) {
    const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
    const after = authJson(`after-${remainingMs}`, "stable-account", NOW + remainingMs, NOW, false);
    const runtime = memoryMaintenanceRuntime(before);
    const disposition = await maintainCodexAuthSlot({
      slot: 1,
      slotDirectory: "/private/slot-1",
      workspace: "/private/empty",
    }, {
      ...runtime.dependencies,
      runAppServer: () => {
        runtime.state.files.set(runtime.stage.authPath, after);
        return Promise.resolve(appServerResult());
      },
    });
    assert.equal(disposition.stateChanged, true);
    assert.equal(disposition.readyForMaintenanceWindow, false);
    assert.equal(runtime.state.files.get(runtime.durablePath), after);
  }
});

Deno.test("an id-token-only transition promotes the complete staged file", async () => {
  const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
  const afterValue = JSON.parse(before);
  afterValue.tokens.id_token = jwt(NOW + 2 * 60 * MINUTE_MS, "rotated-id-only");
  afterValue.last_refresh = new Date(NOW + 1_000).toISOString();
  const after = `${JSON.stringify(afterValue, null, 2)}\n`;
  const runtime = memoryMaintenanceRuntime(before);
  const disposition = await maintainCodexAuthSlot({
    slot: 1,
    slotDirectory: "/private/slot-1",
    workspace: "/private/empty",
  }, {
    ...runtime.dependencies,
    runAppServer: () => {
      runtime.state.files.set(runtime.stage.authPath, after);
      return Promise.resolve(appServerResult());
    },
  });
  assert.equal(disposition.stateChanged, true);
  assert.equal(runtime.state.files.get(runtime.durablePath), after);
});

Deno.test("a refresh-token-only transition preserves the rotated credential", async () => {
  const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
  const afterValue = JSON.parse(before);
  afterValue.tokens.refresh_token = "refresh-rotated-only";
  afterValue.last_refresh = new Date(NOW + 1_000).toISOString();
  const after = `${JSON.stringify(afterValue, null, 2)}\n`;
  const runtime = memoryMaintenanceRuntime(before);
  const disposition = await maintainCodexAuthSlot({
    slot: 1,
    slotDirectory: "/private/slot-1",
    workspace: "/private/empty",
  }, {
    ...runtime.dependencies,
    runAppServer: () => {
      runtime.state.files.set(runtime.stage.authPath, after);
      return Promise.resolve(appServerResult({ code: 9, rpcSucceeded: false }));
    },
  });
  assert.equal(disposition.commandCode, 9);
  assert.equal(disposition.rpcSucceeded, false);
  assert.equal(disposition.stateChanged, true);
  assert.equal(disposition.readyForMaintenanceWindow, false);
  assert.equal(runtime.state.files.get(runtime.durablePath), after);
  assert.equal(JSON.parse(runtime.state.files.get(runtime.durablePath)!).tokens.refresh_token, "refresh-rotated-only");
  assert.equal(runtime.state.promoted, true);
});

Deno.test("unchanged staged auth preserves the durable bytes", async () => {
  const before = `${authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW)}\n`;
  const runtime = memoryMaintenanceRuntime(before);
  const disposition = await maintainCodexAuthSlot({
    slot: 1,
    slotDirectory: "/private/slot-1",
    workspace: "/private/empty",
  }, {
    ...runtime.dependencies,
    runAppServer: () => Promise.resolve(appServerResult()),
  });
  assert.equal(disposition.stateChanged, false);
  assert.equal(disposition.readyForMaintenanceWindow, false);
  assert.equal(runtime.state.files.get(runtime.durablePath), before);
  assert.equal(runtime.state.promoted, false);
  assert.equal(runtime.state.discarded, true);
});

Deno.test("a last-refresh-only rewrite is discarded", async () => {
  const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
  const afterValue = JSON.parse(before);
  afterValue.last_refresh = new Date(NOW + MINUTE_MS).toISOString();
  const after = JSON.stringify(afterValue, null, 2);
  const runtime = memoryMaintenanceRuntime(before);
  const disposition = await maintainCodexAuthSlot({
    slot: 1,
    slotDirectory: "/private/slot-1",
    workspace: "/private/empty",
  }, {
    ...runtime.dependencies,
    runAppServer: () => {
      runtime.state.files.set(runtime.stage.authPath, after);
      return Promise.resolve(appServerResult());
    },
  });
  assert.equal(disposition.stateChanged, false);
  assert.equal(runtime.state.files.get(runtime.durablePath), before);
  assert.equal(runtime.state.promoted, false);
});

Deno.test("a staged account swap is rejected and leaves durable auth unchanged", async () => {
  const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
  const swapped = authJson("after", "different-account", NOW + 2 * 60 * MINUTE_MS, NOW, false);
  const runtime = memoryMaintenanceRuntime(before);
  await assert.rejects(
    () =>
      maintainCodexAuthSlot({
        slot: 1,
        slotDirectory: "/private/slot-1",
        workspace: "/private/empty",
      }, {
        ...runtime.dependencies,
        runAppServer: () => {
          runtime.state.files.set(runtime.stage.authPath, swapped);
          return Promise.resolve(appServerResult());
        },
      }),
    /changed account identity/u,
  );
  assert.equal(runtime.state.files.get(runtime.durablePath), before);
  assert.equal(runtime.state.promoted, false);
  assert.equal(runtime.state.discarded, true);
});

Deno.test("a malformed staged candidate is rejected and leaves durable auth unchanged", async () => {
  const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
  const runtime = memoryMaintenanceRuntime(before);
  await assert.rejects(() =>
    maintainCodexAuthSlot({
      slot: 1,
      slotDirectory: "/private/slot-1",
      workspace: "/private/empty",
    }, {
      ...runtime.dependencies,
      runAppServer: () => {
        runtime.state.files.set(runtime.stage.authPath, "{malformed");
        return Promise.resolve(appServerResult());
      },
    })
  );
  assert.equal(runtime.state.files.get(runtime.durablePath), before);
  assert.equal(runtime.state.promoted, false);
  assert.equal(runtime.state.discarded, true);
});

Deno.test("a runner exception with no transition preserves durable auth", async () => {
  const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
  const runtime = memoryMaintenanceRuntime(before);
  await assert.rejects(
    () =>
      maintainCodexAuthSlot({
        slot: 1,
        slotDirectory: "/private/slot-1",
        workspace: "/private/empty",
      }, {
        ...runtime.dependencies,
        runAppServer: () => Promise.reject(new Error("synthetic runner failure")),
      }),
    /synthetic runner failure/u,
  );
  assert.equal(runtime.state.files.get(runtime.durablePath), before);
  assert.equal(runtime.state.promoted, false);
  assert.equal(runtime.state.discarded, true);
});

Deno.test("a runner exception after token rotation promotes then rethrows", async () => {
  const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
  const after = authJson("after", "stable-account", NOW + 2 * 60 * MINUTE_MS, NOW, false);
  const runtime = memoryMaintenanceRuntime(before);
  await assert.rejects(
    () =>
      maintainCodexAuthSlot({
        slot: 1,
        slotDirectory: "/private/slot-1",
        workspace: "/private/empty",
      }, {
        ...runtime.dependencies,
        runAppServer: () => {
          runtime.state.files.set(runtime.stage.authPath, after);
          return Promise.reject(new Error("synthetic post-write runner failure"));
        },
      }),
    /synthetic post-write runner failure/u,
  );
  assert.equal(runtime.state.files.get(runtime.durablePath), after);
  assert.equal(runtime.state.promoted, true);
  assert.equal(runtime.state.discarded, true);
});

Deno.test("auth maintenance skips a fresh ready file", async () => {
  const fresh = authJson("fresh", "stable-account", NOW + 2 * 24 * 60 * MINUTE_MS, NOW);
  let calls = 0;
  const skipped = await maintainCodexAuthSlot({
    slot: 1,
    slotDirectory: "/private/slot-1",
    workspace: "/private/empty",
  }, {
    now: () => NOW,
    readTextFile: () => Promise.resolve(fresh),
    runAppServer: () => {
      calls += 1;
      return Promise.resolve(appServerResult());
    },
  });
  assert.equal(skipped.due, false);
  assert.equal(skipped.invoked, false);
  assert.equal(skipped.rpcSucceeded, false);
  assert.equal(skipped.duplicateAccountSkipped, false);
  assert.equal(skipped.readyForMaintenanceWindow, true);
  assert.equal(calls, 0);
});

Deno.test("duplicate accounts select one canonical slot and leave the other byte-exact", async () => {
  const cases = [
    {
      name: "later refresh",
      slot1: authJson("one", "shared-account", NOW + 30 * MINUTE_MS, NOW - MINUTE_MS),
      slot2: authJson("two", "shared-account", NOW + 40 * MINUTE_MS, NOW - 2 * MINUTE_MS),
      expectedSlot: 1,
    },
    {
      name: "later access expiry",
      slot1: authJson("one", "shared-account", NOW + 30 * MINUTE_MS, NOW),
      slot2: authJson("two", "shared-account", NOW + 40 * MINUTE_MS, NOW),
      expectedSlot: 2,
    },
    {
      name: "lower slot",
      slot1: authJson("same", "shared-account", NOW + 30 * MINUTE_MS, NOW),
      slot2: authJson("same", "shared-account", NOW + 30 * MINUTE_MS, NOW),
      expectedSlot: 1,
    },
  ] as const;
  for (const candidate of cases) {
    const durablePaths = {
      1: "/private/slots/1/auth.json",
      2: "/private/slots/2/auth.json",
    } as const;
    const files = new Map<string, string>([
      [durablePaths[1], candidate.slot1],
      [durablePaths[2], candidate.slot2],
    ]);
    const invokedSlots: number[] = [];
    const dependencies: CodexAuthMaintenanceDependencies = {
      now: () => NOW,
      readTextFile: (path) => {
        const value = files.get(path);
        return value === undefined ? Promise.reject(new Error(`missing ${path}`)) : Promise.resolve(value);
      },
      stageAuth: (slotDirectory, rawJson) => {
        const stage = { directory: `${slotDirectory}/.stage`, authPath: `${slotDirectory}/.stage/auth.json` };
        files.set(stage.authPath, rawJson);
        return Promise.resolve(stage);
      },
      promoteAuth: () => Promise.reject(new Error("unexpected promotion")),
      discardStage: (stage) => {
        files.delete(stage.authPath);
        return Promise.resolve();
      },
      runAppServer: (appServerRequest) => {
        const slot = Number(appServerRequest.env.CODEX_HOME.split("/").at(-2));
        invokedSlots.push(slot);
        return Promise.resolve(appServerResult());
      },
    };
    const dispositions = await maintainCodexAuthSlots([
      { slot: 1, slotDirectory: "/private/slots/1", workspace: "/private/workspace" },
      { slot: 2, slotDirectory: "/private/slots/2", workspace: "/private/workspace" },
    ], dependencies);
    assert.deepEqual(invokedSlots, [candidate.expectedSlot], candidate.name);
    const invoked = dispositions.find((disposition) => disposition.slot === candidate.expectedSlot)!;
    const skipped = dispositions.find((disposition) => disposition.slot !== candidate.expectedSlot)!;
    assert.equal(invoked.invoked, true, candidate.name);
    assert.equal(invoked.duplicateAccountSkipped, false, candidate.name);
    assert.equal(skipped.invoked, false, candidate.name);
    assert.equal(skipped.rpcSucceeded, false, candidate.name);
    assert.equal(skipped.duplicateAccountSkipped, true, candidate.name);
    assert.equal(skipped.commandCode, null, candidate.name);
    assert.equal(files.get(durablePaths[1]), candidate.slot1, candidate.name);
    assert.equal(files.get(durablePaths[2]), candidate.slot2, candidate.name);
  }
});

Deno.test("distinct accounts are both invoked for due maintenance", async () => {
  const durablePaths = {
    1: "/private/slots/1/auth.json",
    2: "/private/slots/2/auth.json",
  } as const;
  const files = new Map<string, string>([
    [durablePaths[1], authJson("one", "account-one", NOW + 30 * MINUTE_MS, NOW)],
    [durablePaths[2], authJson("two", "account-two", NOW + 40 * MINUTE_MS, NOW)],
  ]);
  const invokedSlots: number[] = [];
  const dependencies: CodexAuthMaintenanceDependencies = {
    now: () => NOW,
    readTextFile: (path) => {
      const value = files.get(path);
      return value === undefined ? Promise.reject(new Error(`missing ${path}`)) : Promise.resolve(value);
    },
    stageAuth: (slotDirectory, rawJson) => {
      const stage = { directory: `${slotDirectory}/.stage`, authPath: `${slotDirectory}/.stage/auth.json` };
      files.set(stage.authPath, rawJson);
      return Promise.resolve(stage);
    },
    promoteAuth: () => Promise.reject(new Error("unexpected promotion")),
    discardStage: (stage) => {
      files.delete(stage.authPath);
      return Promise.resolve();
    },
    runAppServer: (appServerRequest) => {
      invokedSlots.push(Number(appServerRequest.env.CODEX_HOME.split("/").at(-2)));
      return Promise.resolve(appServerResult());
    },
  };
  const dispositions = await maintainCodexAuthSlots([
    { slot: 1, slotDirectory: "/private/slots/1", workspace: "/private/workspace" },
    { slot: 2, slotDirectory: "/private/slots/2", workspace: "/private/workspace" },
  ], dependencies);
  assert.deepEqual(invokedSlots, [1, 2]);
  assert.deepEqual(dispositions.map((disposition) => disposition.slot), [1, 2]);
  assert.ok(dispositions.every((disposition) => disposition.invoked));
  assert.ok(dispositions.every((disposition) => !disposition.duplicateAccountSkipped));
});

Deno.test({
  name: "filesystem staging is private, same-filesystem, atomic, and cleaned after promotion",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-auth-maintenance-" });
    const slotDirectory = `${root}/slot-1`;
    const workspace = `${root}/workspace`;
    const authPath = `${slotDirectory}/auth.json`;
    const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
    const after = authJson("after", "stable-account", NOW + 2 * 60 * MINUTE_MS, NOW, false);
    let stageDirectory = "";
    let stagedInode: number | null = null;
    try {
      await Deno.mkdir(slotDirectory, { mode: 0o700 });
      await Deno.mkdir(workspace, { mode: 0o700 });
      await Deno.writeTextFile(authPath, before, { mode: 0o600 });
      const slotInfo = await Deno.stat(slotDirectory);
      const disposition = await maintainCodexAuthSlot({ slot: 1, slotDirectory, workspace }, {
        now: () => NOW,
        runAppServer: async (candidate) => {
          stageDirectory = candidate.env.CODEX_HOME;
          assert.notEqual(stageDirectory, slotDirectory);
          const stagedPath = `${stageDirectory}/auth.json`;
          const stageInfo = await Deno.stat(stageDirectory);
          const stagedInfo = await Deno.stat(stagedPath);
          assert.equal(stageInfo.dev, slotInfo.dev);
          assert.equal((stageInfo.mode ?? 0) & 0o777, 0o700);
          assert.equal((stagedInfo.mode ?? 0) & 0o777, 0o600);
          assert.equal(await Deno.readTextFile(stagedPath), before);
          await Deno.writeTextFile(stagedPath, after);
          stagedInode = (await Deno.stat(stagedPath)).ino;
          return appServerResult({ code: 9, rpcSucceeded: false });
        },
      });
      assert.equal(disposition.stateChanged, true);
      assert.equal(disposition.commandCode, 9);
      assert.equal(disposition.rpcSucceeded, false);
      assert.equal(await Deno.readTextFile(authPath), after);
      const promotedInfo = await Deno.stat(authPath);
      assert.equal((promotedInfo.mode ?? 0) & 0o777, 0o600);
      assert.notEqual(stagedInode, null);
      assert.equal(promotedInfo.ino, stagedInode);
      await assert.rejects(() => Deno.stat(stageDirectory), Deno.errors.NotFound);
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "filesystem compare-and-swap preserves a concurrent durable writer",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-auth-maintenance-cas-" });
    const slotDirectory = `${root}/slot-1`;
    const workspace = `${root}/workspace`;
    const authPath = `${slotDirectory}/auth.json`;
    const before = authJson("before", "stable-account", NOW + 30 * MINUTE_MS, NOW);
    const after = authJson("after", "stable-account", NOW + 2 * 60 * MINUTE_MS, NOW, false);
    const concurrent = authJson("concurrent", "stable-account", NOW + 3 * 60 * MINUTE_MS, NOW, false);
    let stageDirectory = "";
    try {
      await Deno.mkdir(slotDirectory, { mode: 0o700 });
      await Deno.mkdir(workspace, { mode: 0o700 });
      await Deno.writeTextFile(authPath, before, { mode: 0o600 });
      await assert.rejects(
        () =>
          maintainCodexAuthSlot({ slot: 1, slotDirectory, workspace }, {
            now: () => NOW,
            runAppServer: async (candidate) => {
              stageDirectory = candidate.env.CODEX_HOME;
              await Deno.writeTextFile(`${stageDirectory}/auth.json`, after);
              await Deno.writeTextFile(authPath, concurrent);
              return appServerResult();
            },
          }),
        /changed during maintenance/u,
      );
      assert.equal(await Deno.readTextFile(authPath), concurrent);
      await assert.rejects(() => Deno.stat(stageDirectory), Deno.errors.NotFound);
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});
