import {
  CODEX_AUTH_POOL_KV_KEY,
  CODEX_MODELS_KV_KEY,
  type CodexModelsSnapshot,
  fetchCodexModels,
  preserveCodexDefaultModel,
} from "./codex.ts";
import {
  CODEX_CHATGPT_PROMPT_CACHE_PROVIDER,
  compareCodexClientVersions,
  getUniqueCodexModelBySlug,
  isCodexModelPromptCacheScopeExperimentEligible,
  isConcretePromptCacheScope,
  mergeCodexModelPromptCacheCapabilities,
  normalizeCodexModelsPayload,
  parseCodexClientVersion,
  type PromptCacheScope,
  withCodexModelPromptCacheScope,
} from "./codex_models.ts";
import { openaiError } from "./http.ts";
import { getKv } from "./kv.ts";
import {
  buildRuntimeConfig,
  cacheRuntimeConfig,
  normalizeRuntimeConfig,
  RUNTIME_CONFIG_V2_KEY,
  type RuntimeConfigV2,
} from "./runtime_config.ts";
import { getString, isRecord, sha256Hex } from "./utils.ts";
import { fetchMeteredModels } from "./metered.ts";
import { fetchSurplusModels } from "./surplus.ts";

export const CODEX_CATALOG_FRESH_MS = 5 * 60_000;
export const CODEX_CATALOG_RETENTION_MS = 24 * 60 * 60_000;
export const CODEX_CATALOG_REFRESH_LEASE_MS = 15_000;
export const CODEX_CATALOG_COLD_WAIT_MS = 5_000;
export const CODEX_CATALOG_CHUNK_BYTES = 55_000;
export const CODEX_CATALOG_MAX_VERSIONS = 32;
const PROMPT_CACHE_SCOPE_PROMOTION_LEASE_MS = 120_000;

export const CODEX_CATALOG_AUTH_GENERATION_KEY = ["ubq_ai", "codex_catalog_auth_generation"] as const;
export const CODEX_CATALOG_PREFIX = ["ubq_ai", "codex_catalog"] as const;
export const CODEX_CATALOG_CHUNK_PREFIX = ["ubq_ai", "codex_catalog_chunk"] as const;
export const CODEX_CATALOG_LEASE_PREFIX = ["ubq_ai", "codex_catalog_refresh_lease"] as const;

type CodexCatalogMetadata = Readonly<{
  client_version: string;
  auth_generation: string;
  body_generation: string;
  content_type: string;
  etag: string | null;
  fetched_at_ms: number;
  chunk_count: number;
  compressed_bytes: number;
  body_bytes: number;
  sha256: string;
}>;

type LoadedCodexCatalog = Readonly<{
  metadata: CodexCatalogMetadata;
  body: string;
  parsed: Record<string, unknown>;
}>;

type RefreshLease = Readonly<{ owner: string; lease_until_ms: number }>;
const catalogMemo = new Map<string, LoadedCodexCatalog>();

export type PromptCacheScopePromotionLease = Readonly<{
  key: Deno.KvKey;
  owner: string;
}>;

export type PromptCacheScopePromotionResult =
  | Readonly<{ status: "promoted" }>
  | Readonly<{
    status: "inconclusive";
    reason:
      | "invalid_scope"
      | "lease_lost"
      | "snapshot_unavailable"
      | "runtime_unavailable"
      | "model_drift"
      | "auth_pool_drift"
      | "capability_changed"
      | "catalog_drift"
      | "runtime_drift"
      | "cas_conflict";
  }>;

const catalogMemoKey = (metadata: CodexCatalogMetadata): string =>
  `${metadata.client_version}:${metadata.auth_generation}:${metadata.body_generation}`;

const deleteCatalogMemoVersion = (version: string): void => {
  for (const [key, catalog] of catalogMemo) {
    if (catalog.metadata.client_version === version) catalogMemo.delete(key);
  }
};

const memoizeCatalog = (catalog: LoadedCodexCatalog): void => {
  const key = catalogMemoKey(catalog.metadata);
  // Map insertion order is the LRU order. Reinsert hits and replacements so
  // the first entry is always the least recently used catalog.
  catalogMemo.delete(key);
  catalogMemo.set(key, catalog);
  while (catalogMemo.size > CODEX_CATALOG_MAX_VERSIONS) {
    const leastRecentlyUsed = catalogMemo.keys().next().value;
    if (leastRecentlyUsed === undefined) break;
    catalogMemo.delete(leastRecentlyUsed);
  }
};

