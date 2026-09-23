import { fetchCodexModels, loadFullCodexModelsSnapshot } from "./codex.ts";
import { loadCodexModelsWhitelist, filterWhitelistedCatalogModels } from "./codex_models_whitelist.ts";
import { parseCodexClientVersion } from "./codex_models.ts";
import { openaiError } from "./http.ts";
import { getKv } from "./kv.ts";

import { sha256Hex } from "./utils.ts";

import { fetchMeteredModels, METERED_MODELS_CACHE_TTL_MS } from "./metered.ts";
import type { recordSentinelProviderDegradationFromEnvironment } from "./sentinel_incident_outbox.ts";
import { fetchSurplusModels, SURPLUS_MODELS_CACHE_TTL_MS } from "./surplus.ts";

import { warmOpenRouterModels } from "./openrouter_models.ts";
import { isProviderEnabled, loadProviderSelectionCached, type ProviderSelection } from "./provider_selection.ts";

import { CODEX_CATALOG_FRESH_MS } from "./codex_catalog_types.ts";
import type { LoadedCodexCatalog } from "./codex_catalog_types.ts";
import {
  acquireRefreshLease,
  authGenerationIsCurrent,
  getAuthGeneration,
  loadCatalog,
  loadCurrentGenerationCatalog,
  parseCatalogBody,
  releaseRefreshLease,
  startRefreshLeaseHeartbeat,
  storeCodexCatalog,
  waitForColdCatalog,
} from "./codex_catalog_store.ts";
import {
  catalogModelIds,
  codexSnapshotRecords,
  deepSeekOfficialCodexModels,
  etagMatches,
  lithosCodexModels,
  maybeUpdateNormalizedSnapshot,
  meteredCodexModelRecord,
  uniqueResponsesModels,
  withDeepSeekOfficialModels,
  withLithosModels,
} from "./codex_catalog_models.ts";

const catalogBodyEtag = async (body: string, catalog: LoadedCodexCatalog): Promise<string | null> =>
  body === catalog.body ? catalog.metadata.etag : `"uos-catalog-${(await sha256Hex(body)).slice(0, 32)}"`;

/** Answer with the stored catalog alone, honoring the request's conditional headers. */
const catalogOnlyResponse = (catalog: LoadedCodexCatalog, req: Request, headers: Headers): Response => {
  if (catalog.metadata.etag) headers.set("ETag", catalog.metadata.etag);
  if (etagMatches(req.headers.get("If-None-Match"), catalog.metadata.etag)) {
    return new Response(null, { status: 304, headers });
  }
  return new Response(catalog.body, { status: 200, headers });
};

/** Refresh a paid-tier model list in the background once its cache entry expired. */
const refreshExpiredModelList = (nowMs: number, updatedAtMs: number, ttlMs: number, refresh: () => Promise<unknown>): void => {
  if (nowMs - updatedAtMs >= ttlMs) void refresh().catch(() => {});
};

/** The catalogs of the providers the operator still has switched on. */
const enabledPaidCatalogSources = async (selection: ProviderSelection | null, options: Readonly<{ force?: boolean }> = {}) => {
  const openluxEnabled = isProviderEnabled("openlux", selection);
  const surplusEnabled = isProviderEnabled("surplus", selection);
  if (options.force === true) {
    return await Promise.all([
      openluxEnabled ? fetchMeteredModels({ force: true }) : Promise.resolve(null),
      surplusEnabled ? fetchSurplusModels({ force: true }) : Promise.resolve(null),
    ]);
  }
  const [cachedMetered, cachedSurplus] = await Promise.all([
    openluxEnabled ? fetchMeteredModels({ cachedOnly: true }) : Promise.resolve(null),
    surplusEnabled ? fetchSurplusModels({ cachedOnly: true }) : Promise.resolve(null),
  ]);
  return await Promise.all([
    openluxEnabled ? (cachedMetered ?? fetchMeteredModels()) : Promise.resolve(null),
    surplusEnabled ? (cachedSurplus ?? fetchSurplusModels()) : Promise.resolve(null),
  ]);
};

