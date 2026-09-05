import assert from "node:assert/strict";
import {
  codexBubblewrapArguments,
  type CodexCommandRequest,
  type CodexCommandResult,
  type CodexFilesystem,
  CodexInvocationError,
  createCodexAuthRelayFactory,
  extractNativeReviewOutputFromRollouts,
  runCodexCommandWithRuntime,
  runNativeCodexReview,
  runStructuredCodexAgent,
  syntheticCodexAuthJson,
} from "../scripts/sentinel/codex.ts";
import {
  type CodexAuthSlotSecrets,
  CodexAuthValidationError,
  type CodexUsageFetch,
  isTransientCodexUsageFailure,
  parseCodexAuthJsonB64,
  parseCodexAuthJsonB64ForMaintenance,
  parseCodexUsageHeadroom,
  selectCodexAccountForInvocation,
} from "../scripts/sentinel/quota.ts";

const nowMs = 1_800_000_000_000;

const base64Url = (value: string): string => btoa(value).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");

const jwt = (expiresAtMs: number, label: string): string =>
  `${base64Url(JSON.stringify({ alg: "none", typ: "JWT" }))}.${
    base64Url(JSON.stringify({ exp: Math.floor(expiresAtMs / 1_000), label }))
  }.signature-${label}`;

const encodeUtf8Base64 = (value: string): string => {
  const bytes = new TextEncoder().encode(value);
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
};

const authSecret = (
  slot: 1 | 2,
  expiresAtMs = nowMs + 4 * 60 * 60_000,
): Readonly<{ encoded: string; raw: string; id: string; access: string; refresh: string; account: string }> => {
  const id = jwt(expiresAtMs, `id-slot-${slot}`);
  const access = jwt(expiresAtMs, `slot-${slot}`);
  const refresh = `refresh-token-slot-${slot}-never-log`;
  const account = `account-slot-${slot}-identifier`;
  const raw = JSON.stringify({
    auth_mode: "chatgpt",
    tokens: { id_token: id, access_token: access, refresh_token: refresh, account_id: account },
    last_refresh: "2026-08-21T00:00:00Z",
  });
  return { encoded: encodeUtf8Base64(raw), raw, id, access, refresh, account };
};

const slot1 = authSecret(1);
const slot2 = authSecret(2);
const slots: CodexAuthSlotSecrets = { slot1B64: slot1.encoded, slot2B64: slot2.encoded };

const usage = (
  primaryUsed: number,
  secondaryUsed: number | null = null,
  additional: readonly unknown[] | undefined = undefined,
): Record<string, unknown> => ({
  rate_limit: {
    primary_window: { used_percent: primaryUsed, limit_window_seconds: 10_800, reset_at: 1_900_000_000 },
    secondary_window: secondaryUsed === null
      ? null
      : { used_percent: secondaryUsed, limit_window_seconds: 604_800, reset_at: 1_900_000_000 },
  },
  ...(additional === undefined ? {} : { additional_rate_limits: additional }),
});

const usageResponse = (body: unknown, status = 200): Response =>
  new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });

const timeoutSignal = (): AbortSignal => new AbortController().signal;

const accountFrom = (init: RequestInit | undefined): string | null =>
  new Headers(init?.headers).get("ChatGPT-Account-ID");

class MemoryFilesystem implements CodexFilesystem {
  readonly files = new Map<string, string>();
  readonly modes = new Map<string, number>();
  readonly removed: string[] = [];
  #next = 0;

  makePrivateTempDir(prefix: string): Promise<string> {
    const path = `/private/${prefix}${++this.#next}`;
    this.modes.set(path, 0o700);
    return Promise.resolve(path);
  }

  chmod(path: string, mode: number): Promise<void> {
    this.modes.set(path, mode);
    return Promise.resolve();
  }

  writePrivateTextFile(path: string, contents: string): Promise<void> {
    this.files.set(path, contents);
    this.modes.set(path, 0o600);
    return Promise.resolve();
  }

  readTextFile(path: string): Promise<string> {
    const value = this.files.get(path);
    if (value === undefined) return Promise.reject(new Error("not found"));
    return Promise.resolve(value);
  }

  readPrivateRolloutFiles(codexHome: string): Promise<readonly string[]> {
    return Promise.resolve(
      [...this.files.entries()]
        .filter(([path]) => path.startsWith(`${codexHome}/sessions/`) && path.endsWith(".jsonl"))
        .sort(([left], [right]) => left.localeCompare(right))
        .map(([, contents]) => contents),
    );
  }

  removeTree(path: string): Promise<void> {
    this.removed.push(path);
    for (const file of [...this.files.keys()]) if (file.startsWith(`${path}/`)) this.files.delete(file);
    return Promise.resolve();
  }
}

const healthyFetcher = (
  usedByAccount: Readonly<Record<string, number>> = {
    [slot1.account]: 10,
    [slot2.account]: 20,
  },
  calls: Array<{ account: string | null; init: RequestInit | undefined; url: string }> = [],
): CodexUsageFetch =>
(input, init) => {
  const account = accountFrom(init);
  calls.push({ account, init, url: String(input) });
  return Promise.resolve(usageResponse(usage(usedByAccount[account ?? ""] ?? 100)));
};

const commonDependencies = (filesystem: MemoryFilesystem, fetcher: CodexUsageFetch) => ({
  filesystem,
  fetcher,
  now: () => nowMs,
  createTimeoutSignal: timeoutSignal,
  readEnvironment: (name: string) => name === "PATH" ? "/test/bin:/usr/bin" : "must-not-inherit",
  authRelayFactory: () =>
    Promise.resolve({ baseUrl: "http://127.0.0.1:41771/backend-api", close: () => Promise.resolve() }),
});

const codexCommandResult = (
  overrides: Partial<CodexCommandResult> = {},
): CodexCommandResult => {
  const stdout = overrides.stdout ?? "";
  const stderr = overrides.stderr ?? "";
  return {
    code: overrides.code ?? 0,
    stdout,
    stderr,
    outputExceeded: overrides.outputExceeded ?? false,
    timedOut: overrides.timedOut ?? false,
    stdoutBytes: overrides.stdoutBytes ?? new TextEncoder().encode(stdout).byteLength,
    stderrBytes: overrides.stderrBytes ?? new TextEncoder().encode(stderr).byteLength,
    durationMs: overrides.durationMs ?? 10,
  };
};

const schemaFixture = (title: string): string =>
  JSON.stringify({ title, type: "object", required: ["summary"], additionalProperties: false });

