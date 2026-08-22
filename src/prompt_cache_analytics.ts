import { getKv } from "./kv.ts";
import { PROMPT_CACHE_TELEMETRY_PROVIDERS, PROMPT_CACHE_TELEMETRY_ROUTES } from "./prompt_cache_telemetry_gate.ts";
import { RELEASE_GIT_SHA } from "./release.ts";
import { sha256Hex } from "./utils.ts";

/**
 * V2 is a hard namespace cutover. V1 remains outside this prefix and expires
 * according to its existing retention policy; readers never combine it with V2.
 */
export const PROMPT_CACHE_ANALYTICS_KV_PREFIX = ["uos_ai", "prompt_cache_analytics", "v2"] as const;
const LEGACY_PROMPT_CACHE_ANALYTICS_V1_KV_PREFIX = ["uos_ai", "prompt_cache_analytics", "v1"] as const;
export const PROMPT_CACHE_ANALYTICS_BUCKET_MS = 15 * 60_000;
export const PROMPT_CACHE_ANALYTICS_WINDOW_MS = 7 * 24 * 60 * 60_000;
export const PROMPT_CACHE_ANALYTICS_RETENTION_MS = 8 * 24 * 60 * 60_000;
export const PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET = 32;
export const PROMPT_CACHE_ANALYTICS_MAX_GROUP_BY = 2;
export const PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS = 512;

export const PROMPT_CACHE_ANALYTICS_DIMENSIONS = [
  "provider",
  "model",
  "route",
  "key_presence",
  "mode",
  "fallback",
] as const;
export const PROMPT_CACHE_ANALYTICS_MODES = ["implicit", "explicit", "legacy_retention", "unspecified"] as const;
export const PROMPT_CACHE_ANALYTICS_FALLBACKS = ["none", "primary_429", "primary_quota_blocked", "other"] as const;

const RELEASE_SHA = /^[a-f0-9]{7,64}$/i;
const MAX_MODEL_CHARS = 256;
const COUNTERS = [
  "input_tokens",
  "cached_input_tokens",
  "cache_write_input_tokens",
  "cache_write_reported_sample_count",
  "sample_count",
  "request_cache_hit_sample_count",
  "usage_reported_sample_count",
  "usage_invalid_sample_count",
  "dimension_cardinality_limited_sample_count",
] as const;

type Counter = typeof COUNTERS[number];
type PromptCacheAnalyticsProvider = typeof PROMPT_CACHE_TELEMETRY_PROVIDERS[number];
type PromptCacheAnalyticsRoute = typeof PROMPT_CACHE_TELEMETRY_ROUTES[number];
export type PromptCacheAnalyticsDimension = typeof PROMPT_CACHE_ANALYTICS_DIMENSIONS[number];
type PromptCacheAnalyticsMode = typeof PROMPT_CACHE_ANALYTICS_MODES[number];
type PromptCacheAnalyticsFallback = typeof PROMPT_CACHE_ANALYTICS_FALLBACKS[number];

type PromptCacheAnalyticsCohort = Readonly<{
  provider: PromptCacheAnalyticsProvider;
  modelHash: string;
  route: PromptCacheAnalyticsRoute;
  promptCacheKeyPresent: boolean;
  mode: PromptCacheAnalyticsMode;
  fallback: PromptCacheAnalyticsFallback;
}>;

type StoredCounters = Partial<Record<Counter, number>>;
type CounterDeltas = Partial<Record<Counter, bigint>>;

export type PromptCacheAnalyticsEvent = Readonly<{
  provider: string | null;
  /** Raw model text is accepted only long enough to derive an opaque cohort hash. */
  model?: string | null;
  route: string;
  status: number;
  completed: boolean;
  usageTelemetryStatus: string;
  inputTokens: number | null;
  cachedInputTokens: number | null;
  cacheWriteInputTokens: number | null;
  promptCacheKeyPresent?: boolean;
  promptCacheMode?: string | null;
  fallbackReason?: string | null;
}>;

export type PromptCacheAnalyticsOptions = Readonly<{
  kv?: Deno.Kv | null;
  release?: string;
  now?: () => number;
}>;

export type PromptCacheAnalyticsReadOptions =
  & Pick<PromptCacheAnalyticsOptions, "kv" | "now">
  & Readonly<{
    /** Only these bounded, public-safe dimensions may be selected. */
    groupBy?: readonly PromptCacheAnalyticsDimension[];
  }>;

export type PromptCacheAnalyticsRecordResult = Readonly<{
  status: "recorded" | "ignored" | "unavailable";
  reason:
    | "recorded"
    | "recorded_without_usage"
    | "recorded_invalid_usage"
    | "recorded_cardinality_capped"
    | "unknown_release"
    | "not_completed_2xx"
    | "unsupported_provider"
    | "unsupported_route"
    | "kv_unavailable";
  bucket_start_at_ms: number | null;
}>;

