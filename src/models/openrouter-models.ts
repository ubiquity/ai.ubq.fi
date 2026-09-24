import { isRecord } from "../utils.ts";

/**
 * OpenRouter's public catalog is the gateway's third-party enrichment source for
 * per-model capabilities. It is unauthenticated, so a deployment with no
 * OpenRouter credential still gets the data, and it publishes what the curated
 * tables used to guess: the context window, the maximum context window, and the
 * reasoning efforts a model actually accepts.
 *
 * Two rules keep this source honest:
 *
 * - It never outranks a first-party statement. The uploaded Codex catalog stays
 *   authoritative for every id Codex serves, and a serving provider's own
 *   discovery row outranks it too. See `src/model_metadata.ts` for the order.
 * - The request path never waits on it. Reads are cached-only, so a slow or
 *   unavailable third party degrades a model to "unknown" instead of stalling
 *   `/uos/models/catalog`.
 */
export const OPENROUTER_BASE_URL = "https://openrouter.ai";
const OPENROUTER_MODELS_URL = `${OPENROUTER_BASE_URL}/api/v1/models`;
export const OPENROUTER_FETCH_TIMEOUT_MS = 10_000;
export const OPENROUTER_MODELS_CACHE_TTL_MS = 5 * 60_000;
const OPENROUTER_MODELS_FAILURE_BACKOFF_MS = 30_000;

export type OpenRouterReasoning = Readonly<{
  supported_efforts: readonly string[];
  default_effort: string | null;
  /**
   * True when the upstream always reasons. It qualifies `none`: an effort list
   * without `none` on a mandatory model means "cannot be disabled", while the
   * same list on an optional model means "OpenRouter did not enumerate it".
   */
  mandatory: boolean;
}>;

export type OpenRouterModelMetadata = Readonly<{
  /** Upstream id, always `author/slug`. */
  id: string;
  context_window_tokens: number | null;
  max_context_window_tokens: number | null;
  reasoning: OpenRouterReasoning | null;
}>;

export type OpenRouterModelsSnapshot = Readonly<{
  models: readonly OpenRouterModelMetadata[];
  updated_at_ms: number;
}>;

export type OpenRouterFetch = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;

/**
 * Ids are joined by an ordered set of key tiers rather than one normalized
 * string, because a wrong join silently publishes another model's capabilities.
 * A gateway id such as `xiaomi-mimo-v2-5` and its upstream `xiaomi/mimo-v2.5`
 * differ only in punctuation, while `codex-auto-review` has no upstream row at
 * all and must stay unknown. Each tier is consulted in order and a tier whose key
 * is claimed by more than one upstream model is skipped, so an ambiguous match
 * degrades to unknown instead of guessing.
 */
type OpenRouterMatchIndex = Readonly<{
  exactId: ReadonlyMap<string, OpenRouterModelMetadata | null>;
  exactSlug: ReadonlyMap<string, OpenRouterModelMetadata | null>;
  normalizedId: ReadonlyMap<string, OpenRouterModelMetadata | null>;
  normalizedSlug: ReadonlyMap<string, OpenRouterModelMetadata | null>;
  alias: ReadonlyMap<string, OpenRouterModelMetadata | null>;
  /** Base keys of `:free`/`:batch` rows, consulted last, for a gateway id with only that marker. */
  base: ReadonlyMap<string, OpenRouterModelMetadata | null>;
}>;

const nonEmptyString = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  return trimmed ? trimmed : null;
};

const positiveTokenCount = (value: unknown): number | null => (typeof value === "number" && Number.isSafeInteger(value) && value > 0 ? value : null);

/** The wider of a model's own window and the window its default provider reports. */
const widerWindow = (context: number | null, providerContext: number | null): number | null => {
  if (context === null) return providerContext;
  if (providerContext === null) return context;
  return Math.max(context, providerContext);
};

const parseReasoning = (value: unknown): OpenRouterReasoning | null => {
  if (!isRecord(value)) return null;
  const efforts = Array.isArray(value.supported_efforts)
    ? value.supported_efforts.map((entry) => nonEmptyString(entry)).filter((entry): entry is string => entry !== null)
    : [];
  const defaultEffort = nonEmptyString(value.default_effort);
  if (!efforts.length && !defaultEffort) return null;
  return { supported_efforts: [...new Set(efforts)], default_effort: defaultEffort, mandatory: value.mandatory === true };
};

const openRouterModelFromUpstream = (value: unknown): OpenRouterModelMetadata | null => {
  if (!isRecord(value)) return null;
  const id = nonEmptyString(value.id);
  if (!id) return null;
  const context = positiveTokenCount(value.context_length);
  const topProvider = isRecord(value.top_provider) ? value.top_provider : null;
  const providerContext = positiveTokenCount(topProvider?.context_length);
  return {
    id,
    context_window_tokens: context,
    max_context_window_tokens: widerWindow(context, providerContext),
    reasoning: parseReasoning(value.reasoning),
  };
};