export const resetCodexCatalogMemoForTest = (): void => catalogMemo.clear();

export const getCodexCatalogMemoVersionsForTest = (): string[] =>
  [...catalogMemo.values()].map((catalog) => catalog.metadata.client_version);

const metadataKey = (version: string): Deno.KvKey => [...CODEX_CATALOG_PREFIX, version];
const chunkKey = (version: string, generation: string, index: number): Deno.KvKey => [
  ...CODEX_CATALOG_CHUNK_PREFIX,
  version,
  generation,
  index,
];
const leaseKey = (version: string): Deno.KvKey => [...CODEX_CATALOG_LEASE_PREFIX, version];

const deleteCatalogChunks = async (
  kv: Deno.Kv,
  version: string,
  generation: string,
  chunkCount: number,
): Promise<void> => {
  for (let index = 0; index < chunkCount; index += 1) {
    await kv.delete(chunkKey(version, generation, index));
  }
};

const pruneCatalogVersions = async (kv: Deno.Kv, currentVersion: string): Promise<void> => {
  const catalogs: Array<{ entry: Deno.KvEntry<CodexCatalogMetadata>; metadata: CodexCatalogMetadata }> = [];
  for await (const entry of kv.list<CodexCatalogMetadata>({ prefix: CODEX_CATALOG_PREFIX })) {
    if (isCatalogMetadata(entry.value)) catalogs.push({ entry, metadata: entry.value });
  }
  if (catalogs.length <= CODEX_CATALOG_MAX_VERSIONS) return;

  catalogs.sort((left, right) => left.metadata.fetched_at_ms - right.metadata.fetched_at_ms);
  let remaining = catalogs.length;
  for (const { entry, metadata } of catalogs) {
    if (remaining <= CODEX_CATALOG_MAX_VERSIONS) break;
    if (metadata.client_version === currentVersion) continue;
    const deleted = await kv.atomic().check(entry).delete(entry.key).commit();
    if (!deleted.ok) continue;
    deleteCatalogMemoVersion(metadata.client_version);
    await deleteCatalogChunks(kv, metadata.client_version, metadata.body_generation, metadata.chunk_count);
    remaining -= 1;
  }
};