export type PromptCacheAnalyticsGroup = Readonly<{
  provider?: PromptCacheAnalyticsProvider;
  /** Domain-separated digest or the bounded literal `unknown`; never raw model text. */
  model_hash?: string;
  route?: PromptCacheAnalyticsRoute;
  prompt_cache_key_present?: boolean;
  mode?: PromptCacheAnalyticsMode;
  fallback?: PromptCacheAnalyticsFallback;
  /** Fixed synthetic group for events beyond the per-bucket cohort cap. */
  cardinality_limited?: true;
}>;

export type PromptCacheAnalyticsBucket = Readonly<{
  bucket_start_at_ms: number;
  bucket_end_at_ms: number;
  input_tokens: number | null;
  cached_input_tokens: number | null;
  cache_write_input_tokens: number | null;
  cache_write_reported_sample_count: number;
  /** Kept for the existing capacity view. This is token_hit_percentage when available. */
  cached_percentage: number | null;
  sample_count: number;
  /** Present for the v2 grouped admin API and absent from the compact capacity projection. */
  group?: PromptCacheAnalyticsGroup;
  token_hit_percentage?: number | null;
  request_cache_hit_sample_count?: number;
  request_hit_percentage?: number | null;
  cache_reads_per_write?: number | null;
  usage_reported_sample_count?: number;
  usage_invalid_sample_count?: number;
  usage_missing_sample_count?: number;
  usage_telemetry_coverage_percentage?: number | null;
  dimension_cardinality_limited_sample_count?: number;
}>;

export type PromptCacheAnalyticsView = Readonly<{
  status: "ready" | "unavailable";
  bucket_ms: number;
  window_start_at_ms: number;
  window_end_at_ms: number;
  group_by: readonly PromptCacheAnalyticsDimension[];
  max_buckets: number;
  /** At least one group is a fixed synthetic cohort after the detail cap. */
  cardinality_limited: boolean;
  /** Response or cohort-detail truncation means the grouped view is incomplete. */
  truncated: boolean;
  buckets: readonly PromptCacheAnalyticsBucket[];
}>;

export type PromptCacheAnalyticsPruneResult = Readonly<{
  status: "pruned" | "unavailable";
  deleted: number;
}>;

export class PromptCacheAnalyticsQueryError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PromptCacheAnalyticsQueryError";
  }
}

const safeNow = (now: () => number): number => {
  const value = Math.trunc(now());
  return Number.isSafeInteger(value) && value >= 0 ? value : Date.now();
};

const alignedBucketStart = (timestamp: number): number =>
  Math.floor(timestamp / PROMPT_CACHE_ANALYTICS_BUCKET_MS) * PROMPT_CACHE_ANALYTICS_BUCKET_MS;

const knownRelease = (value: unknown): boolean => {
  if (typeof value !== "string") return false;
  const release = value.trim();
  return release.toLowerCase() !== "unknown" && RELEASE_SHA.test(release);
};

const safeCounter = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const storedCounter = (value: unknown): number | null => {
  if (typeof value !== "object" || value === null || !("value" in value)) return null;
  const counter = (value as { value?: unknown }).value;
  if (typeof counter !== "bigint" || counter < 0n || counter > BigInt(Number.MAX_SAFE_INTEGER)) return null;
  return Number(counter);
};

const storedCardinality = (value: unknown): number | null =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0 &&
    value <= PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET
    ? value
    : null;

const roundedPercentage = (numerator: number, denominator: number): number | null =>
  denominator === 0 ? null : Math.round((numerator / denominator) * 1_000_000) / 10_000;

const roundedRatio = (numerator: number, denominator: number): number | null =>
  denominator === 0 ? null : Math.round((numerator / denominator) * 10_000) / 10_000;

const hasAsciiControlCharacter = (value: string): boolean => {
  for (const character of value) {
    const code = character.codePointAt(0) ?? 0;
    if (code < 0x20 || code === 0x7f) return true;
  }
  return false;
};

const normalizedModel = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const model = value.trim();
  if (!model || model.length > MAX_MODEL_CHARS || hasAsciiControlCharacter(model)) return null;
  return model;
};

const asProvider = (value: unknown): PromptCacheAnalyticsProvider | null =>
  typeof value === "string" && (PROMPT_CACHE_TELEMETRY_PROVIDERS as readonly string[]).includes(value)
    ? value as PromptCacheAnalyticsProvider
    : null;

const asRoute = (value: unknown): PromptCacheAnalyticsRoute | null =>
  typeof value === "string" && (PROMPT_CACHE_TELEMETRY_ROUTES as readonly string[]).includes(value)
    ? value as PromptCacheAnalyticsRoute
    : null;