export const openRouterModelsFromPayload = (payload: unknown): OpenRouterModelMetadata[] => {
  if (!isRecord(payload)) return [];
  const data = Array.isArray(payload.data) ? payload.data : null;
  if (!data) return [];
  const models: OpenRouterModelMetadata[] = [];
  const seen = new Set<string>();
  for (const value of data) {
    const model = openRouterModelFromUpstream(value);
    if (!model || seen.has(model.id)) continue;
    seen.add(model.id);
    models.push(model);
  }
  return models;
};

/** Lowercase alphanumerics joined by single dashes: `z-ai/glm-5.3` and `z-ai-glm-5-3` agree. */
const normalizeModelKey = (value: string): string =>
  value
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-/, "")
    .replace(/-$/, "");

/**
 * `~author/model-latest` ids are rolling aliases of a canonical model. They are
 * indexed only under their alias form so the canonical row always wins a tie,
 * and they still resolve ids that exist nowhere else, such as the official
 * `deepseek-flash`.
 */
const aliasKeyOf = (id: string): string | null => {
  if (!id.startsWith("~")) return null;
  const withoutPrefix = id.slice(1);
  const slug = withoutPrefix.split("/").at(-1) ?? "";
  const strippedSlug = slug.replace(/[-:]latest$/, "");
  return strippedSlug ? normalizeModelKey(strippedSlug) : null;
};

const claim = (tier: Map<string, OpenRouterModelMetadata | null>, key: string, model: OpenRouterModelMetadata): void => {
  if (!key) return;
  const existing = tier.get(key);
  if (existing === undefined) {
    tier.set(key, model);
    return;
  }
  // A key claimed by two upstream models cannot identify either of them.
  if (existing?.id !== model.id) tier.set(key, null);
};

/**
 * A trailing billing or processing marker (`:free`, `:batch`) names the same
 * model under different terms, so its base row still describes the capabilities.
 * Only these two markers are stripped: `preview` and dated suffixes name
 * different snapshots and must not be folded together.
 */
const billingMarkerSuffix = /[-:](?:free|batch)$/;

const baseKeyOf = (slug: string): string | null => {
  const stripped = slug.replace(billingMarkerSuffix, "");
  return stripped === slug ? null : normalizeModelKey(stripped);
};

const buildMatchIndex = (models: readonly OpenRouterModelMetadata[]): OpenRouterMatchIndex => {
  const exactId = new Map<string, OpenRouterModelMetadata | null>();
  const exactSlug = new Map<string, OpenRouterModelMetadata | null>();
  const normalizedId = new Map<string, OpenRouterModelMetadata | null>();
  const normalizedSlug = new Map<string, OpenRouterModelMetadata | null>();
  const alias = new Map<string, OpenRouterModelMetadata | null>();
  const base = new Map<string, OpenRouterModelMetadata | null>();
  for (const model of models) {
    const slug = model.id.split("/").at(-1) ?? model.id;
    claim(exactId, model.id.toLowerCase(), model);
    claim(exactSlug, slug.toLowerCase(), model);
    claim(normalizedId, normalizeModelKey(model.id), model);
    claim(normalizedSlug, normalizeModelKey(slug), model);
    const aliasKey = aliasKeyOf(model.id);
    if (aliasKey) claim(alias, aliasKey, model);
    const baseKey = baseKeyOf(slug);
    if (baseKey) claim(base, baseKey, model);
  }
  return { exactId, exactSlug, normalizedId, normalizedSlug, alias, base };
};

const lookupTier = (tier: ReadonlyMap<string, OpenRouterModelMetadata | null>, key: string): OpenRouterModelMetadata | null => tier.get(key) ?? null;

export const matchOpenRouterModel = (
  models: readonly OpenRouterModelMetadata[],
  modelId: string,
  index?: OpenRouterMatchIndex
): OpenRouterModelMetadata | null => {
  const target = nonEmptyString(modelId);
  if (!target) return null;
  const tiers = index ?? buildMatchIndex(models);
  const slug = target.split("/").at(-1) ?? target;
  // A gateway id may carry the billing marker itself (`hy3-free` where upstream
  // lists `tencent/hy3`), so the marker-stripped slug is tried last.
  const strippedSlug = slug.replace(billingMarkerSuffix, "");
  const stripped =
    strippedSlug === slug
      ? null
      : (lookupTier(tiers.exactSlug, strippedSlug.toLowerCase()) ?? lookupTier(tiers.normalizedSlug, normalizeModelKey(strippedSlug)));
  return (
    lookupTier(tiers.exactId, target.toLowerCase()) ??
    lookupTier(tiers.exactSlug, slug.toLowerCase()) ??
    lookupTier(tiers.normalizedId, normalizeModelKey(target)) ??
    lookupTier(tiers.normalizedSlug, normalizeModelKey(slug)) ??
    lookupTier(tiers.alias, normalizeModelKey(slug)) ??
    lookupTier(tiers.base, normalizeModelKey(slug)) ??
    stripped ??
    null
  );
};