const catalogResponse = async (catalog: LoadedCodexCatalog, req: Request, cacheState: string): Promise<Response> => {
  // Rows this response appends are resolved dynamically, so start an enrichment
  // refresh without waiting for it; the stored catalog already carries Codex's
  // own metadata for every id it lists.
  warmOpenRouterModels();
  const headers = new Headers({
    "Content-Type": catalog.metadata.content_type,
    "Cache-Control": "private, max-age=300",
    "x-uos-upstream": "chatgpt_codex",
    "x-uos-cache": cacheState,
  });
  const selection = await loadProviderSelectionCached();
  const codexEnabled = isProviderEnabled("codex", selection);
  const deepSeekEnabled = isProviderEnabled("deepseek", selection);
  const lithosEnabled = isProviderEnabled("lithos", selection);
  const [metered, surplus] = await enabledPaidCatalogSources(selection);
  const nowMs = Date.now();
  if (metered) refreshExpiredModelList(nowMs, metered.updated_at_ms, METERED_MODELS_CACHE_TTL_MS, fetchMeteredModels);
  if (surplus) refreshExpiredModelList(nowMs, surplus.updated_at_ms, SURPLUS_MODELS_CACHE_TTL_MS, fetchSurplusModels);
  const paidModels = uniqueResponsesModels([...(metered?.models ?? []), ...(surplus?.models ?? [])]);
  // The stored catalog body is Codex's own, so it can only answer for a Codex
  // provider that is still switched on.
  if (!paidModels.length && !deepSeekEnabled && !lithosEnabled && codexEnabled) return catalogOnlyResponse(catalog, req, headers);
  const parsed = {
    ...catalog.parsed,
    models: codexEnabled && Array.isArray(catalog.parsed.models) ? [...catalog.parsed.models] : [],
  };
  const seen = catalogModelIds(parsed.models);
  const codexRecords = codexSnapshotRecords(await loadFullCodexModelsSnapshot());
  for (const model of paidModels) {
    if (seen.has(model.id)) continue;
    parsed.models.push(meteredCodexModelRecord(model, codexRecords.get(model.id) ?? null));
    seen.add(model.id);
  }
  // The official ids are appended first so an operator whitelist still has the
  // final say over every advertised model, this route included.
  parsed.models = deepSeekEnabled ? withDeepSeekOfficialModels(parsed.models) : parsed.models;
  parsed.models = lithosEnabled ? withLithosModels(parsed.models) : parsed.models;
  const catalogKv = await getKv();
  const catalogWhitelist = catalogKv ? await loadCodexModelsWhitelist(catalogKv) : null;
  parsed.models = filterWhitelistedCatalogModels(parsed.models, catalogWhitelist);
  const body = JSON.stringify(parsed);
  const etag = await catalogBodyEtag(body, catalog);
  if (etag) headers.set("ETag", etag);
  if (etagMatches(req.headers.get("If-None-Match"), etag)) {
    return new Response(null, { status: 304, headers });
  }
  return new Response(body, { status: 200, headers });
};

const meteredCatalogResponse = async (selection: ProviderSelection | null): Promise<Response | null> => {
  const [metered, surplus] = await enabledPaidCatalogSources(selection, { force: true });
  const paidModels = uniqueResponsesModels([...(metered?.models ?? []), ...(surplus?.models ?? [])]);
  const configured = [
    ...(isProviderEnabled("deepseek", selection) ? deepSeekOfficialCodexModels() : []),
    ...(isProviderEnabled("lithos", selection) ? lithosCodexModels() : []),
  ];
  if (!paidModels.length && !configured.length) return null;
  // This path answers without a stored catalog, so the Codex snapshot is the only
  // place a Codex-served id's real window can come from.
  const codexRecords = codexSnapshotRecords(await loadFullCodexModelsSnapshot());
  return new Response(
    JSON.stringify({
      models: withLithosModels(withDeepSeekOfficialModels(paidModels.map((model) => meteredCodexModelRecord(model, codexRecords.get(model.id) ?? null)))),
    }),
    {
      status: 200,
      headers: {
        "Content-Type": "application/json",
        "Cache-Control": "private, max-age=300",
        "x-uos-upstream": "metered",
      },
    }
  );
};