const asMode = (value: unknown): PromptCacheAnalyticsMode =>
  typeof value === "string" && (PROMPT_CACHE_ANALYTICS_MODES as readonly string[]).includes(value)
    ? value as PromptCacheAnalyticsMode
    : "unspecified";

const asFallback = (value: unknown): PromptCacheAnalyticsFallback => {
  if (typeof value !== "string" || !value.trim()) return "none";
  return (PROMPT_CACHE_ANALYTICS_FALLBACKS as readonly string[]).includes(value)
    ? value as PromptCacheAnalyticsFallback
    : "other";
};

const recordResult = (
  status: PromptCacheAnalyticsRecordResult["status"],
  reason: PromptCacheAnalyticsRecordResult["reason"],
  bucketStartAtMs: number | null = null,
): PromptCacheAnalyticsRecordResult => ({ status, reason, bucket_start_at_ms: bucketStartAtMs });

const resolveKv = async (options: PromptCacheAnalyticsOptions): Promise<Deno.Kv | null> => {
  try {
    return options.kv === undefined ? await getKv() : options.kv;
  } catch {
    return null;
  }
};

const aggregatePrefix = [...PROMPT_CACHE_ANALYTICS_KV_PREFIX, "all"] as const;
const dimensionPrefix = [...PROMPT_CACHE_ANALYTICS_KV_PREFIX, "dimension"] as const;
const overflowPrefix = [...PROMPT_CACHE_ANALYTICS_KV_PREFIX, "overflow"] as const;
const metaPrefix = [...PROMPT_CACHE_ANALYTICS_KV_PREFIX, "meta"] as const;

export const promptCacheAnalyticsBucketKey = (
  bucketStartAtMs: number,
): Deno.KvKey => [...aggregatePrefix, bucketStartAtMs];

/**
 * This is the aggregate counter helper kept for capacity callers and focused
 * fixtures. Dimension counters remain private to this module.
 */
export const promptCacheAnalyticsCounterKey = (
  bucketStartAtMs: number,
  counter: Counter,
): Deno.KvKey => [...promptCacheAnalyticsBucketKey(bucketStartAtMs), counter];

const dimensionValues = (cohort: PromptCacheAnalyticsCohort): readonly [
  PromptCacheAnalyticsProvider,
  string,
  PromptCacheAnalyticsRoute,
  "keyed" | "unkeyed",
  PromptCacheAnalyticsMode,
  PromptCacheAnalyticsFallback,
] => [
  cohort.provider,
  cohort.modelHash,
  cohort.route,
  cohort.promptCacheKeyPresent ? "keyed" : "unkeyed",
  cohort.mode,
  cohort.fallback,
];

const dimensionCounterKey = (
  bucketStartAtMs: number,
  cohort: PromptCacheAnalyticsCohort,
  counter: Counter,
): Deno.KvKey => [...dimensionPrefix, bucketStartAtMs, ...dimensionValues(cohort), counter];

const dimensionMarkerKey = (
  bucketStartAtMs: number,
  cohort: PromptCacheAnalyticsCohort,
): Deno.KvKey => [...dimensionPrefix, bucketStartAtMs, ...dimensionValues(cohort), "marker"];

const overflowCounterKey = (bucketStartAtMs: number, counter: Counter): Deno.KvKey => [
  ...overflowPrefix,
  bucketStartAtMs,
  counter,
];

const cardinalityKey = (bucketStartAtMs: number): Deno.KvKey => [...metaPrefix, bucketStartAtMs, "cardinality"];

const isKnownCounter = (value: unknown): value is Counter =>
  typeof value === "string" && (COUNTERS as readonly string[]).includes(value);

const incrementCounters = (
  operation: Deno.AtomicOperation,
  keys: (counter: Counter) => Deno.KvKey,
  deltas: CounterDeltas,
): Deno.AtomicOperation => {
  let next = operation;
  for (const counter of COUNTERS) {
    const amount = deltas[counter];
    // A valid upstream zero is presence evidence. In particular, a zero cache
    // read or cache write must not become indistinguishable from a missing
    // field during a later grouped read.
    if (amount === undefined) continue;
    next = next.sum(keys(counter), amount);
  }
  return next;
};