let openRouterModelsCache: OpenRouterModelsSnapshot | null = null;
let openRouterModelsIndex: OpenRouterMatchIndex | null = null;
let openRouterModelsFetchInFlight: Promise<OpenRouterModelsSnapshot | null> | null = null;
let openRouterModelsRetryAfterMs = 0;
let openRouterModelsFetchForTest: OpenRouterFetch | null = null;
const defaultOpenRouterFetch: OpenRouterFetch = globalThis.fetch;

export const resetOpenRouterModelsCacheForTest = (): void => {
  openRouterModelsCache = null;
  openRouterModelsIndex = null;
  openRouterModelsFetchInFlight = null;
  openRouterModelsRetryAfterMs = 0;
};

export const setOpenRouterModelsFetchForTest = (fetcher: OpenRouterFetch | null): void => {
  openRouterModelsFetchForTest = fetcher;
};

const boundedOpenRouterSignal = (signal: AbortSignal | undefined): AbortSignal => {
  const timeout = AbortSignal.timeout(OPENROUTER_FETCH_TIMEOUT_MS);
  return signal ? AbortSignal.any([signal, timeout]) : timeout;
};

const publishOpenRouterSnapshot = (models: OpenRouterModelMetadata[]): OpenRouterModelsSnapshot => {
  const snapshot: OpenRouterModelsSnapshot = { models, updated_at_ms: Date.now() };
  openRouterModelsCache = snapshot;
  openRouterModelsIndex = buildMatchIndex(models);
  openRouterModelsRetryAfterMs = 0;
  return snapshot;
};

export const fetchOpenRouterModels = async (
  options: Readonly<{ fetcher?: OpenRouterFetch; signal?: AbortSignal; force?: boolean; cachedOnly?: boolean }> = {}
): Promise<OpenRouterModelsSnapshot | null> => {
  if (!options.force && openRouterModelsCache && Date.now() - openRouterModelsCache.updated_at_ms < OPENROUTER_MODELS_CACHE_TTL_MS) {
    return openRouterModelsCache;
  }
  if (!options.force && Date.now() < openRouterModelsRetryAfterMs) return openRouterModelsCache;
  if (options.cachedOnly) return openRouterModelsCache;
  const fetcher = options.fetcher ?? openRouterModelsFetchForTest ?? (globalThis.fetch === defaultOpenRouterFetch ? defaultOpenRouterFetch : null);
  if (!fetcher) return openRouterModelsCache;
  const shouldCoalesce = !options.force && options.fetcher === undefined && options.signal === undefined;
  if (shouldCoalesce && openRouterModelsFetchInFlight) return await openRouterModelsFetchInFlight;

  const request = (async (): Promise<OpenRouterModelsSnapshot | null> => {
    const signal = boundedOpenRouterSignal(options.signal);
    try {
      const response = await fetcher(OPENROUTER_MODELS_URL, { headers: { Accept: "application/json" }, signal });
      if (!response.ok) {
        await response.body?.cancel().catch(() => {});
        throw new Error(`OpenRouter models request failed with HTTP ${response.status}`);
      }
      const models = openRouterModelsFromPayload(await response.json());
      // An empty catalog is a failed refresh, not an empty world: keep the last
      // good snapshot so a schema change upstream cannot blank the page.
      if (!models.length) throw new Error("OpenRouter models response carried no models");
      return publishOpenRouterSnapshot(models);
    } catch {
      openRouterModelsRetryAfterMs = Date.now() + OPENROUTER_MODELS_FAILURE_BACKOFF_MS;
      return openRouterModelsCache;
    }
  })();
  if (shouldCoalesce) openRouterModelsFetchInFlight = request;
  try {
    return await request;
  } finally {
    if (openRouterModelsFetchInFlight === request) openRouterModelsFetchInFlight = null;
  }
};

/**
 * Start a refresh without waiting for it. The catalog builder calls this so the
 * first page load after a cold start begins populating the cache while it serves
 * whatever is already known.
 */
export const warmOpenRouterModels = (): void => {
  void fetchOpenRouterModels().catch(() => {});
};

/** The enriched metadata for a model id, from cache only, or null when unknown. */
export const openRouterMetadataFor = (modelId: string): OpenRouterModelMetadata | null => {
  if (!openRouterModelsCache) return null;
  return matchOpenRouterModel(openRouterModelsCache.models, modelId, openRouterModelsIndex ?? undefined);
};

export const openRouterModelsSnapshot = (): OpenRouterModelsSnapshot | null => openRouterModelsCache;