const gzip = async (body: string): Promise<Uint8Array> => {
  const stream = new Blob([body]).stream().pipeThrough(new CompressionStream("gzip"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
};

const gunzip = async (bytes: Uint8Array): Promise<string> => {
  const stream = new Blob([bytes.slice().buffer]).stream().pipeThrough(new DecompressionStream("gzip"));
  return await new Response(stream).text();
};

const parseCatalogBody = (body: string): Record<string, unknown> | null => {
  let parsed: unknown;
  try {
    parsed = JSON.parse(body) as unknown;
  } catch {
    return null;
  }
  if (!isRecord(parsed) || !Array.isArray(parsed.models) || parsed.models.length === 0) return null;
  if (
    !parsed.models.every((model) => {
      if (!isRecord(model)) return false;
      const id = getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name);
      return Boolean(id?.trim());
    })
  ) return null;
  return parsed;
};

const isCatalogMetadata = (value: unknown): value is CodexCatalogMetadata =>
  isRecord(value) &&
  typeof value.client_version === "string" &&
  typeof value.auth_generation === "string" &&
  typeof value.body_generation === "string" &&
  typeof value.content_type === "string" &&
  (value.etag === null || typeof value.etag === "string") &&
  typeof value.fetched_at_ms === "number" &&
  Number.isFinite(value.fetched_at_ms) &&
  typeof value.chunk_count === "number" &&
  Number.isSafeInteger(value.chunk_count) &&
  value.chunk_count > 0 &&
  typeof value.compressed_bytes === "number" &&
  typeof value.body_bytes === "number" &&
  typeof value.sha256 === "string";

const loadCatalog = async (
  kv: Deno.Kv,
  version: string,
  authGeneration: string,
  nowMs: number,
): Promise<LoadedCodexCatalog | null> => {
  const entry = await kv.get<CodexCatalogMetadata>(metadataKey(version));
  const metadata = entry.value;
  if (!isCatalogMetadata(metadata) || metadata.auth_generation !== authGeneration) return null;
  if (nowMs - metadata.fetched_at_ms >= CODEX_CATALOG_RETENTION_MS) return null;
  const memoKey = catalogMemoKey(metadata);
  const cached = catalogMemo.get(memoKey);
  if (cached) {
    memoizeCatalog(cached);
    return cached;
  }

  const entries = await Promise.all(
    Array.from(
      { length: metadata.chunk_count },
      (_, index) => kv.get<Uint8Array>(chunkKey(version, metadata.body_generation, index)),
    ),
  );
  const chunks: Uint8Array[] = [];
  let totalBytes = 0;
  for (const entry of entries) {
    const chunk = entry.value;
    if (!(chunk instanceof Uint8Array)) return null;
    chunks.push(chunk);
    totalBytes += chunk.byteLength;
  }
  if (totalBytes !== metadata.compressed_bytes) return null;
  const compressed = new Uint8Array(totalBytes);
  let offset = 0;
  for (const chunk of chunks) {
    compressed.set(chunk, offset);
    offset += chunk.byteLength;
  }

  try {
    const body = await gunzip(compressed);
    if (new TextEncoder().encode(body).byteLength !== metadata.body_bytes) return null;
    if (await sha256Hex(body) !== metadata.sha256) return null;
    const parsed = parseCatalogBody(body);
    if (!parsed) return null;
    const loaded = { metadata, body, parsed };
    memoizeCatalog(loaded);
    return loaded;
  } catch {
    return null;
  }
};

export const storeCodexCatalog = async (
  kv: Deno.Kv,
  input: Readonly<{
    clientVersion: string;
    authGeneration: string;
    body: string;
    etag?: string | null;
    contentType?: string | null;
    fetchedAtMs?: number;
  }>,
): Promise<boolean> => {
  if (!parseCodexClientVersion(input.clientVersion) || !parseCatalogBody(input.body)) return false;
  const fetchedAtMs = input.fetchedAtMs ?? Date.now();
  const compressed = await gzip(input.body);
  const bodyGeneration = crypto.randomUUID();
  const chunkCount = Math.ceil(compressed.byteLength / CODEX_CATALOG_CHUNK_BYTES);
  const expireIn = Math.max(1, fetchedAtMs + CODEX_CATALOG_RETENTION_MS - Date.now());

  for (let index = 0; index < chunkCount; index += 1) {
    const start = index * CODEX_CATALOG_CHUNK_BYTES;
    await kv.set(
      chunkKey(input.clientVersion, bodyGeneration, index),
      compressed.slice(start, start + CODEX_CATALOG_CHUNK_BYTES),
      { expireIn },
    );
  }

  const metadata: CodexCatalogMetadata = {
    client_version: input.clientVersion,
    auth_generation: input.authGeneration,
    body_generation: bodyGeneration,
    content_type: input.contentType?.trim() || "application/json",
    etag: input.etag?.trim() || null,
    fetched_at_ms: fetchedAtMs,
    chunk_count: chunkCount,
    compressed_bytes: compressed.byteLength,
    body_bytes: new TextEncoder().encode(input.body).byteLength,
    sha256: await sha256Hex(input.body),
  };
  const metadataEntry = await kv.get<CodexCatalogMetadata>(metadataKey(input.clientVersion));
  const generation = await kv.get<string>(CODEX_CATALOG_AUTH_GENERATION_KEY);
  if (generation.value !== input.authGeneration) {
    await deleteCatalogChunks(kv, input.clientVersion, bodyGeneration, chunkCount);
    return false;
  }
  const published = (await kv.atomic()
    .check(generation)
    .check(metadataEntry)
    .set(metadataKey(input.clientVersion), metadata, { expireIn })
    .commit()).ok;
  if (!published) {
    await deleteCatalogChunks(kv, input.clientVersion, bodyGeneration, chunkCount);
    return false;
  }
  deleteCatalogMemoVersion(input.clientVersion);
  const previous = metadataEntry.value;
  if (isCatalogMetadata(previous) && previous.body_generation !== bodyGeneration) {
    await deleteCatalogChunks(kv, input.clientVersion, previous.body_generation, previous.chunk_count);
  }
  await pruneCatalogVersions(kv, input.clientVersion).catch((error) => {
    console.error("[ai.ubq.fi] Codex catalog version pruning failed:", error);
  });
  return true;
};

const getAuthGeneration = async (kv: Deno.Kv): Promise<string> => {
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const entry = await kv.get<string>(CODEX_CATALOG_AUTH_GENERATION_KEY);
    if (entry.value) return entry.value;
    const generation = crypto.randomUUID();
    const commit = await kv.atomic().check(entry).set(CODEX_CATALOG_AUTH_GENERATION_KEY, generation).commit();
    if (commit.ok) return generation;
  }
  const entry = await kv.get<string>(CODEX_CATALOG_AUTH_GENERATION_KEY);
  if (entry.value) return entry.value;
  throw new Error("Deno KV could not initialize the Codex catalog auth generation");
};