const recordUsage = (
  event: PromptCacheAnalyticsEvent,
): Readonly<{ kind: "reported" | "missing" | "invalid"; deltas: CounterDeltas }> => {
  const base: CounterDeltas = { sample_count: 1n };
  if (event.usageTelemetryStatus === "reported") {
    if (
      safeCounter(event.inputTokens) &&
      safeCounter(event.cachedInputTokens) &&
      event.cachedInputTokens <= event.inputTokens &&
      (event.cacheWriteInputTokens === null || safeCounter(event.cacheWriteInputTokens))
    ) {
      const deltas: CounterDeltas = {
        ...base,
        input_tokens: BigInt(event.inputTokens),
        cached_input_tokens: BigInt(event.cachedInputTokens),
        usage_reported_sample_count: 1n,
      };
      if (event.cachedInputTokens > 0) deltas.request_cache_hit_sample_count = 1n;
      if (event.cacheWriteInputTokens !== null) {
        deltas.cache_write_input_tokens = BigInt(event.cacheWriteInputTokens);
        deltas.cache_write_reported_sample_count = 1n;
      }
      return { kind: "reported", deltas };
    }
    return { kind: "invalid", deltas: { ...base, usage_invalid_sample_count: 1n } };
  }
  if (event.usageTelemetryStatus === "invalid") {
    return { kind: "invalid", deltas: { ...base, usage_invalid_sample_count: 1n } };
  }
  return { kind: "missing", deltas: base };
};

const resolveCohort = async (
  event: PromptCacheAnalyticsEvent,
): Promise<PromptCacheAnalyticsCohort | null> => {
  const provider = asProvider(event.provider);
  const route = asRoute(event.route);
  if (!provider || !route) return null;
  const model = normalizedModel(event.model);
  let modelHash = "unknown";
  if (model) modelHash = await sha256Hex(`uos-prompt-cache-analytics-model-v2\u0000${model}`);
  return {
    provider,
    modelHash,
    route,
    promptCacheKeyPresent: event.promptCacheKeyPresent === true,
    mode: asMode(event.promptCacheMode),
    fallback: asFallback(event.fallbackReason),
  };
};

const commitAggregateAndOverflow = async (
  kv: Deno.Kv,
  bucketStartAtMs: number,
  deltas: CounterDeltas,
): Promise<boolean> => {
  try {
    let operation = incrementCounters(
      kv.atomic(),
      (counter) => overflowCounterKey(bucketStartAtMs, counter),
      deltas,
    );
    operation = incrementCounters(
      operation,
      (counter) => promptCacheAnalyticsCounterKey(bucketStartAtMs, counter),
      deltas,
    );
    return (await operation.commit()).ok;
  } catch {
    return false;
  }
};

/**
 * Adds one completed inference outcome to aggregate and bounded cohort
 * counters. The model is hashed before it reaches a durable KV key. A full
 * bucket never admits more than the fixed number of cohort combinations.
 */
export const recordPromptCacheAnalytics = async (
  event: PromptCacheAnalyticsEvent,
  options: PromptCacheAnalyticsOptions = {},
): Promise<PromptCacheAnalyticsRecordResult> => {
  const release = options.release === undefined ? RELEASE_GIT_SHA : options.release;
  if (!knownRelease(release)) return recordResult("ignored", "unknown_release");
  if (
    event.completed !== true ||
    !Number.isInteger(event.status) ||
    event.status < 200 ||
    event.status >= 300
  ) return recordResult("ignored", "not_completed_2xx");
  if (!asProvider(event.provider)) return recordResult("ignored", "unsupported_provider");
  if (!asRoute(event.route)) return recordResult("ignored", "unsupported_route");

  const nowMs = safeNow(options.now ?? Date.now);
  const bucketStartAtMs = alignedBucketStart(nowMs);
  const kv = await resolveKv(options);
  if (!kv) return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);

  let cohort: PromptCacheAnalyticsCohort;
  try {
    cohort = await resolveCohort(event) as PromptCacheAnalyticsCohort;
  } catch {
    return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
  }
  const usage = recordUsage(event);
  const markerKey = dimensionMarkerKey(bucketStartAtMs, cohort);
  const bucketCardinalityKey = cardinalityKey(bucketStartAtMs);

  for (let attempt = 0; attempt < 4; attempt += 1) {
    try {
      const [marker, cardinality] = await kv.getMany<[boolean, number]>([markerKey, bucketCardinalityKey]);
      if (marker.value === true) {
        let operation = incrementCounters(
          kv.atomic(),
          (counter) => dimensionCounterKey(bucketStartAtMs, cohort, counter),
          usage.deltas,
        );
        operation = incrementCounters(
          operation,
          (counter) => promptCacheAnalyticsCounterKey(bucketStartAtMs, counter),
          usage.deltas,
        );
        if ((await operation.commit()).ok) {
          return recordResult(
            "recorded",
            usage.kind === "reported"
              ? "recorded"
              : usage.kind === "invalid"
              ? "recorded_invalid_usage"
              : "recorded_without_usage",
            bucketStartAtMs,
          );
        }
        continue;
      }

      const cardinalityValue = cardinality.value === null ? 0 : storedCardinality(cardinality.value);
      if (cardinalityValue === null) {
        return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
      }
      if (cardinalityValue >= PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET) {
        const cappedDeltas: CounterDeltas = {
          ...usage.deltas,
          dimension_cardinality_limited_sample_count: 1n,
        };
        if (!await commitAggregateAndOverflow(kv, bucketStartAtMs, cappedDeltas)) {
          return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
        }
        return recordResult("recorded", "recorded_cardinality_capped", bucketStartAtMs);
      }

      const retentionMs = PROMPT_CACHE_ANALYTICS_RETENTION_MS;
      let operation = kv.atomic()
        .check(marker)
        .check(cardinality)
        .set(markerKey, true, { expireIn: retentionMs })
        .set(bucketCardinalityKey, cardinalityValue + 1, { expireIn: retentionMs });
      operation = incrementCounters(
        operation,
        (counter) => dimensionCounterKey(bucketStartAtMs, cohort, counter),
        usage.deltas,
      );
      operation = incrementCounters(
        operation,
        (counter) => promptCacheAnalyticsCounterKey(bucketStartAtMs, counter),
        usage.deltas,
      );
      if ((await operation.commit()).ok) {
        return recordResult(
          "recorded",
          usage.kind === "reported"
            ? "recorded"
            : usage.kind === "invalid"
            ? "recorded_invalid_usage"
            : "recorded_without_usage",
          bucketStartAtMs,
        );
      }
    } catch {
      return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
    }
  }

  return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
};

