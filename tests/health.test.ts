import assert from "node:assert/strict";
import { config } from "../src/config.ts";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);

let resetAuthCache = (): void => {};
class HealthKvStore extends Map<string, unknown> {
  override clear(): void {
    super.clear();
    resetAuthCache();
  }
}
const kvStore = new HealthKvStore();

const kvStub = {
  get: (key: Deno.KvKey) =>
    Promise.resolve(({ key, value: kvStore.get(keyToString(key)) ?? null }) as Deno.KvEntryMaybe<unknown>),
  set: (key: Deno.KvKey, value: unknown) => {
    kvStore.set(keyToString(key), value);
    return Promise.resolve({ ok: true } as const);
  },
  delete: (key: Deno.KvKey) => {
    kvStore.delete(keyToString(key));
    return Promise.resolve();
  },
  list: async function* (_selector: Deno.KvListSelector, _options?: Deno.KvListOptions) {
    yield* [];
  },
  atomic: () => {
    const ops: Array<{ type: "set" | "delete"; key: Deno.KvKey; value?: unknown }> = [];
    const chain = {
      check: () => chain,
      set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
        ops.push({ type: "set", key, value });
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        ops.push({ type: "delete", key });
        return chain;
      },
      commit: () => {
        for (const op of ops) {
          if (op.type === "set") kvStore.set(keyToString(op.key), op.value);
          else kvStore.delete(keyToString(op.key));
        }
        return Promise.resolve({ ok: true } as const);
      },
    };
    return chain;
  },
  close: () => {},
} as unknown as Deno.Kv;

(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kvStub);

const { handleHealth, handleHealthAuth, handleHealthUpstream } = await import("../src/health.ts");
const { resetCodexAuthCacheForTest } = await import("../src/codex.ts");
resetAuthCache = resetCodexAuthCacheForTest;

const base64Url = (value: string): string => btoa(value).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");

const makeJwt = (expSeconds: number | null): string => {
  const header = base64Url(JSON.stringify({ alg: "none", typ: "JWT" }));
  const payload = base64Url(JSON.stringify(expSeconds === null ? {} : { exp: expSeconds }));
  return `${header}.${payload}.`;
};

const makeAuthEntry = (accessTokenExpSeconds: number | null): {
  access_token: string;
  refresh_token: string;
  account_id: string;
  updated_at_ms: number;
} => ({
  access_token: makeJwt(accessTokenExpSeconds),
  refresh_token: "refresh",
  account_id: "acct",
  updated_at_ms: Date.now(),
});

const CODEX_AUTH_KEY: Deno.KvKey = ["ubq_ai", "codex_auth"];

