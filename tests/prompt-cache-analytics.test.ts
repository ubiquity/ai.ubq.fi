import assert from "node:assert/strict";

import {
  PROMPT_CACHE_ANALYTICS_BUCKET_MS,
  PROMPT_CACHE_ANALYTICS_WINDOW_MS,
  promptCacheAnalyticsBucketKey,
  readPromptCacheAnalytics,
  recordPromptCacheAnalytics,
} from "../src/prompt_cache_analytics.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

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

Deno.test("prompt cache analytics returns only valid buckets in the trailing seven-day window", async () => {
  const kv = new CountingKv();
  const currentBucket = NOW_MS;
  const windowEnd = currentBucket + PROMPT_CACHE_ANALYTICS_BUCKET_MS;
  const windowStart = windowEnd - PROMPT_CACHE_ANALYTICS_WINDOW_MS;
  const validRecord = (bucketStartAtMs: number) => ({
    v: 1,
    bucket_start_at_ms: bucketStartAtMs,
    input_tokens: 80,
    cached_input_tokens: 40,
    sample_count: 2,
    updated_at_ms: bucketStartAtMs,
  });
  kv.seed(
    promptCacheAnalyticsBucketKey(windowStart - PROMPT_CACHE_ANALYTICS_BUCKET_MS),
    validRecord(
      windowStart - PROMPT_CACHE_ANALYTICS_BUCKET_MS,
    ),
  );
  kv.seed(promptCacheAnalyticsBucketKey(windowStart), validRecord(windowStart));
  kv.seed(promptCacheAnalyticsBucketKey(currentBucket), {
    ...validRecord(currentBucket),
    cached_input_tokens: 81,
  });

  const view = await readPromptCacheAnalytics({ kv: kv as unknown as Deno.Kv, now: () => NOW_MS });
  assert.equal(view.status, "ready");
  assert.equal(view.window_start_at_ms, windowStart);
  assert.equal(view.window_end_at_ms, windowEnd);
  assert.deepEqual(view.buckets.map((bucket) => bucket.bucket_start_at_ms), [windowStart]);

  const unavailable = await readPromptCacheAnalytics({ kv: null, now: () => NOW_MS });
  assert.equal(unavailable.status, "unavailable");
  assert.deepEqual(unavailable.buckets, []);
});
