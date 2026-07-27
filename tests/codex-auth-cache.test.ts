import assert from "node:assert/strict";
import type { CodexAuthPoolState, CodexAuthState } from "../src/types.ts";

const AUTH_KEY = ["ubq_ai", "codex_auth"] as const;

class AuthKv {
  auth: CodexAuthPoolState;
  reads = 0;
  nextReadGate: Promise<void> | null = null;
  authVersion = 1;
  readonly extra = new Map<string, { value: unknown; version: number }>();

  constructor(auth: CodexAuthPoolState) {
    this.auth = auth;
  }

  get<T>(key: Deno.KvKey, options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
    assert.equal(options?.consistency, "strong");
    if (JSON.stringify(key) !== JSON.stringify(AUTH_KEY)) {
      const entry = this.extra.get(JSON.stringify(key));
      return Promise.resolve({
        key,
        value: (entry?.value ?? null) as T | null,
        versionstamp: entry ? String(entry.version).padStart(20, "0") : null,
      } as Deno.KvEntryMaybe<T>);
    }
    this.reads += 1;
    const value = this.auth as T;
    const versionstamp = String(this.authVersion).padStart(20, "0");
    const gate = this.nextReadGate;
    this.nextReadGate = null;
    return (gate ?? Promise.resolve()).then(() => ({ key, value, versionstamp }));
  }

  set(key: Deno.KvKey, value: unknown): Promise<Deno.KvCommitResult> {
    if (JSON.stringify(key) === JSON.stringify(AUTH_KEY)) {
      this.auth = value as CodexAuthPoolState;
      this.authVersion += 1;
    } else {
      const encoded = JSON.stringify(key);
      this.extra.set(encoded, { value, version: (this.extra.get(encoded)?.version ?? 0) + 1 });
    }
    return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" });
  }

  atomic(): Deno.AtomicOperation {
    const checks: Array<{ key: Deno.KvKey; versionstamp: string | null }> = [];
    const writes: Array<{ type: "set" | "delete"; key: Deno.KvKey; value?: unknown }> = [];
    const chain = {
      check: (...entries: Array<{ key: Deno.KvKey; versionstamp: string | null }>) => {
        checks.push(...entries);
        return chain;
      },
      set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
        writes.push({ type: "set", key, value });
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        writes.push({ type: "delete", key });
        return chain;
      },
      commit: () => {
        for (const check of checks) {
          const isAuth = JSON.stringify(check.key) === JSON.stringify(AUTH_KEY);
          const version = isAuth
            ? String(this.authVersion).padStart(20, "0")
            : this.extra.has(JSON.stringify(check.key))
            ? String(this.extra.get(JSON.stringify(check.key))!.version).padStart(20, "0")
            : null;
          if (version !== check.versionstamp) return Promise.resolve({ ok: false } as const);
        }
        for (const write of writes) {
          const isAuth = JSON.stringify(write.key) === JSON.stringify(AUTH_KEY);
          if (isAuth) {
            if (write.type === "set") this.auth = write.value as CodexAuthPoolState;
            this.authVersion += 1;
            continue;
          }
          const encoded = JSON.stringify(write.key);
          if (write.type === "delete") this.extra.delete(encoded);
          else this.extra.set(encoded, { value: write.value, version: (this.extra.get(encoded)?.version ?? 0) + 1 });
        }
        return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" } as const);
      },
    };
    return chain as unknown as Deno.AtomicOperation;
  }
}

