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
  fetchCodexResponses,
  orderCodexAuthAccounts,
  resetCodexAuthCacheForTest,
} = await import("../src/codex.ts");

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
    assert.equal(accountIds.length, 2);
    assert.equal(new Set(accountIds).size, 2);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses retry the other account when a 401 cannot refresh", async () => {
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
    if (request.url.includes("auth.openai.com/oauth/token")) {
      return Promise.resolve(new Response('{"error":"invalid_grant"}', { status: 401 }));
    }
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response("{}", { status: accountIds.length === 1 ? 401 : 200 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "auth-failover" });
    assert.equal(response.status, 200);
    assert.equal(accountIds.length, 2);
    assert.equal(new Set(accountIds).size, 2);
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