Deno.test("auth parsing requires a complete document and rejects tokens inside the expiry safety window", () => {
  const parsed = parseCodexAuthJsonB64(slot1.encoded, 1, { nowMs, minimumValidityMs: 60_000 });
  assert.equal(parsed.rawJson, slot1.raw);
  assert.equal(parsed.lastRefresh, "2026-08-21T00:00:00Z");
  assert.deepEqual(parsed.tokens, {
    id_token: slot1.id,
    access_token: slot1.access,
    refresh_token: slot1.refresh,
    account_id: slot1.account,
  });

  for (
    const invalidDocument of [
      { auth_mode: "api_key", tokens: parsed.tokens, last_refresh: "2026-08-21T00:00:00Z" },
      {
        auth_mode: "chatgpt",
        tokens: { access_token: slot1.access, refresh_token: slot1.refresh, account_id: slot1.account },
        last_refresh: "2026-08-21T00:00:00Z",
      },
      { auth_mode: "chatgpt", tokens: parsed.tokens, last_refresh: "not-a-timestamp" },
      { auth_mode: "chatgpt", tokens: parsed.tokens, last_refresh: "2026-02-31T00:00:00Z" },
    ]
  ) {
    const encoded = encodeUtf8Base64(JSON.stringify(invalidDocument));
    assert.throws(
      () => parseCodexAuthJsonB64(encoded, 1, { nowMs, minimumValidityMs: 1 }),
      (error) => error instanceof CodexAuthValidationError && error.code === "invalid_document",
    );
  }
  const expiring = authSecret(1, nowMs + 60_000);
  assert.equal(
    parseCodexAuthJsonB64ForMaintenance(expiring.encoded, 1).tokens.refresh_token,
    expiring.refresh,
  );
  assert.throws(
    () => parseCodexAuthJsonB64(expiring.encoded, 1, { nowMs, minimumValidityMs: 60_000 }),
    (error) => error instanceof CodexAuthValidationError && error.code === "access_token_expiring",
  );
  assert.throws(
    () => parseCodexAuthJsonB64("not base64", 1, { nowMs, minimumValidityMs: 0 }),
    (error) => error instanceof CodexAuthValidationError && error.code === "invalid_base64",
  );
});

Deno.test("Codex authentication relay keeps real credentials in the parent and restricts upstream routes", async () => {
  const auth = parseCodexAuthJsonB64(slot1.encoded, 1, { nowMs, minimumValidityMs: 1 });
  const calls: Array<{ url: string; headers: Headers; body: string }> = [];
  const relay = await createCodexAuthRelayFactory(async (input, init) => {
    calls.push({
      url: String(input),
      headers: new Headers(init?.headers),
      body: init?.body ? await new Response(init.body).text() : "",
    });
    return new Response("upstream", {
      status: 200,
      headers: { "set-cookie": "private=1", location: "https://example.invalid" },
    });
  })(auth);
  try {
    const denied = await fetch(`${relay.baseUrl}/wham/usage`, { method: "GET" });
    assert.equal(denied.status, 404);
    const response = await fetch(`${relay.baseUrl}/codex/responses`, {
      method: "POST",
      headers: {
        authorization: "Bearer synthetic",
        cookie: "must-not-forward=1",
        "chatgpt-account-id": "synthetic",
        "content-type": "application/json",
      },
      body: '{"model":"gpt-5.6-sol"}',
    });
    assert.equal(await response.text(), "upstream");
    assert.equal(response.headers.has("set-cookie"), false);
    assert.equal(response.headers.has("location"), false);
  } finally {
    await relay.close();
  }
  assert.equal(calls.length, 1);
  assert.equal(calls[0]?.url, "https://chatgpt.com/backend-api/codex/responses");
  assert.equal(calls[0]?.headers.get("authorization"), `Bearer ${slot1.access}`);
  assert.equal(calls[0]?.headers.get("chatgpt-account-id"), slot1.account);
  assert.equal(calls[0]?.headers.has("cookie"), false);
  assert.equal(calls[0]?.body, '{"model":"gpt-5.6-sol"}');
});

Deno.test("Codex authentication relay bounds a silent response stream", async () => {
  const auth = parseCodexAuthJsonB64(slot1.encoded, 1, { nowMs, minimumValidityMs: 1 });
  let keepaliveTimer: ReturnType<typeof setInterval> | null = null;
  const relay = await createCodexAuthRelayFactory(
    () =>
      Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(new TextEncoder().encode(": keepalive\n\n"));
              keepaliveTimer = setInterval(() => {
                controller.enqueue(new TextEncoder().encode(": keepalive\n\n"));
              }, 2);
            },
            cancel() {
              if (keepaliveTimer !== null) clearInterval(keepaliveTimer);
              keepaliveTimer = null;
            },
          }),
          { headers: { "Content-Type": "text/event-stream" } },
        ),
      ),
    { streamIdleTimeoutMs: 20 },
  )(auth);
  let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  try {
    const response = await fetch(`${relay.baseUrl}/codex/responses`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: '{"model":"gpt-5.6-luna"}',
    });
    reader = response.body?.getReader() ?? null;
    assert.ok(reader);
    assert.equal((await reader.read()).done, false);
    await Promise.race([
      (async () => {
        while (true) {
          const next = await reader.read().catch(() => ({ done: true }));
          if (next.done) return;
        }
      })(),
      new Promise<never>((_resolve, reject) => setTimeout(() => reject(new Error("relay did not time out")), 500)),
    ]);
    assert.equal(relay.streamIdleTimedOut?.(), true);
  } finally {
    await reader?.cancel().catch(() => undefined);
    if (keepaliveTimer !== null) clearInterval(keepaliveTimer);
    await relay.close();
  }
});

Deno.test("Codex authentication relay requires a completed data event to reset stream activity", async () => {
  const auth = parseCodexAuthJsonB64(slot1.encoded, 1, { nowMs, minimumValidityMs: 1 });
  const relay = await createCodexAuthRelayFactory(
    () =>
      Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(new TextEncoder().encode('data: {"type":"response.created"}\\n'));
            },
          }),
          { headers: { "Content-Type": "text/event-stream" } },
        ),
      ),
    { streamIdleTimeoutMs: 20 },
  )(auth);
  let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  try {
    const response = await fetch(`${relay.baseUrl}/codex/responses`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: '{"model":"gpt-5.6-luna"}',
    });
    reader = response.body?.getReader() ?? null;
    assert.ok(reader);
    assert.equal((await reader.read()).done, false);
    await Promise.race([
      reader.read(),
      new Promise<never>((_resolve, reject) => setTimeout(() => reject(new Error("relay did not time out")), 500)),
    ]);
    assert.equal(relay.streamIdleTimedOut?.(), true);
  } finally {
    await reader?.cancel().catch(() => undefined);
    await relay.close();
  }
});

