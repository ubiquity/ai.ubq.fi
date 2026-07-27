import assert from "node:assert/strict";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);

let resetAuthCache = (): void => {};
class HealthKvStore extends Map<string, unknown> {
  override clear(): void {
    super.clear();
    resetAuthCache();
  }
}
const kvStore = new HealthKvStore();
let atomicCommitCount = 0;

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
        atomicCommitCount += 1;
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

const {
  handleHealth,
  handleHealthProviders,
  handleHealthUpstream,
  setActiveUpstreamHealthTimeoutMsForTest,
} = await import(
  "../src/health.ts"
);
const { default: handler } = await import("../src/handler.ts");
const { resetCodexAuthCacheForTest } = await import("../src/codex.ts");
const {
  getCodexProviderHealth,
  recordCodexProviderHealth,
  recordYunwuProviderHealth,
  resetProviderHealthThrottleForTest,
} = await import("../src/provider_health.ts");
resetAuthCache = resetCodexAuthCacheForTest;

const base64Url = (value: string): string => btoa(value).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");

const makeJwt = (expSeconds: number | null): string => {
  const header = base64Url(JSON.stringify({ alg: "none", typ: "JWT" }));
  const payload = base64Url(JSON.stringify(expSeconds === null ? {} : { exp: expSeconds }));
  return `${header}.${payload}.`;
};

const makeAuthEntry = (accessTokenExpSeconds: number | null, accountId = "acct"): {
  access_token: string;
  refresh_token: string;
  account_id: string;
  updated_at_ms: number;
} => ({
  access_token: makeJwt(accessTokenExpSeconds),
  refresh_token: "refresh",
  account_id: accountId,
  updated_at_ms: Date.now(),
});
const makeAuthPool = (...accounts: ReturnType<typeof makeAuthEntry>[]) => ({
  accounts,
  updated_at_ms: Date.now(),
});

const CODEX_AUTH_KEY: Deno.KvKey = ["ubq_ai", "codex_auth"];

