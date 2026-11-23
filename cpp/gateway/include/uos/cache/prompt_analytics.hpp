#pragma once
// Port of src/cache/prompt-analytics.ts.
// Prompt cache analytics: public surface re-exported from the core, queue and read modules.
#include "uos/cache/prompt_analytics_core.hpp"
#include "uos/cache/prompt_analytics_queue.hpp"
#include "uos/cache/prompt_analytics_read.hpp"

namespace uos::cache::prompt_analytics {
  using prompt_analytics_core::PromptCacheAnalyticsQueryError;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_BUCKET_MS;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_DIMENSIONS;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_KV_PREFIX;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_MAX_COHORTS_PER_BUCKET;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_MAX_RESPONSE_BUCKETS;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_RETENTION_MS;
  using prompt_analytics_core::PROMPT_CACHE_ANALYTICS_WINDOW_BUCKETS;
  using prompt_analytics_core::promptCacheAnalyticsCounterKey;
  using prompt_analytics_core::prunePromptCacheAnalytics;
  using prompt_analytics_core::recordPromptCacheAnalytics;
  using prompt_analytics_core::PromptCacheAnalyticsBucket;
  using prompt_analytics_core::PromptCacheAnalyticsReadOptions;
  using prompt_analytics_core::PromptCacheAnalyticsRecordResult;
  using prompt_analytics_core::PromptCacheAnalyticsView;
  using prompt_analytics_queue::closeOptionalPromptCacheAnalytics;
  using prompt_analytics_queue::enqueuePromptCacheAnalytics;
  using prompt_analytics_queue::flushOptionalPromptCacheAnalytics;
  using prompt_analytics_queue::optionalPromptCacheAnalyticsSnapshot;
  using prompt_analytics_queue::resetOptionalPromptCacheAnalyticsForTest;
  using prompt_analytics_queue::writePromptCacheAnalyticsQueueEntry;
  using prompt_analytics_queue::PromptCacheAnalyticsQueueEntry;
  using prompt_analytics_read::isValidPromptCacheAnalyticsGroupBy;
  using prompt_analytics_read::readPromptCacheAnalytics;
}
