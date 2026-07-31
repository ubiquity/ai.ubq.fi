import assert from "node:assert/strict";
import { CODEX_BANKED_RESET_LEASE_MS, type CodexBankedResetConfig } from "../src/codex_banked_reset.ts";
import type { CodexUsageResetProvider } from "../src/codex_banked_reset_provider.ts";
import type { CodexAuthPoolState, CodexAuthState } from "../src/types.ts";

const AUTH_KEY = ["ubq_ai", "codex_auth"] as const;

class AuthKv {
  auth: CodexAuthPoolState;
  reads = 0;
  routingReads = 0;
  nextReadGate: Promise<void> | null = null;
  onRoutingRead: ((read: number) => void | Promise<void>) | null = null;
  authVersion = 1;
  readonly extra = new Map<string, { value: unknown; version: number }>();

  constructor(auth: CodexAuthPoolState) {
    this.auth = auth;
  }

  get<T>(key: Deno.KvKey, options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
    assert.equal(options?.consistency, "strong");
    if (JSON.stringify(key) !== JSON.stringify(AUTH_KEY)) {
      const routingRead = ++this.routingReads;
      const hook = this.onRoutingRead;
      return Promise.resolve(hook?.(routingRead)).then(() => {
        const entry = this.extra.get(JSON.stringify(key));
        return {
          key,
          value: (entry?.value ?? null) as T | null,
          versionstamp: entry ? String(entry.version).padStart(20, "0") : null,
        } as Deno.KvEntryMaybe<T>;
      });
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
  getCodexResponseSlot,
  getCodexRoutingProbe,
  markCodexResponseCompleted,
  orderCodexAuthAccounts,
  releaseCodexResponseProbe,
  resetCodexAuthCacheForTest,
} = await import("../src/codex.ts");
const {
  claimCodexRoutingProbe,
  CODEX_ACCOUNT_ROUTING_KV_KEY,
  markCodexQuotaBlocked,
  parseCodexAccountRoutingState,
  resetCodexAccountRoutingForTest,
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
    assert.equal(getCodexResponseSlot(response), 2);
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

Deno.test("a valid persisted Codex pool is not overlaid by a local configured seed", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const originalSeed = config.codexAuthJsonB64;
  const authorizations: string[] = [];
  const persisted = auth("persisted");
  const localSeed = { ...auth("local-stale"), account_id: persisted.account_id };
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean; codexAuthJsonB64: string }).isDeploy = false;
  (config as { isDeploy: boolean; codexAuthJsonB64: string }).codexAuthJsonB64 = btoa(
    JSON.stringify({
      tokens: {
        access_token: localSeed.access_token,
        refresh_token: localSeed.refresh_token,
        account_id: localSeed.account_id,
      },
    }),
  );
  kv.auth = pool(persisted);
  kv.extra.clear();
  const versionBefore = kv.authVersion;
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    authorizations.push(request.headers.get("authorization") ?? "");
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "persisted-authority" });
    assert.equal(response.status, 200);
    assert.deepEqual(authorizations, [`Bearer ${persisted.access_token}`]);
    assert.deepEqual(kv.auth, pool(persisted));
    assert.equal(kv.authVersion, versionBefore, "loading a persisted pool must not write a local seed into KV");
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean; codexAuthJsonB64: string }).isDeploy = originalDeployFlag;
    (config as { isDeploy: boolean; codexAuthJsonB64: string }).codexAuthJsonB64 = originalSeed;
  }
});

const liveBankedResetConfig = (accountId = "account-one"): CodexBankedResetConfig => ({
  enabled: true,
  mode: "live",
  accountAllowlist: new Set([accountId]),
  maxGlobalPerDay: 1,
  maxPerAccountPerWindow: 1,
});

const shadowBankedResetConfig = (...accountIds: string[]): CodexBankedResetConfig => ({
  ...liveBankedResetConfig(),
  mode: "shadow",
  accountAllowlist: new Set(accountIds.length ? accountIds : ["account-one"]),
});

const stableBankedResetRetryAfter = new Date(fixedStartMs + 60_000).toUTCString();

const seedStableBankedResetBlock = async (accountId = "account-one"): Promise<void> => {
  const initial = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, fixedStartMs);
  assert.equal(initial.kind, "eligible");
  if (initial.kind !== "eligible") throw new Error("expected an eligible routing pool");
  const account = initial.accounts.find((candidate) => candidate.auth.account_id === accountId);
  assert.ok(account);
  await markCodexQuotaBlocked(
    account,
    new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
      status: 429,
      headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
    }),
    fixedStartMs,
  );
  resetCodexAccountRoutingForTest();
};

