#pragma once
/**
 * Context-window arithmetic shared by every model surface.
 *
 * This module used to own a curated table mapping model-id patterns to context
 * windows. That table is commented out (in the implementation) while the dynamic
 * sources prove themselves: `src/model_metadata.ts` now resolves a model's context
 * from the uploaded Codex catalog, the serving provider's discovery row, and
 * OpenRouter enrichment, and reports `unknown` when no source describes the id.
 *
 * What remains here is derivation, not per-model knowledge: given a context
 * window, the auto-compaction trigger is arithmetic, and it stays in one place so
 * every surface computes the same boundary.
 */
#include <optional>

namespace uos::recent_model_context {
  inline constexpr double CONTEXT_COMPACTION_TRIGGER_RATIO = 0.9;
  inline constexpr double CONTEXT_COMPACTION_RESERVED_TOKENS = 50'000;
  inline constexpr double CODEX_EFFECTIVE_CONTEXT_WINDOW_PERCENT = 95;

  /**
   * Trigger before either 90% of the model window is consumed or fewer than
   * 50,000 tokens remain, whichever happens first. The rule applies to every model.
   *
   * Throws rt::RangeError "contextWindowTokens must be a safe integer greater than
   * the reserved token budget".
   */
  double deriveAutoCompactTokenLimit(double contextWindowTokens);

  // `configuredLimit: number | null | undefined`: null and undefined are both std::nullopt.
  double resolvedAutoCompactTokenLimit(double contextWindowTokens, std::optional<double> configuredLimit);
}