const storageBucketStart = (key: Deno.KvKey): number | null => {
  const namespace = key[PROMPT_CACHE_ANALYTICS_KV_PREFIX.length];
  const bucketStartAtMs = key[PROMPT_CACHE_ANALYTICS_KV_PREFIX.length + 1];
  if (
    (namespace !== "all" && namespace !== "dimension" && namespace !== "overflow" && namespace !== "meta") ||
    !safeCounter(bucketStartAtMs)
  ) {
    return null;
  }
  return bucketStartAtMs;
};

const legacyStorageBucketStart = (key: Deno.KvKey): number | null => {
  const bucketStartAtMs = key[LEGACY_PROMPT_CACHE_ANALYTICS_V1_KV_PREFIX.length];
  return safeCounter(bucketStartAtMs) ? bucketStartAtMs : null;
};

/** Removes only V2 entries at or beyond the eight-day retention boundary. */
export const prunePromptCacheAnalytics = async (
  options: Pick<PromptCacheAnalyticsOptions, "kv" | "now"> = {},
): Promise<PromptCacheAnalyticsPruneResult> => {
  const nowMs = safeNow(options.now ?? Date.now);
  const cutoffBucketStartAtMs = alignedBucketStart(Math.max(0, nowMs - PROMPT_CACHE_ANALYTICS_RETENTION_MS));
  const kv = await resolveKv(options);
  if (!kv) return { status: "unavailable", deleted: 0 };

  let deleted = 0;
  let batch: Deno.KvKey[] = [];
  const deleteBatch = async (): Promise<void> => {
    const current = batch;
    batch = [];
    await Promise.all(current.map((key) => kv.delete(key)));
    deleted += current.length;
  };
  try {
    for (
      const [prefix, bucketStart] of [
        [PROMPT_CACHE_ANALYTICS_KV_PREFIX, storageBucketStart],
        [LEGACY_PROMPT_CACHE_ANALYTICS_V1_KV_PREFIX, legacyStorageBucketStart],
      ] as const
    ) {
      for await (const entry of kv.list({ prefix })) {
        const bucketStartAtMs = bucketStart(entry.key);
        if (bucketStartAtMs === null || bucketStartAtMs > cutoffBucketStartAtMs) continue;
        batch.push(entry.key);
        if (batch.length === 64) await deleteBatch();
      }
    }
    if (batch.length) await deleteBatch();
    return { status: "pruned", deleted };
  } catch {
    return { status: "unavailable", deleted };
  }
};

const normalizedGroupBy = (value: unknown): readonly PromptCacheAnalyticsDimension[] | null => {
  if (value === undefined) return [];
  if (!Array.isArray(value) || value.length > PROMPT_CACHE_ANALYTICS_MAX_GROUP_BY) return null;
  const groupBy: PromptCacheAnalyticsDimension[] = [];
  for (const dimension of value) {
    if (
      typeof dimension !== "string" ||
      !(PROMPT_CACHE_ANALYTICS_DIMENSIONS as readonly string[]).includes(dimension) ||
      groupBy.includes(dimension as PromptCacheAnalyticsDimension)
    ) return null;
    groupBy.push(dimension as PromptCacheAnalyticsDimension);
  }
  return groupBy;
};

