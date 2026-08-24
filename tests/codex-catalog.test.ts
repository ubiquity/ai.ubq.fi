import assert from "node:assert/strict";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const kvStore = new Map<string, { value: unknown; versionstamp: string }>();
let versionCounter = 0;
type AtomicTestOp = { kind: "set" | "delete"; key: Deno.KvKey; value?: unknown };
let beforeAtomicCommit: ((ops: readonly AtomicTestOp[]) => void) | null = null;

const nextVersion = (): string => String(++versionCounter).padStart(20, "0");
const entryFor = (key: Deno.KvKey): Deno.KvEntryMaybe<unknown> => {
  const stored = kvStore.get(keyToString(key));
  return stored
    ? { key, value: stored.value, versionstamp: stored.versionstamp }
    : { key, value: null, versionstamp: null };
};
const matchesPrefix = (key: Deno.KvKey, prefix: Deno.KvKey): boolean =>
  prefix.every((part, index) => key[index] === part);

const kvStub = {
  get: (key: Deno.KvKey) => Promise.resolve(entryFor(key)),
  set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
    kvStore.set(keyToString(key), { value, versionstamp: nextVersion() });
    return Promise.resolve({ ok: true } as const);
  },
  delete: (key: Deno.KvKey) => {
    kvStore.delete(keyToString(key));
    return Promise.resolve();
  },
  list: async function* (selector: Deno.KvListSelector) {
    const prefix = "prefix" in selector ? selector.prefix : [];
    for (const [encodedKey, stored] of kvStore) {
      const key = JSON.parse(encodedKey) as Deno.KvKey;
      if (matchesPrefix(key, prefix)) yield { key, value: stored.value, versionstamp: stored.versionstamp };
    }
  },
  atomic: () => {
    const checks: Deno.KvEntryMaybe<unknown>[] = [];
    const ops: AtomicTestOp[] = [];
    const chain = {
      check: (entry: Deno.KvEntryMaybe<unknown>) => {
        checks.push(entry);
        return chain;
      },
      set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
        ops.push({ kind: "set", key, value });
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        ops.push({ kind: "delete", key });
        return chain;
      },
      commit: () => {
        beforeAtomicCommit?.(ops);
        const valid = checks.every((expected) => entryFor(expected.key).versionstamp === expected.versionstamp);
        if (!valid) return Promise.resolve({ ok: false } as const);
        for (const op of ops) {
          if (op.kind === "delete") kvStore.delete(keyToString(op.key));
          else kvStore.set(keyToString(op.key), { value: op.value, versionstamp: nextVersion() });
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
  CODEX_CATALOG_AUTH_GENERATION_KEY,
  CODEX_CATALOG_CHUNK_BYTES,
  CODEX_CATALOG_CHUNK_PREFIX,
  CODEX_CATALOG_FRESH_MS,
  CODEX_CATALOG_LEASE_PREFIX,
  CODEX_CATALOG_MAX_VERSIONS,
  CODEX_CATALOG_PREFIX,
  CODEX_CATALOG_RETENTION_MS,
  getCodexCatalogMemoVersionsForTest,
  handleCodexCatalogModels,
  resetCodexCatalogMemoForTest,
  storeCodexCatalog,
} = await import("../src/codex_catalog.ts");
const { resetCodexAuthCacheForTest } = await import("../src/codex.ts");
const { handleModels } = await import("../src/openai.ts");
const {
  fetchMeteredModels,
  METERED_MODELS_CACHE_TTL_MS,
  resetMeteredModelsCacheForTest,
  setMeteredModelsFetchForTest,
} = await import("../src/metered.ts");
const {
  fetchSurplusModels,
  resetSurplusModelsCacheForTest,
  SURPLUS_MODELS_CACHE_TTL_MS,
} = await import("../src/surplus.ts");
const {
  loadRuntimeConfig,
  resetRuntimeConfigCacheForTest,
  RUNTIME_CONFIG_V2_KEY,
} = await import("../src/runtime_config.ts");

const AUTH_GENERATION = "auth-generation-test";
const AUTH_KEY = ["ubq_ai", "codex_auth"] as const;
const SNAPSHOT_KEY = ["ubq_ai", "codex_models"] as const;
const CATALOG_KEY = (version: string): Deno.KvKey => ["ubq_ai", "codex_catalog", version];

const seedBaseState = (snapshotVersion = "0.200.0"): void => {
  kvStore.clear();
  beforeAtomicCommit = null;
  resetCodexAuthCacheForTest();
  resetCodexCatalogMemoForTest();
  resetRuntimeConfigCacheForTest();
  kvStore.set(keyToString(CODEX_CATALOG_AUTH_GENERATION_KEY), {
    value: AUTH_GENERATION,
    versionstamp: nextVersion(),
  });
  kvStore.set(keyToString(AUTH_KEY), {
    value: {
      accounts: [{
        access_token: "server-access",
        refresh_token: "server-refresh",
        account_id: "server-account",
        updated_at_ms: Date.now(),
      }],
      updated_at_ms: Date.now(),
    },
    versionstamp: nextVersion(),
  });
  const snapshot = {
    source: "chatgpt_codex",
    client_version: snapshotVersion,
    updated_at_ms: Date.now(),
    models: [{ slug: `snapshot-${snapshotVersion}` }],
  };
  kvStore.set(keyToString(SNAPSHOT_KEY), {
    value: snapshot,
    versionstamp: nextVersion(),
  });
  kvStore.set(keyToString(RUNTIME_CONFIG_V2_KEY), {
    value: {
      version: 2,
      default_model: `snapshot-${snapshotVersion}`,
      default_reasoning_effort: "medium",
      codex_models: snapshot,
      updated_at_ms: Date.now(),
    },
    versionstamp: nextVersion(),
  });
};

const catalogBody = (version: string, extra: Record<string, unknown> = {}): string =>
  JSON.stringify({
    models: [{
      slug: `gpt-${version}`,
      display_name: `Rich ${version}`,
      base_instructions: "must remain untouched",
      supported_reasoning_levels: [{ effort: "high", description: "deep" }],
    }],
    ...extra,
  });

const request = (version: string, headers: HeadersInit = {}): Request =>
  new Request(`https://ai.ubq.fi/v1/models?client_version=${version}`, {
    headers: { Authorization: "Bearer gateway-client-token", Cookie: "incoming=secret", ...headers },
  });

Deno.test("codex catalog: unversioned models retain the exact OpenAI list shape", async () => {
  seedBaseState();
  const response = await handleModels(new Request("https://ai.ubq.fi/v1/models"));
  assert.equal(response.status, 200);
  const payload = await response.json() as Record<string, unknown>;
  assert.deepEqual(Object.keys(payload).sort(), ["data", "object"]);
  assert.equal(payload.object, "list");
});

Deno.test("codex catalog: exact versions preserve rich JSON, isolate caches, and forward ETags", async () => {
  seedBaseState("0.100.0");
  const originalFetch = globalThis.fetch;
  const calls: Array<{ url: string; headers: Headers }> = [];
  globalThis.fetch = (input, init) => {
    const upstream = new Request(input, init);
    calls.push({ url: upstream.url, headers: upstream.headers });
    const version = new URL(upstream.url).searchParams.get("client_version") ?? "missing";
    return Promise.resolve(
      new Response(catalogBody(version, { version_marker: version }), {
        status: 200,
        headers: { "Content-Type": "application/json; charset=utf-8", ETag: `"${version}"` },
      }),
    );
  };

  try {
    const first = await handleCodexCatalogModels(request("0.144.3"), "0.144.3");
    assert.equal(first.status, 200);
    assert.equal(first.headers.get("etag"), '"0.144.3"');
    assert.deepEqual(await first.json(), JSON.parse(catalogBody("0.144.3", { version_marker: "0.144.3" })));
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, "https://chatgpt.com/backend-api/codex/models?client_version=0.144.3");
    assert.equal(calls[0].headers.get("cookie"), null);
    assert.notEqual(calls[0].headers.get("authorization"), "Bearer gateway-client-token");
    assert.match(calls[0].headers.get("user-agent") ?? "", /codex_cli_rs\/0\.144\.3/);

    const hit = await handleCodexCatalogModels(request("0.144.3"), "0.144.3");
    assert.equal(hit.headers.get("x-uos-cache"), "hit");
    assert.equal(calls.length, 1);

    const secondVersion = await handleCodexCatalogModels(request("0.145.0"), "0.145.0");
    assert.equal(secondVersion.status, 200);
    assert.equal(calls.length, 2);
    assert.equal((await secondVersion.json() as { version_marker?: string }).version_marker, "0.145.0");

    const notModified = await handleCodexCatalogModels(
      request("0.144.3", { "If-None-Match": '"0.144.3"' }),
      "0.144.3",
    );
    assert.equal(notModified.status, 304);
    assert.equal(await notModified.text(), "");
    assert.equal(calls.length, 2);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: malformed versions are rejected before upstream access", async () => {
  seedBaseState();
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => {
    throw new Error("invalid versions must not fetch");
  };
  try {
    for (const version of ["0.144", "v0.144.3", "0.144.3.1", "0.144.x", ""]) {
      const response = await handleCodexCatalogModels(request(version), version);
      assert.equal(response.status, 400);
    }
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: stale catalogs survive refresh failures but expired catalogs do not", async () => {
  seedBaseState();
  const version = "0.150.0";
  const observedAtMs = Date.now();
  await storeCodexCatalog(kvStub, {
    clientVersion: version,
    authGeneration: AUTH_GENERATION,
    body: catalogBody(version),
    etag: '"stale"',
    fetchedAtMs: observedAtMs - CODEX_CATALOG_FRESH_MS - 1,
  });
  const originalFetch = globalThis.fetch;
  const degradationSignals: number[] = [];
  const dependencies = {
    now: () => observedAtMs,
    recordSentinelDegradation: (timestamp: number) => {
      degradationSignals.push(timestamp);
      return Promise.reject(new Error("sentinel unavailable"));
    },
  };
  globalThis.fetch = () => Promise.resolve(new Response("temporary failure", { status: 503 }));
  try {
    const stale = await handleCodexCatalogModels(request(version), version, dependencies);
    assert.equal(stale.status, 200);
    assert.equal(stale.headers.get("x-uos-cache"), "stale");
    assert.equal((await stale.json() as { models: Array<{ slug: string }> }).models[0].slug, `gpt-${version}`);
    assert.deepEqual(degradationSignals, [observedAtMs]);

    const metadata = kvStore.get(keyToString(CATALOG_KEY(version)))?.value as { fetched_at_ms: number };
    metadata.fetched_at_ms = observedAtMs - CODEX_CATALOG_RETENTION_MS - 1;
    const expired = await handleCodexCatalogModels(request(version), version, dependencies);
    assert.equal(expired.status, 502);
    assert.deepEqual(degradationSignals, [observedAtMs, observedAtMs]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: stale catalogs signal masked upstream transport failures", async () => {
  seedBaseState();
  const version = "0.150.2";
  const observedAtMs = Date.now();
  await storeCodexCatalog(kvStub, {
    clientVersion: version,
    authGeneration: AUTH_GENERATION,
    body: catalogBody(version),
    fetchedAtMs: observedAtMs - CODEX_CATALOG_FRESH_MS - 1,
  });
  const originalFetch = globalThis.fetch;
  const degradationSignals: number[] = [];
  globalThis.fetch = () => {
    throw new Error("connection reset");
  };
  try {
    const response = await handleCodexCatalogModels(request(version), version, {
      now: () => observedAtMs,
      recordSentinelDegradation: (timestamp: number) => {
        degradationSignals.push(timestamp);
        return Promise.resolve(true);
      },
    });
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-cache"), "stale");
    assert.deepEqual(degradationSignals, [observedAtMs]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: account fallback signals the masked transport failure once", async () => {
  seedBaseState();
  const version = "0.150.3";
  const observedAtMs = Date.now();
  const authPool = kvStore.get(keyToString(AUTH_KEY))?.value as {
    accounts: Array<Record<string, unknown>>;
  };
  authPool.accounts.push({
    access_token: "fallback-access",
    refresh_token: "fallback-refresh",
    account_id: "fallback-account",
    updated_at_ms: observedAtMs,
  });
  const originalFetch = globalThis.fetch;
  let upstreamCalls = 0;
  const degradationSignals: number[] = [];
  globalThis.fetch = () => {
    upstreamCalls += 1;
    if (upstreamCalls === 1) throw new Error("first account connection reset");
    return Promise.resolve(
      new Response(catalogBody(version), { headers: { "Content-Type": "application/json" } }),
    );
  };
  try {
    const response = await handleCodexCatalogModels(request(version), version, {
      now: () => observedAtMs,
      recordSentinelDegradation: (timestamp: number) => {
        degradationSignals.push(timestamp);
        return Promise.resolve(true);
      },
    });
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-cache"), "miss");
    assert.equal(upstreamCalls, 2);
    assert.deepEqual(degradationSignals, [observedAtMs]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: stale catalogs signal response body transport failures", async () => {
  seedBaseState();
  const version = "0.150.4";
  const observedAtMs = Date.now();
  await storeCodexCatalog(kvStub, {
    clientVersion: version,
    authGeneration: AUTH_GENERATION,
    body: catalogBody(version),
    fetchedAtMs: observedAtMs - CODEX_CATALOG_FRESH_MS - 1,
  });
  const originalFetch = globalThis.fetch;
  const degradationSignals: number[] = [];
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(
        new ReadableStream({
          start(controller) {
            controller.error(new Error("body socket reset"));
          },
        }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      ),
    );
  try {
    const response = await handleCodexCatalogModels(request(version), version, {
      now: () => observedAtMs,
      recordSentinelDegradation: (timestamp: number) => {
        degradationSignals.push(timestamp);
        return Promise.resolve(true);
      },
    });
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-cache"), "stale");
    assert.deepEqual(degradationSignals, [observedAtMs]);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: stale ETags revalidate upstream and matching clients receive 304", async () => {
  seedBaseState();
  const version = "0.150.1";
  await storeCodexCatalog(kvStub, {
    clientVersion: version,
    authGeneration: AUTH_GENERATION,
    body: catalogBody(version),
    etag: '"catalog-etag"',
    fetchedAtMs: Date.now() - CODEX_CATALOG_FRESH_MS - 1,
  });
  const originalFetch = globalThis.fetch;
  let upstreamIfNoneMatch: string | null = null;
  globalThis.fetch = (input, init) => {
    upstreamIfNoneMatch = new Request(input, init).headers.get("If-None-Match");
    return Promise.resolve(new Response(null, { status: 304, headers: { ETag: '"catalog-etag"' } }));
  };
  try {
    const response = await handleCodexCatalogModels(
      request(version, { "If-None-Match": '"catalog-etag"' }),
      version,
    );
    assert.equal(response.status, 304);
    assert.equal(response.headers.get("x-uos-cache"), "revalidated");
    assert.equal(upstreamIfNoneMatch, '"catalog-etag"');
    assert.equal(await response.text(), "");
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: refresh leases suppress duplicate upstream requests", async () => {
  seedBaseState();
  const version = "0.151.0";
  await storeCodexCatalog(kvStub, {
    clientVersion: version,
    authGeneration: AUTH_GENERATION,
    body: catalogBody(version),
    fetchedAtMs: Date.now() - CODEX_CATALOG_FRESH_MS - 1,
  });
  kvStore.set(keyToString([...CODEX_CATALOG_LEASE_PREFIX, version]), {
    value: { owner: "other-request", lease_until_ms: Date.now() + 10_000 },
    versionstamp: nextVersion(),
  });
  const originalFetch = globalThis.fetch;
  let calls = 0;
  let degradationSignals = 0;
  globalThis.fetch = () => {
    calls += 1;
    return Promise.resolve(new Response(catalogBody(version), { headers: { "Content-Type": "application/json" } }));
  };
  try {
    const response = await handleCodexCatalogModels(request(version), version, {
      recordSentinelDegradation: () => {
        degradationSignals += 1;
        return Promise.resolve(true);
      },
    });
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-cache"), "stale");
    assert.equal(calls, 0);
    assert.equal(degradationSignals, 0);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: concurrent cold misses share one upstream refresh", async () => {
  seedBaseState();
  const version = "0.151.1";
  const originalFetch = globalThis.fetch;
  let resolveFetch!: (response: Response) => void;
  let markFetchStarted!: () => void;
  const fetchStarted = new Promise<void>((resolve) => markFetchStarted = resolve);
  const deferredResponse = new Promise<Response>((resolve) => resolveFetch = resolve);
  let calls = 0;
  globalThis.fetch = () => {
    calls += 1;
    markFetchStarted();
    return deferredResponse;
  };

  try {
    const firstPromise = handleCodexCatalogModels(request(version), version);
    await fetchStarted;
    const secondPromise = handleCodexCatalogModels(request(version), version);
    resolveFetch(new Response(catalogBody(version), { headers: { "Content-Type": "application/json" } }));
    const [first, second] = await Promise.all([firstPromise, secondPromise]);
    assert.equal(first.status, 200);
    assert.equal(second.status, 200);
    assert.equal(second.headers.get("x-uos-cache"), "wait");
    assert.equal(calls, 1);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: auth rotation discards an in-flight old-generation refresh", async () => {
  seedBaseState("0.200.0");
  const version = "0.201.1";
  const replacementGeneration = "replacement-auth-generation";
  const replacementBody = catalogBody(version, { account: "replacement" });
  const originalFetch = globalThis.fetch;
  let resolveFetch!: (response: Response) => void;
  let markFetchStarted!: () => void;
  const fetchStarted = new Promise<void>((resolve) => markFetchStarted = resolve);
  const deferredResponse = new Promise<Response>((resolve) => resolveFetch = resolve);
  globalThis.fetch = () => {
    markFetchStarted();
    return deferredResponse;
  };

  try {
    const responsePromise = handleCodexCatalogModels(request(version), version);
    await fetchStarted;
    kvStore.set(keyToString(CODEX_CATALOG_AUTH_GENERATION_KEY), {
      value: replacementGeneration,
      versionstamp: nextVersion(),
    });
    kvStore.set(keyToString(SNAPSHOT_KEY), {
      value: {
        source: "chatgpt_codex",
        client_version: version,
        updated_at_ms: Date.now(),
        models: [{ slug: "replacement-snapshot" }],
      },
      versionstamp: nextVersion(),
    });
    assert.equal(
      await storeCodexCatalog(kvStub, {
        clientVersion: version,
        authGeneration: replacementGeneration,
        body: replacementBody,
      }),
      true,
    );
    resolveFetch(
      new Response(catalogBody(version, { account: "old" }), {
        headers: { "Content-Type": "application/json" },
      }),
    );

    const response = await responsePromise;
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-cache"), "rotated");
    assert.equal((await response.json() as { account?: string }).account, "replacement");
    const metadata = kvStore.get(keyToString(CATALOG_KEY(version)))?.value as { auth_generation?: string };
    assert.equal(metadata.auth_generation, replacementGeneration);
    const snapshot = kvStore.get(keyToString(SNAPSHOT_KEY))?.value as { models?: Array<{ slug?: string }> };
    assert.equal(snapshot.models?.[0]?.slug, "replacement-snapshot");
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: slow refreshes renew their lease", async () => {
  seedBaseState();
  const version = "0.151.2";
  const leaseStorageKey = keyToString([...CODEX_CATALOG_LEASE_PREFIX, version]);
  const originalFetch = globalThis.fetch;
  let resolveFetch!: (response: Response) => void;
  let markFetchStarted!: () => void;
  const fetchStarted = new Promise<void>((resolve) => markFetchStarted = resolve);
  const deferredResponse = new Promise<Response>((resolve) => resolveFetch = resolve);
  globalThis.fetch = () => {
    markFetchStarted();
    return deferredResponse;
  };

  try {
    const responsePromise = handleCodexCatalogModels(request(version), version);
    await fetchStarted;
    const initialLease = kvStore.get(leaseStorageKey)?.value as { lease_until_ms: number };
    await new Promise((resolve) => setTimeout(resolve, 5_100));
    const renewedLease = kvStore.get(leaseStorageKey)?.value as { lease_until_ms: number };
    assert.ok(renewedLease.lease_until_ms > initialLease.lease_until_ms);
    resolveFetch(new Response(catalogBody(version), { headers: { "Content-Type": "application/json" } }));
    assert.equal((await responsePromise).status, 200);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: gzip chunks are bounded and integrity failures force a refresh", async () => {
  seedBaseState();
  const version = "0.152.0";
  const noisyModels = Array.from({ length: 4_000 }, (_, index) => ({
    slug: `model-${index}-${crypto.randomUUID()}`,
    description: crypto.randomUUID(),
  }));
  const body = JSON.stringify({ models: noisyModels });
  assert.equal(
    await storeCodexCatalog(kvStub, {
      clientVersion: version,
      authGeneration: AUTH_GENERATION,
      body,
    }),
    true,
  );
  const chunkEntries = [...kvStore.entries()].filter(([encoded]) => {
    const key = JSON.parse(encoded) as Deno.KvKey;
    return matchesPrefix(key, [...CODEX_CATALOG_CHUNK_PREFIX, version]);
  });
  assert.ok(chunkEntries.length > 1);
  for (const [, stored] of chunkEntries) {
    assert.ok((stored.value as Uint8Array).byteLength <= CODEX_CATALOG_CHUNK_BYTES);
  }
  const first = chunkEntries[0][1];
  const corrupted = (first.value as Uint8Array).slice();
  corrupted[0] ^= 0xff;
  first.value = corrupted;

  const originalFetch = globalThis.fetch;
  let calls = 0;
  globalThis.fetch = () => {
    calls += 1;
    return Promise.resolve(new Response("unavailable", { status: 503 }));
  };
  try {
    const response = await handleCodexCatalogModels(request(version), version);
    assert.equal(response.status, 502);
    assert.equal(calls, 1);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: replacing metadata reclaims superseded chunks", async () => {
  seedBaseState();
  const version = "0.152.1";
  assert.equal(
    await storeCodexCatalog(kvStub, {
      clientVersion: version,
      authGeneration: AUTH_GENERATION,
      body: catalogBody(version, { generation: "first" }),
    }),
    true,
  );
  const firstMetadata = kvStore.get(keyToString(CATALOG_KEY(version)))?.value as {
    body_generation: string;
    chunk_count: number;
  };
  const firstChunkKeys = Array.from(
    { length: firstMetadata.chunk_count },
    (_, index) => keyToString([...CODEX_CATALOG_CHUNK_PREFIX, version, firstMetadata.body_generation, index]),
  );
  assert.ok(firstChunkKeys.every((key) => kvStore.has(key)));

  assert.equal(
    await storeCodexCatalog(kvStub, {
      clientVersion: version,
      authGeneration: AUTH_GENERATION,
      body: catalogBody(version, { generation: "second" }),
    }),
    true,
  );
  const secondMetadata = kvStore.get(keyToString(CATALOG_KEY(version)))?.value as {
    body_generation: string;
    chunk_count: number;
  };
  assert.notEqual(secondMetadata.body_generation, firstMetadata.body_generation);
  assert.ok(firstChunkKeys.every((key) => !kvStore.has(key)));
  for (let index = 0; index < secondMetadata.chunk_count; index += 1) {
    assert.equal(
      kvStore.has(keyToString([
        ...CODEX_CATALOG_CHUNK_PREFIX,
        version,
        secondMetadata.body_generation,
        index,
      ])),
      true,
    );
  }
});

Deno.test("codex catalog: rejected old-generation writes reclaim their chunks", async () => {
  seedBaseState();
  const version = "0.152.2";
  assert.equal(
    await storeCodexCatalog(kvStub, {
      clientVersion: version,
      authGeneration: "superseded-generation",
      body: catalogBody(version),
    }),
    false,
  );
  const orphanChunks = [...kvStore.keys()].filter((encoded) => {
    const key = JSON.parse(encoded) as Deno.KvKey;
    return matchesPrefix(key, [...CODEX_CATALOG_CHUNK_PREFIX, version]);
  });
  assert.equal(orphanChunks.length, 0);
  assert.equal(kvStore.has(keyToString(CATALOG_KEY(version))), false);
});

Deno.test("codex catalog: version cache evicts the oldest catalog beyond its bound", async () => {
  seedBaseState();
  const totalVersions = CODEX_CATALOG_MAX_VERSIONS + 3;
  for (let index = 0; index < totalVersions; index += 1) {
    const version = `1.0.${index}`;
    assert.equal(
      await storeCodexCatalog(kvStub, {
        clientVersion: version,
        authGeneration: AUTH_GENERATION,
        body: catalogBody(version),
        fetchedAtMs: Date.now() + index,
      }),
      true,
    );
    const loaded = await handleCodexCatalogModels(request(version), version);
    assert.equal(loaded.status, 200);
    await loaded.body?.cancel();
  }

  const metadataEntries = [...kvStore.entries()].filter(([encoded, stored]) => {
    const key = JSON.parse(encoded) as Deno.KvKey;
    return matchesPrefix(key, CODEX_CATALOG_PREFIX) && key.length === CODEX_CATALOG_PREFIX.length + 1 &&
      typeof (stored.value as { body_generation?: unknown }).body_generation === "string";
  });
  assert.equal(metadataEntries.length, CODEX_CATALOG_MAX_VERSIONS);
  for (let index = 0; index < totalVersions - CODEX_CATALOG_MAX_VERSIONS; index += 1) {
    const version = `1.0.${index}`;
    assert.equal(kvStore.has(keyToString(CATALOG_KEY(version))), false);
    const chunks = [...kvStore.keys()].filter((encoded) => {
      const key = JSON.parse(encoded) as Deno.KvKey;
      return matchesPrefix(key, [...CODEX_CATALOG_CHUNK_PREFIX, version]);
    });
    assert.equal(chunks.length, 0);
  }
  assert.equal(kvStore.has(keyToString(CATALOG_KEY(`1.0.${totalVersions - 1}`))), true);
  const memoVersions = getCodexCatalogMemoVersionsForTest();
  assert.equal(memoVersions.length, CODEX_CATALOG_MAX_VERSIONS);
  for (let index = 0; index < totalVersions - CODEX_CATALOG_MAX_VERSIONS; index += 1) {
    assert.equal(memoVersions.includes(`1.0.${index}`), false);
  }
  assert.equal(memoVersions.at(-1), `1.0.${totalVersions - 1}`);
});

Deno.test("codex catalog: only same-or-newer clients update the normalized snapshot", async () => {
  seedBaseState("0.200.0");
  const originalFetch = globalThis.fetch;
  globalThis.fetch = (input) => {
    const version = new URL(input instanceof Request ? input.url : input.toString()).searchParams.get(
      "client_version",
    )!;
    return Promise.resolve(
      new Response(
        catalogBody(version, {
          models: [{
            slug: `gpt-${version}`,
            display_name: `Rich ${version}`,
            supported_reasoning_levels: [{ effort: "high", description: "deep" }],
          }, {
            slug: "codex-auto-review",
            display_name: "Codex Auto Review",
            visibility: "hide",
            supported_in_api: true,
          }, {
            slug: "codex-internal-evals",
            visibility: "hide",
            supported_in_api: false,
          }],
        }),
        {
          headers: { "Content-Type": "application/json" },
        },
      ),
    );
  };
  try {
    assert.equal((await handleCodexCatalogModels(request("0.199.0"), "0.199.0")).status, 200);
    const olderSnapshot = kvStore.get(keyToString(SNAPSHOT_KEY))?.value as { client_version: string };
    assert.equal(olderSnapshot.client_version, "0.200.0");

    assert.equal((await handleCodexCatalogModels(request("0.201.0"), "0.201.0")).status, 200);
    const snapshot = kvStore.get(keyToString(SNAPSHOT_KEY))?.value as {
      client_version: string;
      models: Array<{ slug: string }>;
    };
    assert.equal(snapshot.client_version, "0.201.0");
    assert.deepEqual(snapshot.models.map((model) => model.slug), ["gpt-0.201.0", "codex-auto-review"]);
    const runtime = kvStore.get(keyToString(RUNTIME_CONFIG_V2_KEY))?.value as {
      default_model: string;
      default_reasoning_effort: string;
      codex_models: { client_version: string; models: Array<{ slug: string }> };
    };
    assert.equal(runtime.default_model, "gpt-0.201.0");
    assert.equal(runtime.default_reasoning_effort, "medium");
    assert.equal(runtime.codex_models.client_version, "0.201.0");
    assert.deepEqual(runtime.codex_models.models.map((model) => model.slug), ["gpt-0.201.0", "codex-auto-review"]);

    // The catalog publisher must seed the isolate cache with exactly the
    // compact configuration committed in the same transaction.
    kvStore.set(keyToString(RUNTIME_CONFIG_V2_KEY), {
      value: { ...runtime, default_model: "stale-uncommitted-value" },
      versionstamp: nextVersion(),
    });
    assert.equal((await loadRuntimeConfig(kvStub))?.default_model, "gpt-0.201.0");
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: normalized snapshot retry preserves a concurrent admin default", async () => {
  seedBaseState("0.200.0");
  const currentSnapshot = {
    source: "chatgpt_codex",
    client_version: "0.200.0",
    updated_at_ms: Date.now(),
    models: [
      { slug: "shared-default", supported_reasoning_levels: ["medium"] },
      { slug: "admin-default", supported_reasoning_levels: ["high"] },
    ],
  };
  kvStore.set(keyToString(SNAPSHOT_KEY), { value: currentSnapshot, versionstamp: nextVersion() });
  kvStore.set(keyToString(RUNTIME_CONFIG_V2_KEY), {
    value: {
      version: 2,
      default_model: "shared-default",
      default_reasoning_effort: "medium",
      codex_models: currentSnapshot,
      updated_at_ms: Date.now(),
    },
    versionstamp: nextVersion(),
  });
  resetRuntimeConfigCacheForTest();

  const nextModels = [
    { slug: "shared-default", supported_reasoning_levels: ["medium"] },
    { slug: "admin-default", supported_reasoning_levels: ["high"] },
  ];
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(catalogBody("0.201.0", { models: nextModels }), {
        headers: { "Content-Type": "application/json" },
      }),
    );

  beforeAtomicCommit = (ops) => {
    if (!ops.some((op) => keyToString(op.key) === keyToString(RUNTIME_CONFIG_V2_KEY))) return;
    beforeAtomicCommit = null;
    const current = kvStore.get(keyToString(RUNTIME_CONFIG_V2_KEY))!.value as Record<string, unknown>;
    kvStore.set(keyToString(RUNTIME_CONFIG_V2_KEY), {
      value: {
        ...current,
        default_model: "admin-default",
        default_reasoning_effort: "high",
        updated_at_ms: Date.now(),
      },
      versionstamp: nextVersion(),
    });
  };

  try {
    assert.equal((await handleCodexCatalogModels(request("0.201.0"), "0.201.0")).status, 200);
    const snapshot = kvStore.get(keyToString(SNAPSHOT_KEY))?.value as { client_version: string };
    const runtime = kvStore.get(keyToString(RUNTIME_CONFIG_V2_KEY))?.value as {
      default_model: string;
      default_reasoning_effort: string;
      codex_models: { client_version: string };
    };
    assert.equal(snapshot.client_version, "0.201.0");
    assert.equal(runtime.codex_models.client_version, "0.201.0");
    assert.equal(runtime.default_model, "admin-default");
    assert.equal(runtime.default_reasoning_effort, "high");
  } finally {
    beforeAtomicCommit = null;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: normalized refresh retries preserve same-slug prompt-cache probe evidence", async () => {
  seedBaseState("0.200.0");
  const promptCache = {
    version: 1,
    providers: [{
      id: "codex_chatgpt",
      scope: {
        probe_profile: "responses_implicit_input_text_keyed_cycle_isolated_v5",
        account_slots: "unknown",
        token_refresh: "preserved",
        conversation_id: "independent",
        reproducible_cycles: 3,
        source: "live_probe",
        verified_at_ms: 2_000,
      },
    }],
  };
  const existingSnapshot = {
    source: "chatgpt_codex",
    client_version: "0.200.0",
    updated_at_ms: Date.now(),
    models: [{ slug: "gpt-cache-probe", prompt_cache: promptCache }],
  };
  kvStore.set(keyToString(SNAPSHOT_KEY), { value: existingSnapshot, versionstamp: nextVersion() });
  kvStore.set(keyToString(RUNTIME_CONFIG_V2_KEY), {
    value: {
      version: 2,
      default_model: "gpt-cache-probe",
      default_reasoning_effort: "medium",
      codex_models: existingSnapshot,
      updated_at_ms: Date.now(),
    },
    versionstamp: nextVersion(),
  });
  resetRuntimeConfigCacheForTest();

  const originalFetch = globalThis.fetch;
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(
        catalogBody("0.201.0", {
          models: [{ slug: "gpt-cache-probe", supported_reasoning_levels: ["medium"] }],
        }),
        { headers: { "Content-Type": "application/json" } },
      ),
    );

  beforeAtomicCommit = (ops) => {
    if (!ops.some((op) => keyToString(op.key) === keyToString(SNAPSHOT_KEY))) return;
    beforeAtomicCommit = null;
    kvStore.set(keyToString(SNAPSHOT_KEY), {
      value: { ...existingSnapshot, updated_at_ms: Date.now() + 1 },
      versionstamp: nextVersion(),
    });
  };

  try {
    assert.equal((await handleCodexCatalogModels(request("0.201.0"), "0.201.0")).status, 200);
    const snapshot = kvStore.get(keyToString(SNAPSHOT_KEY))?.value as {
      client_version?: string;
      models?: Array<{ slug?: string; prompt_cache?: unknown }>;
    };
    const runtime = kvStore.get(keyToString(RUNTIME_CONFIG_V2_KEY))?.value as {
      codex_models?: { models?: Array<{ slug?: string; prompt_cache?: unknown }> };
    };
    assert.equal(snapshot.client_version, "0.201.0");
    assert.deepEqual(snapshot.models?.[0]?.prompt_cache, promptCache);
    assert.equal(runtime.codex_models?.models?.[0]?.prompt_cache, undefined);
  } finally {
    beforeAtomicCommit = null;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("codex catalog: model picker receives the complete unique union of paid provider models", async () => {
  seedBaseState("0.200.0");
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();
  const originalFetch = globalThis.fetch;
  const originalMeteredKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusKey = Deno.env.get("SURPLUS_API_KEY");
  Deno.env.set("METERED_API_KEY", "metered-catalog-test-key");
  Deno.env.set("SURPLUS_API_KEY", "surplus-catalog-test-key");
  await fetchMeteredModels({
    force: true,
    fetcher: () =>
      Promise.resolve(Response.json({
        data: [
          {
            id: "shared-paid-model",
            owned_by: "openlux",
            supported_endpoint_types: ["openai-response"],
            description: "OpenLux route",
          },
          {
            id: "openlux-only-model",
            owned_by: "openlux",
            supported_endpoint_types: ["openai-response"],
          },
          {
            id: "chat-only-model",
            owned_by: "openlux",
            supported_endpoint_types: ["openai"],
          },
        ],
      })),
  });
  await fetchSurplusModels({
    apiKey: "surplus-catalog-test-key",
    force: true,
    fetcher: () =>
      Promise.resolve(Response.json({
        data: [
          { id: "shared-paid-model", provider: "surplus" },
          { id: "surplus-only-model", provider: "surplus" },
          { id: "deepseek-v4-flash", provider: "surplus" },
          { id: "deepseek-v4-flash-0731", provider: "surplus" },
          { id: "deepseek-v4-flash:web", provider: "surplus" },
          { id: "minimax-m2.7", provider: "surplus" },
        ],
      })),
  });
  globalThis.fetch = (input) => {
    const version = new URL(String(input)).searchParams.get("client_version") ?? "missing";
    return Promise.resolve(new Response(catalogBody(version), { headers: { "Content-Type": "application/json" } }));
  };

  try {
    const response = await handleCodexCatalogModels(request("0.148.0"), "0.148.0");
    assert.equal(response.status, 200);
    const payload = await response.json() as { models: Array<Record<string, unknown>> };
    const slugs = payload.models.map((model) => model.slug);
    assert.deepEqual(slugs, [
      "gpt-0.148.0",
      "shared-paid-model",
      "openlux-only-model",
      "surplus-only-model",
      "deepseek-v4-flash",
      "deepseek-v4-flash-0731",
      "deepseek-v4-flash:web",
      "minimax-m2.7",
    ]);
    assert.equal(new Set(slugs).size, slugs.length);
    assert.equal(payload.models.find((model) => model.slug === "shared-paid-model")?.visibility, "list");
    assert.deepEqual(
      payload.models.find((model) => model.slug === "surplus-only-model")?.supported_reasoning_levels,
      [{ effort: "none", description: "No reasoning" }],
    );
    for (const slug of ["deepseek-v4-flash", "deepseek-v4-flash-0731", "deepseek-v4-flash:web"]) {
      const model = payload.models.find((candidate) => candidate.slug === slug);
      assert.deepEqual(
        model?.supported_reasoning_levels,
        [
          { effort: "none", description: "Disable optional reasoning" },
          { effort: "low", description: "Reasoning effort: low" },
          { effort: "high", description: "Reasoning effort: high" },
          { effort: "max", description: "Maximum reasoning depth" },
        ],
      );
      assert.equal(model?.default_reasoning_level, "high");
      assert.equal(model?.context_window, 1_000_000);
      assert.equal(model?.max_context_window, 1_000_000);
      assert.equal(model?.auto_compact_token_limit, 850_000);
      assert.equal(model?.effective_context_window_percent, 95);
    }
    const minimax = payload.models.find((model) => model.slug === "minimax-m2.7");
    assert.equal(minimax?.context_window, 204_800);
    assert.equal(minimax?.auto_compact_token_limit, 154_800);
    assert.equal(slugs.includes("chat-only-model"), false);
  } finally {
    globalThis.fetch = originalFetch;
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredKey);
    if (originalSurplusKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusKey);
  }
});

Deno.test("codex catalog: cold provider caches cannot publish the incomplete Codex-only catalog", async () => {
  seedBaseState("0.200.0");
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();
  const originalFetch = globalThis.fetch;
  const originalMeteredKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusKey = Deno.env.get("SURPLUS_API_KEY");
  Deno.env.set("METERED_API_KEY", "metered-cold-catalog-test-key");
  Deno.env.set("SURPLUS_API_KEY", "surplus-cold-catalog-test-key");
  setMeteredModelsFetchForTest(() =>
    Promise.resolve(Response.json({
      data: [{ id: "cold-openlux-model", owned_by: "openlux", supported_endpoint_types: ["openai-response"] }],
    }))
  );
  globalThis.fetch = (input) => {
    const url = String(input);
    if (url === "https://api.surplusintelligence.ai/v1/models") {
      return Promise.resolve(Response.json({ data: [{ id: "deepseek-v4-flash", provider: "surplus" }] }));
    }
    const version = new URL(url).searchParams.get("client_version") ?? "missing";
    return Promise.resolve(new Response(catalogBody(version), { headers: { "Content-Type": "application/json" } }));
  };

  try {
    const response = await handleCodexCatalogModels(request("0.203.0"), "0.203.0");
    assert.equal(response.status, 200);
    const payload = await response.json() as { models: Array<Record<string, unknown>> };
    const slugs = payload.models.map((model) => model.slug);
    assert.equal(slugs.includes("cold-openlux-model"), true);
    const deepseek = payload.models.find((model) => model.slug === "deepseek-v4-flash");
    assert.deepEqual(
      (deepseek?.supported_reasoning_levels as Array<{ effort: string }>).map((level) => level.effort),
      ["none", "low", "high", "max"],
    );
  } finally {
    globalThis.fetch = originalFetch;
    setMeteredModelsFetchForTest(null);
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredKey);
    if (originalSurplusKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusKey);
  }
});

Deno.test("paid-provider model caches ignore out-of-order Metered refreshes", async () => {
  resetMeteredModelsCacheForTest();
  const originalMeteredKey = Deno.env.get("METERED_API_KEY");
  Deno.env.set("METERED_API_KEY", "metered-cache-race-test-key");
  let calls = 0;
  let resolveOlder!: (response: Response) => void;
  let resolveNewer!: (response: Response) => void;
  const olderResponse = new Promise<Response>((resolve) => resolveOlder = resolve);
  const newerResponse = new Promise<Response>((resolve) => resolveNewer = resolve);
  const fetcher = () => {
    calls += 1;
    return calls === 1 ? olderResponse : newerResponse;
  };

  try {
    const older = fetchMeteredModels({ force: true, fetcher });
    const newer = fetchMeteredModels({ force: true, fetcher });
    assert.equal(calls, 2);
    resolveNewer(Response.json({
      data: [{ id: "new-metered-model", owned_by: "openlux", supported_endpoint_types: ["openai-response"] }],
    }));
    assert.deepEqual((await newer)?.models.map((model) => model.id), ["new-metered-model"]);
    resolveOlder(Response.json({
      data: [{ id: "old-metered-model", owned_by: "openlux", supported_endpoint_types: ["openai-response"] }],
    }));
    await older;
    const cached = await fetchMeteredModels({ fetcher });
    assert.deepEqual(cached?.models.map((model) => model.id), ["new-metered-model"]);
    assert.equal(calls, 2);
  } finally {
    resetMeteredModelsCacheForTest();
    if (originalMeteredKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredKey);
  }
});

Deno.test("paid-provider model caches ignore out-of-order Surplus refreshes", async () => {
  resetSurplusModelsCacheForTest();
  let calls = 0;
  let resolveOlder!: (response: Response) => void;
  let resolveNewer!: (response: Response) => void;
  const olderResponse = new Promise<Response>((resolve) => resolveOlder = resolve);
  const newerResponse = new Promise<Response>((resolve) => resolveNewer = resolve);
  const fetcher = () => {
    calls += 1;
    return calls === 1 ? olderResponse : newerResponse;
  };

  try {
    const older = fetchSurplusModels({ apiKey: "surplus-cache-race-test-key", force: true, fetcher });
    const newer = fetchSurplusModels({ apiKey: "surplus-cache-race-test-key", force: true, fetcher });
    assert.equal(calls, 2);
    resolveNewer(Response.json({ data: [{ id: "new-surplus-model" }] }));
    assert.deepEqual((await newer)?.models.map((model) => model.id), ["new-surplus-model"]);
    resolveOlder(Response.json({ data: [{ id: "old-surplus-model" }] }));
    await older;
    const cached = await fetchSurplusModels({ apiKey: "surplus-cache-race-test-key", fetcher });
    assert.deepEqual(cached?.models.map((model) => model.id), ["new-surplus-model"]);
    assert.equal(calls, 2);
  } finally {
    resetSurplusModelsCacheForTest();
  }
});

Deno.test("paid-provider model refresh failures use a cooldown", async () => {
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();
  const originalNow = Date.now;
  const originalMeteredKey = Deno.env.get("METERED_API_KEY");
  const baseNow = originalNow();
  Deno.env.set("METERED_API_KEY", "metered-cache-backoff-test-key");
  Date.now = () => baseNow;

  try {
    await fetchMeteredModels({
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{ id: "stale-metered-model", owned_by: "openlux", supported_endpoint_types: ["openai-response"] }],
        })),
    });
    await fetchSurplusModels({
      apiKey: "surplus-cache-backoff-test-key",
      force: true,
      fetcher: () => Promise.resolve(Response.json({ data: [{ id: "stale-surplus-model" }] })),
    });
    Date.now = () => baseNow + Math.max(METERED_MODELS_CACHE_TTL_MS, SURPLUS_MODELS_CACHE_TTL_MS) + 1;
    let meteredFailures = 0;
    let surplusFailures = 0;
    const failingMeteredFetcher = () => {
      meteredFailures += 1;
      return Promise.resolve(new Response("unavailable", { status: 503 }));
    };
    const failingSurplusFetcher = () => {
      surplusFailures += 1;
      return Promise.resolve(new Response("unavailable", { status: 503 }));
    };

    await fetchMeteredModels({ fetcher: failingMeteredFetcher });
    await fetchMeteredModels({ fetcher: failingMeteredFetcher });
    await fetchSurplusModels({ apiKey: "surplus-cache-backoff-test-key", fetcher: failingSurplusFetcher });
    await fetchSurplusModels({ apiKey: "surplus-cache-backoff-test-key", fetcher: failingSurplusFetcher });
    assert.equal(meteredFailures, 1);
    assert.equal(surplusFailures, 1);
  } finally {
    Date.now = originalNow;
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredKey);
  }
});

Deno.test("codex catalog: expired paid-provider caches schedule background refreshes", async () => {
  seedBaseState("0.200.0");
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalMeteredKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusKey = Deno.env.get("SURPLUS_API_KEY");
  const baseNow = originalNow();
  const staleNow = baseNow + Math.max(METERED_MODELS_CACHE_TTL_MS, SURPLUS_MODELS_CACHE_TTL_MS) + 1;
  let meteredRefreshes = 0;
  let surplusRefreshes = 0;
  let resolveMeteredRefresh!: (response: Response) => void;
  let resolveSurplusRefresh!: (response: Response) => void;
  const meteredRefreshResponse = new Promise<Response>((resolve) => resolveMeteredRefresh = resolve);
  const surplusRefreshResponse = new Promise<Response>((resolve) => resolveSurplusRefresh = resolve);
  Deno.env.set("METERED_API_KEY", "metered-catalog-stale-test-key");
  Deno.env.set("SURPLUS_API_KEY", "surplus-catalog-stale-test-key");
  Date.now = () => baseNow;
  await fetchMeteredModels({
    force: true,
    fetcher: () =>
      Promise.resolve(Response.json({
        data: [{ id: "stale-openlux-model", owned_by: "openlux", supported_endpoint_types: ["openai-response"] }],
      })),
  });
  await fetchSurplusModels({
    apiKey: "surplus-catalog-stale-test-key",
    force: true,
    fetcher: () => Promise.resolve(Response.json({ data: [{ id: "stale-surplus-model" }] })),
  });
  Date.now = () => staleNow;
  setMeteredModelsFetchForTest((input) => {
    if (String(input) !== "https://api.openlux.ai/v1/models") {
      return Promise.resolve(new Response("unexpected Metered URL", { status: 500 }));
    }
    meteredRefreshes += 1;
    return meteredRefreshResponse;
  });
  globalThis.fetch = (input) => {
    const url = String(input);
    if (url === "https://api.openlux.ai/v1/models") {
      meteredRefreshes += 1;
      return Promise.resolve(Response.json({
        data: [{ id: "fresh-openlux-model", owned_by: "openlux", supported_endpoint_types: ["openai-response"] }],
      }));
    }
    if (url === "https://api.surplusintelligence.ai/v1/models") {
      surplusRefreshes += 1;
      return surplusRefreshResponse;
    }
    const version = new URL(url).searchParams.get("client_version") ?? "missing";
    return Promise.resolve(new Response(catalogBody(version), { headers: { "Content-Type": "application/json" } }));
  };

  try {
    const response = await handleCodexCatalogModels(request("0.202.0"), "0.202.0");
    assert.equal(response.status, 200);
    const concurrent = await handleCodexCatalogModels(request("0.202.0"), "0.202.0");
    assert.equal(concurrent.status, 200);
    assert.equal(meteredRefreshes, 1);
    assert.equal(surplusRefreshes, 1);

    resolveMeteredRefresh(Response.json({
      data: [{ id: "fresh-openlux-model", owned_by: "openlux", supported_endpoint_types: ["openai-response"] }],
    }));
    resolveSurplusRefresh(Response.json({ data: [{ id: "fresh-surplus-model" }] }));
    await new Promise((resolve) => setTimeout(resolve, 0));

    const refreshed = await handleCodexCatalogModels(request("0.202.0"), "0.202.0");
    const payload = await refreshed.json() as { models: Array<{ slug?: string }> };
    const slugs = payload.models.map((model) => model.slug);
    assert.equal(slugs.includes("fresh-openlux-model"), true);
    assert.equal(slugs.includes("fresh-surplus-model"), true);
  } finally {
    Date.now = originalNow;
    globalThis.fetch = originalFetch;
    setMeteredModelsFetchForTest(null);
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredKey);
    if (originalSurplusKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusKey);
  }
});
