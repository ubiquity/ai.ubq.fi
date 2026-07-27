import assert from "node:assert/strict";
import { config } from "../src/config.ts";
import { isHealthAvailable, refreshHealthBadge } from "../static/app.js";

const kvStore = new Map<string, unknown>();

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

const { handleHealth, handleHealthProviders, handleHealthUpstream } = await import("../src/health.ts");
const { default: handler } = await import("../src/handler.ts");

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);

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

const setConfigCodexAuth = (accessTokenExpSeconds: number | null): string => {
  const payload = {
    tokens: {
      access_token: makeJwt(accessTokenExpSeconds),
      refresh_token: "refresh",
      account_id: "acct",
    },
  };
  return btoa(JSON.stringify(payload));
};

Deno.test("homepage logic renders OK for 200 status with available", () => {
  const response = { ok: true } as Response;
  assert.equal(isHealthAvailable(response, { status: "available" }), true);
});

Deno.test("homepage logic renders Degraded for legacy ok=true payload", () => {
  const response = { ok: true } as Response;
  assert.equal(isHealthAvailable(response, { ok: true }), false);
});

Deno.test("homepage logic renders Degraded for non-2xx responses", () => {
  const response = { ok: false } as Response;
  assert.equal(isHealthAvailable(response, { status: "available" }), false);
});

Deno.test("homepage logic renders Degraded for malformed payload", () => {
  const response = { ok: true } as Response;
  assert.equal(isHealthAvailable(response, null), false);
  assert.equal(isHealthAvailable(response, "not-json"), false);
});

Deno.test("homepage logic renders Offline for rejected fetch", async () => {
  const badge: { dataset: { state?: string }; textContent: string } = {
    dataset: {},
    textContent: "Checking...",
  };
  await refreshHealthBadge(() => Promise.reject(new Error("offline")), badge);
  assert.equal(badge.dataset.state, "bad");
  assert.equal(badge.textContent, "Offline");
});

Deno.test("health readiness is healthy when Codex auth config exists and upstream probe succeeds", async () => {
  kvStore.clear();
  const originalConfigAuth = config.codexAuthJsonB64;
  const originalConfigDeploy = config.isDeploy;
  (config as { codexAuthJsonB64: string }).codexAuthJsonB64 = setConfigCodexAuth(Math.floor(Date.now() / 1000) + 3600);
  (config as { isDeploy: boolean }).isDeploy = true;

  const originalFetch = globalThis.fetch;
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(JSON.stringify({ models: [{ slug: "gpt-5.5" }] }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
    );

  try {
    const response = await handler(new Request("http://example.local/health"));
    const payload = await response.json() as { status?: string; auth?: { source?: string } };

    assert.equal(response.status, 200);
    assert.equal(payload.status, "available");
    assert.equal(payload.auth, undefined);
  } finally {
    (config as { codexAuthJsonB64: string }).codexAuthJsonB64 = originalConfigAuth;
    (config as { isDeploy: boolean }).isDeploy = originalConfigDeploy;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("health readiness is degraded without configured Codex auth", async () => {
  kvStore.clear();
  const originalFetch = globalThis.fetch;
  const originalConfigAuth = config.codexAuthJsonB64;

  globalThis.fetch = () => {
    throw new Error("/health should not contact upstream auth when auth is missing");
  };

  try {
    (config as { codexAuthJsonB64: string }).codexAuthJsonB64 = "";
    const response = await handleHealth();
    const payload = await response.json() as { status?: string };
    assert.equal(response.status, 503);
    assert.equal(payload.status, "degraded");
  } finally {
    (config as { codexAuthJsonB64: string }).codexAuthJsonB64 = originalConfigAuth;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("health upstream reports 503 when Codex upstream refresh fails (401 upstream auth flow)", async () => {
  kvStore.clear();
  const originalConfigAuth = config.codexAuthJsonB64;
  const originalConfigDeploy = config.isDeploy;
  (config as { codexAuthJsonB64: string }).codexAuthJsonB64 = setConfigCodexAuth(
    Math.floor(Date.now() / 1000) - 10_000,
  );
  (config as { isDeploy: boolean }).isDeploy = true;

  const originalFetch = globalThis.fetch;
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
    if (url.includes("/codex/models")) {
      return Promise.resolve(new Response("unauthorized", { status: 401, headers: { "Content-Type": "text/plain" } }));
    }
    return Promise.resolve(new Response("upstream", { status: 503, headers: { "Content-Type": "text/plain" } }));
  };

  try {
    const response = await handleHealthUpstream();
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
    (config as { isDeploy: boolean }).isDeploy = originalConfigDeploy;
    (config as { codexAuthJsonB64: string }).codexAuthJsonB64 = originalConfigAuth;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("/health and /health/upstream share health semantics by probe outcome", async () => {
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
    const providers = await handleHealthProviders();

    const readinessPayload = await readiness.json() as Record<string, unknown>;
    const upstreamPayload = await upstream.json() as Record<string, unknown>;
    const providersPayload = await providers.json() as Record<string, unknown>;

    assert.equal(readiness.status, 503);
    assert.equal(upstream.status, 503);
    assert.equal(providers.status, 503);
    assert.equal(readinessPayload.status, "degraded");
    assert.equal(upstreamPayload.ok, false);
    assert.equal(providersPayload.ok, false);
    assert.equal(upstreamPayload.status, 503);
    assert.equal(providersPayload.status, 503);
    assert.equal(upstreamPayload.problems, providersPayload.problems);
    assert.equal(readinessPayload.status, "degraded");
    assert.equal(upstreamPayload.problems, providersPayload.problems);
    assert.equal(upstreamPayload.content_type, providersPayload.content_type);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("/health/auth is intentionally unsupported", async () => {
  const response = await handler(new Request("http://example.local/health/auth"));
  assert.equal(response.status, 404);
  const payload = await response.json().catch(() => null) as {
    error?: { code?: string };
  } | null;
  assert.equal(payload?.error?.code, "not_found");
});

Deno.test("supported health routes keep intended liveness and diagnostic behavior", async () => {
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
    const [health, providers, upstream] = await Promise.all([
      handler(new Request("http://example.local/health")),
      handler(new Request("http://example.local/health/providers")),
      handler(new Request("http://example.local/health/upstream")),
    ]);

    const healthPayload = await health.json() as { status?: string };
    const providersPayload = await providers.json() as { ok?: boolean; status?: number };
    const upstreamPayload = await upstream.json() as { ok?: boolean; status?: number };

    assert.equal(health.status, 200);
    assert.equal(providers.status, 200);
    assert.equal(upstream.status, 200);
    assert.equal(healthPayload.status, "available");
    assert.equal(providersPayload.ok, true);
    assert.equal(upstreamPayload.ok, true);
    assert.equal(providersPayload.status, 200);
    assert.equal(upstreamPayload.status, 200);
  } finally {
    globalThis.fetch = originalFetch;
  }
});