/** Used by the admin boundary before it reads KV. */
export const isValidPromptCacheAnalyticsGroupBy = (
  value: unknown,
): value is readonly PromptCacheAnalyticsDimension[] => normalizedGroupBy(value) !== null;

const parsedDimensionValues = (key: Deno.KvKey): PromptCacheAnalyticsCohort | null => {
  const offset = dimensionPrefix.length;
  if (key.length !== offset + 8) return null;
  const provider = asProvider(key[offset + 1]);
  const modelHash = key[offset + 2];
  const route = asRoute(key[offset + 3]);
  const keyState = key[offset + 4];
  const mode = asMode(key[offset + 5]);
  const fallback = asFallback(key[offset + 6]);
  const counter = key[offset + 7];
  if (
    !provider ||
    typeof modelHash !== "string" ||
    !/^(?:unknown|[a-f0-9]{64})$/.test(modelHash) ||
    !route ||
    (keyState !== "keyed" && keyState !== "unkeyed") ||
    !isKnownCounter(counter)
  ) return null;
  return {
    provider,
    modelHash,
    route,
    promptCacheKeyPresent: keyState === "keyed",
    mode,
    fallback,
  };
};

const counterFromDimensionKey = (key: Deno.KvKey): Counter | null => {
  const counter = key[dimensionPrefix.length + 7];
  return isKnownCounter(counter) ? counter : null;
};

const groupForCohort = (
  cohort: PromptCacheAnalyticsCohort,
  groupBy: readonly PromptCacheAnalyticsDimension[],
): PromptCacheAnalyticsGroup => {
  const group: Record<string, string | boolean> = {};
  for (const dimension of groupBy) {
    if (dimension === "provider") group.provider = cohort.provider;
    if (dimension === "model") group.model_hash = cohort.modelHash;
    if (dimension === "route") group.route = cohort.route;
    if (dimension === "key_presence") group.prompt_cache_key_present = cohort.promptCacheKeyPresent;
    if (dimension === "mode") group.mode = cohort.mode;
    if (dimension === "fallback") group.fallback = cohort.fallback;
  }
  return group as PromptCacheAnalyticsGroup;
};

const groupIdentity = (bucketStartAtMs: number, group: PromptCacheAnalyticsGroup): string =>
  JSON.stringify([
    bucketStartAtMs,
    group.provider,
    group.model_hash,
    group.route,
    group.prompt_cache_key_present,
    group.mode,
    group.fallback,
    group.cardinality_limited,
  ]);

const parsedAggregateCounter = (key: Deno.KvKey): Readonly<{ bucketStartAtMs: number; counter: Counter }> | null => {
  if (key.length !== aggregatePrefix.length + 2) return null;
  const bucketStartAtMs = key[aggregatePrefix.length];
  const counter = key[aggregatePrefix.length + 1];
  if (!safeCounter(bucketStartAtMs) || !isKnownCounter(counter)) return null;
  return { bucketStartAtMs, counter };
};

const parsedOverflowCounter = (key: Deno.KvKey): Readonly<{ bucketStartAtMs: number; counter: Counter }> | null => {
  if (key.length !== overflowPrefix.length + 2) return null;
  const bucketStartAtMs = key[overflowPrefix.length];
  const counter = key[overflowPrefix.length + 1];
  if (!safeCounter(bucketStartAtMs) || !isKnownCounter(counter)) return null;
  return { bucketStartAtMs, counter };
};