const scriptedResetProvider = (
  options: Readonly<{
    verify?: boolean;
    redeemKind?: "completed" | "unknown" | "already_redeemed";
    onInventory?: () => void | Promise<void>;
    onRedeem?: () => void;
    onVerify?: () => void;
    redeemGate?: Promise<void>;
  }> = {},
) => {
  const calls: string[] = [];
  const inventoryAccountIds: string[] = [];
  const idempotencyKeys: string[] = [];
  const redeemAccountIds: string[] = [];
  const provider: CodexUsageResetProvider = {
    contract: {
      idempotency: { callerSupplied: true, retentionMs: 86_400_000 },
      lookup: { byIdempotencyKey: true, byProviderReceiptId: true },
      verification: { independentlyVerifiable: true },
      receiptIdsSafeToPersistAndLog: true,
      supportedResetTypes: ["codex_rate_limits"],
    },
    readInventory: async (input) => {
      calls.push("inventory");
      inventoryAccountIds.push(input.accountId);
      await options.onInventory?.();
      return {
        availableCount: 1,
        observedAtMs: fixedStartMs,
        credits: [
          { id: "fixture-credit", status: "available", resetType: "codex_rate_limits", expiresAtMs: null },
        ],
      };
    },
    redeem: async (input) => {
      calls.push("redeem");
      idempotencyKeys.push(input.idempotencyKey);
      redeemAccountIds.push(input.accountId);
      options.onRedeem?.();
      if (options.redeemGate) await options.redeemGate;
      return options.redeemKind === "unknown"
        ? { kind: "unknown", providerReceiptId: null }
        : options.redeemKind === "already_redeemed"
        ? { kind: "already_redeemed", providerReceiptId: "receipt-sanitized" }
        : { kind: "completed", providerReceiptId: "receipt-sanitized" };
    },
    lookup: (input) => {
      calls.push("lookup");
      idempotencyKeys.push(input.idempotencyKey);
      return Promise.resolve({ kind: "completed", providerReceiptId: "receipt-sanitized" });
    },
    verifyApplied: () => {
      calls.push("verify");
      options.onVerify?.();
      return Promise.resolve(options.verify ?? true);
    },
  };
  return { provider, calls, inventoryAccountIds, idempotencyKeys, redeemAccountIds };
};