const fixedStartMs = 1_000_000;
const encodeBase64Url = (value: unknown): string =>
  btoa(JSON.stringify(value)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
const accessToken = (label: string): string =>
  `${encodeBase64Url({ alg: "none" })}.${encodeBase64Url({ exp: (fixedStartMs + 60 * 60_000) / 1000 })}.${label}`;
const auth = (label: string): CodexAuthState => ({
  access_token: accessToken(label),
  refresh_token: `refresh-${label}`,
  account_id: `account-${label}`,
  updated_at_ms: fixedStartMs,
});
const staleAuth = (label: string): CodexAuthState => ({
  ...auth(label),
  access_token: `${encodeBase64Url({ alg: "none" })}.${
    encodeBase64Url({ exp: (fixedStartMs + 30_000) / 1000 })
  }.${label}`,
});
const pool = (...accounts: CodexAuthState[]): CodexAuthPoolState => ({
  accounts,
  updated_at_ms: fixedStartMs,
});

const kv = new AuthKv(pool(auth("old")));
(Deno as unknown as { openKv: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kv as unknown as Deno.Kv);

const { config } = await import("../src/config.ts");

const {
  cacheCodexAuthPool,
  CODEX_AUTH_CACHE_TTL_MS,
  CodexError,
  fetchCodexResponses,
  orderCodexAuthAccounts,
  resetCodexAuthCacheForTest,
} = await import("../src/codex.ts");
const {
  markCodexQuotaBlocked,
  selectCodexRoutingAccounts,
} = await import("../src/codex_account_routing.ts");

Deno.test("Codex auth account ordering rotates from the selected account", () => {
  const accounts = [auth("one"), auth("two")];
  assert.deepEqual(orderCodexAuthAccounts(accounts, 0).map((candidate) => candidate.account_id), [
    "account-one",
    "account-two",
  ]);
  assert.deepEqual(orderCodexAuthAccounts(accounts, 1).map((candidate) => candidate.account_id), [
    "account-two",
    "account-one",
  ]);
});

Deno.test("Codex responses retry the other account after an account-level 429", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response("{}", { status: accountIds.length === 1 ? 429 : 200 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "balance" });
    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses try the second account after 403", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response("{}", { status: accountIds.length === 1 ? 403 : 200 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "forbidden-failover" });
    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses make one bounded final retry after both accounts return 429", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const retryDelays: number[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length === 3) return Promise.resolve(new Response("{}", { status: 200 }));
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "rate_limit_error" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "1" },
      }),
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "bounded-retry" },
      {
        requestId: "request-bounded-retry",
        retrySleep: (milliseconds) => {
          retryDelays.push(milliseconds);
          return Promise.resolve();
        },
      },
    );
    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one", "account-two", "account-one"]);
    assert.deepEqual(retryDelays, [1_000]);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex 429 retry sleep normalizes a shared timeout as a gateway timeout", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const controller = new AbortController();
  const timeoutReason = new DOMException("request deadline exceeded", "TimeoutError");
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "rate_limit_error" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "1" },
      }),
    );
  };

  try {
    await assert.rejects(
      () =>
        fetchCodexResponses(
          { input: "retry-timeout" },
          {
            signal: controller.signal,
            retrySleep: () => {
              queueMicrotask(() => controller.abort(timeoutReason));
              return new Promise<void>(() => {});
            },
          },
        ),
      (error: unknown) => {
        if (!(error instanceof CodexError)) return false;
        assert.equal(error.code, "gateway_timeout");
        assert.equal(error.status, 504);
        assert.equal((error as Error & { cause?: unknown }).cause, timeoutReason);
        return true;
      },
    );
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex 429 retry sleep preserves ordinary cancellation", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const controller = new AbortController();
  const abortReason = new DOMException("client disconnected", "AbortError");
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "rate_limit_error" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "1" },
      }),
    );
  };

  try {
    await assert.rejects(
      () =>
        fetchCodexResponses(
          { input: "retry-cancelled" },
          {
            signal: controller.signal,
            retrySleep: () => {
              queueMicrotask(() => controller.abort(abortReason));
              return new Promise<void>(() => {});
            },
          },
        ),
      (error: unknown) => {
        assert.equal(error, abortReason);
        return true;
      },
    );
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an expired generic 429 retry preserves the later 403 fallback response", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  let now = fixedStartMs;
  Date.now = () => now;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length === 1) {
      return Promise.resolve(
        Response.json(
          {
            error: {
              message: "First account temporarily rate limited",
              type: "rate_limit_error",
              code: "rate_limit_exceeded",
            },
          },
          { status: 429 },
        ),
      );
    }
    if (accountIds.length === 2) {
      now = fixedStartMs + 5_000;
      return Promise.resolve(
        Response.json(
          {
            error: {
              message: "Second account forbidden",
              type: "invalid_request_error",
              code: "second_account_forbidden",
            },
          },
          { status: 403 },
        ),
      );
    }
    return Promise.resolve(new Response("late retry must not run", { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "expired-generic-retry" },
      {
        requestId: "request-expired-generic-retry",
        retrySleep: () => Promise.resolve(),
      },
    );
    assert.equal(response.status, 403);
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
    assert.equal(now, fixedStartMs + 5_000);
    assert.deepEqual(await response.json(), {
      error: {
        message: "Second account forbidden",
        type: "invalid_request_error",
        code: "second_account_forbidden",
      },
    });
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex routing logs attempts, refresh, and bounded retry without sensitive values", async () => {
  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const lines: string[] = [];
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  console.info = (...args: unknown[]) => lines.push(args.map(String).join(" "));
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url.includes("auth.openai.com/oauth/token")) {
      return Promise.resolve(
        new Response(
          JSON.stringify({
            access_token: "refreshed-secret-access",
            refresh_token: "refreshed-secret-refresh",
          }),
          { status: 200, headers: { "Content-Type": "application/json" } },
        ),
      );
    }
    inferenceCalls += 1;
    const status = inferenceCalls === 1 ? 401 : 429;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "full-sensitive-upstream-error-body" } }), {
        status,
        headers: status === 429 ? { "Retry-After": "1" } : undefined,
      }),
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "redacted-logs" },
      {
        requestId: "request-redacted-logs",
        retrySleep: () => Promise.resolve(),
      },
    );
    assert.equal(response.status, 429);
    const output = lines.join("\n");
    assert.match(output, /"event":"codex_attempt"/);
    assert.match(output, /"event":"codex_token_refresh"/);
    assert.match(output, /"event":"codex_two_second_retry"/);
    assert.match(output, /"status_class":"401"/);
    assert.match(output, /"status_class":"429"/);
    for (
      const forbidden of [
        "account-one",
        "account-two",
        "access-one",
        "refresh-one",
        "refreshed-secret-access",
        "refreshed-secret-refresh",
        "full-sensitive-upstream-error-body",
      ]
    ) {
      assert.equal(output.includes(forbidden), false, forbidden);
    }
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    console.info = originalInfo;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses retry the other account when a 401 cannot refresh", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  let firstCancellationStarted = false;
  let refreshCancellationStarted = false;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url.includes("auth.openai.com/oauth/token")) {
      return Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(new TextEncoder().encode('{"error":"invalid_grant"}'));
            },
            cancel() {
              refreshCancellationStarted = true;
              return new Promise<void>(() => {});
            },
          }),
          { status: 401 },
        ),
      );
    }
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length === 1) {
      return Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(new TextEncoder().encode("{}"));
            },
            cancel() {
              firstCancellationStarted = true;
              return new Promise<void>(() => {});
            },
          }),
          { status: 401 },
        ),
      );
    }
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "auth-failover" });
    assert.equal(response.status, 200);
    assert.equal(accountIds.length, 2);
    assert.equal(new Set(accountIds).size, 2);
    assert.equal(firstCancellationStarted, true);
    assert.equal(refreshCancellationStarted, true);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a credential replacement landing after 401 is retried without an OAuth refresh", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const attempted = auth("one");
  const replacement: CodexAuthState = {
    ...attempted,
    access_token: accessToken("replacement"),
    refresh_token: "refresh-replacement",
    updated_at_ms: fixedStartMs + 1,
  };
  const authorizationHeaders: string[] = [];
  let oauthCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(attempted);
  kv.authVersion += 1;
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url.includes("auth.openai.com/oauth/token")) {
      oauthCalls += 1;
      return Promise.resolve(new Response('{"error":"must_not_refresh_replacement"}', { status: 401 }));
    }
    authorizationHeaders.push(request.headers.get("Authorization") ?? "");
    if (authorizationHeaders.length === 1) {
      kv.auth = pool(replacement);
      kv.authVersion += 1;
      return Promise.resolve(new Response("{}", { status: 401 }));
    }
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "rotation-between-401-and-refresh" });
    assert.equal(response.status, 200);
    assert.equal(oauthCalls, 0);
    assert.deepEqual(authorizationHeaders, [
      `Bearer ${attempted.access_token}`,
      `Bearer ${replacement.access_token}`,
    ]);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses synthesize 401 only after every account has an invalid refresh credential", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  let refreshCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      return Promise.resolve(new Response('{"error":"invalid_grant"}', { status: 401 }));
    }
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response("{}", { status: 401 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "auth-exhaustion" });
    assert.equal(response.status, 401);
    assert.equal((await response.json() as { error?: { code?: string } }).error?.code, "codex_auth_invalid");
    assert.equal(accountIds.length, 2);
    assert.equal(new Set(accountIds).size, 2);
    assert.equal(refreshCalls, 2);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("concurrent proactive refreshes share one OAuth exchange", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let refreshCalls = 0;
  let releaseRefresh = (): void => {};
  const refreshGate = new Promise<void>((resolve) => {
    releaseRefresh = resolve;
  });
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(staleAuth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = async (input) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    if (url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      await refreshGate;
      return new Response(JSON.stringify({ access_token: "refreshed-access", refresh_token: "refreshed-refresh" }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    }
    return new Response("{}", { status: 200 });
  };

  try {
    const requests = Array.from({ length: 8 }, (_, index) => fetchCodexResponses({ input: `refresh-${index}` }));
    for (let attempt = 0; attempt < 100 && refreshCalls === 0; attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 1));
    }
    assert.equal(refreshCalls, 1, "expected one refresh to begin");
    releaseRefresh();
    const responses = await Promise.all(requests);
    assert.equal(refreshCalls, 1);
    assert.deepEqual(responses.map((response) => response.status), Array(8).fill(200));
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a deterministic proactive refresh rejection quarantines the credential before inference", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let refreshCalls = 0;
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(staleAuth("invalid"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    if (url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      return Promise.resolve(new Response(JSON.stringify({ error: "invalid_grant" }), { status: 401 }));
    }
    inferenceCalls += 1;
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const first = await fetchCodexResponses({ input: "expired-auth" });
    const second = await fetchCodexResponses({ input: "expired-auth-again" });
    assert.equal(first.status, 401);
    assert.equal(second.status, 401);
    assert.equal(refreshCalls, 1);
    assert.equal(inferenceCalls, 0);
    assert.equal((await first.json() as { error?: { code?: string } }).error?.code, "codex_auth_invalid");
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a malformed successful refresh is transient and does not quarantine the credential", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let refreshCalls = 0;
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    if (url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      if (refreshCalls === 1) {
        return Promise.resolve(
          new Response(JSON.stringify({ refresh_token: "refresh-one" }), {
            status: 200,
            headers: { "Content-Type": "application/json" },
          }),
        );
      }
      return Promise.resolve(
        new Response(
          JSON.stringify({ access_token: accessToken("recovered"), refresh_token: "refresh-recovered" }),
          { status: 200, headers: { "Content-Type": "application/json" } },
        ),
      );
    }
    inferenceCalls += 1;
    return Promise.resolve(new Response("{}", { status: inferenceCalls <= 2 ? 401 : 200 }));
  };

  try {
    await assert.rejects(
      () => fetchCodexResponses({ input: "malformed-refresh" }),
      (error: unknown) =>
        error instanceof Error &&
        "status" in error &&
        error.status === 503 &&
        error.message.includes("missing access_token"),
    );
    const recovered = await fetchCodexResponses({ input: "valid-refresh" });
    assert.equal(recovered.status, 200);
    assert.equal(refreshCalls, 2);
    assert.equal(inferenceCalls, 3);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("every direct half-open probe outcome releases its expired quota lease", async (t) => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let now = fixedStartMs;
  Date.now = () => now;
  (config as { isDeploy: boolean }).isDeploy = true;
  try {
    for (
      const testCase of [
        { name: "403", status: 403 },
        { name: "invalid 400", status: 400 },
        { name: "500", status: 500 },
        { name: "network", status: null },
        { name: "timeout", status: null, timeout: true },
      ] as const
    ) {
      await t.step(testCase.name, async () => {
        now = fixedStartMs;
        kv.auth = pool(auth("one"));
        kv.extra.clear();
        resetCodexAuthCacheForTest();
        const initial = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, now);
        assert.equal(initial.kind, "eligible");
        if (initial.kind !== "eligible") return;
        await markCodexQuotaBlocked(
          initial.accounts[0]!,
          new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
            status: 429,
            headers: { "Content-Type": "application/json", "Retry-After": "1" },
          }),
          now,
        );

        now += 1_001;
        let codexCalls = 0;
        globalThis.fetch = (_input, init) => {
          codexCalls += 1;
          if (codexCalls > 1) return Promise.resolve(new Response("{}", { status: 200 }));
          if (testCase.timeout) {
            return Promise.reject(init?.signal?.reason ?? new DOMException("timed out", "TimeoutError"));
          }
          if (testCase.status === null) return Promise.reject(new TypeError("network fixture"));
          return Promise.resolve(new Response("{}", { status: testCase.status }));
        };

        if (testCase.timeout) {
          const controller = new AbortController();
          controller.abort(new DOMException("timed out", "TimeoutError"));
          await assert.rejects(
            () => fetchCodexResponses({ input: testCase.name }, { signal: controller.signal }),
            (error: unknown) => error instanceof Error && "status" in error && error.status === 504,
          );
        } else if (testCase.status === null) {
          await assert.rejects(
            () => fetchCodexResponses({ input: testCase.name }),
            (error: unknown) => error instanceof Error && "status" in error && error.status === 502,
          );
        } else {
          const direct = await fetchCodexResponses({ input: testCase.name });
          assert.equal(direct.status, testCase.status);
        }

        const second = await fetchCodexResponses({ input: `${testCase.name}-second` });
        assert.equal(second.status, 200);
        assert.equal(codexCalls, 2);
      });
    }
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a 429 retry that proves invalid credentials remains quarantined", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let now = fixedStartMs;
  let inferenceCalls = 0;
  let refreshCalls = 0;
  Date.now = () => now;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  const initial = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, now);
  assert.equal(initial.kind, "eligible");
  if (initial.kind !== "eligible") return;
  await markCodexQuotaBlocked(
    initial.accounts[0]!,
    new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
      status: 429,
      headers: { "Content-Type": "application/json", "Retry-After": "1" },
    }),
    now,
  );
  now += 1_001;
  globalThis.fetch = (input) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    if (url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      return Promise.resolve(new Response('{"error":"invalid_grant"}', { status: 401 }));
    }
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": "1" },
        }),
      );
    }
    return Promise.resolve(new Response("{}", { status: 401 }));
  };

  try {
    const first = await fetchCodexResponses(
      { input: "retry-invalid" },
      { retrySleep: () => Promise.resolve() },
    );
    assert.equal(first.status, 401);
    const second = await fetchCodexResponses({ input: "retry-invalid-again" });
    assert.equal(second.status, 401);
    assert.equal(inferenceCalls, 2);
    assert.equal(refreshCalls, 1);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a 401 after proactive refresh does not refresh the same account twice", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let refreshCalls = 0;
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(staleAuth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    if (url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      return Promise.resolve(
        new Response(
          JSON.stringify({
            access_token: accessToken("refreshed-once"),
            refresh_token: "refresh-refreshed-once",
          }),
          { status: 200, headers: { "Content-Type": "application/json" } },
        ),
      );
    }
    inferenceCalls += 1;
    return Promise.resolve(new Response("{}", { status: 401 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "one-refresh-only" });
    assert.equal(response.status, 401);
    assert.equal(refreshCalls, 1);
    assert.equal(inferenceCalls, 1);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex auth cache revalidates rotations across warm isolates without per-request KV reads", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const authorizations: string[] = [];
  const accountIds: string[] = [];
  let nowMs = fixedStartMs;
  Date.now = () => nowMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    authorizations.push(request.headers.get("authorization") ?? "");
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    kv.auth = pool(auth("old"));
    kv.extra.clear();
    kv.reads = 0;
    resetCodexAuthCacheForTest();

    await fetchCodexResponses({ input: "cold" });
    assert.equal(kv.reads, 1);
    assert.equal(authorizations.at(-1), `Bearer ${accessToken("old")}`);

    kv.auth = pool(auth("rotated"));
    nowMs += CODEX_AUTH_CACHE_TTL_MS - 1;
    await fetchCodexResponses({ input: "warm" });
    assert.equal(kv.reads, 1, "warm auth-cache hits must not read KV");
    assert.equal(authorizations.at(-1), `Bearer ${accessToken("old")}`);

    nowMs += 2;
    await Promise.all(
      Array.from({ length: 8 }, (_, index) => fetchCodexResponses({ input: `revalidate-${index}` })),
    );
    assert.equal(kv.reads, 2, "concurrent expiry must coalesce to one credential read");
    assert.deepEqual(authorizations.slice(-8), Array(8).fill(`Bearer ${accessToken("rotated")}`));
    assert.deepEqual(accountIds.slice(-8), Array(8).fill("account-rotated"));

    await fetchCodexResponses({ input: "warm-again" });
    assert.equal(kv.reads, 2, "the revalidated credential must remain a zero-read warm hit");

    let releaseDelayedRead = (): void => {};
    kv.nextReadGate = new Promise<void>((resolve) => {
      releaseDelayedRead = resolve;
    });
    kv.auth = pool(auth("stale-read"));
    nowMs += CODEX_AUTH_CACHE_TTL_MS + 1;
    const delayedRequest = fetchCodexResponses({ input: "delayed-revalidation" });
    while (kv.reads < 3) await Promise.resolve();

    const racedAdmin = auth("admin-race");
    kv.auth = pool(racedAdmin);
    cacheCodexAuthPool(pool(racedAdmin));
    releaseDelayedRead();
    await delayedRequest;
    assert.equal(authorizations.at(-1), `Bearer ${accessToken("admin-race")}`);
    assert.equal(accountIds.at(-1), "account-admin-race");

    await fetchCodexResponses({ input: "after-delayed-revalidation" });
    assert.equal(kv.reads, 3, "a delayed stale read must not evict the admin credential");
    assert.equal(authorizations.at(-1), `Bearer ${accessToken("admin-race")}`);

    const immediate = auth("admin");
    kv.auth = pool(immediate);
    cacheCodexAuthPool(pool(immediate));
    await fetchCodexResponses({ input: "admin-update" });
    assert.equal(kv.reads, 3, "the admin isolate cache update must take effect without another KV read");
    assert.equal(authorizations.at(-1), `Bearer ${accessToken("admin")}`);
    assert.equal(accountIds.at(-1), "account-admin");
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});
