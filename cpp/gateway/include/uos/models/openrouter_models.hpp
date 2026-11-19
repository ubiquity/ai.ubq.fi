#pragma once
// Port of src/models/openrouter-models.ts.
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/prelude.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace uos::models::openrouter_models {
  /**
   * OpenRouter's public catalog is the gateway's third-party enrichment source for
   * per-model capabilities. It is unauthenticated, so a deployment with no
   * OpenRouter credential still gets the data, and it publishes what the curated
   * tables used to guess: the context window, the maximum context window, and the
   * reasoning efforts a model actually accepts.
   *
   * Three rules keep this source honest:
   *
   * - It never outranks a first-party statement. The uploaded Codex catalog stays
   *   authoritative for every id Codex serves, and a serving provider's own
   *   discovery row outranks it too. See `src/model_metadata.ts` for the order.
   * - The request path never waits on it. Reads are cached-only, so a slow or
   *   unavailable third party degrades a model to "unknown" instead of stalling
   *   `/uos/models/catalog`.
   * - OpenRouter's model list defaults to text output. The request explicitly
   *   includes decisions so decision models are discoverable without also pulling
   *   image-only or embedding rows this gateway does not serve.
   */
  inline constexpr std::string_view OPENROUTER_BASE_URL = "https://openrouter.ai";
  inline constexpr double OPENROUTER_FETCH_TIMEOUT_MS = 10'000;
  inline constexpr double OPENROUTER_MODELS_CACHE_TTL_MS = 5 * 60'000;

  struct OpenRouterReasoning {
    std::vector<std::string> supported_efforts = {};
    std::optional<std::string> default_effort = std::nullopt;
    /**
     * True when the upstream always reasons. It qualifies `none`: an effort list
     * without `none` on a mandatory model means "cannot be disabled", while the
     * same list on an optional model means "OpenRouter did not enumerate it".
     */
    bool mandatory = false;
    friend bool operator==(const OpenRouterReasoning&, const OpenRouterReasoning&) = default;
  };

  struct OpenRouterModelMetadata {
    /** Upstream id, always `author/slug`. */
    std::string id = {};
    std::optional<std::int64_t> context_window_tokens = std::nullopt;
    std::optional<std::int64_t> max_context_window_tokens = std::nullopt;
    std::optional<OpenRouterReasoning> reasoning = std::nullopt;
    /**
     * The upstream's declared output modalities. The public model API defaults to
     * text rows, so the discovery request asks for text and decisions together.
     * A fixture without an architecture block keeps the text default.
     */
    std::optional<std::vector<std::string>> output_modalities = std::nullopt;
    friend bool operator==(const OpenRouterModelMetadata&, const OpenRouterModelMetadata&) = default;
  };

  struct OpenRouterModelsSnapshot {
    std::vector<OpenRouterModelMetadata> models = {};
    double updated_at_ms = 0;
  };

  using OpenRouterFetch = rt::Fetch;

  /**
   * Ids are joined by an ordered set of key tiers rather than one normalized
   * string, because a wrong join silently publishes another model's capabilities.
   * A gateway id such as `xiaomi-mimo-v2-5` and its upstream `xiaomi/mimo-v2.5`
   * differ only in punctuation, while `codex-auto-review` has no upstream row at
   * all and must stay unknown. Each tier is consulted in order and a tier whose key
   * is claimed by more than one upstream model is skipped, so an ambiguous match
   * degrades to unknown instead of guessing.
   */
  // A tier value of nullptr is a key claimed by two models; the pointers name rows of the
  // models the index was built from, which must outlive it.
  struct OpenRouterMatchIndex {
    std::unordered_map<std::string, const OpenRouterModelMetadata*> exactId;
    std::unordered_map<std::string, const OpenRouterModelMetadata*> exactSlug;
    std::unordered_map<std::string, const OpenRouterModelMetadata*> normalizedId;
    std::unordered_map<std::string, const OpenRouterModelMetadata*> normalizedSlug;
    std::unordered_map<std::string, const OpenRouterModelMetadata*> alias;
    /** Base keys of `:free`/`:batch` rows, consulted last, for a gateway id with only that marker. */
    std::unordered_map<std::string, const OpenRouterModelMetadata*> base;
  };

  /** A row with no architecture statement keeps the historical text default. */
  bool openRouterModelSupportsText(const OpenRouterModelMetadata& model);

  bool openRouterModelSupportsDecisions(const OpenRouterModelMetadata& model);

  /** The gateway routes this row actually serves, in stable wire order. */
  std::vector<std::string> openRouterModelEndpointPaths(const OpenRouterModelMetadata& model);

  // `payload: unknown`: nullptr is undefined.
  std::vector<OpenRouterModelMetadata> openRouterModelsFromPayload(const json::value* payload);
  std::vector<OpenRouterModelMetadata> openRouterModelsFromPayload(const json::value& payload);

  std::optional<OpenRouterModelMetadata> matchOpenRouterModel(const std::vector<OpenRouterModelMetadata>& models, std::string_view modelId,
                                                              const OpenRouterMatchIndex* index = nullptr);

  void resetOpenRouterModelsCacheForTest();

  // An empty OpenRouterFetch is null.
  void setOpenRouterModelsFetchForTest(OpenRouterFetch fetcher);
  void setOpenRouterIndexAllocationFailureForTest(bool fail);

  // The options of fetchOpenRouterModels; an empty fetcher is undefined.
  struct FetchOpenRouterModelsOptions {
    OpenRouterFetch fetcher = {};
    std::optional<rt::AbortSignal> signal = std::nullopt;
    bool force = false;
    bool cachedOnly = false;
  };

  // nullptr is null.
  rt::Task<std::shared_ptr<const OpenRouterModelsSnapshot>> fetchOpenRouterModels(FetchOpenRouterModelsOptions options = {});

  /**
   * Start a refresh without waiting for it. The catalog builder calls this so the
   * first page load after a cold start begins populating the cache while it serves
   * whatever is already known.
   */
  void warmOpenRouterModels();

  /** The enriched metadata for a model id, from cache only, or null when unknown. */
  std::optional<OpenRouterModelMetadata> openRouterMetadataFor(std::string_view modelId);

  std::shared_ptr<const OpenRouterModelsSnapshot> openRouterModelsSnapshot();
}