Deno.test("banked reset exhausts normal routing, verifies, and retries the redeemed account once", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        }),
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "response-after-reset" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-happy" },
      {
        requestId: "banked-reset-happy",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-happy",
        },
      },
    );
    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one", "account-one"]);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
    assert.equal(reset.idempotencyKeys.length, 1);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("the default upstream adapter shadows and redeems one partial blocked cohort through terminal codes", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const originalCodexBaseUrl = config.codexBaseUrl;
  const requests: Array<
    Readonly<{ url: string; method: string; headers: Headers; body: string; signal: AbortSignal | null }>
  > = [];
  let live = false;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = true;
  (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl =
    "https://upstream-reset.test/backend-api/codex";
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = async (input, init) => {
    const request = new Request(input, init);
    requests.push({
      url: request.url,
      method: request.method,
      headers: new Headers(request.headers),
      body: request.method === "POST" ? await request.text() : "",
      signal: init?.signal ?? null,
    });
    const accountId = request.headers.get("chatgpt-account-id");
    if (request.url.endsWith("/backend-api/codex/responses")) {
      if (accountId === "account-one" && !live) {
        return new Response(
          JSON.stringify({
            error: {
              type: "usage_limit_reached",
              resets_at: Math.floor(Date.parse(stableBankedResetRetryAfter) / 1_000),
            },
          }),
          {
            status: 429,
            headers: { "Content-Type": "application/json" },
          },
        );
      }
      return new Response(JSON.stringify({ id: `response-${accountId}` }), { status: 200 });
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits")) {
      assert.equal(request.method, "GET");
      assert.equal(accountId, "account-one");
      assert.ok(init?.signal instanceof AbortSignal);
      return Response.json({
        available_count: 1,
        credits: [
          {
            id: "expiring-credit",
            status: "available",
            reset_type: "codex_rate_limits",
            expires_at: null,
          },
        ],
      });
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits/consume")) {
      assert.equal(request.method, "POST");
      assert.equal(accountId, "account-one");
      return Response.json({ code: "reset", windows_reset: 1 });
    }
    throw new Error(`unexpected request ${request.method} ${request.url}`);
  };

  try {
    const shadowOptions = (requestId: string) => ({
      clientVersion: "0.145.0",
      requestId,
      bankedReset: {
        config: shadowBankedResetConfig("account-one", "account-two"),
        kv: kv as unknown as Deno.Kv,
        now: () => fixedStartMs,
        newOwnerToken: () => `owner-${requestId}`,
      },
    });
    const seeded = await fetchCodexResponses(
      { input: "seed-partial-block" },
      shadowOptions("seed-partial-block"),
    );
    assert.equal(seeded.status, 200);

    const shadowed = await fetchCodexResponses(
      { input: "shadow-partial-block" },
      shadowOptions("shadow-partial-block"),
    );
    assert.equal(shadowed.status, 200);
    const inventoryCountAfterShadow = requests.filter((request) =>
      request.url.endsWith("/rate-limit-reset-credits")
    ).length;
    assert.equal(inventoryCountAfterShadow, 1);

    const duplicateShadow = await fetchCodexResponses(
      { input: "shadow-partial-block-duplicate" },
      shadowOptions("shadow-partial-block-duplicate"),
    );
    assert.equal(duplicateShadow.status, 200);
    assert.equal(
      requests.filter((request) => request.url.endsWith("/rate-limit-reset-credits")).length,
      inventoryCountAfterShadow,
    );

    live = true;
    const redeemed = await fetchCodexResponses(
      { input: "live-partial-block" },
      {
        clientVersion: "0.145.0",
        requestId: "live-partial-block",
        bankedReset: {
          config: liveBankedResetConfig(),
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-live-partial-block",
        },
      },
    );
    assert.equal(redeemed.status, 200);
    assert.deepEqual(
      requests.map((request) =>
        `${request.method} ${new URL(request.url).pathname} ${request.headers.get("chatgpt-account-id")}`
      ),
      [
        "POST /backend-api/codex/responses account-one",
        "POST /backend-api/codex/responses account-two",
        "GET /backend-api/wham/rate-limit-reset-credits account-one",
        "POST /backend-api/codex/responses account-two",
        "POST /backend-api/codex/responses account-two",
        "GET /backend-api/wham/rate-limit-reset-credits account-one",
        "POST /backend-api/wham/rate-limit-reset-credits/consume account-one",
        "POST /backend-api/codex/responses account-one",
      ],
    );
    const consume = requests.find((request) => request.url.endsWith("/consume"));
    assert.ok(consume);
    assert.equal(JSON.parse(consume.body).credit_id, "expiring-credit");
    assert.equal(typeof JSON.parse(consume.body).redeem_request_id, "string");
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = originalDeployFlag;
    (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl = originalCodexBaseUrl;
  }
});

Deno.test("persistent live auto-arms a partial cohort before one later consume and reset retry", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const originalCodexBaseUrl = config.codexBaseUrl;
  const inventoryAccountIds: string[] = [];
  const consumeAccountIds: string[] = [];
  const inferenceAccountIds: string[] = [];
  const consumeBodies: unknown[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = true;
  (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl =
    "https://upstream-reset.test/backend-api/codex";
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  globalThis.fetch = async (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    if (request.url.endsWith("/backend-api/codex/responses")) {
      inferenceAccountIds.push(accountId);
      return Response.json({ id: `response-${accountId}` });
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits")) {
      inventoryAccountIds.push(accountId);
      return Response.json({
        available_count: 1,
        credits: [
          {
            id: `credit-${accountId}`,
            status: "available",
            reset_type: "codex_rate_limits",
            expires_at: null,
          },
        ],
      });
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits/consume")) {
      consumeAccountIds.push(accountId);
      consumeBodies.push(JSON.parse(await request.text()));
      return Response.json({ code: "reset", windows_reset: 1 });
    }
    throw new Error(`unexpected request ${request.method} ${request.url}`);
  };

  const options = (requestId: string) => ({
    clientVersion: "0.145.0",
    requestId,
    bankedReset: {
      config: liveBankedResetConfig(),
      kv: kv as unknown as Deno.Kv,
      now: () => fixedStartMs,
      newOwnerToken: () => `owner-${requestId}`,
    },
  });

  try {
    const armed = await fetchCodexResponses(
      { input: "persistent-live-partial-arm" },
      options("persistent-live-partial-arm"),
    );
    assert.equal(armed.status, 200);
    assert.deepEqual(inventoryAccountIds, ["account-one"]);
    assert.deepEqual(consumeAccountIds, []);
    assert.deepEqual(inferenceAccountIds, ["account-two"]);

    const consumed = await fetchCodexResponses(
      { input: "persistent-live-partial-consume" },
      options("persistent-live-partial-consume"),
    );
    assert.equal(consumed.status, 200);
    assert.ok(getCodexRoutingProbe(consumed));
    assert.deepEqual(inventoryAccountIds, ["account-one", "account-one"]);
    assert.deepEqual(consumeAccountIds, ["account-one"]);
    assert.deepEqual(inferenceAccountIds, ["account-two", "account-one"]);
    assert.equal(consumeBodies.length, 1);
    assert.equal((consumeBodies[0] as { credit_id: unknown }).credit_id, "credit-account-one");
    assert.equal(typeof (consumeBodies[0] as { redeem_request_id: unknown }).redeem_request_id, "string");
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = originalDeployFlag;
    (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl = originalCodexBaseUrl;
  }
});

Deno.test("persistent live auto-arms an all-blocked cohort before one later consume and reset retry", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const originalCodexBaseUrl = config.codexBaseUrl;
  const inventoryAccountIds: string[] = [];
  const consumeAccountIds: string[] = [];
  const inferenceAccountIds: string[] = [];
  const consumeBodies: unknown[] = [];
  const persistentLiveConfig: CodexBankedResetConfig = {
    ...liveBankedResetConfig(),
    accountAllowlist: new Set(["account-one", "account-two"]),
  };
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = true;
  (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl =
    "https://upstream-reset.test/backend-api/codex";
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock("account-one");
  await seedStableBankedResetBlock("account-two");
  globalThis.fetch = async (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    if (request.url.endsWith("/backend-api/codex/responses")) {
      inferenceAccountIds.push(accountId);
      return Response.json({ id: `response-${accountId}` });
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits")) {
      inventoryAccountIds.push(accountId);
      return Response.json({
        available_count: 1,
        credits: [
          {
            id: `credit-${accountId}`,
            status: "available",
            reset_type: "codex_rate_limits",
            expires_at: null,
          },
        ],
      });
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits/consume")) {
      consumeAccountIds.push(accountId);
      consumeBodies.push(JSON.parse(await request.text()));
      return Response.json({ code: "reset", windows_reset: 1 });
    }
    throw new Error(`unexpected request ${request.method} ${request.url}`);
  };

  const options = (requestId: string) => ({
    clientVersion: "0.145.0",
    requestId,
    bankedReset: {
      config: persistentLiveConfig,
      kv: kv as unknown as Deno.Kv,
      now: () => fixedStartMs,
      newOwnerToken: () => `owner-${requestId}`,
    },
  });

  try {
    const armed = await fetchCodexResponses(
      { input: "persistent-live-all-blocked-arm" },
      options("persistent-live-all-blocked-arm"),
    );
    assert.equal(armed.status, 429);
    assert.equal((await armed.json()).error.code, "codex_quota_blocked");
    assert.deepEqual([...inventoryAccountIds].sort(), ["account-one", "account-two"]);
    assert.deepEqual(consumeAccountIds, []);
    assert.deepEqual(inferenceAccountIds, []);

    const consumed = await fetchCodexResponses(
      { input: "persistent-live-all-blocked-consume" },
      options("persistent-live-all-blocked-consume"),
    );
    assert.equal(consumed.status, 200);
    assert.ok(getCodexRoutingProbe(consumed));
    assert.equal(inventoryAccountIds.filter((accountId) => accountId === "account-one").length, 2);
    assert.equal(inventoryAccountIds.filter((accountId) => accountId === "account-two").length, 2);
    assert.deepEqual(consumeAccountIds, ["account-one"]);
    assert.deepEqual(inferenceAccountIds, ["account-one"]);
    assert.equal(consumeBodies.length, 1);
    assert.equal((consumeBodies[0] as { credit_id: unknown }).credit_id, "credit-account-one");
    assert.equal(typeof (consumeBodies[0] as { redeem_request_id: unknown }).redeem_request_id, "string");
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = originalDeployFlag;
    (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl = originalCodexBaseUrl;
  }
});

Deno.test("post-reset response probes retain their tombstone until an explicit completed outcome", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;

  const routingSlot = () => {
    const state = parseCodexAccountRoutingState(kv.extra.get(JSON.stringify(CODEX_ACCOUNT_ROUTING_KV_KEY))?.value);
    return state?.slots[0] ?? null;
  };
  const assertTombstone = () => {
    const slot = routingSlot();
    assert.equal(slot?.banked_reset_generation_ambiguous, true);
    assert.notEqual(slot?.observed_reset_at_ms, null);
  };
  const fetchPostResetResponse = async (owner: string): Promise<Response> => {
    const reset = scriptedResetProvider();
    let inferenceCalls = 0;
    kv.auth = pool(auth("one"));
    kv.extra.clear();
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = () => {
      inferenceCalls += 1;
      return Promise.resolve(
        inferenceCalls === 1
          ? new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
            status: 429,
            headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
          })
          : new Response(
            'data: {"type":"response.completed","response":{"output":[]}}\n\n',
            { status: 200, headers: { "Content-Type": "text/event-stream" } },
          ),
      );
    };
    const response = await fetchCodexResponses(
      { input: `post-reset-probe-${owner}` },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => owner,
        },
      },
    );
    assert.equal(response.status, 200);
    assert.equal(inferenceCalls, 2);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
    assert.ok(getCodexRoutingProbe(response));
    assertTombstone();
    return response;
  };

  try {
    for (const outcome of ["failed", "incomplete", "cancelled"] as const) {
      const response = await fetchPostResetResponse(`owner-post-reset-${outcome}`);
      await releaseCodexResponseProbe(response);
      assert.equal(getCodexRoutingProbe(response), null, outcome);
      assertTombstone();
      assert.equal(routingSlot()?.probe_lease, null, outcome);
    }

    const completed = await fetchPostResetResponse("owner-post-reset-completed");
    await markCodexResponseCompleted(completed);
    assert.equal(getCodexRoutingProbe(completed), null);
    const slot = routingSlot();
    assert.equal(slot?.banked_reset_generation_ambiguous, false);
    assert.equal(slot?.observed_reset_at_ms, null);
    assert.equal(slot?.probe_lease, null);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("simultaneous gateway requests share one durable banked-reset submission", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let signalRedeemEntered!: () => void;
  let releaseRedeem!: () => void;
  const redeemEntered = new Promise<void>((resolve) => {
    signalRedeemEntered = resolve;
  });
  const redeemGate = new Promise<void>((resolve) => {
    releaseRedeem = resolve;
  });
  const reset = scriptedResetProvider({
    onRedeem: signalRedeemEntered,
    redeemGate,
  });
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        }),
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "response-after-concurrent-reset" }), { status: 200 }));
  };

  const bankedReset = {
    config: liveBankedResetConfig(),
    provider: reset.provider,
    kv: kv as unknown as Deno.Kv,
    now: () => fixedStartMs,
    newOwnerToken: () => "owner-concurrent-gateway-reset",
  };
  try {
    const first = fetchCodexResponses(
      { input: "first-concurrent-banked-reset" },
      { requestId: "first-concurrent-banked-reset", bankedReset },
    );
    await redeemEntered;

    // The second request sees the durable `submitted` transaction while its
    // provider call is stalled. It may return the normal quota response but
    // must neither dispatch inference nor submit another reset.
    const second = await fetchCodexResponses(
      { input: "second-concurrent-banked-reset" },
      { requestId: "second-concurrent-banked-reset", bankedReset },
    );
    assert.equal(second.status, 429);
    assert.equal(inferenceCalls, 1);
    assert.deepEqual(reset.calls, ["inventory", "redeem"]);
    assert.equal(reset.idempotencyKeys.length, 1);

    releaseRedeem();
    const firstResponse = await first;
    assert.equal(firstResponse.status, 200);
    assert.equal(inferenceCalls, 2);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
    assert.equal(reset.idempotencyKeys.length, 1);
    assert.equal(new Set(reset.idempotencyKeys).size, 1);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an already-redeemed reset is independently verified before one same-account retry", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider({ redeemKind: "already_redeemed" });
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        }),
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "response-after-already-redeemed" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-already-redeemed" },
      {
        requestId: "banked-reset-already-redeemed",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-already-redeemed",
        },
      },
    );
    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one", "account-one"]);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
    assert.equal(reset.idempotencyKeys.length, 1);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an auth rotation after verification fences off the post-reset retry", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const reset = scriptedResetProvider({
    onVerify: () => {
      kv.auth = pool({
        ...auth("one"),
        access_token: accessToken("one-rotated"),
        refresh_token: "refresh-one-rotated",
      });
      kv.authVersion += 1;
    },
  });
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
      }),
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-auth-rotation" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-auth-rotation",
        },
      },
    );
    assert.equal(response.status, 429);
    assert.equal(inferenceCalls, 1, "a rotated auth-pool entry must prevent the post-reset retry");
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an auth rotation inside the final dispatch hook fences off a post-reset retry", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let inferenceCalls = 0;
  let beforeDispatchCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const reset = scriptedResetProvider();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
      }),
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-dispatch-race" },
      {
        beforeDispatch: () => {
          beforeDispatchCalls += 1;
          if (beforeDispatchCalls === 2) {
            // This occurs after the verified record and routing repair, but
            // before the post-reset transport can mark itself started.
            kv.auth = pool({
              ...auth("one"),
              access_token: accessToken("one-rotated-during-dispatch"),
              refresh_token: "refresh-one-rotated-during-dispatch",
            });
            kv.authVersion += 1;
          }
          return Promise.resolve();
        },
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-dispatch-race",
        },
      },
    );
    assert.equal(response.status, 429);
    assert.equal(beforeDispatchCalls, 2);
    assert.equal(inferenceCalls, 1, "the rotated second attempt must not reach upstream transport");
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an auth-pool slot reorder during a claimed reset fences submission before redemption", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const stableRetryAfter = new Date(fixedStartMs + 60_000).toUTCString();
  const upstreamAccounts: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const reset = scriptedResetProvider({
    onInventory: () => {
      // Both accounts are quota-blocked. The second account is the reset
      // candidate, then an operator reorders the pool while it is claimed.
      kv.auth = pool(auth("two"), auth("one"));
      kv.authVersion += 1;
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    upstreamAccounts.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableRetryAfter },
      }),
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-slot-reorder" },
      {
        bankedReset: {
          config: liveBankedResetConfig("account-two"),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-slot-reorder",
        },
      },
    );
    assert.equal(response.status, 429);
    assert.deepEqual(upstreamAccounts, ["account-one", "account-two"]);
    // The all-blocked evaluator must inspect both account-bound inventories
    // before it can rule out a live spend after the routing reorder.
    assert.deepEqual(reset.calls, ["inventory", "inventory"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("the request that first discovers a healthy fallback does not spend before a fresh cohort read", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(
      accountIds.length === 1
        ? new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        })
        : new Response(JSON.stringify({ id: "fallback-success" }), { status: 200 }),
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-fallback" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-fallback",
        },
      },
    );
    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
    assert.deepEqual(reset.calls, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a sibling blocked during partial preflight is not dispatched from the stale snapshot", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  const warmed = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, fixedStartMs);
  assert.equal(warmed.kind, "eligible");
  assert.deepEqual(
    warmed.kind === "eligible" ? warmed.accounts.map((account) => account.auth.account_id) : [],
    ["account-two"],
  );
  const reset = scriptedResetProvider({
    onInventory: () => {
      // Simulate a different isolate writing the durable record directly. The
      // local routing module's five-second cache intentionally remains stale.
      const routingKey = JSON.stringify(CODEX_ACCOUNT_ROUTING_KV_KEY);
      const entry = kv.extra.get(routingKey);
      assert.ok(entry);
      const state = structuredClone(parseCodexAccountRoutingState(entry.value));
      assert.ok(state);
      if (!state) throw new Error("expected durable routing state");
      const sibling = state.slots[1];
      assert.ok(sibling);
      if (!sibling) throw new Error("expected sibling routing slot");
      const retryAtMs = Date.parse(stableBankedResetRetryAfter);
      const slots = [...state.slots];
      slots[1] = {
        ...sibling,
        quota_blocked_until_ms: retryAtMs,
        quota_block_source: "header_retry_after",
        observed_reset_at_ms: retryAtMs,
        observed_reset_at_is_stable: true,
        banked_reset_generation_ambiguous: false,
        generation: sibling.generation + 1,
        probe_lease: null,
      };
      kv.extra.set(routingKey, {
        value: { ...state, updated_at_ms: fixedStartMs, slots },
        version: entry.version + 1,
      });
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response(JSON.stringify({ id: "stale-sibling-fallback" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "partial-preflight-sibling-blocked" },
      {
        requestId: "partial-preflight-sibling-blocked",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-partial-preflight-sibling-blocked",
        },
      },
    );
    assert.equal(response.status, 429);
    assert.equal((await response.json()).error.code, "codex_quota_blocked");
    assert.deepEqual(reset.inventoryAccountIds, ["account-one"]);
    assert.deepEqual(reset.redeemAccountIds, []);
    assert.deepEqual(reset.calls, ["inventory"]);
    assert.deepEqual(accountIds, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a routing KV outage after partial preflight never dispatches a cached fallback", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  const warmed = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, fixedStartMs);
  assert.equal(warmed.kind, "eligible");
  const reset = scriptedResetProvider({
    onInventory: () => {
      kv.onRoutingRead = () => {
        throw new Error("routing KV unavailable");
      };
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response(JSON.stringify({ id: "cached-sibling-fallback" }), { status: 200 }));
  };

  try {
    await assert.rejects(
      () =>
        fetchCodexResponses(
          { input: "partial-preflight-routing-unavailable" },
          {
            requestId: "partial-preflight-routing-unavailable",
            bankedReset: {
              config: liveBankedResetConfig(),
              provider: reset.provider,
              kv: kv as unknown as Deno.Kv,
              now: () => fixedStartMs,
              newOwnerToken: () => "owner-partial-preflight-routing-unavailable",
            },
          },
        ),
      (error: unknown) =>
        error instanceof CodexError &&
        error.status === 503 &&
        error.message === "Codex routing state is unavailable after banked-reset preflight.",
    );
    assert.deepEqual(reset.inventoryAccountIds, ["account-one"]);
    assert.deepEqual(reset.redeemAccountIds, []);
    assert.deepEqual(reset.calls, ["inventory"]);
    assert.deepEqual(accountIds, []);
  } finally {
    kv.onRoutingRead = null;
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("partial preflight reselects a rotated healthy sibling before ordinary fallback", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const authorizations: string[] = [];
  const rotated = {
    ...auth("two"),
    access_token: accessToken("two-rotated"),
    refresh_token: "refresh-two-rotated",
  };
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  const reset = scriptedResetProvider({
    onInventory: async () => {
      await kv.set(AUTH_KEY, pool(auth("one"), rotated));
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    authorizations.push(request.headers.get("authorization") ?? "");
    return Promise.resolve(new Response(JSON.stringify({ id: "rotated-sibling-fallback" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "partial-preflight-sibling-rotated" },
      {
        requestId: "partial-preflight-sibling-rotated",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-partial-preflight-sibling-rotated",
        },
      },
    );
    assert.equal(response.status, 200);
    assert.deepEqual(reset.inventoryAccountIds, ["account-one"]);
    assert.deepEqual(reset.redeemAccountIds, []);
    assert.deepEqual(reset.calls, ["inventory"]);
    assert.deepEqual(accountIds, ["account-two"]);
    assert.deepEqual(authorizations, [`Bearer ${rotated.access_token}`]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("partial preflight reselects a reordered healthy sibling before ordinary fallback", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  const reset = scriptedResetProvider({
    onInventory: async () => {
      await kv.set(AUTH_KEY, pool(auth("two"), auth("one")));
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response(JSON.stringify({ id: "reordered-sibling-fallback" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "partial-preflight-sibling-reordered" },
      {
        requestId: "partial-preflight-sibling-reordered",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-partial-preflight-sibling-reordered",
        },
      },
    );
    assert.equal(response.status, 200);
    assert.deepEqual(reset.inventoryAccountIds, ["account-one"]);
    assert.deepEqual(reset.redeemAccountIds, []);
    assert.deepEqual(reset.calls, ["inventory"]);
    assert.deepEqual(accountIds, ["account-two"]);
    assert.equal(getCodexResponseSlot(response), 1);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("definitive partial-cohort probe failures fall through once to the healthy sibling", async (t) => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;

  try {
    for (const status of [401, 403, 429]) {
      await t.step(String(status), async () => {
        const accountIds: string[] = [];
        const reset = scriptedResetProvider();
        kv.auth = pool(auth("one"), auth("two"));
        kv.extra.clear();
        resetCodexAuthCacheForTest();
        resetCodexAccountRoutingForTest();
        await seedStableBankedResetBlock();
        globalThis.fetch = (input, init) => {
          const request = new Request(input, init);
          const accountId = request.headers.get("chatgpt-account-id") ?? "";
          accountIds.push(accountId);
          if (accountId === "account-two") {
            return Promise.resolve(new Response(JSON.stringify({ id: "healthy-fallback" }), { status: 200 }));
          }
          const headers = new Headers({ "Content-Type": "application/json" });
          if (status === 429) headers.set("Retry-After", stableBankedResetRetryAfter);
          return Promise.resolve(
            new Response(
              JSON.stringify({
                error: {
                  type: status === 429 ? "usage_limit_reached" : status === 401 ? "authentication_error" : "forbidden",
                },
              }),
              { status, headers },
            ),
          );
        };

        const response = await fetchCodexResponses(
          { input: `partial-probe-${status}` },
          {
            requestId: `partial-probe-${status}`,
            bankedReset: {
              config: liveBankedResetConfig(),
              provider: reset.provider,
              kv: kv as unknown as Deno.Kv,
              now: () => fixedStartMs,
              newOwnerToken: () => `owner-partial-probe-${status}`,
            },
          },
        );

        assert.equal(response.status, 200);
        assert.deepEqual(accountIds, ["account-one", "account-two"]);
        assert.deepEqual(reset.inventoryAccountIds, ["account-one"]);
        assert.deepEqual(reset.redeemAccountIds, ["account-one"]);
        assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
      });
    }
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an ambiguous partial-cohort post-reset transport outcome never replays on the healthy sibling", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.reject(new DOMException("post-reset deadline elapsed", "TimeoutError"));
  };

  try {
    await assert.rejects(() =>
      fetchCodexResponses(
        { input: "partial-probe-transport-ambiguous" },
        {
          requestId: "partial-probe-transport-ambiguous",
          bankedReset: {
            config: liveBankedResetConfig(),
            provider: reset.provider,
            kv: kv as unknown as Deno.Kv,
            now: () => fixedStartMs,
            newOwnerToken: () => "owner-partial-probe-transport-ambiguous",
          },
        },
      )
    );
    assert.deepEqual(accountIds, ["account-one"]);
    assert.deepEqual(reset.inventoryAccountIds, ["account-one"]);
    assert.deepEqual(reset.redeemAccountIds, ["account-one"]);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("simultaneous partial-cohort requests share one consume and let the contender use the healthy sibling", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let signalRedeemEntered!: () => void;
  let releaseRedeem!: () => void;
  const redeemEntered = new Promise<void>((resolve) => {
    signalRedeemEntered = resolve;
  });
  const redeemGate = new Promise<void>((resolve) => {
    releaseRedeem = resolve;
  });
  const reset = scriptedResetProvider({ onRedeem: signalRedeemEntered, redeemGate });
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    accountIds.push(accountId);
    return Promise.resolve(new Response(JSON.stringify({ id: `response-${accountId}` }), { status: 200 }));
  };
  const bankedReset = {
    config: liveBankedResetConfig(),
    provider: reset.provider,
    kv: kv as unknown as Deno.Kv,
    now: () => fixedStartMs,
    newOwnerToken: () => "owner-partial-concurrent",
  };

  try {
    const first = fetchCodexResponses(
      { input: "partial-concurrent-first" },
      { requestId: "partial-concurrent-first", bankedReset },
    );
    await redeemEntered;

    const second = await fetchCodexResponses(
      { input: "partial-concurrent-second" },
      { requestId: "partial-concurrent-second", bankedReset },
    );
    assert.equal(second.status, 200);
    assert.deepEqual(accountIds, ["account-two"]);
    assert.deepEqual(reset.calls, ["inventory", "redeem"]);
    assert.equal(reset.idempotencyKeys.length, 1);

    releaseRedeem();
    const firstResponse = await first;
    assert.equal(firstResponse.status, 200);
    assert.deepEqual(accountIds, ["account-two", "account-one"]);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
    assert.equal(reset.idempotencyKeys.length, 1);
    assert.deepEqual(reset.redeemAccountIds, ["account-one"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a skipped half-open probe prevents a sibling banked-reset redemption", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  let now = fixedStartMs;
  Date.now = () => now;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  kv.onRoutingRead = null;
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();

  try {
    const initial = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, now);
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    const second = initial.accounts.find((account) => account.auth.account_id === "account-two");
    assert.ok(second);
    if (!second) return;
    const expiredAtMs = now + 1_000;
    await markCodexQuotaBlocked(
      second,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(expiredAtMs).toUTCString() },
      }),
      now,
    );
    now = expiredAtMs + 1;

    resetCodexAccountRoutingForTest();
    const halfOpen = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, now);
    assert.equal(halfOpen.kind, "eligible");
    if (halfOpen.kind !== "eligible") return;
    const foreignProbeCandidate = halfOpen.accounts.find((account) => account.auth.account_id === "account-two");
    assert.ok(foreignProbeCandidate?.probeRequired);
    if (!foreignProbeCandidate) return;

    // Force a fresh selection, then have another isolate claim the half-open
    // slot immediately after that selection and before this request reaches it.
    resetCodexAccountRoutingForTest();
    const routingReadsBeforeFetch = kv.routingReads;
    let foreignProbeClaimed = false;
    kv.onRoutingRead = async (routingRead) => {
      if (routingRead !== routingReadsBeforeFetch + 2) return;
      kv.onRoutingRead = null;
      const claimed = await claimCodexRoutingProbe(kv.auth, foreignProbeCandidate, now);
      assert.ok(claimed);
      foreignProbeClaimed = true;
    };
    const candidateRetryAfter = new Date(now + 60_000).toUTCString();
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": candidateRetryAfter },
        }),
      );
    };

    const response = await fetchCodexResponses(
      { input: "banked-reset-probe-unavailable" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => now,
          newOwnerToken: () => "owner-probe-unavailable",
        },
      },
    );

    assert.equal(response.status, 429);
    assert.equal(foreignProbeClaimed, true);
    assert.deepEqual(accountIds, ["account-one"]);
    assert.deepEqual(reset.calls, []);
  } finally {
    kv.onRoutingRead = null;
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a 403 sibling blocks a full-pool banked reset", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    accountIds.push(accountId);
    if (accountIds.length === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        }),
      );
    }
    if (accountIds.length === 2) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "forbidden" } }), {
          status: 403,
          headers: { "Content-Type": "application/json" },
        }),
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "response-after-reset" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-after-403" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-after-403",
        },
      },
    );

    assert.equal(response.status, 403);
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
    assert.deepEqual(reset.calls, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an earlier allowlisted exhausted account is redeemed after a later sibling also exhausts quota", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length <= 2) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        }),
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "response-after-earlier-reset" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-earlier-allowlisted" },
      {
        bankedReset: {
          config: liveBankedResetConfig("account-one"),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-earlier-allowlisted",
        },
      },
    );

    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one", "account-two", "account-one"]);
    assert.deepEqual(reset.redeemAccountIds, ["account-one"]);
    assert.deepEqual(reset.calls, ["inventory", "inventory", "redeem", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a 403 during the bounded retry blocks a full-pool redemption", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  const shortStableRetryAfter = new Date(fixedStartMs + 2_000).toUTCString();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    accountIds.push(accountId);
    if (accountIds.length <= 2) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": shortStableRetryAfter },
        }),
      );
    }
    if (accountIds.length === 3) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "forbidden" } }), {
          status: 403,
          headers: { "Content-Type": "application/json" },
        }),
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "response-after-reset" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-bounded-retry-403" },
      {
        retrySleep: () => Promise.resolve(),
        bankedReset: {
          config: liveBankedResetConfig("account-two"),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-bounded-retry-403",
        },
      },
    );

    assert.equal(response.status, 403);
    assert.deepEqual(accountIds, ["account-one", "account-two", "account-one"]);
    assert.deepEqual(reset.calls, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a successful ordinary bounded retry never spends a banked reset", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const reset = scriptedResetProvider();
  const shortStableRetryAfter = new Date(fixedStartMs + 2_000).toUTCString();
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": shortStableRetryAfter },
        }),
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "ordinary-retry-success" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "ordinary-retry-success" },
      {
        retrySleep: () => Promise.resolve(),
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-ordinary-retry-success",
        },
      },
    );

    assert.equal(response.status, 200);
    assert.equal(inferenceCalls, 2, "the ordinary retry is the only successful inference request");
    assert.deepEqual(reset.calls, [], "a served request must not read inventory or submit a reset");
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("only a complete stable usage-limit response can reach the banked-reset provider", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const stableRetryAfter = new Date(fixedStartMs + 60_000).toUTCString();
  const cases: Array<
    Readonly<{
      name: string;
      status: number;
      body: string;
      retryAfter?: string;
      expectedStatus: number;
    }>
  > = [
    {
      name: "generic rate limit",
      status: 429,
      body: JSON.stringify({ error: { type: "rate_limit_error" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "burst throttling",
      status: 429,
      body: JSON.stringify({ error: { type: "requests_per_minute" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "overload",
      status: 429,
      body: JSON.stringify({ error: { type: "server_error" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "policy error",
      status: 429,
      body: JSON.stringify({ error: { type: "policy_error" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "invalid request",
      status: 429,
      body: JSON.stringify({ error: { type: "invalid_request_error" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "unknown future error type",
      status: 429,
      body: JSON.stringify({ error: { type: "future_quota_signal" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "malformed body",
      status: 429,
      body: "{not JSON",
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "relative retry-after cannot name a reset window",
      status: 429,
      body: JSON.stringify({ error: { type: "usage_limit_reached" } }),
      retryAfter: "60",
      expectedStatus: 429,
    },
    {
      name: "invalid decimal retry-after",
      status: 429,
      body: JSON.stringify({ error: { type: "usage_limit_reached" } }),
      retryAfter: "0.5",
      expectedStatus: 429,
    },
    {
      name: "expired retry-after",
      status: 429,
      body: JSON.stringify({ error: { type: "usage_limit_reached" } }),
      retryAfter: new Date(fixedStartMs - 1_000).toUTCString(),
      expectedStatus: 429,
    },
    {
      name: "overflowing retry-after",
      status: 429,
      body: JSON.stringify({ error: { type: "usage_limit_reached" } }),
      retryAfter: "999999999999999999999999999999999999",
      expectedStatus: 429,
    },
    {
      name: "401",
      status: 401,
      body: JSON.stringify({ error: { type: "invalid_auth" } }),
      expectedStatus: 401,
    },
    {
      name: "403",
      status: 403,
      body: JSON.stringify({ error: { type: "forbidden" } }),
      expectedStatus: 403,
    },
  ];

  try {
    Date.now = () => fixedStartMs;
    (config as { isDeploy: boolean }).isDeploy = true;
    for (const testCase of cases) {
      kv.auth = pool(auth("one"));
      kv.extra.clear();
      resetCodexAuthCacheForTest();
      resetCodexAccountRoutingForTest();
      const reset = scriptedResetProvider();
      globalThis.fetch = (input) => {
        const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
        if (url.includes("oauth/token")) {
          return Promise.resolve(new Response(JSON.stringify({ error: "invalid_grant" }), { status: 401 }));
        }
        const headers = new Headers({ "Content-Type": "application/json" });
        if (testCase.retryAfter) headers.set("Retry-After", testCase.retryAfter);
        return Promise.resolve(new Response(testCase.body, { status: testCase.status, headers }));
      };

      const response = await fetchCodexResponses(
        { input: `nonqualifying-${testCase.name}` },
        {
          retrySleep: async () => {},
          bankedReset: {
            config: liveBankedResetConfig(),
            provider: reset.provider,
            kv: kv as unknown as Deno.Kv,
            now: () => fixedStartMs,
            newOwnerToken: () => `owner-nonqualifying-${testCase.name}`,
          },
        },
      );
      assert.equal(response.status, testCase.expectedStatus, testCase.name);
      assert.deepEqual(reset.calls, [], testCase.name);
    }
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a later non-qualifying 429 clears an earlier banked-reset candidate", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const reset = scriptedResetProvider();
  const stableRetryAfter = new Date(fixedStartMs + 60_000).toUTCString();
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    accountIds.push(accountId);
    const errorType = accountIds.length === 1 ? "usage_limit_reached" : "rate_limit_error";
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: errorType } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableRetryAfter },
      }),
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "later-nonqualifying-429" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-later-nonqualifying-429",
        },
      },
    );
    assert.equal(response.status, 429);
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
    assert.deepEqual(reset.calls, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("post-reset inference may return one normal 429 but never triggers a second redemption", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const reset = scriptedResetProvider();
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
      }),
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-second-429" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-second-429",
        },
      },
    );
    assert.equal(response.status, 429);
    assert.equal(inferenceCalls, 2);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("all-blocked routing recovers an unknown reset while new submissions are disabled", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const reset = scriptedResetProvider({ redeemKind: "unknown" });
  let now = fixedStartMs;
  let live = true;
  let inferenceCalls = 0;
  Date.now = () => now;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(
      inferenceCalls === 1
        ? new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        })
        : new Response(JSON.stringify({ id: "recovered-after-unknown" }), { status: 200 }),
    );
  };

  const liveConfig = liveBankedResetConfig();
  const disabledConfig: CodexBankedResetConfig = { ...liveConfig, enabled: false, mode: "disabled" };
  const bankedReset = {
    config: liveConfig,
    reloadConfig: () => live ? liveConfig : disabledConfig,
    provider: reset.provider,
    kv: kv as unknown as Deno.Kv,
    now: () => now,
    newOwnerToken: () => "owner-recovery",
  };
  try {
    const first = await fetchCodexResponses({ input: "unknown-reset" }, { bankedReset });
    assert.equal(first.status, 429);
    assert.deepEqual(reset.calls, ["inventory", "redeem"]);

    // Let the durable unknown record's lease expire while retaining its
    // original 60-second routing fence, then simulate an operator rollback.
    now += CODEX_BANKED_RESET_LEASE_MS + 1;
    live = false;
    const recovered = await fetchCodexResponses({ input: "recover-reset" }, { bankedReset });
    assert.equal(recovered.status, 200);
    assert.equal(inferenceCalls, 2);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "lookup", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});