Deno.test("Codex authentication relay cancellation does not wait for an upstream cancellation", async () => {
  const auth = parseCodexAuthJsonB64(slot1.encoded, 1, { nowMs, minimumValidityMs: 1 });
  const upstreamCancellationStarted = Promise.withResolvers<void>();
  const completeUpstreamCancellation = Promise.withResolvers<void>();
  const relay = await createCodexAuthRelayFactory(
    () =>
      Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(new TextEncoder().encode("data: ready\\n\\n"));
            },
            cancel() {
              upstreamCancellationStarted.resolve();
              return completeUpstreamCancellation.promise;
            },
          }),
          { headers: { "Content-Type": "text/event-stream" } },
        ),
      ),
    { streamIdleTimeoutMs: 1_000 },
  )(auth);
  let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  try {
    const response = await fetch(`${relay.baseUrl}/codex/responses`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: '{"model":"gpt-5.6-luna"}',
    });
    reader = response.body?.getReader() ?? null;
    assert.ok(reader);
    assert.equal((await reader.read()).done, false);
    await Promise.race([
      reader.cancel(),
      new Promise<never>((_resolve, reject) =>
        setTimeout(() => reject(new Error("relay cancellation did not settle")), 500)
      ),
    ]);
    await Promise.race([
      upstreamCancellationStarted.promise,
      new Promise<never>((_resolve, reject) =>
        setTimeout(() => reject(new Error("relay did not cancel the upstream stream")), 500)
      ),
    ]);
  } finally {
    completeUpstreamCancellation.resolve();
    await reader?.cancel().catch(() => undefined);
    await relay.close();
  }
});

Deno.test("Codex bubblewrap exposes only system files, the repository, and the selected private home", () => {
  const args = codexBubblewrapArguments({
    executable: "codex",
    args: ["exec"],
    cwd: "/home/runner/work/ai/ai/.sentinel/candidate-worktree",
    repositoryRoot: "/home/runner/work/ai/ai",
    workspaceWritable: true,
    env: { CODEX_HOME: "/tmp/uos-sentinel-codex-1" },
    clearEnv: true,
    stdin: "",
    outputLimitBytes: 1_024,
    timeoutMs: 1_000,
  });
  const joined = args.join("\0");
  assert.equal(joined.includes("--ro-bind\0/\0/"), false);
  assert.ok(joined.includes("--ro-bind\0/home/runner/work/ai/ai\0/home/runner/work/ai/ai"));
  assert.ok(
    joined.includes(
      "--bind\0/home/runner/work/ai/ai/.sentinel/candidate-worktree\0" +
        "/home/runner/work/ai/ai/.sentinel/candidate-worktree\0--ro-bind\0" +
        "/home/runner/work/ai/ai/.sentinel/candidate-worktree/.git",
    ),
  );
  assert.ok(joined.includes("--bind\0/tmp/uos-sentinel-codex-1\0/tmp/uos-sentinel-codex-1"));
  assert.equal(joined.includes("/home/runner/runners"), false);
  assert.deepEqual(args.slice(-3), ["--", "codex", "exec"]);
});

Deno.test("Codex command records timeout when status resolves or rejects after abort", async () => {
  const request: CodexCommandRequest = {
    executable: "codex",
    args: ["exec"],
    cwd: "/checkout",
    repositoryRoot: "/checkout",
    workspaceWritable: false,
    env: { CODEX_HOME: "/private/codex" },
    clearEnv: true,
    stdin: "prompt",
    outputLimitBytes: 1_024,
    timeoutMs: 1_000,
  };
  for (const statusKind of ["resolved", "rejected"] as const) {
    const controller = new AbortController();
    let now = 100;
    const result = await runCodexCommandWithRuntime(request, {
      createTimeoutSignal: () => controller.signal,
      spawn: () => {
        controller.abort(new DOMException("deadline", "TimeoutError"));
        return {
          status: statusKind === "resolved"
            ? Promise.resolve({ code: 143 })
            : Promise.reject(new DOMException("deadline", "AbortError")),
          stdout: new Blob(["partial output"]).stream(),
          stderr: new Blob(["deadline reached"]).stream(),
          stdin: new WritableStream<Uint8Array>(),
        };
      },
      now: () => {
        const value = now;
        now = 1_150;
        return value;
      },
    });
    assert.equal(result.timedOut, true);
    assert.equal(result.code, statusKind === "resolved" ? 143 : -1);
    assert.equal(result.stdoutBytes, 14);
    assert.equal(result.stderrBytes, 16);
    assert.equal(result.durationMs, 1_050);
  }
});

Deno.test("usage parsing replaces base with the best matching model pool and fails closed", () => {
  const body = usage(20, 60, [{
    limit_name: "GPT-5.6-Sol",
    rate_limit: {
      primary_window: { used_percent: 95 },
      secondary_window: { used_percent: 30 },
    },
  }, {
    limit_name: "gpt_5_6_sol",
    rate_limit: {
      primary_window: { used_percent: 25 },
      secondary_window: { used_percent: 70 },
    },
  }, {
    limit_name: "GPT-5.6-Luna",
    rate_limit: { primary_window: { used_percent: 99 }, secondary_window: null },
  }]);
  assert.equal(parseCodexUsageHeadroom(body, "gpt-5.6-sol"), 30);
  assert.equal(parseCodexUsageHeadroom(body, "gpt-5.6-luna"), 1);
  assert.equal(parseCodexUsageHeadroom(body, null), 40);
  assert.equal(parseCodexUsageHeadroom(body, "gpt-5.6-terra"), 40);
  assert.equal(parseCodexUsageHeadroom({ rate_limit: { primary_window: { used_percent: 10 } } }, null), null);
  assert.equal(
    parseCodexUsageHeadroom({
      rate_limit: { primary_window: { used_percent: 10 }, secondary_window: "broken" },
    }, null),
    null,
  );
  assert.equal(
    parseCodexUsageHeadroom({
      rate_limit: { primary_window: null, secondary_window: null },
    }, null),
    null,
  );
  assert.equal(
    parseCodexUsageHeadroom(
      usage(10, null, [{
        limit_name: "unrelated-model",
        rate_limit: { primary_window: { used_percent: "10" }, secondary_window: null },
      }]),
      "gpt-5.6-sol",
    ),
    null,
  );
  assert.equal(
    parseCodexUsageHeadroom(
      usage(10, null, [{
        limit_name: "---",
        rate_limit: { primary_window: { used_percent: 10 }, secondary_window: null },
      }]),
      "gpt-5.6-sol",
    ),
    null,
  );
});