const acquireRefreshLease = async (
  kv: Deno.Kv,
  version: string,
  owner: string,
  nowMs: number,
): Promise<boolean> => {
  const key = leaseKey(version);
  const entry = await kv.get<RefreshLease>(key);
  if (entry.value && entry.value.lease_until_ms > nowMs) return false;
  const lease: RefreshLease = { owner, lease_until_ms: nowMs + CODEX_CATALOG_REFRESH_LEASE_MS };
  return (await kv.atomic().check(entry).set(key, lease, { expireIn: CODEX_CATALOG_REFRESH_LEASE_MS * 2 }).commit()).ok;
};

const renewRefreshLease = async (kv: Deno.Kv, version: string, owner: string): Promise<boolean> => {
  const key = leaseKey(version);
  const entry = await kv.get<RefreshLease>(key);
  if (entry.value?.owner !== owner) return false;
  const lease: RefreshLease = { owner, lease_until_ms: Date.now() + CODEX_CATALOG_REFRESH_LEASE_MS };
  return (await kv.atomic().check(entry).set(key, lease, { expireIn: CODEX_CATALOG_REFRESH_LEASE_MS * 2 }).commit()).ok;
};

const startRefreshLeaseHeartbeat = (kv: Deno.Kv, version: string, owner: string): {
  lost: () => boolean;
  stop: () => Promise<void>;
} => {
  let stopped = false;
  let lost = false;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let renewal: Promise<void> | null = null;
  const schedule = (): void => {
    timer = setTimeout(() => {
      renewal = renewRefreshLease(kv, version, owner)
        .then((renewed) => {
          if (!renewed) lost = true;
        })
        .catch((error) => {
          lost = true;
          console.error(`[ai.ubq.fi] Codex catalog lease renewal failed for ${version}:`, error);
        })
        .finally(() => {
          renewal = null;
          if (!stopped && !lost) schedule();
        });
    }, Math.floor(CODEX_CATALOG_REFRESH_LEASE_MS / 3));
  };
  schedule();
  return {
    lost: () => lost,
    stop: async () => {
      stopped = true;
      if (timer !== null) clearTimeout(timer);
      await renewal;
    },
  };
};

const releaseRefreshLease = async (kv: Deno.Kv, version: string, owner: string): Promise<void> => {
  try {
    const key = leaseKey(version);
    const entry = await kv.get<RefreshLease>(key);
    if (entry.value?.owner === owner) await kv.atomic().check(entry).delete(key).commit();
  } catch (error) {
    console.error(`[ai.ubq.fi] Codex catalog lease release failed for ${version}:`, error);
  }
};

const authGenerationIsCurrent = async (kv: Deno.Kv, expected: string): Promise<boolean> =>
  (await kv.get<string>(CODEX_CATALOG_AUTH_GENERATION_KEY)).value === expected;

const loadCurrentGenerationCatalog = async (kv: Deno.Kv, version: string): Promise<LoadedCodexCatalog | null> => {
  const generation = (await kv.get<string>(CODEX_CATALOG_AUTH_GENERATION_KEY)).value;
  return generation ? await loadCatalog(kv, version, generation, Date.now()) : null;
};

const waitForColdCatalog = async (kv: Deno.Kv, version: string): Promise<LoadedCodexCatalog | null> => {
  const deadline = Date.now() + CODEX_CATALOG_COLD_WAIT_MS;
  while (Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 50));
    const authGeneration = (await kv.get<string>(CODEX_CATALOG_AUTH_GENERATION_KEY)).value;
    if (!authGeneration) continue;
    const catalog = await loadCatalog(kv, version, authGeneration, Date.now());
    if (catalog) return catalog;
  }
  return null;
};

const ownsPromptCacheScopePromotionLease = (value: unknown, owner: string): boolean =>
  isRecord(value) && value.owner === owner && typeof value.lease_until_ms === "number" &&
  Number.isFinite(value.lease_until_ms) && value.lease_until_ms > Date.now();

/**
 * Publish a concrete live scope observation without replacing catalog-owned
 * controls or another model's evidence. The catalog and compact runtime view
 * are committed together, and the warm runtime cache changes only after that
 * compare-and-swap succeeds.
 */