type CodexCatalogDependencies = Readonly<{
  now?: () => number;
  recordSentinelDegradation?: typeof recordSentinelProviderDegradationFromEnvironment;
}>;

const recordCatalogDegradation = async (version: string, dependencies: CodexCatalogDependencies): Promise<void> => {
  try {
    await dependencies.recordSentinelDegradation?.(dependencies.now?.() ?? Date.now());
  } catch (error) {
    console.error(`[ai.ubq.fi] Sentinel catalog degradation record failed for ${version}:`, error);
  }
};

/** State shared by the helpers of one lease-holding catalog refresh. */
type CodexCatalogRefreshContext = Readonly<{
  kv: Deno.Kv;
  req: Request;
  version: string;
  authGeneration: string;
  cached: LoadedCodexCatalog | null;
  nowMs: number;
  leaseLost: () => boolean;
  observeProviderDegradation: () => void;
  recordObservedProviderDegradation: () => Promise<void>;
}>;

/** The upstream catalog a refresh attempt parsed, plus the headers it arrived with. */
type RefreshedCatalogPayload = Readonly<{
  body: string;
  parsed: Record<string, unknown>;
  contentType: string | null;
  etag: string | null;
}>;

/** The upstream catalog body and whether reading it failed. */
type CodexUpstreamBodyRead = Readonly<{ body: string; readFailed: boolean }>;

/** Answer with the metered catalog, or report the Codex catalog as unavailable. */
const meteredCatalogOrError = async (message: string): Promise<Response> =>
  (await meteredCatalogResponse(await loadProviderSelectionCached())) ?? openaiError(502, message, "codex_catalog_unavailable");

/** Serve the catalog of the current authentication generation after a rotation. */
const rotatedCatalogResponse = async (kv: Deno.Kv, req: Request, version: string): Promise<Response> => {
  const replacement = await loadCurrentGenerationCatalog(kv, version).catch(() => null);
  if (replacement) return catalogResponse(replacement, req, "rotated");
  return openaiError(502, "Codex authentication changed during catalog refresh", "codex_catalog_unavailable");
};

/** Fall back to the cached, rotated, or metered catalog when a refresh step fails. */
const staleOrRotatedOrUnavailable = async (context: CodexCatalogRefreshContext, unavailableMessage: string): Promise<Response> => {
  if (context.cached && (await authGenerationIsCurrent(context.kv, context.authGeneration))) {
    return catalogResponse(context.cached, context.req, "stale");
  }
  const replacement = await loadCurrentGenerationCatalog(context.kv, context.version).catch(() => null);
  if (replacement) return catalogResponse(replacement, context.req, "rotated");
  return meteredCatalogOrError(unavailableMessage);
};

/** Read the upstream catalog body, reporting a failed read instead of throwing. */
const readUpstreamCatalogBody = async (upstream: Response, version: string): Promise<CodexUpstreamBodyRead> => {
  let readFailed = false;
  const body = await upstream.text().catch((error: unknown) => {
    readFailed = true;
    console.error(`[ai.ubq.fi] Codex catalog response read failed for ${version}:`, error);
    return "";
  });
  return { body, readFailed };
};