Deno.test("selection probes both accounts passively, chooses greater headroom, and sends no request body", async () => {
  const calls: Array<{ account: string | null; init: RequestInit | undefined; url: string }> = [];
  const timeoutRequests: number[] = [];
  const selected = await selectCodexAccountForInvocation({
    slots,
    model: "gpt-5.6-sol",
    minimumValidityMs: 1_000,
    fetcher: healthyFetcher({ [slot1.account]: 45, [slot2.account]: 15 }, calls),
    now: () => nowMs,
    timeoutMs: 8_000,
    createTimeoutSignal: (timeoutMs) => {
      timeoutRequests.push(timeoutMs);
      return timeoutSignal();
    },
  });
  assert.equal(selected.kind, "selected");
  if (selected.kind !== "selected") return;
  assert.equal(selected.slot, 2);
  assert.equal(selected.headroomPercent, 85);
  assert.equal(Object.keys(selected).includes("auth"), false);
  assert.equal(JSON.stringify(selected).includes(slot2.access), false);
  assert.equal(calls.length, 2);
  assert.deepEqual(timeoutRequests, [8_000, 8_000]);
  for (const call of calls) {
    const headers = new Headers(call.init?.headers);
    assert.equal(call.url, "https://chatgpt.com/backend-api/wham/usage");
    assert.equal(call.init?.method, "GET");
    assert.equal(call.init?.redirect, "manual");
    assert.equal(call.init?.body, undefined);
    assert.equal(headers.get("Accept"), "application/json");
    assert.match(headers.get("Authorization") ?? "", /^Bearer /u);
    assert.ok(headers.get("ChatGPT-Account-ID"));
  }
});

Deno.test("account selection uses model pools instead of tighter or looser base pools", async () => {
  const selected = await selectCodexAccountForInvocation({
    slots,
    model: "gpt-5.6-sol",
    minimumValidityMs: 1_000,
    fetcher: (_input, init) => {
      const slotOne = accountFrom(init) === slot1.account;
      return Promise.resolve(usageResponse(usage(slotOne ? 95 : 10, null, [{
        limit_name: "GPT-5.6-Sol",
        rate_limit: {
          primary_window: { used_percent: slotOne ? 10 : 60 },
          secondary_window: null,
        },
      }])));
    },
    now: () => nowMs,
    createTimeoutSignal: timeoutSignal,
  });
  assert.equal(selected.kind, "selected");
  if (selected.kind !== "selected") return;
  assert.equal(selected.slot, 1);
  assert.equal(selected.headroomPercent, 90);
});

Deno.test("selection handles one invalid account, both exhausted accounts, and deterministic ties", async () => {
  let calls = 0;
  const oneValid = await selectCodexAccountForInvocation({
    slots: { slot1B64: "invalid value", slot2B64: slot2.encoded },
    model: "gpt-5.6-sol",
    minimumValidityMs: 1_000,
    fetcher: (_input, init) => {
      calls++;
      assert.equal(accountFrom(init), slot2.account);
      return Promise.resolve(usageResponse(usage(10)));
    },
    now: () => nowMs,
    createTimeoutSignal: timeoutSignal,
  });
  assert.equal(oneValid.kind, "selected");
  if (oneValid.kind === "selected") assert.equal(oneValid.slot, 2);
  assert.equal(calls, 1);

  const oneProbeUnavailable = await selectCodexAccountForInvocation({
    slots,
    model: "gpt-5.6-sol",
    minimumValidityMs: 1_000,
    fetcher: (_input, init) => {
      if (accountFrom(init) === slot1.account) {
        return Promise.resolve(
          new Response(
            new ReadableStream({
              start(controller) {
                controller.error(new Error("private upstream read failure"));
              },
            }),
            { status: 200 },
          ),
        );
      }
      return Promise.resolve(usageResponse(usage(10)));
    },
    now: () => nowMs,
    createTimeoutSignal: timeoutSignal,
    probeRetry: { delaysMs: [0, 0], sleep: () => Promise.resolve() },
  });
  assert.equal(oneProbeUnavailable.kind, "selected");
  if (oneProbeUnavailable.kind === "selected") assert.equal(oneProbeUnavailable.slot, 2);
  assert.equal(oneProbeUnavailable.probes[0].kind, "unavailable");
  if (oneProbeUnavailable.probes[0].kind === "unavailable") {
    assert.equal(oneProbeUnavailable.probes[0].failure, "network_error");
  }

  const exhausted = await selectCodexAccountForInvocation({
    slots,
    model: "gpt-5.6-sol",
    minimumValidityMs: 1_000,
    fetcher: () => Promise.resolve(usageResponse(usage(100, 90))),
    now: () => nowMs,
    createTimeoutSignal: timeoutSignal,
  });
  assert.equal(exhausted.kind, "unavailable");
  assert.deepEqual(exhausted.probes.map((probe) => probe.kind === "unavailable" ? probe.failure : null), [
    "quota_exhausted",
    "quota_exhausted",
  ]);

  const tied = await selectCodexAccountForInvocation({
    slots,
    model: "gpt-5.6-sol",
    minimumValidityMs: 1_000,
    fetcher: () => Promise.resolve(usageResponse(usage(25))),
    now: () => nowMs,
    createTimeoutSignal: timeoutSignal,
  });
  assert.equal(tied.kind, "selected");
  if (tied.kind === "selected") assert.equal(tied.slot, 1);
});

Deno.test("malformed usage, redirects, and expiring credentials cannot be selected", async () => {
  let fetchCalls = 0;
  const expiring1 = authSecret(1, nowMs + 2_000);
  const result = await selectCodexAccountForInvocation({
    slots: { slot1B64: expiring1.encoded, slot2B64: slot2.encoded },
    model: "gpt-5.6-sol",
    minimumValidityMs: 2_000,
    fetcher: (_input, init) => {
      fetchCalls++;
      if (accountFrom(init) === slot2.account) {
        return Promise.resolve(new Response(null, { status: 302, headers: { Location: "https://example.invalid" } }));
      }
      throw new Error("expiring slot must not be probed");
    },
    now: () => nowMs,
    createTimeoutSignal: timeoutSignal,
  });
  assert.equal(result.kind, "unavailable");
  assert.equal(fetchCalls, 1);
  assert.deepEqual(result.probes.map((probe) => probe.kind === "unavailable" ? probe.failure : null), [
    "access_token_expiring",
    "redirect_rejected",
  ]);

  const malformed = await selectCodexAccountForInvocation({
    slots,
    model: "gpt-5.6-sol",
    minimumValidityMs: 1,
    fetcher: () => Promise.resolve(usageResponse({ rate_limit: { primary_window: { used_percent: 1 } } })),
    now: () => nowMs,
    createTimeoutSignal: timeoutSignal,
  });
  assert.equal(malformed.kind, "unavailable");
  assert.ok(
    malformed.probes.every((probe) => probe.kind === "unavailable" && probe.failure === "invalid_usage_document"),
  );
});