export const promoteCodexPromptCacheScope = async (
  kv: Deno.Kv,
  input: Readonly<{
    model: string;
    scope: PromptCacheScope;
    lease: PromptCacheScopePromotionLease;
    authPoolVersionstamp: string;
    /**
     * The scope runner binds a campaign to one exact full-catalog revision.
     * A model may be non-default, but it may never publish against a catalog
     * revision other than the one whose controls it actually probed.
     */
    catalogVersionstamp: string;
    /** The same fence for the compact runtime/default-model configuration. */
    runtimeVersionstamp: string;
  }>,
): Promise<PromptCacheScopePromotionResult> => {
  const model = input.model.trim();
  if (
    !model || !input.authPoolVersionstamp.trim() || !input.catalogVersionstamp.trim() ||
    !input.runtimeVersionstamp.trim() || input.scope.effective_model?.trim() !== model ||
    !isConcretePromptCacheScope(input.scope, 3)
  ) {
    return { status: "inconclusive", reason: "invalid_scope" };
  }

  for (let attempt = 0; attempt < 3; attempt += 1) {
    const [snapshotEntry, runtimeEntry, leaseEntry, authPoolEntry] = await Promise.all([
      kv.get<CodexModelsSnapshot>(CODEX_MODELS_KV_KEY, { consistency: "strong" }),
      kv.get<RuntimeConfigV2>(RUNTIME_CONFIG_V2_KEY, { consistency: "strong" }),
      kv.get(input.lease.key, { consistency: "strong" }),
      kv.get(CODEX_AUTH_POOL_KV_KEY, { consistency: "strong" }),
    ]);
    if (!ownsPromptCacheScopePromotionLease(leaseEntry.value, input.lease.owner)) {
      return { status: "inconclusive", reason: "lease_lost" };
    }
    if (authPoolEntry.versionstamp !== input.authPoolVersionstamp) {
      return { status: "inconclusive", reason: "auth_pool_drift" };
    }

    const snapshot = snapshotEntry.value;
    if (
      !snapshot || !Array.isArray(snapshot.models) || !getString(snapshot.source)?.trim() ||
      !Number.isSafeInteger(snapshot.updated_at_ms) || snapshot.updated_at_ms <= 0
    ) {
      return { status: "inconclusive", reason: "snapshot_unavailable" };
    }
    if (snapshotEntry.versionstamp !== input.catalogVersionstamp) {
      return { status: "inconclusive", reason: "catalog_drift" };
    }
    if (!getUniqueCodexModelBySlug(snapshot, model)) {
      return { status: "inconclusive", reason: "model_drift" };
    }
    if (!isCodexModelPromptCacheScopeExperimentEligible(snapshot, model)) {
      return { status: "inconclusive", reason: "capability_changed" };
    }

    const currentRuntime = normalizeRuntimeConfig(runtimeEntry.value);
    if (!currentRuntime) return { status: "inconclusive", reason: "runtime_unavailable" };
    if (runtimeEntry.versionstamp !== input.runtimeVersionstamp) {
      return { status: "inconclusive", reason: "runtime_drift" };
    }
    // Scope evidence belongs to a catalog model, not necessarily the active
    // default. Rebuilding the compact view must retain the configured default
    // verbatim when the probed model is non-default.
    const defaultModel = preserveCodexDefaultModel(snapshot, currentRuntime.default_model);
    if (!defaultModel) return { status: "inconclusive", reason: "runtime_unavailable" };

    const nextSnapshot = withCodexModelPromptCacheScope(
      snapshot,
      model,
      CODEX_CHATGPT_PROMPT_CACHE_PROVIDER,
      input.scope,
    );
    if (!nextSnapshot) return { status: "inconclusive", reason: "model_drift" };

    let nextRuntime: RuntimeConfigV2;
    try {
      nextRuntime = buildRuntimeConfig(nextSnapshot, {
        defaultModel,
        defaultReasoningEffort: currentRuntime.default_reasoning_effort,
        nowMs: input.scope.verified_at_ms,
      });
    } catch {
      return { status: "inconclusive", reason: "runtime_unavailable" };
    }
    const renewedLease: RefreshLease = {
      owner: input.lease.owner,
      lease_until_ms: Date.now() + PROMPT_CACHE_SCOPE_PROMOTION_LEASE_MS,
    };

    const commit = await kv.atomic()
      .check(snapshotEntry)
      .check(runtimeEntry)
      .check(leaseEntry)
      .check(authPoolEntry)
      .set(CODEX_MODELS_KV_KEY, nextSnapshot)
      .set(RUNTIME_CONFIG_V2_KEY, nextRuntime)
      .set(input.lease.key, renewedLease, { expireIn: PROMPT_CACHE_SCOPE_PROMOTION_LEASE_MS * 2 })
      .commit();
    if (!commit.ok) continue;
    cacheRuntimeConfig(nextRuntime);
    return { status: "promoted" };
  }
  return { status: "inconclusive", reason: "cas_conflict" };
};