Deno.test("passive provider health returns every Codex slot without contacting upstream", async () => {
  kvStore.clear();
  resetProviderHealthThrottleForTest();
  const future = Math.floor(Date.now() / 1000) + 3600;
  kvStore.set(
    keyToString(CODEX_AUTH_KEY),
    makeAuthPool(makeAuthEntry(future, "private-account-a"), makeAuthEntry(future, "private-account-b")),
  );
  await recordCodexProviderHealth("private-account-a", "quota_exhausted", 429, () => 1_000);
  await recordCodexProviderHealth("private-account-b", "refresh_failed", 401, () => 2_000);
  await recordYunwuProviderHealth("success", 200, () => 3_000);

  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => {
    throw new Error("/health/providers must not contact any upstream");
  };
  try {
    const response = await handleHealthProviders();
    const text = await response.text();
    const payload = JSON.parse(text) as {
      mode?: string;
      codex?: {
        account_count?: number;
        state?: string;
        accounts?: Array<{ slot?: number; health?: { state?: string; last_status?: number | null } }>;
      };
      yunwu?: { health?: { state?: string; last_status?: number | null }; quota?: { balance_credits?: unknown } };
    };

    assert.equal(response.status, 200);
    assert.equal(payload.mode, "passive");
    assert.equal(payload.codex?.account_count, 2);
    assert.equal(payload.codex?.state, "degraded");
    assert.deepEqual(
      payload.codex?.accounts?.map((account) => [account.slot, account.health?.state, account.health?.last_status]),
      [[1, "exhausted", 429], [2, "invalid", 401]],
    );
    assert.equal(payload.yunwu?.health?.state, "healthy");
    assert.equal("balance_credits" in (payload.yunwu?.quota ?? {}), false);
    assert.equal(text.includes("private-account-a"), false);
    assert.equal(text.includes("private-account-b"), false);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("admin provider health includes cached quota fields without an active refresh", async () => {
  kvStore.clear();
  kvStore.set(keyToString(CODEX_AUTH_KEY), makeAuthPool(makeAuthEntry(Math.floor(Date.now() / 1000) + 3600)));
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => {
    throw new Error("/admin/providers must not contact any upstream");
  };
  try {
    const response = await handleHealthProviders({ includeQuota: true });
    const payload = await response.json() as { yunwu?: { quota?: { available?: boolean; balance_credits?: unknown } } };
    assert.equal(response.status, 200);
    assert.equal(typeof payload.yunwu?.quota?.available, "boolean");
    assert.equal("balance_credits" in (payload.yunwu?.quota ?? {}), true);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("provider health persists a recovery immediately while retaining the last 429", async () => {
  kvStore.clear();
  resetProviderHealthThrottleForTest();
  await recordCodexProviderHealth("recovering-account", "success", 200, () => 1_000);
  await recordCodexProviderHealth("recovering-account", "quota_exhausted", 429, () => 2_000);
  await recordCodexProviderHealth("recovering-account", "success", 200, () => 3_000);

  const health = await getCodexProviderHealth("recovering-account", () => 3_001);
  assert.equal(health.state, "healthy");
  assert.equal(health.last_429_at_ms, 2_000);
  assert.equal(health.last_success_at_ms, 3_000);
});

Deno.test("provider health coalesces identical quota observations without hiding a later transition", async () => {
  kvStore.clear();
  resetProviderHealthThrottleForTest();
  atomicCommitCount = 0;
  await Promise.all(
    Array.from(
      { length: 100 },
      () => recordCodexProviderHealth("coalesced-account", "quota_exhausted", 429, () => 1_000),
    ),
  );

  const coalesced = await getCodexProviderHealth("coalesced-account", () => 1_001);
  assert.equal(coalesced.state, "exhausted");
  assert.equal(coalesced.last_observed_at_ms, 1_000);
  assert.equal(atomicCommitCount, 1);

  await recordCodexProviderHealth("coalesced-account", "success", 200, () => 2_000);
  const recovered = await getCodexProviderHealth("coalesced-account", () => 2_001);
  assert.equal(recovered.state, "healthy");
  assert.equal(recovered.last_observed_at_ms, 2_000);
  assert.equal(recovered.last_429_at_ms, 1_000);
});

Deno.test("provider health serializes success to 429 to success transitions", async () => {
  kvStore.clear();
  resetProviderHealthThrottleForTest();
  atomicCommitCount = 0;
  await Promise.all([
    recordCodexProviderHealth("ordered-account", "success", 200, () => 1_000),
    recordCodexProviderHealth("ordered-account", "quota_exhausted", 429, () => 2_000),
    recordCodexProviderHealth("ordered-account", "success", 200, () => 3_000),
  ]);

  const health = await getCodexProviderHealth("ordered-account", () => 3_001);
  assert.equal(health.state, "healthy");
  assert.equal(health.last_observed_at_ms, 3_000);
  assert.equal(health.last_success_at_ms, 3_000);
  assert.equal(health.last_429_at_ms, 2_000);
  assert.equal(atomicCommitCount, 3);
});

Deno.test("public health is passive release provenance with zero upstream and KV work", async () => {
  kvStore.clear();
  let fetchCalls = 0;
  let kvCalls = 0;
  const originalFetch = globalThis.fetch;
  const originalGet = kvStub.get;
  globalThis.fetch = () => {
    fetchCalls += 1;
    throw new Error("public health must not fetch");
  };
  (kvStub as { get: typeof kvStub.get }).get = (...args) => {
    kvCalls += 1;
    return originalGet(...args);
  };

  try {
    const response = await handleHealth();
    const payload = await response.json() as {
      status?: string;
      release?: { git_sha?: string; deployment_id?: string };
    };

    assert.equal(response.status, 200);
    assert.equal(payload.status, "available");
    assert.equal(typeof payload.release?.git_sha, "string");
    assert.equal(typeof payload.release?.deployment_id, "string");
    assert.equal(fetchCalls, 0);
    assert.equal(kvCalls, 0);
  } finally {
    globalThis.fetch = originalFetch;
    (kvStub as { get: typeof kvStub.get }).get = originalGet;
  }
});

Deno.test("public health HEAD is release liveness without a response body", async () => {
  const response = await handler(new Request("https://ai.ubq.fi/health", { method: "HEAD" }));

  assert.equal(response.status, 200);
  assert.equal(response.headers.get("content-type"), "application/json");
  assert.equal(typeof response.headers.get("x-uos-git-sha"), "string");
  assert.equal(typeof response.headers.get("x-uos-deployment-id"), "string");
  assert.equal(await response.text(), "");
});

Deno.test("obsolete auth health route is not exposed", async () => {
  const response = await handler(new Request("https://ai.ubq.fi/health/auth"));
  assert.equal(response.status, 404);
});

Deno.test("detailed provider health and recheck routes require admin authentication", async () => {
  for (
    const request of [
      new Request("https://ai.ubq.fi/health/providers"),
      new Request("https://ai.ubq.fi/health/upstream"),
      new Request("https://ai.ubq.fi/admin/providers/codex/1/recheck", { method: "POST" }),
    ]
  ) {
    const response = await handler(request);
    assert.equal(response.status, 401);
  }
});

Deno.test("active upstream health retains detailed failure diagnostics", async () => {
  kvStore.clear();
  kvStore.set(keyToString(CODEX_AUTH_KEY), makeAuthPool(makeAuthEntry(Math.floor(Date.now() / 1000) + 3600)));

  const originalFetch = globalThis.fetch;
  globalThis.fetch = () =>
    Promise.resolve(
      new Response("temporary error", {
        status: 503,
        headers: { "Content-Type": "text/plain" },
      }),
    );

  try {
    const upstream = await handleHealthUpstream();
    const upstreamPayload = await upstream.json() as Record<string, unknown>;

    assert.equal(upstream.status, 503);
    assert.equal(upstreamPayload.status, 503);
    assert.equal(
      (upstreamPayload.probes as { codex?: { provider?: string; status?: number } } | undefined)?.codex?.provider,
      "chatgpt_codex",
    );
    assert.equal((upstreamPayload.probes as { codex?: { status?: number } } | undefined)?.codex?.status, 503);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("active upstream health preserves the provider that finishes before the shared deadline", async () => {
  const originalToken = Deno.env.get("YUNWU_SYSTEM_TOKEN");
  const originalUserId = Deno.env.get("YUNWU_USER_ID");
  const originalFetch = globalThis.fetch;
  Deno.env.set("YUNWU_SYSTEM_TOKEN", "system-token");
  Deno.env.set("YUNWU_USER_ID", "717235");
  setActiveUpstreamHealthTimeoutMsForTest(20);

  const restoreEnv = (key: string, value: string | undefined): void => {
    if (value === undefined) Deno.env.delete(key);
    else Deno.env.set(key, value);
  };
  const waitForAbort = (signal: AbortSignal | null | undefined): Promise<Response> =>
    new Promise<Response>((_resolve, reject) => {
      if (!signal) {
        reject(new Error("health probe did not pass an abort signal"));
        return;
      }
      const rejectWithReason = () => reject(signal.reason);
      if (signal.aborted) rejectWithReason();
      else signal.addEventListener("abort", rejectWithReason, { once: true });
    });
  const jsonResponse = (value: unknown): Response =>
    new Response(JSON.stringify(value), { headers: { "Content-Type": "application/json" } });

  try {
    for (const stalledProvider of ["codex", "yunwu"] as const) {
      kvStore.clear();
      kvStore.set(keyToString(CODEX_AUTH_KEY), makeAuthPool(makeAuthEntry(Math.floor(Date.now() / 1000) + 3600)));
      globalThis.fetch = (input, init) => {
        const url = new URL(input instanceof Request ? input.url : input.toString());
        const isCodex = url.hostname === "chatgpt.com";
        if ((stalledProvider === "codex" && isCodex) || (stalledProvider === "yunwu" && !isCodex)) {
          return waitForAbort(init?.signal);
        }
        if (isCodex) return Promise.resolve(jsonResponse({ models: [{ slug: "gpt-health" }] }));
        if (url.pathname === "/api/user/self") {
          return Promise.resolve(jsonResponse({ success: true, data: { quota: 1_000_000, used_quota: 10 } }));
        }
        if (url.pathname === "/api/user/topuprecords") {
          return Promise.resolve(jsonResponse({ success: true, data: { records: [] } }));
        }
        return Promise.resolve(jsonResponse({ success: true, data: { quota_per_unit: 500_000 } }));
      };

      const response = await handleHealthUpstream();
      const payload = await response.json() as {
        probes?: {
          codex?: { status?: number; error?: string };
          yunwu_quota?: { status?: number; error?: string } | null;
        };
      };
      assert.equal(response.status, 503, stalledProvider);
      if (stalledProvider === "codex") {
        assert.equal(payload.probes?.codex?.status, 503);
        assert.equal(payload.probes?.codex?.error, "Codex models probe timed out.");
        assert.equal(payload.probes?.yunwu_quota?.status, 200);
      } else {
        assert.equal(payload.probes?.codex?.status, 200);
        assert.equal(payload.probes?.yunwu_quota?.status, 503);
        assert.equal(payload.probes?.yunwu_quota?.error, "YunWu quota probe timed out.");
      }
    }
  } finally {
    globalThis.fetch = originalFetch;
    setActiveUpstreamHealthTimeoutMsForTest(null);
    restoreEnv("YUNWU_SYSTEM_TOKEN", originalToken);
    restoreEnv("YUNWU_USER_ID", originalUserId);
  }
});
