// Prompt cache analytics: public surface re-exported from the core, queue and read modules.

export {
  PromptCacheAnalyticsQueryError,
  PROMPT_CACHE_ANALYTICS_BUCKET_MS,
  PROMPT_CACHE_ANALYTICS_DIMENSIONS,
  PROMPT_CACHE_ANALYTICS_KV_PREFIX,
  PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET,
  PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS,
  PROMPT_CACHE_ANALYTICS_RETENTION_MS,
  PROMPT_CACHE_ANALYTICS_WINDOW_BUCKETS,
  promptCacheAnalyticsCounterKey,
  prunePromptCacheAnalytics,
  recordPromptCacheAnalytics,
} from "./prompt_cache_analytics_core.ts";
export type {
  PromptCacheAnalyticsBucket,
  PromptCacheAnalyticsReadOptions,
  PromptCacheAnalyticsRecordResult,
  PromptCacheAnalyticsView,
} from "./prompt_cache_analytics_core.ts";
export {
  closeOptionalPromptCacheAnalytics,
  enqueuePromptCacheAnalytics,
  flushOptionalPromptCacheAnalytics,
  optionalPromptCacheAnalyticsSnapshot,
  resetOptionalPromptCacheAnalyticsForTest,
  writePromptCacheAnalyticsQueueEntry,
} from "./prompt_cache_analytics_queue.ts";
export type { PromptCacheAnalyticsQueueEntry } from "./prompt_cache_analytics_queue.ts";
export { isValidPromptCacheAnalyticsGroupBy, readPromptCacheAnalytics } from "./prompt_cache_analytics_read.ts";