const maybeUpdateNormalizedSnapshot = async (
  kv: Deno.Kv,
  version: string,
  authGeneration: string,
  parsed: Record<string, unknown>,
  updatedAtMs: number,
): Promise<void> => {
  const next = normalizeCodexModelsPayload(parsed, {
    source: "chatgpt_codex",
    clientVersion: version,
    updatedAtMs,
  });
  if (!next) return;
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const generation = await kv.get<string>(CODEX_CATALOG_AUTH_GENERATION_KEY);
    if (generation.value !== authGeneration) return;
    const [current, runtimeEntry] = await Promise.all([
      kv.get<CodexModelsSnapshot>(CODEX_MODELS_KV_KEY, { consistency: "strong" }),
      kv.get<RuntimeConfigV2>(RUNTIME_CONFIG_V2_KEY, { consistency: "strong" }),
    ]);
    const currentVersion = current.value?.client_version;
    if (currentVersion) {
      const comparison = compareCodexClientVersions(version, currentVersion);
      if (comparison === null || comparison < 0) return;
    }
    const nextWithPromptCacheEvidence = mergeCodexModelPromptCacheCapabilities(next, current.value);
    const currentRuntime = normalizeRuntimeConfig(runtimeEntry.value);
    const nextRuntime = buildRuntimeConfig(nextWithPromptCacheEvidence, {
      defaultModel: preserveCodexDefaultModel(nextWithPromptCacheEvidence, currentRuntime?.default_model),
      defaultReasoningEffort: currentRuntime?.default_reasoning_effort,
      nowMs: updatedAtMs,
    });
    const commit = await kv.atomic()
      .check(generation)
      .check(current)
      .check(runtimeEntry)
      .set(CODEX_MODELS_KV_KEY, nextWithPromptCacheEvidence)
      .set(RUNTIME_CONFIG_V2_KEY, nextRuntime)
      .commit();
    if (commit.ok) {
      cacheRuntimeConfig(nextRuntime);
      return;
    }
  }
};

const etagMatches = (requestValue: string | null, etag: string | null): boolean => {
  if (!requestValue || !etag) return false;
  return requestValue.split(",").some((candidate) => candidate.trim() === "*" || candidate.trim() === etag);
};

const meteredCodexModelRecord = (
  model: Readonly<{
    id: string;
    description?: string;
    owned_by: string;
    supported_endpoint_types: readonly string[];
  }>,
) => ({
  slug: model.id,
  display_name: model.id,
  description: model.description,
  owned_by: model.owned_by,
  supported_endpoint_types: [...model.supported_endpoint_types],
  supported_reasoning_levels: [{ effort: "none", description: "No reasoning" }],
  default_reasoning_level: "none",
  shell_type: "shell_command",
  visibility: "list",
  supported_in_api: true,
  priority: 1000,
  availability_nux: null,
  upgrade: null,
  base_instructions: "",
  support_verbosity: false,
  default_verbosity: null,
  apply_patch_tool_type: null,
  web_search_tool_type: "text",
  truncation_policy: { mode: "tokens", limit: 10000 },
  supports_parallel_tool_calls: false,
  experimental_supported_tools: [],
});

const catalogResponse = async (catalog: LoadedCodexCatalog, req: Request, cacheState: string): Promise<Response> => {
  const headers = new Headers({
    "Content-Type": catalog.metadata.content_type,
    "Cache-Control": "private, max-age=300",
    "x-uos-upstream": "chatgpt_codex",
    "x-uos-cache": cacheState,
  });
  const [metered, surplus] = await Promise.all([
    fetchMeteredModels({ cachedOnly: true }),
    fetchSurplusModels({ cachedOnly: true }),
  ]);
  if (!metered) void fetchMeteredModels().catch(() => {});
  if (!surplus) void fetchSurplusModels().catch(() => {});
  const paidModels = [...(metered?.models ?? []), ...(surplus?.models ?? [])];
  if (!paidModels.length) {
    if (catalog.metadata.etag) headers.set("ETag", catalog.metadata.etag);
    if (etagMatches(req.headers.get("If-None-Match"), catalog.metadata.etag)) {
      return new Response(null, { status: 304, headers });
    }
    return new Response(catalog.body, { status: 200, headers });
  }
  const parsed = {
    ...catalog.parsed,
    models: [...(Array.isArray(catalog.parsed.models) ? catalog.parsed.models : [])],
  };
  const seen = new Set(
    parsed.models.map((model) => {
      if (!isRecord(model)) return null;
      return (getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name))
        ?.trim() ?? null;
    }).filter((id): id is string => Boolean(id)),
  );
  for (const model of paidModels) {
    if (!model.supported_endpoint_types.includes("openai-response")) continue;
    if (seen.has(model.id)) continue;
    parsed.models.push(meteredCodexModelRecord(model));
    seen.add(model.id);
  }
  const body = JSON.stringify(parsed);
  const etag = body === catalog.body ? catalog.metadata.etag : `"uos-catalog-${(await sha256Hex(body)).slice(0, 32)}"`;
  if (etag) headers.set("ETag", etag);
  if (etagMatches(req.headers.get("If-None-Match"), etag)) {
    return new Response(null, { status: 304, headers });
  }
  return new Response(body, { status: 200, headers });
};