Deno.test("transient usage probe failures are retried and can recover within the bounded attempts", async () => {
  const callsByAccount = new Map<string, number>();
  const observedDelays: number[] = [];
  const selected = await selectCodexAccountForInvocation({
    slots,
    model: "gpt-5.6-sol",
    minimumValidityMs: 1_000,
    fetcher: (_input, init) => {
      const account = accountFrom(init)!;
      const calls = (callsByAccount.get(account) ?? 0) + 1;
      callsByAccount.set(account, calls);
      if (calls === 1) return Promise.resolve(new Response(null, { status: 503 }));
      if (calls === 2) return Promise.reject(new TypeError("upstream reset"));
      return Promise.resolve(usageResponse(usage(10)));
    },
    now: () => nowMs,
    createTimeoutSignal: timeoutSignal,
    probeRetry: {
      delaysMs: [1, 1],
      sleep: (delayMs) => {
        observedDelays.push(delayMs);
        return Promise.resolve();
      },
    },
  });
  assert.equal(selected.kind, "selected");
  assert.deepEqual(callsByAccount, new Map([[slot1.account, 3], [slot2.account, 3]]));
  assert.deepEqual(observedDelays, [1, 1, 1, 1]);
});

Deno.test("authoritative usage failures are never retried", async () => {
  let calls = 0;
  const exhausted = await selectCodexAccountForInvocation({
    slots,
    model: "gpt-5.6-sol",
    minimumValidityMs: 1_000,
    fetcher: () => {
      calls++;
      return Promise.resolve(usageResponse(usage(100, 90)));
    },
    now: () => nowMs,
    createTimeoutSignal: timeoutSignal,
  });
  assert.equal(exhausted.kind, "unavailable");
  assert.equal(calls, 2);
  assert.deepEqual(exhausted.probes.map((probe) => probe.kind === "unavailable" ? probe.failure : null), [
    "quota_exhausted",
    "quota_exhausted",
  ]);
});

Deno.test("persistent transient failures report the final probe after bounded retries", async () => {
  let calls = 0;
  const result = await selectCodexAccountForInvocation({
    slots: { slot1B64: slot1.encoded, slot2B64: "invalid value" },
    model: "gpt-5.6-sol",
    minimumValidityMs: 1_000,
    fetcher: () => {
      calls++;
      return Promise.resolve(new Response(null, { status: 429 }));
    },
    now: () => nowMs,
    createTimeoutSignal: timeoutSignal,
    probeRetry: {
      attempts: 3,
      delaysMs: [0, 0],
      sleep: (delayMs) => {
        assert.equal(delayMs, 0);
        return Promise.resolve();
      },
    },
  });
  assert.equal(result.kind, "unavailable");
  assert.equal(calls, 3);
  assert.deepEqual(result.probes.map((probe) => probe.kind === "unavailable" ? probe.failure : null), [
    "http_error",
    "invalid_base64",
  ]);
  assert.ok(isTransientCodexUsageFailure(result.probes[0]!));
  assert.equal(isTransientCodexUsageFailure(result.probes[1]!), false);
  assert.equal(
    isTransientCodexUsageFailure({ kind: "available", slot: 1, headroomPercent: 50, observedAtMs: nowMs }),
    false,
  );
});

Deno.test("structured execution uses fixed policy, relay-only auth, a private home, and a clean environment", async () => {
  const filesystem = new MemoryFilesystem();
  const triageSchema = schemaFixture("triage-cell-result");
  filesystem.files.set("/checkout/schemas/triage.json", triageSchema);
  let command: CodexCommandRequest | null = null;
  let runtimeAuth = "";
  const result = await runStructuredCodexAgent({
    role: "triage",
    checkoutPath: "/checkout",
    prompt: "Inspect the supplied files.",
    outputSchemaPath: "/checkout/schemas/triage.json",
    authSlots: slots,
    expectedMaximumRuntimeMs: 1_000,
  }, {
    ...commonDependencies(filesystem, healthyFetcher()),
    commandRunner: (request) => {
      command = request;
      runtimeAuth = filesystem.files.get(`${request.env.CODEX_HOME}/auth.json`) ?? "";
      const lastMessagePath = request.args[request.args.indexOf("--output-last-message") + 1]!;
      filesystem.files.set(lastMessagePath, JSON.stringify({ findings: [] }));
      return Promise.resolve(codexCommandResult({ stdout: "json event" }));
    },
  });
  assert.equal(result.slot, 1);
  assert.equal(result.lastMessage, JSON.stringify({ findings: [] }));
  assert.ok(command);
  const captured = command as unknown as CodexCommandRequest;
  assert.equal(captured.clearEnv, true);
  assert.equal(captured.cwd, "/checkout");
  assert.equal(captured.repositoryRoot, "/checkout");
  assert.equal(captured.workspaceWritable, false);
  assert.equal(captured.stdin, "Inspect the supplied files.");
  assert.equal(captured.timeoutMs, 1_000);
  assert.deepEqual(Object.keys(captured.env).sort(), [
    "CI",
    "CODEX_HOME",
    "HOME",
    "LANG",
    "LC_ALL",
    "NO_COLOR",
    "PATH",
    "TERM",
  ]);
  assert.equal(captured.env.CODEX_HOME, captured.env.HOME);
  assert.equal(captured.env.PATH, "/test/bin:/usr/bin");
  assert.equal(captured.args[0], "exec");
  assert.ok(captured.args.includes("--ephemeral"));
  assert.ok(captured.args.includes("--ignore-user-config"));
  assert.ok(captured.args.includes("--ignore-rules"));
  assert.ok(captured.args.includes("gpt-5.6-sol"));
  assert.ok(captured.args.includes('model_reasoning_effort="medium"'));
  assert.ok(captured.args.includes("read-only"));
  assert.ok(captured.args.includes("sandbox_workspace_write.network_access=false"));
  assert.ok(captured.args.includes('chatgpt_base_url="http://127.0.0.1:41771/backend-api"'));
  assert.ok(captured.args.includes('model_provider="sentinel_relay"'));
  assert.ok(
    captured.args.includes(
      'model_providers.sentinel_relay.base_url="http://127.0.0.1:41771/backend-api/codex"',
    ),
  );
  assert.ok(captured.args.includes("model_providers.sentinel_relay.requires_openai_auth=true"));
  assert.ok(captured.args.includes("model_providers.sentinel_relay.supports_websockets=false"));
  assert.ok(captured.args.includes("model_providers.sentinel_relay.supports_standalone_web_search=false"));
  assert.ok(captured.args.includes("features.apps=false"));
  assert.equal(
    runtimeAuth,
    syntheticCodexAuthJson(
      parseCodexAuthJsonB64(slot1.encoded, 1, {
        nowMs,
        minimumValidityMs: 1_000 + 5 * 60_000,
      }),
      nowMs,
    ),
  );
  assert.equal(JSON.parse(runtimeAuth).last_refresh, new Date(nowMs).toISOString());
  assert.equal(runtimeAuth.includes(slot1.access), false);
  assert.equal(runtimeAuth.includes(slot1.refresh), false);
  assert.equal(runtimeAuth.includes(slot1.account), false);
  const home = captured.env.CODEX_HOME;
  assert.equal(filesystem.modes.get(home), 0o700);
  assert.equal(filesystem.modes.get(`${home}/auth.json`), 0o600);
  assert.equal(filesystem.modes.get(`${home}/last-message.json`), 0o600);
  assert.equal(filesystem.modes.get(`${home}/output-schema.json`), 0o600);
  assert.equal(
    captured.args[captured.args.indexOf("--output-schema") + 1]!,
    `${home}/output-schema.json`,
  );
  assert.equal(captured.args.includes("/checkout/schemas/triage.json"), false);
  assert.equal(filesystem.files.get("/checkout/schemas/triage.json"), triageSchema);
  assert.deepEqual(filesystem.removed, [home]);
});

