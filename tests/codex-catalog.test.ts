import assert from "node:assert/strict";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const kvStore = new Map<string, { value: unknown; versionstamp: string }>();
let versionCounter = 0;

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
    const ops: Array<{ kind: "set" | "delete"; key: Deno.KvKey; value?: unknown }> = [];
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
  CODEX_CATALOG_RETENTION_MS,
  handleCodexCatalogModels,
  storeCodexCatalog,
} = await import("../src/codex_catalog.ts");
const { handleModels } = await import("../src/openai.ts");

const AUTH_GENERATION = "auth-generation-test";
const AUTH_KEY = ["ubq_ai", "codex_auth"] as const;
const SNAPSHOT_KEY = ["ubq_ai", "codex_models"] as const;
const CATALOG_KEY = (version: string): Deno.KvKey => ["ubq_ai", "codex_catalog", version];

const seedBaseState = (snapshotVersion = "0.200.0"): void => {
  kvStore.clear();
  kvStore.set(keyToString(CODEX_CATALOG_AUTH_GENERATION_KEY), {
    value: AUTH_GENERATION,
    versionstamp: nextVersion(),
  });
  kvStore.set(keyToString(AUTH_KEY), {
    value: {
      access_token: "server-access",
      refresh_token: "server-refresh",
      account_id: "server-account",
      updated_at_ms: Date.now(),
    },
    versionstamp: nextVersion(),
  });
  kvStore.set(keyToString(SNAPSHOT_KEY), {
    value: {
      source: "chatgpt_codex",
      client_version: snapshotVersion,
      updated_at_ms: Date.now(),
      models: [{ slug: `snapshot-${snapshotVersion}` }],
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
  await storeCodexCatalog(kvStub, {
    clientVersion: version,
    authGeneration: AUTH_GENERATION,
    body: catalogBody(version),
    etag: '"stale"',
    fetchedAtMs: Date.now() - CODEX_CATALOG_FRESH_MS - 1,
  });
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => Promise.resolve(new Response("temporary failure", { status: 503 }));
  try {
    const stale = await handleCodexCatalogModels(request(version), version);
    assert.equal(stale.status, 200);
    assert.equal(stale.headers.get("x-uos-cache"), "stale");
    assert.equal((await stale.json() as { models: Array<{ slug: string }> }).models[0].slug, `gpt-${version}`);

    const metadata = kvStore.get(keyToString(CATALOG_KEY(version)))?.value as { fetched_at_ms: number };
    metadata.fetched_at_ms = Date.now() - CODEX_CATALOG_RETENTION_MS - 1;
    const expired = await handleCodexCatalogModels(request(version), version);
    assert.equal(expired.status, 502);
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
  globalThis.fetch = () => {
    calls += 1;
    return Promise.resolve(new Response(catalogBody(version), { headers: { "Content-Type": "application/json" } }));
  };
  try {
    const response = await handleCodexCatalogModels(request(version), version);
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-cache"), "stale");
    assert.equal(calls, 0);
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

Deno.test("codex catalog: only same-or-newer clients update the normalized snapshot", async () => {
  seedBaseState("0.200.0");
  const originalFetch = globalThis.fetch;
  globalThis.fetch = (input) => {
    const version = new URL(input instanceof Request ? input.url : input.toString()).searchParams.get(
      "client_version",
    )!;
    return Promise.resolve(
      new Response(catalogBody(version), {
        headers: { "Content-Type": "application/json" },
      }),
    );
  };
  try {
    assert.equal((await handleCodexCatalogModels(request("0.199.0"), "0.199.0")).status, 200);
    const olderSnapshot = kvStore.get(keyToString(SNAPSHOT_KEY))?.value as { client_version: string };
    assert.equal(olderSnapshot.client_version, "0.200.0");

    assert.equal((await handleCodexCatalogModels(request("0.201.0"), "0.201.0")).status, 200);
    const snapshot = kvStore.get(keyToString(SNAPSHOT_KEY))?.value as {
      client_version: string;
      models: unknown[];
    };
    assert.equal(snapshot.client_version, "0.201.0");
    assert.equal(snapshot.models.length, 1);
  } finally {
    globalThis.fetch = originalFetch;
  }
});