/** Re-store the catalog the upstream confirmed with a 304, or fall back to a replacement. */
const revalidateCachedCatalog = async (context: CodexCatalogRefreshContext, cached: LoadedCodexCatalog): Promise<Response> => {
  if (context.leaseLost() || !(await authGenerationIsCurrent(context.kv, context.authGeneration))) {
    return rotatedCatalogResponse(context.kv, context.req, context.version);
  }
  const revalidated = await storeCodexCatalog(context.kv, {
    clientVersion: context.version,
    authGeneration: context.authGeneration,
    body: cached.body,
    etag: cached.metadata.etag,
    contentType: cached.metadata.content_type,
    fetchedAtMs: context.nowMs,
  });
  if (!revalidated || !(await authGenerationIsCurrent(context.kv, context.authGeneration))) {
    return rotatedCatalogResponse(context.kv, context.req, context.version);
  }
  const refreshed = await loadCatalog(context.kv, context.version, context.authGeneration, context.nowMs);
  return catalogResponse(refreshed ?? cached, context.req, "revalidated");
};

/** Store a freshly parsed upstream catalog and answer with what the cache now holds. */
const storeRefreshedCatalog = async (context: CodexCatalogRefreshContext, payload: RefreshedCatalogPayload): Promise<Response> => {
  if (context.leaseLost() || !(await authGenerationIsCurrent(context.kv, context.authGeneration))) {
    return rotatedCatalogResponse(context.kv, context.req, context.version);
  }
  const stored = await storeCodexCatalog(context.kv, {
    clientVersion: context.version,
    authGeneration: context.authGeneration,
    body: payload.body,
    etag: payload.etag,
    contentType: payload.contentType,
    fetchedAtMs: context.nowMs,
  });
  if (!stored) return staleOrRotatedOrUnavailable(context, "Codex model catalog could not be cached");
  await maybeUpdateNormalizedSnapshot(context.kv, context.version, context.authGeneration, payload.parsed, context.nowMs).catch((error: unknown) => {
    console.error(`[ai.ubq.fi] Codex normalized snapshot update failed for ${context.version}:`, error);
  });
  if (!(await authGenerationIsCurrent(context.kv, context.authGeneration))) {
    return rotatedCatalogResponse(context.kv, context.req, context.version);
  }
  const storedCatalog = await loadCatalog(context.kv, context.version, context.authGeneration, context.nowMs);
  if (storedCatalog) return catalogResponse(storedCatalog, context.req, "miss");
  return meteredCatalogOrError("Codex model catalog could not be read after caching");
};

/** Run one upstream refresh for a caller that holds the catalog refresh lease. */
const refreshCodexCatalog = async (context: CodexCatalogRefreshContext): Promise<Response> => {
  const upstream = await fetchCodexModels({
    clientVersion: context.version,
    ifNoneMatch: context.cached?.metadata.etag ?? null,
    onProviderTransportFailure: () => {
      context.observeProviderDegradation();
    },
  });
  if (upstream.status >= 500 && upstream.status <= 599) {
    context.observeProviderDegradation();
  }
  await context.recordObservedProviderDegradation();
  if (upstream.status === 304 && context.cached) {
    return revalidateCachedCatalog(context, context.cached);
  }

  const contentType = upstream.headers.get("Content-Type");
  const { body, readFailed } = await readUpstreamCatalogBody(upstream, context.version);
  if (readFailed) {
    context.observeProviderDegradation();
    await context.recordObservedProviderDegradation();
  }
  const parsed = contentType?.toLowerCase().includes("application/json") ? parseCatalogBody(body) : null;
  if (!upstream.ok || !parsed) {
    console.error(`[ai.ubq.fi] Codex catalog refresh failed for ${context.version}: upstream ${upstream.status} ${body.slice(0, 240)}`);
    return staleOrRotatedOrUnavailable(context, "Codex upstream did not return a valid model catalog");
  }
  return storeRefreshedCatalog(context, { body, parsed, contentType, etag: upstream.headers.get("ETag") });
};

/** Cache state label for a catalog recovered after a failed refresh. */
const recoveredCacheState = (generationCurrent: boolean, cached: LoadedCodexCatalog | null): string => {
  if (!generationCurrent) return "rotated";
  return cached ? "stale" : "miss";
};

