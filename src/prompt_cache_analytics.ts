import { getKv } from "./kv.ts";
import { PROMPT_CACHE_TELEMETRY_PROVIDERS, PROMPT_CACHE_TELEMETRY_ROUTES } from "./prompt_cache_telemetry_gate.ts";
import { RELEASE_GIT_SHA } from "./release.ts";
import { isRecord } from "./utils.ts";

export const PROMPT_CACHE_ANALYTICS_KV_PREFIX = ["uos_ai", "prompt_cache_analytics", "v1"] as const;
export const PROMPT_CACHE_ANALYTICS_BUCKET_MS = 15 * 60_000;
export const PROMPT_CACHE_ANALYTICS_WINDOW_MS = 7 * 24 * 60 * 60_000;
export const PROMPT_CACHE_ANALYTICS_RETENTION_MS = 8 * 24 * 60 * 60_000;

const RELEASE_SHA = /^[a-f0-9]{7,64}$/i;
const MAX_WRITE_ATTEMPTS = 6;

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
    | "invalid_bucket"
    | "kv_unavailable"
    | "write_contention";
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

type StoredPromptCacheAnalyticsBucket = Readonly<{
  v: 1;
  bucket_start_at_ms: number;
  input_tokens: number;
  cached_input_tokens: number;
  sample_count: number;
  updated_at_ms: number;
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

const parseStoredBucket = (
  value: unknown,
  expectedBucketStartAtMs?: number,
): StoredPromptCacheAnalyticsBucket | null => {
  if (!isRecord(value) || value.v !== 1) return null;
  if (
    !safeCounter(value.bucket_start_at_ms) ||
    value.bucket_start_at_ms % PROMPT_CACHE_ANALYTICS_BUCKET_MS !== 0 ||
    !safeCounter(value.input_tokens) ||
    !safeCounter(value.cached_input_tokens) ||
    value.cached_input_tokens > value.input_tokens ||
    !safeCounter(value.sample_count) ||
    !safeCounter(value.updated_at_ms)
  ) return null;
  if (expectedBucketStartAtMs !== undefined && value.bucket_start_at_ms !== expectedBucketStartAtMs) return null;
  return {
    v: 1,
    bucket_start_at_ms: value.bucket_start_at_ms,
    input_tokens: value.input_tokens,
    cached_input_tokens: value.cached_input_tokens,
    sample_count: value.sample_count,
    updated_at_ms: value.updated_at_ms,
  };
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
  const key = promptCacheAnalyticsBucketKey(bucketStartAtMs);

  try {
    for (let attempt = 0; attempt < MAX_WRITE_ATTEMPTS; attempt += 1) {
      const entry = await kv.get<StoredPromptCacheAnalyticsBucket>(key, { consistency: "strong" });
      const current = entry.value === null ? null : parseStoredBucket(entry.value, bucketStartAtMs);
      if (entry.value !== null && !current) return recordResult("unavailable", "invalid_bucket", bucketStartAtMs);
      const inputTokens = (current?.input_tokens ?? 0) + event.inputTokens;
      const cachedInputTokens = (current?.cached_input_tokens ?? 0) + event.cachedInputTokens;
      const sampleCount = (current?.sample_count ?? 0) + 1;
      if (!safeCounter(inputTokens) || !safeCounter(cachedInputTokens) || !safeCounter(sampleCount)) {
        return recordResult("ignored", "invalid_usage", bucketStartAtMs);
      }
      const next: StoredPromptCacheAnalyticsBucket = {
        v: 1,
        bucket_start_at_ms: bucketStartAtMs,
        input_tokens: inputTokens,
        cached_input_tokens: cachedInputTokens,
        sample_count: sampleCount,
        updated_at_ms: nowMs,
      };
      const expireIn = Math.max(1, bucketStartAtMs + PROMPT_CACHE_ANALYTICS_RETENTION_MS - nowMs);
      const committed = await kv.atomic().check(entry).set(key, next, { expireIn }).commit();
      if (committed.ok) return recordResult("recorded", "recorded", bucketStartAtMs);
    }
  } catch {
    return recordResult("unavailable", "kv_unavailable", bucketStartAtMs);
  }

  return recordResult("unavailable", "write_contention", bucketStartAtMs);
};

const projectedBucket = (stored: StoredPromptCacheAnalyticsBucket): PromptCacheAnalyticsBucket => ({
  bucket_start_at_ms: stored.bucket_start_at_ms,
  bucket_end_at_ms: stored.bucket_start_at_ms + PROMPT_CACHE_ANALYTICS_BUCKET_MS,
  input_tokens: stored.input_tokens,
  cached_input_tokens: stored.cached_input_tokens,
  cached_percentage: stored.input_tokens === 0
    ? null
    : Math.round((stored.cached_input_tokens / stored.input_tokens) * 1_000_000) / 10_000,
  sample_count: stored.sample_count,
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

  const buckets: PromptCacheAnalyticsBucket[] = [];
  try {
    for await (const entry of kv.list<StoredPromptCacheAnalyticsBucket>({ prefix: PROMPT_CACHE_ANALYTICS_KV_PREFIX })) {
      const keyBucketStartAtMs = entry.key[PROMPT_CACHE_ANALYTICS_KV_PREFIX.length];
      if (
        !safeCounter(keyBucketStartAtMs) ||
        keyBucketStartAtMs < windowStartAtMs ||
        keyBucketStartAtMs > currentBucketStartAtMs
      ) continue;
      const stored = parseStoredBucket(entry.value, keyBucketStartAtMs);
      if (stored) buckets.push(projectedBucket(stored));
    }
  } catch {
    return unavailable();
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