const projectedBucket = (
  bucketStartAtMs: number,
  counters: StoredCounters,
  group: PromptCacheAnalyticsGroup | null,
): PromptCacheAnalyticsBucket | null => {
  const sampleCount = counters.sample_count;
  if (!safeCounter(sampleCount)) return null;

  const explicitReported = counters.usage_reported_sample_count;
  const hasInputCounters = counters.input_tokens !== undefined || counters.cached_input_tokens !== undefined;
  // Synthetic control-plane fixtures may use the aggregate helper directly.
  // Both token counters are sufficient evidence of reported usage in that case.
  const usageReportedSampleCount = explicitReported === undefined
    ? hasInputCounters && safeCounter(counters.input_tokens) && safeCounter(counters.cached_input_tokens)
      ? sampleCount
      : 0
    : explicitReported;
  const usageInvalidSampleCount = counters.usage_invalid_sample_count ?? 0;
  if (
    !safeCounter(usageReportedSampleCount) ||
    !safeCounter(usageInvalidSampleCount) ||
    usageReportedSampleCount > sampleCount ||
    usageInvalidSampleCount > sampleCount ||
    usageReportedSampleCount + usageInvalidSampleCount > sampleCount
  ) return null;

  let inputTokens: number | null = null;
  let cachedInputTokens: number | null = null;
  if (usageReportedSampleCount > 0) {
    if (
      !safeCounter(counters.input_tokens) || !safeCounter(counters.cached_input_tokens) ||
      counters.cached_input_tokens > counters.input_tokens
    ) return null;
    inputTokens = counters.input_tokens;
    cachedInputTokens = counters.cached_input_tokens;
  } else if (hasInputCounters) {
    return null;
  }

  const cacheWriteInputTokens = counters.cache_write_input_tokens;
  const cacheWriteReportedSampleCount = counters.cache_write_reported_sample_count;
  let projectedCacheWriteInputTokens: number | null = null;
  let projectedCacheWriteReportedSampleCount = 0;
  if (cacheWriteInputTokens !== undefined || cacheWriteReportedSampleCount !== undefined) {
    if (
      !safeCounter(cacheWriteInputTokens) ||
      !safeCounter(cacheWriteReportedSampleCount) ||
      cacheWriteReportedSampleCount === 0 ||
      cacheWriteReportedSampleCount > usageReportedSampleCount
    ) return null;
    projectedCacheWriteInputTokens = cacheWriteInputTokens;
    projectedCacheWriteReportedSampleCount = cacheWriteReportedSampleCount;
  }

  const requestCacheHitSampleCount = counters.request_cache_hit_sample_count ?? 0;
  const cardinalityLimitedSampleCount = counters.dimension_cardinality_limited_sample_count ?? 0;
  if (
    !safeCounter(requestCacheHitSampleCount) ||
    requestCacheHitSampleCount > usageReportedSampleCount ||
    !safeCounter(cardinalityLimitedSampleCount) ||
    cardinalityLimitedSampleCount > sampleCount
  ) return null;

  const tokenHitPercentage = inputTokens === null || cachedInputTokens === null
    ? null
    : roundedPercentage(cachedInputTokens, inputTokens);
  const requestHitPercentage = roundedPercentage(requestCacheHitSampleCount, usageReportedSampleCount);
  const usageMissingSampleCount = sampleCount - usageReportedSampleCount - usageInvalidSampleCount;
  const compact: PromptCacheAnalyticsBucket = {
    bucket_start_at_ms: bucketStartAtMs,
    bucket_end_at_ms: bucketStartAtMs + PROMPT_CACHE_ANALYTICS_BUCKET_MS,
    input_tokens: inputTokens,
    cached_input_tokens: cachedInputTokens,
    cache_write_input_tokens: projectedCacheWriteInputTokens,
    cache_write_reported_sample_count: projectedCacheWriteReportedSampleCount,
    cached_percentage: tokenHitPercentage,
    sample_count: sampleCount,
  };
  if (group === null) return compact;
  return {
    ...compact,
    group,
    token_hit_percentage: tokenHitPercentage,
    request_cache_hit_sample_count: requestCacheHitSampleCount,
    request_hit_percentage: requestHitPercentage,
    cache_reads_per_write: inputTokens === null || cachedInputTokens === null || projectedCacheWriteInputTokens === null
      ? null
      : roundedRatio(cachedInputTokens, projectedCacheWriteInputTokens),
    usage_reported_sample_count: usageReportedSampleCount,
    usage_invalid_sample_count: usageInvalidSampleCount,
    usage_missing_sample_count: usageMissingSampleCount,
    usage_telemetry_coverage_percentage: roundedPercentage(usageReportedSampleCount, sampleCount),
    dimension_cardinality_limited_sample_count: cardinalityLimitedSampleCount,
  };
};

const viewWindow = (now: () => number): Readonly<{
  currentBucketStartAtMs: number;
  windowStartAtMs: number;
  windowEndAtMs: number;
}> => {
  const currentBucketStartAtMs = alignedBucketStart(safeNow(now));
  const windowEndAtMs = currentBucketStartAtMs + PROMPT_CACHE_ANALYTICS_BUCKET_MS;
  return {
    currentBucketStartAtMs,
    windowStartAtMs: windowEndAtMs - PROMPT_CACHE_ANALYTICS_WINDOW_MS,
    windowEndAtMs,
  };
};

const inWindow = (
  bucketStartAtMs: number,
  window: Readonly<{ currentBucketStartAtMs: number; windowStartAtMs: number }>,
): boolean => bucketStartAtMs >= window.windowStartAtMs && bucketStartAtMs <= window.currentBucketStartAtMs;

/**
 * Reads the compact aggregate used by capacity history or a bounded grouped
 * view for the admin API. Grouped responses retain the newest rows if the
 * response limit is reached and mark the truncation explicitly.
 */
