#pragma once
// Port of src/cache/prompt-analytics-read.ts.
// Prompt cache analytics read model, split out of src/prompt_cache_analytics.ts.
#include "uos/cache/prompt_analytics_core.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <string>
#include <vector>

namespace uos::cache::prompt_analytics_read {
  /** Used by the admin boundary before it reads KV. */
  // `(value: unknown): value is readonly PromptCacheAnalyticsDimension[]`; a null pointer is
  // undefined. The string list overload takes the admin boundary's `string[]` as it is.
  bool isValidPromptCacheAnalyticsGroupBy(const json::value& value);
  bool isValidPromptCacheAnalyticsGroupBy(const json::value* value);
  bool isValidPromptCacheAnalyticsGroupBy(const std::vector<std::string>& value);

  /**
   * Reads the compact aggregate used by capacity history or a bounded grouped
   * view for the admin API. Grouped responses retain the newest rows if the
   * response limit is reached and mark the truncation explicitly.
   */
  // The overload without `options` is `{}`. An invalid `groupBy` rejects with
  // prompt_analytics_core::PromptCacheAnalyticsQueryError.
  rt::Task<prompt_analytics_core::PromptCacheAnalyticsView> readPromptCacheAnalytics();
  rt::Task<prompt_analytics_core::PromptCacheAnalyticsView> readPromptCacheAnalytics(prompt_analytics_core::PromptCacheAnalyticsReadOptions options);
}