const meteredCatalogResponse = async (): Promise<Response | null> => {
  const [metered, surplus] = await Promise.all([
    fetchMeteredModels({ force: true }),
    fetchSurplusModels({ force: true }),
  ]);
  const paidModels = [...(metered?.models ?? []), ...(surplus?.models ?? [])];
  if (!paidModels.length) return null;
  return new Response(
    JSON.stringify({
      models: paidModels.filter((model) => model.supported_endpoint_types.includes("openai-response"))
        .map(meteredCodexModelRecord),
    }),
    {
      status: 200,
      headers: {
        "Content-Type": "application/json",
        "Cache-Control": "private, max-age=300",
        "x-uos-upstream": "metered",
      },
    },
  );
};

export const handleCodexCatalogModels = async (req: Request, rawVersion: string): Promise<Response> => {
  const version = rawVersion.trim();
  if (!parseCodexClientVersion(version)) {
    return openaiError(400, "client_version must be an exact X.Y.Z version", "invalid_client_version", {
      param: "client_version",
    });
  }
  const kv = await getKv();
  if (!kv) {
    return await meteredCatalogResponse() ??
      openaiError(502, "Codex model catalog cache is unavailable", "codex_catalog_unavailable");
  }

  let authGeneration: string;
  try {
    authGeneration = await getAuthGeneration(kv);
  } catch (error) {
    console.error("[ai.ubq.fi] Codex catalog generation initialization failed:", error);
    return await meteredCatalogResponse() ??
      openaiError(502, "Codex model catalog cache is unavailable", "codex_catalog_unavailable");
  }
  const nowMs = Date.now();
  const cached = await loadCatalog(kv, version, authGeneration, nowMs).catch((error) => {
    console.error(`[ai.ubq.fi] Codex catalog cache read failed for ${version}:`, error);
    return null;
  });
  if (cached && nowMs - cached.metadata.fetched_at_ms < CODEX_CATALOG_FRESH_MS) {
    return catalogResponse(cached, req, "hit");
  }

  const leaseOwner = crypto.randomUUID();
  const acquired = await acquireRefreshLease(kv, version, leaseOwner, nowMs).catch((error) => {
    console.error(`[ai.ubq.fi] Codex catalog lease acquisition failed for ${version}:`, error);
    return false;
  });
  if (!acquired) {
    if (cached) return catalogResponse(cached, req, "stale");
    const waited = await waitForColdCatalog(kv, version).catch((error) => {
      console.error(`[ai.ubq.fi] Codex catalog cold-cache wait failed for ${version}:`, error);
      return null;
    });
    if (waited) return catalogResponse(waited, req, "wait");
    return await meteredCatalogResponse() ??
      openaiError(502, "Codex model catalog refresh is already in progress", "codex_catalog_unavailable");
  }

  const leaseHeartbeat = startRefreshLeaseHeartbeat(kv, version, leaseOwner);
  try {
    const upstream = await fetchCodexModels({
      clientVersion: version,
      ifNoneMatch: cached?.metadata.etag ?? null,
    });
    if (upstream.status === 304 && cached) {
      if (leaseHeartbeat.lost() || !await authGenerationIsCurrent(kv, authGeneration)) {
        const replacement = await loadCurrentGenerationCatalog(kv, version).catch(() => null);
        return replacement
          ? catalogResponse(replacement, req, "rotated")
          : openaiError(502, "Codex authentication changed during catalog refresh", "codex_catalog_unavailable");
      }
      const revalidated = await storeCodexCatalog(kv, {
        clientVersion: version,
        authGeneration,
        body: cached.body,
        etag: cached.metadata.etag,
        contentType: cached.metadata.content_type,
        fetchedAtMs: nowMs,
      });
      if (!revalidated || !await authGenerationIsCurrent(kv, authGeneration)) {
        const replacement = await loadCurrentGenerationCatalog(kv, version).catch(() => null);
        return replacement
          ? catalogResponse(replacement, req, "rotated")
          : openaiError(502, "Codex authentication changed during catalog refresh", "codex_catalog_unavailable");
      }
      const refreshed = await loadCatalog(kv, version, authGeneration, nowMs);
      return catalogResponse(refreshed ?? cached, req, "revalidated");
    }

    const contentType = upstream.headers.get("Content-Type");
    const body = await upstream.text().catch(() => "");
    const parsed = contentType?.toLowerCase().includes("application/json") ? parseCatalogBody(body) : null;
    if (!upstream.ok || !parsed) {
      console.error(
        `[ai.ubq.fi] Codex catalog refresh failed for ${version}: upstream ${upstream.status} ${body.slice(0, 240)}`,
      );
      if (cached && await authGenerationIsCurrent(kv, authGeneration)) {
        return catalogResponse(cached, req, "stale");
      }
      const replacement = await loadCurrentGenerationCatalog(kv, version).catch(() => null);
      return replacement ? catalogResponse(replacement, req, "rotated") : await meteredCatalogResponse() ??
        openaiError(502, "Codex upstream did not return a valid model catalog", "codex_catalog_unavailable");
    }

    if (leaseHeartbeat.lost() || !await authGenerationIsCurrent(kv, authGeneration)) {
      const replacement = await loadCurrentGenerationCatalog(kv, version).catch(() => null);
      return replacement
        ? catalogResponse(replacement, req, "rotated")
        : openaiError(502, "Codex authentication changed during catalog refresh", "codex_catalog_unavailable");
    }

    const stored = await storeCodexCatalog(kv, {
      clientVersion: version,
      authGeneration,
      body,
      etag: upstream.headers.get("ETag"),
      contentType,
      fetchedAtMs: nowMs,
    });
    if (!stored) {
      if (cached && await authGenerationIsCurrent(kv, authGeneration)) {
        return catalogResponse(cached, req, "stale");
      }
      const replacement = await loadCurrentGenerationCatalog(kv, version).catch(() => null);
      return replacement ? catalogResponse(replacement, req, "rotated") : await meteredCatalogResponse() ??
        openaiError(502, "Codex model catalog could not be cached", "codex_catalog_unavailable");
    }
    await maybeUpdateNormalizedSnapshot(kv, version, authGeneration, parsed, nowMs).catch((error) => {
      console.error(`[ai.ubq.fi] Codex normalized snapshot update failed for ${version}:`, error);
    });
    if (!await authGenerationIsCurrent(kv, authGeneration)) {
      const replacement = await loadCurrentGenerationCatalog(kv, version).catch(() => null);
      return replacement
        ? catalogResponse(replacement, req, "rotated")
        : openaiError(502, "Codex authentication changed during catalog refresh", "codex_catalog_unavailable");
    }
    const storedCatalog = await loadCatalog(kv, version, authGeneration, nowMs);
    return storedCatalog ? catalogResponse(storedCatalog, req, "miss") : await meteredCatalogResponse() ??
      openaiError(502, "Codex model catalog could not be read after caching", "codex_catalog_unavailable");
  } catch (error) {
    console.error(`[ai.ubq.fi] Codex catalog refresh failed for ${version}:`, error);
    const generationCurrent = await authGenerationIsCurrent(kv, authGeneration).catch(() => false);
    const recovered = generationCurrent
      ? cached ?? await loadCatalog(kv, version, authGeneration, Date.now()).catch(() => null)
      : await loadCurrentGenerationCatalog(kv, version).catch(() => null);
    return recovered
      ? catalogResponse(recovered, req, generationCurrent ? (cached ? "stale" : "miss") : "rotated")
      : await meteredCatalogResponse() ??
        openaiError(502, "Codex upstream model catalog is unavailable", "codex_catalog_unavailable");
  } finally {
    await leaseHeartbeat.stop();
    await releaseRefreshLease(kv, version, leaseOwner);
  }
};

export const getCatalogClientVersion = (req: Request): string | null => {
  const values = new URL(req.url).searchParams.getAll("client_version");
  return values.length === 1 ? values[0] : values.length === 0 ? null : "";
};
