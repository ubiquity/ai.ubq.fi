import { getKv } from "./kv.ts";
import { PROMPT_CACHE_TELEMETRY_PROVIDERS, PROMPT_CACHE_TELEMETRY_ROUTES } from "./prompt_cache_telemetry_gate.ts";
import { RELEASE_GIT_SHA } from "./release.ts";

export const PROMPT_CACHE_ANALYTICS_KV_PREFIX = ["uos_ai", "prompt_cache_analytics", "v1"] as const;
export const PROMPT_CACHE_ANALYTICS_BUCKET_MS = 15 * 60_000;
export const PROMPT_CACHE_ANALYTICS_WINDOW_MS = 7 * 24 * 60 * 60_000;
export const PROMPT_CACHE_ANALYTICS_RETENTION_MS = 8 * 24 * 60 * 60_000;

const RELEASE_SHA = /^[a-f0-9]{7,64}$/i;
const COUNTERS = ["input_tokens", "cached_input_tokens", "sample_count"] as const;
type Counter = typeof COUNTERS[number];

export type PromptCacheAnalyticsEvent = Readonly<{
  provider: string | null;
  route: string;
  status: number;
  completed: boolean;
  usageTelemetryStatus: string;
  inputTokens: number | null;
  cachedInputTokens: number | null;
}>;

export type PromptCacheAnalyticsOptions = Readonly<{
  kv?: Deno.Kv | null;
  release?: string;
  now?: () => number;
}>;

export type PromptCacheAnalyticsRecordResult = Readonly<{
  status: "recorded" | "ignored" | "unavailable";
  reason:
    | "recorded"
    | "unknown_release"
    | "not_completed_2xx"
    | "unsupported_provider"
    | "unsupported_route"
    | "usage_unavailable"
    | "invalid_usage"
    | "kv_unavailable";
  bucket_start_at_ms: number | null;
}>;

export type PromptCacheAnalyticsBucket = Readonly<{
  bucket_start_at_ms: number;
  bucket_end_at_ms: number;
  input_tokens: number;
  cached_input_tokens: number;
  cached_percentage: number | null;
  sample_count: number;
}>;

export type PromptCacheAnalyticsView = Readonly<{
  status: "ready" | "unavailable";
  bucket_ms: number;
  window_start_at_ms: number;
  window_end_at_ms: number;
  buckets: readonly PromptCacheAnalyticsBucket[];
}>;

export type PromptCacheAnalyticsPruneResult = Readonly<{
  status: "pruned" | "unavailable";
  deleted: number;
}>;

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

export const promptCacheAnalyticsBucketKey = (
  bucketStartAtMs: number,
): Deno.KvKey => [...PROMPT_CACHE_ANALYTICS_KV_PREFIX, bucketStartAtMs];

export const promptCacheAnalyticsCounterKey = (
  bucketStartAtMs: number,
  counter: Counter,
): Deno.KvKey => [...promptCacheAnalyticsBucketKey(bucketStartAtMs), counter];

/**
 * Adds one completed response to its UTC-aligned 15-minute cache bucket.
 * The aggregate is content-free and does not retain model, account, key, or request identifiers.
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
  if (!(PROMPT_CACHE_TELEMETRY_PROVIDERS as readonly string[]).includes(event.provider ?? "")) {
    return recordResult("ignored", "unsupported_provider");
  }
  if (!(PROMPT_CACHE_TELEMETRY_ROUTES as readonly string[]).includes(event.route)) {
    return recordResult("ignored", "unsupported_route");
  }
  if (event.usageTelemetryStatus !== "reported") return recordResult("ignored", "usage_unavailable");
  if (
    !safeCounter(event.inputTokens) ||
    !safeCounter(event.cachedInputTokens) ||
    event.cachedInputTokens > event.inputTokens
  ) return recordResult("ignored", "invalid_usage");

  const nowMs = safeNow(options.now ?? Date.now);
  const bucketStartAtMs = alignedBucketStart(nowMs);
  const kv = await resolveKv(options);
  if (!kv) return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);

  try {
    const committed = await kv.atomic()
      .sum(promptCacheAnalyticsCounterKey(bucketStartAtMs, "input_tokens"), BigInt(event.inputTokens))
      .sum(promptCacheAnalyticsCounterKey(bucketStartAtMs, "cached_input_tokens"), BigInt(event.cachedInputTokens))
      .sum(promptCacheAnalyticsCounterKey(bucketStartAtMs, "sample_count"), 1n)
      .commit();
    if (!committed.ok) return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
  } catch {
    return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
  }

  return recordResult("recorded", "recorded", bucketStartAtMs);
};

const isAnalyticsStorageKey = (key: Deno.KvKey, cutoffBucketStartAtMs: number): boolean => {
  const bucketStartAtMs = key[PROMPT_CACHE_ANALYTICS_KV_PREFIX.length];
  if (!safeCounter(bucketStartAtMs) || bucketStartAtMs > cutoffBucketStartAtMs) return false;
  if (key.length === PROMPT_CACHE_ANALYTICS_KV_PREFIX.length + 1) return true;
  const counter = key[PROMPT_CACHE_ANALYTICS_KV_PREFIX.length + 1];
  return key.length === PROMPT_CACHE_ANALYTICS_KV_PREFIX.length + 2 &&
    typeof counter === "string" &&
    (COUNTERS as readonly string[]).includes(counter);
};

/** Removes every analytics bucket at or beyond the eight-day retention boundary. */
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
    for await (const entry of kv.list({ prefix: PROMPT_CACHE_ANALYTICS_KV_PREFIX })) {
      if (!isAnalyticsStorageKey(entry.key, cutoffBucketStartAtMs)) continue;
      batch.push(entry.key);
      if (batch.length === 64) await deleteBatch();
    }
    if (batch.length) await deleteBatch();
    return { status: "pruned", deleted };
  } catch {
    return { status: "unavailable", deleted };
  }
};

