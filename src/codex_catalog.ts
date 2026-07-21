import { CODEX_MODELS_KV_KEY, type CodexModelsSnapshot, fetchCodexModels } from "./codex.ts";
import { compareCodexClientVersions, normalizeCodexModelsPayload, parseCodexClientVersion } from "./codex_models.ts";
import { openaiError } from "./http.ts";
import { kvPromise } from "./kv.ts";
import { getString, isRecord, sha256Hex } from "./utils.ts";

export const CODEX_CATALOG_FRESH_MS = 5 * 60_000;
export const CODEX_CATALOG_RETENTION_MS = 24 * 60 * 60_000;
export const CODEX_CATALOG_REFRESH_LEASE_MS = 15_000;
export const CODEX_CATALOG_COLD_WAIT_MS = 5_000;
export const CODEX_CATALOG_CHUNK_BYTES = 55_000;

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

const metadataKey = (version: string): Deno.KvKey => [...CODEX_CATALOG_PREFIX, version];
const chunkKey = (version: string, generation: string, index: number): Deno.KvKey => [
  ...CODEX_CATALOG_CHUNK_PREFIX,
  version,
  generation,
  index,
];
const leaseKey = (version: string): Deno.KvKey => [...CODEX_CATALOG_LEASE_PREFIX, version];

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

  const chunks: Uint8Array[] = [];
  let totalBytes = 0;
  for (let index = 0; index < metadata.chunk_count; index += 1) {
    const chunk = (await kv.get<Uint8Array>(chunkKey(version, metadata.body_generation, index))).value;
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
    return parsed ? { metadata, body, parsed } : null;
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
  const generation = await kv.get<string>(CODEX_CATALOG_AUTH_GENERATION_KEY);
  if (generation.value !== input.authGeneration) return false;
  return (await kv.atomic()
    .check(generation)
    .set(metadataKey(input.clientVersion), metadata, { expireIn })
    .commit()).ok;
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
    const current = await kv.get<CodexModelsSnapshot>(CODEX_MODELS_KV_KEY);
    const currentVersion = current.value?.client_version;
    if (currentVersion) {
      const comparison = compareCodexClientVersions(version, currentVersion);
      if (comparison === null || comparison < 0) return;
    }
    const commit = await kv.atomic().check(generation).check(current).set(CODEX_MODELS_KV_KEY, next).commit();
    if (commit.ok) return;
  }
};

const etagMatches = (requestValue: string | null, etag: string | null): boolean => {
  if (!requestValue || !etag) return false;
  return requestValue.split(",").some((candidate) => candidate.trim() === "*" || candidate.trim() === etag);
};

const catalogResponse = (catalog: LoadedCodexCatalog, req: Request, cacheState: string): Response => {
  const headers = new Headers({
    "Content-Type": catalog.metadata.content_type,
    "Cache-Control": "private, max-age=300",
    "x-ubq-upstream": "chatgpt_codex",
    "x-uos-cache": cacheState,
  });
  if (catalog.metadata.etag) headers.set("ETag", catalog.metadata.etag);
  if (etagMatches(req.headers.get("If-None-Match"), catalog.metadata.etag)) {
    return new Response(null, { status: 304, headers });
  }
  return new Response(catalog.body, { status: 200, headers });
};

export const handleCodexCatalogModels = async (req: Request, rawVersion: string): Promise<Response> => {
  const version = rawVersion.trim();
  if (!parseCodexClientVersion(version)) {
    return openaiError(400, "client_version must be an exact X.Y.Z version", "invalid_client_version", {
      param: "client_version",
    });
  }
  const kv = await kvPromise;
  if (!kv) return openaiError(502, "Codex model catalog cache is unavailable", "codex_catalog_unavailable");

  let authGeneration: string;
  try {
    authGeneration = await getAuthGeneration(kv);
  } catch (error) {
    console.error("[ai.ubq.fi] Codex catalog generation initialization failed:", error);
    return openaiError(502, "Codex model catalog cache is unavailable", "codex_catalog_unavailable");
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
    return openaiError(502, "Codex model catalog refresh is already in progress", "codex_catalog_unavailable");
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
      return replacement
        ? catalogResponse(replacement, req, "rotated")
        : openaiError(502, "Codex upstream did not return a valid model catalog", "codex_catalog_unavailable");
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
      return replacement
        ? catalogResponse(replacement, req, "rotated")
        : openaiError(502, "Codex model catalog could not be cached", "codex_catalog_unavailable");
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
    return storedCatalog
      ? catalogResponse(storedCatalog, req, "miss")
      : openaiError(502, "Codex model catalog could not be read after caching", "codex_catalog_unavailable");
  } catch (error) {
    console.error(`[ai.ubq.fi] Codex catalog refresh failed for ${version}:`, error);
    const generationCurrent = await authGenerationIsCurrent(kv, authGeneration).catch(() => false);
    const recovered = generationCurrent
      ? cached ?? await loadCatalog(kv, version, authGeneration, Date.now()).catch(() => null)
      : await loadCurrentGenerationCatalog(kv, version).catch(() => null);
    return recovered
      ? catalogResponse(recovered, req, generationCurrent ? (cached ? "stale" : "miss") : "rotated")
      : openaiError(502, "Codex upstream model catalog is unavailable", "codex_catalog_unavailable");
  } finally {
    await leaseHeartbeat.stop();
    await releaseRefreshLease(kv, version, leaseOwner);
  }
};

export const getCatalogClientVersion = (req: Request): string | null => {
  const values = new URL(req.url).searchParams.getAll("client_version");
  return values.length === 1 ? values[0] : values.length === 0 ? null : "";
};