Deno.test("implementation execution uses the owner-controlled Luna model in the writable sandbox", async () => {
  const filesystem = new MemoryFilesystem();
  filesystem.files.set("/checkout/schemas/implementation.json", schemaFixture("implementation-cell-result"));
  let command: CodexCommandRequest | null = null;
  await runStructuredCodexAgent({
    role: "implementation",
    checkoutPath: "/checkout",
    prompt: "Implement the selected issue.",
    outputSchemaPath: "/checkout/schemas/implementation.json",
    authSlots: slots,
    expectedMaximumRuntimeMs: 1_000,
  }, {
    ...commonDependencies(filesystem, healthyFetcher()),
    commandRunner: (request) => {
      command = request;
      const lastMessagePath = request.args[request.args.indexOf("--output-last-message") + 1]!;
      filesystem.files.set(lastMessagePath, "{}");
      return Promise.resolve(codexCommandResult());
    },
  });
  assert.ok(command);
  const captured = command as CodexCommandRequest;
  assert.equal(captured.workspaceWritable, true);
  assert.ok(captured.args.includes("gpt-5.6-luna"));
  assert.equal(captured.args.includes("gpt-5.6-sol"), false);
  assert.ok(captured.args.includes('model_reasoning_effort="max"'));
  assert.equal(captured.args.includes('model_reasoning_effort="medium"'), false);
  assert.ok(captured.args.includes("model_providers.sentinel_relay.stream_max_retries=1"));
  assert.ok(captured.args.includes("model_providers.sentinel_relay.stream_idle_timeout_ms=300000"));
  assert.ok(captured.args.includes("workspace-write"));
});

Deno.test("account quota is re-probed before every Codex invocation", async () => {
  const filesystem = new MemoryFilesystem();
  filesystem.files.set("/checkout/schema.json", schemaFixture("monitor-cell-result"));
  let usageCall = 0;
  const fetcher: CodexUsageFetch = (_input, init) => {
    const invocation = Math.floor(usageCall++ / 2);
    const account = accountFrom(init);
    const primaryUsed = invocation === 0 ? (account === slot1.account ? 5 : 80) : (account === slot1.account ? 90 : 10);
    return Promise.resolve(usageResponse(usage(primaryUsed)));
  };
  const invokedSlots: number[] = [];
  const commandRunner = (request: CodexCommandRequest): Promise<CodexCommandResult> => {
    const lastMessagePath = request.args[request.args.indexOf("--output-last-message") + 1]!;
    filesystem.files.set(lastMessagePath, "{}");
    return Promise.resolve(codexCommandResult());
  };
  const options = {
    role: "monitoring" as const,
    checkoutPath: "/checkout",
    prompt: "Monitor.",
    outputSchemaPath: "/checkout/schema.json",
    authSlots: slots,
    expectedMaximumRuntimeMs: 1_000,
  };
  const dependencies = {
    ...commonDependencies(filesystem, fetcher),
    commandRunner,
    authRelayFactory: (auth: { slot: 1 | 2 }) => {
      invokedSlots.push(auth.slot);
      return Promise.resolve({ baseUrl: "http://127.0.0.1:41771/backend-api", close: () => Promise.resolve() });
    },
  };
  assert.equal((await runStructuredCodexAgent(options, dependencies)).slot, 1);
  assert.equal((await runStructuredCodexAgent(options, dependencies)).slot, 2);
  assert.deepEqual(invokedSlots, [1, 2]);
  assert.equal(usageCall, 4);
});

Deno.test("Codex output containing a credential is rejected and never returned", async () => {
  const filesystem = new MemoryFilesystem();
  filesystem.files.set("/checkout/schema.json", schemaFixture("implementation-cell-result"));
  await assert.rejects(
    () =>
      runStructuredCodexAgent({
        role: "implementation",
        checkoutPath: "/checkout",
        prompt: "Implement.",
        outputSchemaPath: "/checkout/schema.json",
        authSlots: slots,
        expectedMaximumRuntimeMs: 1_000,
      }, {
        ...commonDependencies(filesystem, healthyFetcher()),
        commandRunner: (request) => {
          const lastMessagePath = request.args[request.args.indexOf("--output-last-message") + 1]!;
          filesystem.files.set(lastMessagePath, "{}");
          return Promise.resolve(codexCommandResult({ stdout: slot1.refresh }));
        },
      }),
    (error) =>
      error instanceof CodexInvocationError && error.failure === "secret_in_output" &&
      !error.message.includes(slot1.refresh),
  );
  assert.equal(filesystem.removed.length, 1);
});