/** Recover the best available catalog after a refresh threw. */
const recoverFailedCatalogRefresh = async (context: CodexCatalogRefreshContext): Promise<Response> => {
  const generationCurrent = await authGenerationIsCurrent(context.kv, context.authGeneration).catch(() => false);
  const recovered = generationCurrent
    ? (context.cached ?? (await loadCatalog(context.kv, context.version, context.authGeneration, Date.now()).catch(() => null)))
    : await loadCurrentGenerationCatalog(context.kv, context.version).catch(() => null);
  if (!recovered) return meteredCatalogOrError("Codex upstream model catalog is unavailable");
  return catalogResponse(recovered, context.req, recoveredCacheState(generationCurrent, context.cached));
};

export const handleCodexCatalogModels = async (req: Request, rawVersion: string, dependencies: CodexCatalogDependencies = {}): Promise<Response> => {
  const version = rawVersion.trim();
  if (!parseCodexClientVersion(version)) {
    return openaiError(400, "client_version must be an exact X.Y.Z version", "invalid_client_version", {
      param: "client_version",
    });
  }
  const kv = await getKv();
  if (!kv) {
    return meteredCatalogOrError("Codex model catalog cache is unavailable");
  }

  let authGeneration: string;
  try {
    authGeneration = await getAuthGeneration(kv);
  } catch (error) {
    console.error("[ai.ubq.fi] Codex catalog generation initialization failed:", error);
    return meteredCatalogOrError("Codex model catalog cache is unavailable");
  }
  const nowMs = Date.now();
  const cached = await loadCatalog(kv, version, authGeneration, nowMs).catch((error: unknown) => {
    console.error(`[ai.ubq.fi] Codex catalog cache read failed for ${version}:`, error);
    return null;
  });
  if (cached && nowMs - cached.metadata.fetched_at_ms < CODEX_CATALOG_FRESH_MS) {
    return catalogResponse(cached, req, "hit");
  }

  const leaseOwner = crypto.randomUUID();
  const acquired = await acquireRefreshLease(kv, version, leaseOwner, nowMs).catch((error: unknown) => {
    console.error(`[ai.ubq.fi] Codex catalog lease acquisition failed for ${version}:`, error);
    return false;
  });
  if (!acquired) {
    if (cached) return catalogResponse(cached, req, "stale");
    const waited = await waitForColdCatalog(kv, version).catch((error: unknown) => {
      console.error(`[ai.ubq.fi] Codex catalog cold-cache wait failed for ${version}:`, error);
      return null;
    });
    if (waited) return catalogResponse(waited, req, "wait");
    return meteredCatalogOrError("Codex model catalog refresh is already in progress");
  }

  const leaseHeartbeat = startRefreshLeaseHeartbeat(kv, version, leaseOwner);
  let providerDegradationObserved = false;
  let providerDegradationRecorded = false;
  const recordObservedProviderDegradation = async (): Promise<void> => {
    if (!providerDegradationObserved || providerDegradationRecorded) return;
    providerDegradationRecorded = true;
    await recordCatalogDegradation(version, dependencies);
  };
  const context: CodexCatalogRefreshContext = {
    kv,
    req,
    version,
    authGeneration,
    cached,
    nowMs,
    leaseLost: () => leaseHeartbeat.lost(),
    observeProviderDegradation: () => {
      providerDegradationObserved = true;
    },
    recordObservedProviderDegradation,
  };
  try {
    return await refreshCodexCatalog(context);
  } catch (error) {
    console.error(`[ai.ubq.fi] Codex catalog refresh failed for ${version}:`, error);
    await recordObservedProviderDegradation();
    return await recoverFailedCatalogRefresh(context);
  } finally {
    await leaseHeartbeat.stop();
    await releaseRefreshLease(kv, version, leaseOwner);
  }
};

export const getCatalogClientVersion = (req: Request): string | null => {
  const values = new URL(req.url).searchParams.getAll("client_version");
  if (values.length === 1) return values[0];
  if (values.length === 0) return null;
  return "";
};
