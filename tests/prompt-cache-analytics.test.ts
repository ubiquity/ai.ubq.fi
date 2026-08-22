import assert from "node:assert/strict";

import {
  PROMPT_CACHE_ANALYTICS_BUCKET_MS,
  PROMPT_CACHE_ANALYTICS_RETENTION_MS,
  PROMPT_CACHE_ANALYTICS_WINDOW_MS,
  promptCacheAnalyticsCounterKey,
  readPromptCacheAnalytics,
  recordPromptCacheAnalytics,
} from "../src/prompt_cache_analytics.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

if (typeof Deno.KvU64 !== "function") {
  (Deno as unknown as { KvU64: typeof Deno.KvU64 }).KvU64 = class {
    constructor(readonly value: bigint) {}
  } as typeof Deno.KvU64;
}

const RELEASE = "0123456789abcdef0123456789abcdef01234567";
const NOW_MS = 1_800_000_000_000;

const event = (overrides: Partial<Parameters<typeof recordPromptCacheAnalytics>[0]> = {}) => ({
  provider: "chatgpt_codex",
  route: "responses",
  status: 200,
  completed: true,
  usageTelemetryStatus: "reported",
  inputTokens: 100,
  cachedInputTokens: 25,
  ...overrides,
});

const seedBucket = (
  kv: CountingKv,
  bucketStartAtMs: number,
  counters: Readonly<{ inputTokens: number; cachedInputTokens: number; sampleCount: number }>,
): void => {
  kv.seed(
    promptCacheAnalyticsCounterKey(bucketStartAtMs, "input_tokens"),
    new Deno.KvU64(BigInt(counters.inputTokens)),
  );
  kv.seed(
    promptCacheAnalyticsCounterKey(bucketStartAtMs, "cached_input_tokens"),
    new Deno.KvU64(BigInt(counters.cachedInputTokens)),
  );
  kv.seed(
    promptCacheAnalyticsCounterKey(bucketStartAtMs, "sample_count"),
    new Deno.KvU64(BigInt(counters.sampleCount)),
  );
};

Deno.test("prompt cache analytics aggregates cached-token share into UTC-aligned 15-minute buckets", async () => {
  const kv = new CountingKv();
  const options = { kv: kv as unknown as Deno.Kv, release: RELEASE };

  const first = await recordPromptCacheAnalytics(event(), { ...options, now: () => NOW_MS + 1 });
  const second = await recordPromptCacheAnalytics(
    event({ inputTokens: 300, cachedInputTokens: 225 }),
    { ...options, now: () => NOW_MS + 5 * 60_000 },
  );
  const zero = await recordPromptCacheAnalytics(
    event({ route: "chat.completions", inputTokens: 200, cachedInputTokens: 0 }),
    { ...options, now: () => NOW_MS + PROMPT_CACHE_ANALYTICS_BUCKET_MS + 1 },
  );

  assert.deepEqual([first.status, second.status, zero.status], ["recorded", "recorded", "recorded"]);
  assert.equal(first.bucket_start_at_ms, NOW_MS);
  const view = await readPromptCacheAnalytics({
    kv: kv as unknown as Deno.Kv,
    now: () => NOW_MS + PROMPT_CACHE_ANALYTICS_BUCKET_MS + 1,
  });
  assert.equal(view.status, "ready");
  assert.equal(view.bucket_ms, PROMPT_CACHE_ANALYTICS_BUCKET_MS);
  assert.deepEqual(view.buckets, [
    {
      bucket_start_at_ms: NOW_MS,
      bucket_end_at_ms: NOW_MS + PROMPT_CACHE_ANALYTICS_BUCKET_MS,
      input_tokens: 400,
      cached_input_tokens: 250,
      cached_percentage: 62.5,
      sample_count: 2,
    },
    {
      bucket_start_at_ms: NOW_MS + PROMPT_CACHE_ANALYTICS_BUCKET_MS,
      bucket_end_at_ms: NOW_MS + 2 * PROMPT_CACHE_ANALYTICS_BUCKET_MS,
      input_tokens: 200,
      cached_input_tokens: 0,
      cached_percentage: 0,
      sample_count: 1,
    },
  ]);
});

