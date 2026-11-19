#pragma once
// Port of src/models/metadata.ts.
#include "uos/defaults.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::models::metadata {
  /**
   * One place resolves what the gateway knows about a model.
   *
   * Context windows come from `openrouter`, which publishes the model's real
   * maximum. The Codex endpoint's own catalog understates what it accepts: 916,463
   * tokens verified for `gpt-6-astra` against a 272,000 served window and an
   * 872,000 override cap that Codex clamps client overrides to. OpenRouter leads
   * the candidate list and breaks ties; a narrower third-party entry never shrinks
   * a route below what that route itself declares, and the Codex subscription bound
   * is the last resort so a Codex-served id is never reported as unknown.
   *
   * Reasoning tiers keep the opposite order — `codex_upload`, then the serving
   * provider, then OpenRouter — because the uploaded catalog is authoritative for
   * the tier strings the gateway must preserve verbatim.
   *
   * The curated per-model tables this replaced are commented out in
   * `src/recent_model_context.ts`. Nothing here invents a value: an id no source
   * describes resolves to nulls and reports `unknown`, so a gap shows up as a gap
   * instead of as a plausible-looking wrong number. Derivation is still allowed and
   * is labeled as such — the auto-compaction limit is computed from a known
   * context window by the shared 90%-or-50k-reserve rule, which is arithmetic, not
   * per-model knowledge.
   */
  // "codex_upload" | "codex_subscription" | "provider_discovery" | "openrouter" | "unknown"
  using ModelMetadataSource = std::string;

  /**
   * What a single source claims about a model, in upstream vocabulary: Codex
   * snapshot records use `context_window`/`default_reasoning_level`, and callers
   * adapt those before handing them over.
   */
  struct ModelMetadataHint {
    rt::Nullable<double> context_window_tokens{};
    rt::Nullable<double> max_context_window_tokens{};
    rt::Nullable<double> auto_compact_token_limit_tokens{};
    rt::Nullable<double> effective_context_window_percent{};
    rt::Nullable<json::array> supported_reasoning_levels{};
    std::optional<json::value> default_reasoning_effort{};  // unknown; std::nullopt is undefined
    friend bool operator==(const ModelMetadataHint&, const ModelMetadataHint&) = default;
  };

  struct ResolvedModelMetadata {
    std::optional<double> context_window_tokens;
    std::optional<double> max_context_window_tokens;
    std::optional<double> auto_compact_token_limit_tokens;
    std::optional<double> effective_context_window_percent;
    std::optional<std::vector<defaults::ReasoningEffort>> supported_reasoning_levels;
    std::optional<defaults::ReasoningEffort> default_reasoning_effort;
    ModelMetadataSource context_source;
    ModelMetadataSource reasoning_source;
    friend bool operator==(const ResolvedModelMetadata&, const ResolvedModelMetadata&) = default;
  };

  struct ModelMetadataSources {
    rt::Nullable<ModelMetadataHint> codex{};
    /**
     * The conservative Codex-subscription bound, passed by callers that know the
     * id is served by the Codex subscription. It outranks provider discovery and
     * enrichment so a subscription model never advertises a window Codex will not
     * honor, and it never overrides a window the upload stated.
     */
    rt::Nullable<ModelMetadataHint> codexSubscription{};
    rt::Nullable<ModelMetadataHint> provider{};
    /** Undefined reads the cached OpenRouter catalog; null consults no enrichment. */
    rt::Nullable<openrouter_models::OpenRouterModelMetadata> openRouter{};
  };

  /**
   * Adapt a Codex snapshot record to a hint. The uploaded catalog publishes
   * `context_window`, `max_context_window`, `auto_compact_token_limit` and
   * `effective_context_window_percent` as plain numbers, a
   * `supported_reasoning_levels` array whose entries are bare strings, `null`, or
   * `{ effort }` objects, and a `default_reasoning_level` whose explicit `null`
   * means `none`.
   */
  // `record: Record<string, unknown> | null | undefined`: nullptr is null or undefined.
  std::optional<ModelMetadataHint> codexSnapshotMetadataHint(const json::object* record);
  std::optional<ModelMetadataHint> codexSnapshotMetadataHint(const json::object& record);

  /**
   * The window a Codex subscription serves when the uploaded catalog does not state
   * one. A subscription serves frontier models below their API-level maximum, and
   * third-party catalogs publish that larger maximum, so falling through to
   * enrichment would advertise a window the subscription cannot honor. Measured
   * from the uploaded catalog on 2026-09-17: every Codex-served id reports
   * `context_window` 272,000 with `max_context_window` 872,000.
   *
   * This is a conservative deployment bound, not per-model curation: an explicit
   * window in the uploaded catalog still wins, and ids Codex does not serve are
   * untouched.
   */
  inline constexpr double CODEX_SUBSCRIPTION_CONTEXT_WINDOW_TOKENS = 272'000;
  inline constexpr double CODEX_SUBSCRIPTION_MAX_CONTEXT_WINDOW_TOKENS = 872'000;

  /** The conservative Codex window as a hint, for Codex-served ids the upload leaves unstated. */
  ModelMetadataHint codexSubscriptionMetadataHint();

  ResolvedModelMetadata resolveModelMetadata(std::string_view modelId);
  ResolvedModelMetadata resolveModelMetadata(std::string_view modelId, const ModelMetadataSources& sources);

  // JSON members in declaration order (an undefined member is omitted, null written as
  // null); reading is strict (rt::TypeError naming the member) and drops other members.
  void tag_invoke(const json::value_from_tag&, json::value& out, const ModelMetadataHint& value);
  ModelMetadataHint tag_invoke(const json::value_to_tag<ModelMetadataHint>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ResolvedModelMetadata& value);
  ResolvedModelMetadata tag_invoke(const json::value_to_tag<ResolvedModelMetadata>&, const json::value& value);
}