const projectedBucket = (
  bucketStartAtMs: number,
  counters: Readonly<Record<Counter, number>>,
): PromptCacheAnalyticsBucket => ({
  bucket_start_at_ms: bucketStartAtMs,
  bucket_end_at_ms: bucketStartAtMs + PROMPT_CACHE_ANALYTICS_BUCKET_MS,
  input_tokens: counters.input_tokens,
  cached_input_tokens: counters.cached_input_tokens,
  cached_percentage: counters.input_tokens === 0
    ? null
    : Math.round((counters.cached_input_tokens / counters.input_tokens) * 1_000_000) / 10_000,
  sample_count: counters.sample_count,
});

export const readPromptCacheAnalytics = async (
  options: Pick<PromptCacheAnalyticsOptions, "kv" | "now"> = {},
): Promise<PromptCacheAnalyticsView> => {
  const nowMs = safeNow(options.now ?? Date.now);
  const currentBucketStartAtMs = alignedBucketStart(nowMs);
  const windowEndAtMs = currentBucketStartAtMs + PROMPT_CACHE_ANALYTICS_BUCKET_MS;
  const windowStartAtMs = windowEndAtMs - PROMPT_CACHE_ANALYTICS_WINDOW_MS;
  const unavailable = (): PromptCacheAnalyticsView => ({
    status: "unavailable",
    bucket_ms: PROMPT_CACHE_ANALYTICS_BUCKET_MS,
    window_start_at_ms: windowStartAtMs,
    window_end_at_ms: windowEndAtMs,
    buckets: [],
  });
  const kv = await resolveKv(options);
  if (!kv) return unavailable();

  const storedBuckets = new Map<number, Partial<Record<Counter, number>>>();
  try {
    for await (const entry of kv.list<Deno.KvU64>({ prefix: PROMPT_CACHE_ANALYTICS_KV_PREFIX })) {
      const keyBucketStartAtMs = entry.key[PROMPT_CACHE_ANALYTICS_KV_PREFIX.length];
      const counter = entry.key[PROMPT_CACHE_ANALYTICS_KV_PREFIX.length + 1];
      if (
        entry.key.length !== PROMPT_CACHE_ANALYTICS_KV_PREFIX.length + 2 ||
        !safeCounter(keyBucketStartAtMs) ||
        keyBucketStartAtMs < windowStartAtMs ||
        keyBucketStartAtMs > currentBucketStartAtMs ||
        typeof counter !== "string" ||
        !(COUNTERS as readonly string[]).includes(counter)
      ) continue;
      const value = storedCounter(entry.value);
      if (value === null) continue;
      const bucket = storedBuckets.get(keyBucketStartAtMs) ?? {};
      bucket[counter as Counter] = value;
      storedBuckets.set(keyBucketStartAtMs, bucket);
    }
  } catch {
    return unavailable();
  }

  const buckets: PromptCacheAnalyticsBucket[] = [];
  for (const [bucketStartAtMs, counters] of storedBuckets) {
    const inputTokens = counters.input_tokens;
    const cachedInputTokens = counters.cached_input_tokens;
    const sampleCount = counters.sample_count;
    if (
      !safeCounter(inputTokens) ||
      !safeCounter(cachedInputTokens) ||
      cachedInputTokens > inputTokens ||
      !safeCounter(sampleCount)
    ) continue;
    buckets.push(projectedBucket(bucketStartAtMs, {
      input_tokens: inputTokens,
      cached_input_tokens: cachedInputTokens,
      sample_count: sampleCount,
    }));
  }
  buckets.sort((left, right) => left.bucket_start_at_ms - right.bucket_start_at_ms);
  return {
    status: "ready",
    bucket_ms: PROMPT_CACHE_ANALYTICS_BUCKET_MS,
    window_start_at_ms: windowStartAtMs,
    window_end_at_ms: windowEndAtMs,
    buckets,
  };
};