Deno.test("Codex auth mutation is fail-closed", async () => {
  const filesystem = new MemoryFilesystem();
  filesystem.files.set("/checkout/schema.json", schemaFixture("implementation-cell-result"));
  await assert.rejects(
    () =>
      runStructuredCodexAgent({
        role: "implementation",
        checkoutPath: "/checkout",
        prompt: "Implement.",
        outputSchemaPath: "/checkout/schema.json",
        authSlots: slots,
        expectedMaximumRuntimeMs: 1_000,
      }, {
        ...commonDependencies(filesystem, healthyFetcher()),
        commandRunner: (request) => {
          filesystem.files.set(`${request.env.CODEX_HOME}/auth.json`, "{}");
          const lastMessagePath = request.args[request.args.indexOf("--output-last-message") + 1]!;
          filesystem.files.set(lastMessagePath, "{}");
          return Promise.resolve(codexCommandResult());
        },
      }),
    (error) => error instanceof CodexInvocationError && error.failure === "auth_mutated",
  );
  assert.equal(filesystem.removed.length, 1);
});

Deno.test("a relayed upstream 401 requests parent-owned refresh before synthetic auth mutation", async () => {
  const filesystem = new MemoryFilesystem();
  filesystem.files.set("/checkout/schema.json", schemaFixture("implementation-cell-result"));
  await assert.rejects(
    () =>
      runStructuredCodexAgent({
        role: "implementation",
        checkoutPath: "/checkout",
        prompt: "Implement.",
        outputSchemaPath: "/checkout/schema.json",
        authSlots: slots,
        expectedMaximumRuntimeMs: 1_000,
      }, {
        ...commonDependencies(filesystem, healthyFetcher()),
        authRelayFactory: () =>
          Promise.resolve({
            baseUrl: "http://127.0.0.1:41771/backend-api",
            authenticationRejected: () => true,
            close: () => Promise.resolve(),
          }),
        commandRunner: (request) => {
          filesystem.files.set(`${request.env.CODEX_HOME}/auth.json`, "{}");
          return Promise.resolve(codexCommandResult({ code: 1 }));
        },
      }),
    (error) => error instanceof CodexInvocationError && error.failure === "auth_refresh_required",
  );
  assert.equal(filesystem.removed.length, 1);
});

Deno.test("Codex invocation timeout is fail-closed and closes the authentication relay", async () => {
  const filesystem = new MemoryFilesystem();
  filesystem.files.set("/checkout/schema.json", schemaFixture("triage-cell-result"));
  let relayClosed = false;
  await assert.rejects(
    () =>
      runStructuredCodexAgent({
        role: "triage",
        checkoutPath: "/checkout",
        prompt: "Inspect.",
        outputSchemaPath: "/checkout/schema.json",
        authSlots: slots,
        expectedMaximumRuntimeMs: 1_000,
      }, {
        ...commonDependencies(filesystem, healthyFetcher()),
        authRelayFactory: () =>
          Promise.resolve({
            baseUrl: "http://127.0.0.1:41771/backend-api",
            close() {
              relayClosed = true;
              return Promise.resolve();
            },
          }),
        commandRunner: () =>
          Promise.resolve(codexCommandResult({
            code: 143,
            stdout: "private partial model output",
            stderr: "private process diagnostics",
            timedOut: true,
            stdoutBytes: 12_345,
            stderrBytes: 678,
            durationMs: 2_700_001,
          })),
      }),
    (error) => {
      assert.ok(error instanceof CodexInvocationError);
      assert.equal(error.failure, "invocation_timeout");
      assert.equal(error.exitCode, 143);
      assert.equal(error.stdoutBytes, 12_345);
      assert.equal(error.stderrBytes, 678);
      assert.equal(error.durationMs, 2_700_001);
      assert.equal(error.outputExceeded, false);
      assert.equal(error.timedOut, true);
      assert.equal(error.message.includes("private partial model output"), false);
      assert.equal(error.message.includes("private process diagnostics"), false);
      return true;
    },
  );
  assert.equal(relayClosed, true);
  assert.equal(filesystem.removed.length, 1);
});

Deno.test("a relay-detected idle stream preserves the final implementation continuation", async () => {
  const filesystem = new MemoryFilesystem();
  filesystem.files.set("/checkout/schema.json", schemaFixture("implementation-cell-result"));
  await assert.rejects(
    () =>
      runStructuredCodexAgent({
        role: "implementation",
        checkoutPath: "/checkout",
        prompt: "Implement.",
        outputSchemaPath: "/checkout/schema.json",
        authSlots: slots,
        expectedMaximumRuntimeMs: 1_000,
      }, {
        ...commonDependencies(filesystem, healthyFetcher()),
        authRelayFactory: () =>
          Promise.resolve({
            baseUrl: "http://127.0.0.1:41771/backend-api",
            streamIdleTimedOut: () => true,
            close: () => Promise.resolve(),
          }),
        commandRunner: () => Promise.resolve(codexCommandResult({ code: 1, durationMs: 600_000 })),
      }),
    (error) => error instanceof CodexInvocationError && error.failure === "invocation_timeout",
  );
  assert.equal(filesystem.removed.length, 1);
});

Deno.test("native review delegates reviewer model selection to Codex", async () => {
  const filesystem = new MemoryFilesystem();
  let command: CodexCommandRequest | null = null;
  const nativeReviewOutput = {
    findings: [],
    overall_correctness: "patch is correct",
    overall_explanation: "No defects found.",
    overall_confidence_score: 0.94,
  };
  const result = await runNativeCodexReview({
    checkoutPath: "/checkout",
    authSlots: slots,
    expectedMaximumRuntimeMs: 1_000,
  }, {
    ...commonDependencies(filesystem, healthyFetcher()),
    commandRunner: (request) => {
      command = request;
      filesystem.files.set(
        `${request.env.CODEX_HOME}/sessions/2026/08/22/rollout-review.jsonl`,
        `${
          JSON.stringify({
            timestamp: "2026-08-22T00:00:00Z",
            type: "event_msg",
            payload: {
              type: "item_completed",
              item: { type: "ExitedReviewMode", review_output: nativeReviewOutput },
            },
          })
        }\n`,
      );
      return Promise.resolve(codexCommandResult({ stdout: "Arbitrary rendered explanation." }));
    },
  });
  assert.equal(result.lastMessage, null);
  assert.deepEqual(result.nativeReviewOutput, nativeReviewOutput);
  assert.ok(command);
  const args = (command as unknown as CodexCommandRequest).args;
  assert.deepEqual(args.slice(-4), ["review", "--strict-config", "--base", "origin/development"]);
  assert.ok(args.includes("review"));
  assert.ok(args.includes("origin/development"));
  assert.equal(args.includes("-m"), false);
  assert.equal(args.some((arg) => arg.startsWith("model_reasoning_effort=")), false);
  assert.equal(args.some((arg) => arg.includes("gpt-5.6-sol")), false);
  assert.ok(args.includes('model_provider="sentinel_relay"'));
  assert.ok(args.includes("model_providers.sentinel_relay.supports_websockets=false"));
});

