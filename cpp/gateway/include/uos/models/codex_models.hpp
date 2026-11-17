#pragma once
// Port of src/models/codex-models.ts.
#include "uos/rt/prelude.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::models::codex_models {
  // Readonly<{ models: Record<string, unknown>[]; source: string; updated_at_ms: number; client_version?: string | null }>.
  // A stored catalog snapshot, read back from KV and from casts, so it stays wire JSON:
  // the functions below validate `models` instead of trusting it. `client_version:
  // undefined` (normalizeCodexModelsPayload without a version) is an absent member.
  using CodexModelsSnapshot = json::object;

  // Module-private literal unions in the TS, named here because the exported records use them.
  using PromptCacheControlSource = std::string;  // "catalog" | "live_probe" | "inferred"
  using PromptCacheScopeSource = std::string;    // "live_probe"
  using PromptCacheMode = std::string;           // "implicit" | "explicit"
  using PromptCacheUsageField = std::string;     // "cached_tokens" | "cache_write_tokens"
  using PromptCacheAccountSlots = std::string;     // "shared" | "account_scoped" | "unknown"
  using PromptCacheTokenRefresh = std::string;     // "preserved" | "changed" | "unknown"
  using PromptCacheConversationId = std::string;   // "independent" | "scoped" | "unknown"

  /**
   * Scope evidence is valid only for this fixed v5 implicit Responses request
   * shape. Keep this literal immutable so an earlier experiment definition,
   * plain-key observation, explicit-mode request, or provider-wide result cannot
   * be read as current scope evidence.
   */
  inline constexpr std::string_view PROMPT_CACHE_SCOPE_PROBE_PROFILE = "responses_implicit_input_text_keyed_cycle_isolated_v5";
  using PromptCacheScopeProbeProfile = std::string;  // typeof PROMPT_CACHE_SCOPE_PROBE_PROFILE

  /** The capability-catalog identity for the ChatGPT Codex transport. */
  inline constexpr std::string_view CODEX_CHATGPT_PROMPT_CACHE_PROVIDER = "codex_chatgpt";

  struct PromptCacheControls {
    // Readonly<{ responses?: string[]; chat_completions?: string[] }>
    struct BreakpointBlockTypes {
      std::optional<std::vector<std::string>> responses{};
      std::optional<std::vector<std::string>> chat_completions{};
      friend bool operator==(const BreakpointBlockTypes&, const BreakpointBlockTypes&) = default;
    };
    std::optional<bool> key{};
    std::optional<bool> implicit{};
    std::optional<bool> explicit_breakpoints{};
    std::optional<std::vector<PromptCacheMode>> modes{};
    std::optional<std::vector<std::string>> ttls{};
    std::optional<std::vector<std::string>> legacy_retentions{};
    std::optional<BreakpointBlockTypes> breakpoint_block_types{};
    std::optional<std::vector<PromptCacheUsageField>> expected_usage_fields{};
    PromptCacheControlSource source;
    double verified_at_ms;
    friend bool operator==(const PromptCacheControls&, const PromptCacheControls&) = default;
  };

  struct PromptCacheScope {
    PromptCacheScopeProbeProfile probe_profile;
    PromptCacheAccountSlots account_slots;
    PromptCacheTokenRefresh token_refresh;
    PromptCacheConversationId conversation_id;
    std::optional<std::string> effective_model{};
    double reproducible_cycles;
    PromptCacheScopeSource source;
    double verified_at_ms;
    friend bool operator==(const PromptCacheScope&, const PromptCacheScope&) = default;
  };

  struct PromptCacheProvider {
    std::string id;
    std::optional<PromptCacheControls> controls{};
    std::optional<PromptCacheScope> scope{};
    friend bool operator==(const PromptCacheProvider&, const PromptCacheProvider&) = default;
  };

  /**
   * `false` is an explicit, verified unsupported result. An omitted field stays
   * unknown so standard OpenAI cache controls continue to pass through.
   */
  // The `false` arm of PromptCacheCapabilities (JSON: false).
  struct PromptCacheCapabilitiesUnsupported {
    friend bool operator==(const PromptCacheCapabilitiesUnsupported&, const PromptCacheCapabilitiesUnsupported&) = default;
  };
  // The `Readonly<{ version: 1; providers: PromptCacheProvider[] }>` arm.
  struct PromptCacheCapabilitiesV1 {
    double version = 1;
    std::vector<PromptCacheProvider> providers;
    friend bool operator==(const PromptCacheCapabilitiesV1&, const PromptCacheCapabilitiesV1&) = default;
  };
  using PromptCacheCapabilities = std::variant<PromptCacheCapabilitiesUnsupported, PromptCacheCapabilitiesV1>;

  /** Runtime config retains cache controls only; probe scope remains full-catalog evidence. */
  using RuntimePromptCacheCapabilitiesUnsupported = PromptCacheCapabilitiesUnsupported;
  // The `Readonly<{ version: 1; providers: Readonly<{ id: string; controls: PromptCacheControls }>[] }>` arm.
  struct RuntimePromptCacheCapabilitiesV1 {
    // Readonly<{ id: string; controls: PromptCacheControls }>
    struct Provider {
      std::string id;
      PromptCacheControls controls;
      friend bool operator==(const Provider&, const Provider&) = default;
    };
    double version = 1;
    std::vector<Provider> providers;
    friend bool operator==(const RuntimePromptCacheCapabilitiesV1&, const RuntimePromptCacheCapabilitiesV1&) = default;
  };
  using RuntimePromptCacheCapabilities = std::variant<RuntimePromptCacheCapabilitiesUnsupported, RuntimePromptCacheCapabilitiesV1>;

  // The records as JSON: members in declaration order, an undefined optional member omitted.
  json::value toJson(const PromptCacheControls& value);
  json::value toJson(const PromptCacheScope& value);
  json::value toJson(const PromptCacheProvider& value);
  json::value toJson(const PromptCacheCapabilities& value);
  json::value toJson(const RuntimePromptCacheCapabilities& value);

  /**
   * Normalize the gateway-owned cache capability envelope. This stays separate
   * from OpenAI request schemas; it only describes observed provider behavior.
   */
  // `value: unknown`: nullptr is undefined.
  std::optional<PromptCacheCapabilities> normalizePromptCacheCapabilities(const json::value* value);
  std::optional<PromptCacheCapabilities> normalizePromptCacheCapabilities(const json::value& value);

  std::optional<RuntimePromptCacheCapabilities> compactPromptCacheCapabilities(const json::value* value);
  std::optional<RuntimePromptCacheCapabilities> compactPromptCacheCapabilities(const json::value& value);

  /**
   * Snapshot writers use this exact-slug lookup before publishing probe
   * evidence. A duplicate or renamed entry is intentionally indistinguishable
   * from absence: neither is safe to update from a prior live observation.
   */
  std::optional<json::object> getUniqueCodexModelBySlug(const CodexModelsSnapshot& snapshot, std::string_view slug);

  std::optional<PromptCacheProvider> getCodexModelPromptCacheProvider(const CodexModelsSnapshot& snapshot, std::string_view slug, std::string_view providerId);

  /** Exact controls required before the fixed plain-key scope matrix may dispatch. */
  bool isCodexModelPromptCacheScopeExperimentEligible(const CodexModelsSnapshot& snapshot, std::string_view slug);

  /** A scope may be schema-valid but still unsafe to publish from a live probe. */
  bool isConcretePromptCacheScope(const PromptCacheScope& scope, double reproducibleCycles = 3);

  /**
   * Return a copy with scope evidence attached to one already-present provider.
   * Controls and all other provider records remain byte-for-byte catalog-owned.
   */
  std::optional<CodexModelsSnapshot> withCodexModelPromptCacheScope(const CodexModelsSnapshot& snapshot, std::string_view slug, std::string_view providerId,
                                                                    const PromptCacheScope& scope);

  /**
   * Prefer the incoming catalog's evidence when it supplies it, while retaining
   * cached evidence that a catalog refresh cannot know (for example a live
   * account-scope probe). Provider IDs are deliberately not collapsed.
   */
  // `previousRaw: unknown, nextRaw: unknown`: nullptr is undefined.
  std::optional<PromptCacheCapabilities> mergePromptCacheCapabilities(const json::value* previousRaw, const json::value* nextRaw);
  std::optional<PromptCacheCapabilities> mergePromptCacheCapabilities(const json::value& previousRaw, const json::value& nextRaw);

  /**
   * Catalog snapshots are refreshed from upstream metadata, while cache evidence
   * can be written independently. Preserve valid evidence only for matching
   * model slugs so removed/renamed models do not inherit stale capabilities.
   */
  // `previous: CodexModelsSnapshot | null | undefined`. When previous holds models and
  // `next.models` is not an array, throws rt::TypeError with JavaScriptCore's text.
  CodexModelsSnapshot mergeCodexModelPromptCacheCapabilities(const CodexModelsSnapshot& next, const std::optional<CodexModelsSnapshot>& previous);

  std::optional<std::array<double, 3>> parseCodexClientVersion(std::string_view value);

  std::optional<double> compareCodexClientVersions(std::string_view left, std::string_view right);

  std::optional<std::string> getCodexModelsSnapshotDefaultModel(const std::optional<CodexModelsSnapshot>& snapshot);

  // Readonly<{ source?: string; clientVersion?: string | null; updatedAtMs?: number | null }>
  struct CodexModelsPayloadOverrides {
    std::optional<std::string> source{};
    std::optional<std::string> clientVersion{};  // null and undefined alike
    std::optional<double> updatedAtMs{};         // null and undefined alike
  };

  // `value: unknown`: nullptr is undefined. Without overrides: `overrides = {}`.
  std::optional<CodexModelsSnapshot> normalizeCodexModelsPayload(const json::value* value);
  std::optional<CodexModelsSnapshot> normalizeCodexModelsPayload(const json::value* value, const CodexModelsPayloadOverrides& overrides);
  std::optional<CodexModelsSnapshot> normalizeCodexModelsPayload(const json::value& value);
  std::optional<CodexModelsSnapshot> normalizeCodexModelsPayload(const json::value& value, const CodexModelsPayloadOverrides& overrides);

  // Boost.JSON conversions (json::value_from / json::value_to): value_from writes the
  // members in declaration order (an undefined optional member is omitted, the `false`
  // arm is `false`); value_to is strict and throws rt::TypeError for a shape the C++
  // type cannot hold. The TS never trusts stored capability JSON: read it with the
  // normalizers above, not value_to.
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheControls::BreakpointBlockTypes& value);
  PromptCacheControls::BreakpointBlockTypes tag_invoke(const json::value_to_tag<PromptCacheControls::BreakpointBlockTypes>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheControls& value);
  PromptCacheControls tag_invoke(const json::value_to_tag<PromptCacheControls>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheScope& value);
  PromptCacheScope tag_invoke(const json::value_to_tag<PromptCacheScope>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheProvider& value);
  PromptCacheProvider tag_invoke(const json::value_to_tag<PromptCacheProvider>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheCapabilitiesUnsupported& value);
  PromptCacheCapabilitiesUnsupported tag_invoke(const json::value_to_tag<PromptCacheCapabilitiesUnsupported>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheCapabilitiesV1& value);
  PromptCacheCapabilitiesV1 tag_invoke(const json::value_to_tag<PromptCacheCapabilitiesV1>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const RuntimePromptCacheCapabilitiesV1::Provider& value);
  RuntimePromptCacheCapabilitiesV1::Provider tag_invoke(const json::value_to_tag<RuntimePromptCacheCapabilitiesV1::Provider>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const RuntimePromptCacheCapabilitiesV1& value);
  RuntimePromptCacheCapabilitiesV1 tag_invoke(const json::value_to_tag<RuntimePromptCacheCapabilitiesV1>&, const json::value& value);
}