Deno.test("prompt cache analytics keeps missing telemetry distinct from a reported zero cache hit rate", async () => {
  const kv = new CountingKv();
  const options = { kv: kv as unknown as Deno.Kv, release: RELEASE, now: () => NOW_MS };

  const ignored = await Promise.all([
    recordPromptCacheAnalytics(event({ usageTelemetryStatus: "partial", cachedInputTokens: null }), options),
    recordPromptCacheAnalytics(event({ completed: false }), options),
    recordPromptCacheAnalytics(event({ status: 503 }), options),
    recordPromptCacheAnalytics(event({ provider: "gateway" }), options),
    recordPromptCacheAnalytics(event({ route: "embeddings" }), options),
    recordPromptCacheAnalytics(event({ cachedInputTokens: 101 }), options),
    recordPromptCacheAnalytics(event(), { ...options, release: "unknown" }),
  ]);

  assert.deepEqual(ignored.map((result) => result.reason), [
    "usage_unavailable",
    "not_completed_2xx",
    "not_completed_2xx",
    "unsupported_provider",
    "unsupported_route",
    "invalid_usage",
    "unknown_release",
  ]);
  assert.equal(kv.entries.size, 0);

  const recordedZero = await recordPromptCacheAnalytics(event({ cachedInputTokens: 0 }), options);
  assert.equal(recordedZero.status, "recorded");
  const view = await readPromptCacheAnalytics({ kv: kv as unknown as Deno.Kv, now: () => NOW_MS });
  assert.equal(view.buckets[0]?.cached_percentage, 0);
  assert.equal(view.buckets[0]?.input_tokens, 100);
});

Deno.test("prompt cache analytics preserves concurrent completions with atomic counters", async () => {
  const kv = new CountingKv();
  const options = { kv: kv as unknown as Deno.Kv, release: RELEASE, now: () => NOW_MS };
  const results = await Promise.all(Array.from({ length: 100 }, () => recordPromptCacheAnalytics(event(), options)));

  assert.ok(results.every((result) => result.status === "recorded"));
  const view = await readPromptCacheAnalytics({ kv: kv as unknown as Deno.Kv, now: () => NOW_MS });
  assert.deepEqual(view.buckets[0], {
    bucket_start_at_ms: NOW_MS,
    bucket_end_at_ms: NOW_MS + PROMPT_CACHE_ANALYTICS_BUCKET_MS,
    input_tokens: 10_000,
    cached_input_tokens: 2_500,
    cached_percentage: 25,
    sample_count: 100,
  });
});

Deno.test("prompt cache analytics returns only valid buckets in the trailing seven-day window", async () => {
  const kv = new CountingKv();
  const currentBucket = NOW_MS;
  const windowEnd = currentBucket + PROMPT_CACHE_ANALYTICS_BUCKET_MS;
  const windowStart = windowEnd - PROMPT_CACHE_ANALYTICS_WINDOW_MS;
  const validCounters = { inputTokens: 80, cachedInputTokens: 40, sampleCount: 2 };
  seedBucket(kv, windowStart - PROMPT_CACHE_ANALYTICS_BUCKET_MS, validCounters);
  seedBucket(kv, windowStart, validCounters);
  seedBucket(kv, currentBucket, {
    ...validCounters,
    cachedInputTokens: 81,
  });

  const view = await readPromptCacheAnalytics({ kv: kv as unknown as Deno.Kv, now: () => NOW_MS });
  assert.equal(view.status, "ready");
  assert.equal(view.window_start_at_ms, windowStart);
  assert.equal(view.window_end_at_ms, windowEnd);
  assert.deepEqual(view.buckets.map((bucket) => bucket.bucket_start_at_ms), [windowStart]);

  const unavailable = await readPromptCacheAnalytics({ kv: null, now: () => NOW_MS });
  assert.equal(unavailable.status, "unavailable");
  assert.deepEqual(unavailable.buckets, []);

  const expiredBucket = NOW_MS - PROMPT_CACHE_ANALYTICS_RETENTION_MS;
  seedBucket(kv, expiredBucket, validCounters);
  await recordPromptCacheAnalytics(event(), { kv: kv as unknown as Deno.Kv, release: RELEASE, now: () => NOW_MS });
  for (const counter of ["input_tokens", "cached_input_tokens", "sample_count"] as const) {
    assert.equal(kv.entries.has(JSON.stringify(promptCacheAnalyticsCounterKey(expiredBucket, counter))), false);
  }
});