Deno.test("native review rollout extraction fails closed on missing, duplicate, or malformed output", () => {
  const line = JSON.stringify({
    type: "event_msg",
    payload: {
      type: "item_completed",
      item: {
        type: "ExitedReviewMode",
        review_output: {
          findings: [],
          overall_correctness: "patch is correct",
          overall_explanation: "Clean.",
          overall_confidence_score: 0.9,
        },
      },
    },
  });
  assert.deepEqual(
    extractNativeReviewOutputFromRollouts([`${JSON.stringify({ type: "session_meta" })}\n${line}\n`]),
    JSON.parse(line).payload.item.review_output,
  );
  assert.throws(() => extractNativeReviewOutputFromRollouts([]), /exactly one/);
  assert.throws(() => extractNativeReviewOutputFromRollouts([`${line}\n${line}\n`]), /exactly one/);
  assert.throws(() => extractNativeReviewOutputFromRollouts(["not-json\n"]), /invalid JSONL/);
});

Deno.test("native review stops when the private rollout has no structured completion", async () => {
  const filesystem = new MemoryFilesystem();
  await assert.rejects(
    () =>
      runNativeCodexReview({
        checkoutPath: "/checkout",
        authSlots: slots,
        expectedMaximumRuntimeMs: 1_000,
      }, {
        ...commonDependencies(filesystem, healthyFetcher()),
        commandRunner: () => Promise.resolve(codexCommandResult({ stdout: "The patch looks correct." })),
      }),
    (error) => error instanceof CodexInvocationError && error.failure === "native_review_missing",
  );
  assert.equal(filesystem.removed.length, 1);
});

Deno.test("structured output schema staged from runner temp is copied byte-exact into the private home", async () => {
  const filesystem = new MemoryFilesystem();
  const source = "/home/runner/work/_temp/sentinel-cell-implementation-schema.json";
  const schema = JSON.stringify({
    title: "implementation-cell-result",
    description: "cell fix summary – exact bytes",
    type: "object",
    required: ["summary", "changed_files"],
    additionalProperties: false,
  });
  filesystem.files.set(source, schema);
  let command: CodexCommandRequest | null = null;
  let stagedBytes = "";
  let stagedMode: number | undefined;
  await runStructuredCodexAgent({
    role: "implementation",
    checkoutPath: "/checkout",
    prompt: "Implement the selected issue.",
    outputSchemaPath: source,
    authSlots: slots,
    expectedMaximumRuntimeMs: 1_000,
  }, {
    ...commonDependencies(filesystem, healthyFetcher()),
    commandRunner: (request) => {
      command = request;
      const privateSchemaPath = `${request.env.CODEX_HOME}/output-schema.json`;
      stagedBytes = filesystem.files.get(privateSchemaPath) ?? "";
      stagedMode = filesystem.modes.get(privateSchemaPath);
      const lastMessagePath = request.args[request.args.indexOf("--output-last-message") + 1]!;
      filesystem.files.set(lastMessagePath, "{}");
      return Promise.resolve(codexCommandResult());
    },
  });
  assert.ok(command);
  const captured = command as unknown as CodexCommandRequest;
  const privateHome = captured.env.CODEX_HOME;
  const privateSchemaPath = `${privateHome}/output-schema.json`;
  assert.ok(privateSchemaPath.startsWith(privateHome));
  assert.equal(privateSchemaPath.startsWith("/home/runner/work/_temp"), false);
  assert.equal(captured.args.includes(source), false);
  assert.equal(captured.args[captured.args.indexOf("--output-schema") + 1]!, privateSchemaPath);
  assert.equal(stagedBytes, schema);
  assert.equal(stagedMode, 0o600);
  assert.equal(filesystem.files.get(source), schema);
  assert.equal(filesystem.files.has(privateSchemaPath), false);
  assert.deepEqual(filesystem.removed, [privateHome]);
});

Deno.test("staged output schema is cleaned up when the invocation fails", async () => {
  const filesystem = new MemoryFilesystem();
  const source = "/home/runner/work/_temp/sentinel-cell-implementation-schema.json";
  const schema = schemaFixture("implementation-cell-result");
  filesystem.files.set(source, schema);
  await assert.rejects(
    () =>
      runStructuredCodexAgent({
        role: "implementation",
        checkoutPath: "/checkout",
        prompt: "Implement the selected issue.",
        outputSchemaPath: source,
        authSlots: slots,
        expectedMaximumRuntimeMs: 1_000,
      }, {
        ...commonDependencies(filesystem, healthyFetcher()),
        commandRunner: () => Promise.resolve(codexCommandResult({ code: 1 })),
      }),
    (error) => error instanceof CodexInvocationError && error.failure === "command_failed",
  );
  assert.equal(filesystem.removed.length, 1);
  const home = filesystem.removed[0]!;
  assert.equal(filesystem.files.get(source), schema);
  assert.equal(filesystem.files.has(`${home}/output-schema.json`), false);
});

Deno.test("an unreadable structured schema source fails before spawning and cleans the private home", async () => {
  const filesystem = new MemoryFilesystem();
  let spawned = false;
  await assert.rejects(
    () =>
      runStructuredCodexAgent({
        role: "implementation",
        checkoutPath: "/checkout",
        prompt: "Implement the selected issue.",
        outputSchemaPath: "/home/runner/work/_temp/missing-cell-schema.json",
        authSlots: slots,
        expectedMaximumRuntimeMs: 1_000,
      }, {
        ...commonDependencies(filesystem, healthyFetcher()),
        commandRunner: () => {
          spawned = true;
          return Promise.resolve(codexCommandResult());
        },
      }),
    (error) => error instanceof CodexInvocationError && error.failure === "invalid_options",
  );
  assert.equal(spawned, false);
  assert.equal(filesystem.removed.length, 1);
});