Deno.test("health readiness is healthy when Codex auth config exists and upstream probe succeeds", async () => {
  kvStore.clear();
  kvStore.set(keyToString(CODEX_AUTH_KEY), makeAuthEntry(Math.floor(Date.now() / 1000) + 3600));

  const originalFetch = globalThis.fetch;
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(JSON.stringify({ models: [{ slug: "gpt-5.5" }] }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
    );

  try {
    const response = await handleHealth();
    const payload = await response.json() as {
      ok?: boolean;
      status?: number;
      upstream?: string;
      problems?: string[];
      auth?: { source?: string; access_token_expired?: boolean | null; refresh_recommended?: boolean | null };
    };

    assert.equal(response.status, 200);
    assert.equal(payload.ok, true);
    assert.equal(payload.upstream, "chatgpt_codex");
    assert.equal(payload.status, 200);
    assert.equal(payload.problems?.length, 0);
    assert.equal(payload.auth?.source, "kv");
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("health readiness is unavailable without configured Codex auth", async () => {
  kvStore.clear();
  const originalFetch = globalThis.fetch;
  const originalConfigAuth = config.codexAuthJsonB64;

  globalThis.fetch = () => {
    throw new Error("/health should not contact upstream auth when auth is missing");
  };

  try {
    (config as { codexAuthJsonB64: string }).codexAuthJsonB64 = "";
    const response = await handleHealth();
    const payload = await response.json() as { ok?: boolean; status?: number; problems?: string[] };
    assert.equal(response.status, 503);
    assert.equal(payload.ok, false);
    assert.equal(payload.status, 503);
    assert.ok(payload.problems?.some((problem) => problem.includes("No Codex auth configured")));
  } finally {
    (config as { codexAuthJsonB64: string }).codexAuthJsonB64 = originalConfigAuth;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("health readiness reports 503 when Codex upstream refresh fails (401 upstream auth flow)", async () => {
  kvStore.clear();
  kvStore.set(keyToString(CODEX_AUTH_KEY), makeAuthEntry(Math.floor(Date.now() / 1000) - 10_000));

  const originalFetch = globalThis.fetch;
  const originalDeployFlag = (config as { isDeploy: boolean }).isDeploy;
  const originalConfigAuth = config.codexAuthJsonB64;
  (config as { isDeploy: boolean }).isDeploy = true;
  globalThis.fetch = (input: RequestInfo | URL) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    if (url.includes("auth.openai.com/oauth/token")) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: "invalid_grant" }), {
          status: 401,
          headers: { "Content-Type": "application/json" },
        }),
      );
    }
    return Promise.resolve(new Response("upstream", { status: 200, headers: { "Content-Type": "text/plain" } }));
  };

  try {
    (config as { codexAuthJsonB64: string }).codexAuthJsonB64 = "";
    const response = await handleHealth();
    const payload = await response.json() as {
      ok?: boolean;
      status?: number;
      error?: string;
      details?: string;
      upstream?: string;
    };
    assert.equal(response.status, 503);
    assert.equal(payload.ok, false);
    assert.equal(payload.status, 503);
    assert.equal(payload.error, "Upstream fetch failed");
    assert.equal(payload.upstream, "chatgpt_codex");
    assert.match(payload.details ?? "", /Codex auth refresh failed/);
  } finally {
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
    (config as { codexAuthJsonB64: string }).codexAuthJsonB64 = originalConfigAuth;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("/health and /health/upstream share upstream semantics", async () => {
  kvStore.clear();
  kvStore.set(keyToString(CODEX_AUTH_KEY), makeAuthEntry(Math.floor(Date.now() / 1000) + 3600));

  const originalFetch = globalThis.fetch;
  globalThis.fetch = () =>
    Promise.resolve(
      new Response("temporary error", {
        status: 503,
        headers: { "Content-Type": "text/plain" },
      }),
    );

  try {
    const readiness = await handleHealth();
    const upstream = await handleHealthUpstream();

    const readinessPayload = await readiness.json() as Record<string, unknown>;
    const upstreamPayload = await upstream.json() as Record<string, unknown>;

    assert.equal(readiness.status, 503);
    assert.equal(upstream.status, 503);
    assert.equal(readinessPayload.ok, false);
    assert.equal(upstreamPayload.ok, false);
    assert.equal(readinessPayload.upstream, upstreamPayload.upstream);
    assert.equal(readinessPayload.status, upstreamPayload.status);
    assert.equal(readinessPayload.content_type, upstreamPayload.content_type);
    assert.equal(readinessPayload.error, upstreamPayload.error);
    assert.equal(readinessPayload.details, upstreamPayload.details);
    assert.equal(
      (readinessPayload.auth as { source?: string } | undefined)?.source,
      (upstreamPayload.auth as {
        source?: string;
      } | undefined)?.source,
    );
    assert.ok(Array.isArray(readinessPayload.problems));
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("health auth summary remains passive and does not refresh upstream auth", async () => {
  kvStore.clear();
  kvStore.set(keyToString(CODEX_AUTH_KEY), {
    access_token: makeJwt(Math.floor(Date.now() / 1000) + 3600),
    refresh_token: "refresh",
    account_id: "acct",
    updated_at_ms: Date.now(),
  });

  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => {
    throw new Error("/health/auth should not contact upstream auth");
  };

  try {
    const response = await handleHealthAuth();
    assert.equal(response.status, 200);
    const payload = await response.json() as { upstream?: string; auth?: { source?: string } };
    assert.equal(payload.upstream, "chatgpt_codex");
    assert.equal(payload.auth?.source, "kv");
  } finally {
    globalThis.fetch = originalFetch;
  }
});