export const readPromptCacheAnalytics = async (
  options: PromptCacheAnalyticsReadOptions = {},
): Promise<PromptCacheAnalyticsView> => {
  const groupBy = normalizedGroupBy(options.groupBy);
  if (groupBy === null) {
    throw new PromptCacheAnalyticsQueryError("group_by must contain distinct approved dimensions only");
  }
  const window = viewWindow(options.now ?? Date.now);
  const unavailable = (): PromptCacheAnalyticsView => ({
    status: "unavailable",
    bucket_ms: PROMPT_CACHE_ANALYTICS_BUCKET_MS,
    window_start_at_ms: window.windowStartAtMs,
    window_end_at_ms: window.windowEndAtMs,
    group_by: groupBy,
    max_buckets: PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS,
    cardinality_limited: false,
    truncated: false,
    buckets: [],
  });
  const kv = await resolveKv(options);
  if (!kv) return unavailable();

  try {
    const storedBuckets = new Map<
      string,
      Readonly<{
        bucketStartAtMs: number;
        group: PromptCacheAnalyticsGroup | null;
        counters: StoredCounters;
      }>
    >();
    let cardinalityLimited = false;
    if (groupBy.length === 0) {
      for await (const entry of kv.list<Deno.KvU64>({ prefix: aggregatePrefix })) {
        const parsed = parsedAggregateCounter(entry.key);
        if (!parsed || !inWindow(parsed.bucketStartAtMs, window)) continue;
        const value = storedCounter(entry.value);
        if (value === null) continue;
        const identity = String(parsed.bucketStartAtMs);
        const existing = storedBuckets.get(identity) ?? {
          bucketStartAtMs: parsed.bucketStartAtMs,
          group: null,
          counters: {},
        };
        const previous = existing.counters[parsed.counter] ?? 0;
        if (previous > Number.MAX_SAFE_INTEGER - value) continue;
        existing.counters[parsed.counter] = previous + value;
        storedBuckets.set(identity, existing);
      }
    } else {
      for await (const entry of kv.list<Deno.KvU64>({ prefix: dimensionPrefix })) {
        const bucketStartAtMs = entry.key[dimensionPrefix.length];
        if (!safeCounter(bucketStartAtMs) || !inWindow(bucketStartAtMs, window)) continue;
        const cohort = parsedDimensionValues(entry.key);
        const counter = counterFromDimensionKey(entry.key);
        const value = storedCounter(entry.value);
        if (!cohort || !counter || value === null) continue;
        const group = groupForCohort(cohort, groupBy);
        const identity = groupIdentity(bucketStartAtMs, group);
        const existing = storedBuckets.get(identity) ?? { bucketStartAtMs, group, counters: {} };
        const previous = existing.counters[counter] ?? 0;
        if (previous > Number.MAX_SAFE_INTEGER - value) continue;
        existing.counters[counter] = previous + value;
        storedBuckets.set(identity, existing);
      }
      for await (const entry of kv.list<Deno.KvU64>({ prefix: overflowPrefix })) {
        const parsed = parsedOverflowCounter(entry.key);
        if (!parsed || !inWindow(parsed.bucketStartAtMs, window)) continue;
        const value = storedCounter(entry.value);
        if (value === null) continue;
        cardinalityLimited = true;
        const group: PromptCacheAnalyticsGroup = { cardinality_limited: true };
        const identity = groupIdentity(parsed.bucketStartAtMs, group);
        const existing = storedBuckets.get(identity) ??
          { bucketStartAtMs: parsed.bucketStartAtMs, group, counters: {} };
        const previous = existing.counters[parsed.counter] ?? 0;
        if (previous > Number.MAX_SAFE_INTEGER - value) continue;
        existing.counters[parsed.counter] = previous + value;
        storedBuckets.set(identity, existing);
      }
    }

    const projected = [...storedBuckets.values()]
      .map(({ bucketStartAtMs, group, counters }) => projectedBucket(bucketStartAtMs, counters, group))
      .filter((bucket): bucket is PromptCacheAnalyticsBucket => bucket !== null)
      .sort((left, right) =>
        left.bucket_start_at_ms - right.bucket_start_at_ms ||
        JSON.stringify(left.group ?? {}).localeCompare(JSON.stringify(right.group ?? {}))
      );
    const responseTruncated = projected.length > PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS;
    const truncated = responseTruncated || cardinalityLimited;
    const buckets = responseTruncated ? projected.slice(-PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS) : projected;
    return {
      status: "ready",
      bucket_ms: PROMPT_CACHE_ANALYTICS_BUCKET_MS,
      window_start_at_ms: window.windowStartAtMs,
      window_end_at_ms: window.windowEndAtMs,
      group_by: groupBy,
      max_buckets: PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS,
      cardinality_limited: cardinalityLimited,
      truncated,
      buckets,
    };
  } catch {
    return unavailable();
  }
};
