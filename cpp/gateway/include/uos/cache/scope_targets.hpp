#pragma once
// Port of src/cache/scope-targets.ts.
#include "uos/models/codex_models.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::cache::scope_targets {
  inline constexpr std::string_view PROMPT_CACHE_SCOPE_TARGET_CODEX_PROVIDER = models::codex_models::CODEX_CHATGPT_PROMPT_CACHE_PROVIDER;
  inline constexpr std::string_view PROMPT_CACHE_SCOPE_TARGET_METERED_PROVIDER = "metered";
  inline constexpr std::string_view PROMPT_CACHE_SCOPE_TARGET_CODEX_TELEMETRY_PROVIDER = "chatgpt_codex";
  inline constexpr std::string_view PROMPT_CACHE_SCOPE_TARGET_METERED_TELEMETRY_PROVIDER = "metered";
  using PromptCacheScopeTargetProvider = std::string;
  using PromptCacheScopeTargetTelemetryProvider = std::string;
  struct MeteredFallbackRosterUnknown { static constexpr std::string_view status = "unknown"; };
  struct MeteredFallbackRosterAuthoritative { static constexpr std::string_view status = "authoritative"; std::vector<std::string> model_ids; };
  using MeteredFallbackRoster = std::variant<MeteredFallbackRosterUnknown, MeteredFallbackRosterAuthoritative>;
  struct PromptCacheScopeTargetTopologyCodexAccountPool {
    static constexpr std::string_view kind = "codex_account_pool";
    double configured_slot_count;
    std::optional<std::string> auth_pool_versionstamp;
  };
  struct PromptCacheScopeTargetTopologySingleCredential { static constexpr std::string_view kind = "single_credential"; };
  using PromptCacheScopeTargetTopology = std::variant<PromptCacheScopeTargetTopologyCodexAccountPool, PromptCacheScopeTargetTopologySingleCredential>;
  using PromptCacheScopeTargetTopologyKind = std::string;
  struct PromptCacheScopeTargetProbeabilityProbeable { static constexpr std::string_view status = "probeable"; std::string adapter = "codex_two_slot"; };
  struct PromptCacheScopeTargetProbeabilityUnprobeable { static constexpr std::string_view status = "unprobeable"; std::string reason; };
  using PromptCacheScopeTargetProbeability = std::variant<PromptCacheScopeTargetProbeabilityProbeable, PromptCacheScopeTargetProbeabilityUnprobeable>;
  struct PromptCacheScopeTarget {
    std::string id;
    PromptCacheScopeTargetProvider provider;
    PromptCacheScopeTargetTelemetryProvider telemetry_provider;
    std::string model;
    std::string model_family_id;
    std::string model_family_source;
    models::codex_models::PromptCacheScopeProbeProfile probe_profile;
    bool catalog_model_present;
    std::string codex_cache_qualification;
    PromptCacheScopeTargetTopology topology;
    PromptCacheScopeTargetProbeability probeability;
    std::optional<std::string> catalog_versionstamp;
    std::optional<std::string> catalog_client_version;
    std::optional<std::string> codex_auth_pool_versionstamp;
    std::optional<std::string> codex_auth_pool_identity_fingerprint;
    std::string capability_fingerprint;
  };
  struct PromptCacheScopeTargetMeteredRosterDiagnostic {
    std::string status;
    std::vector<std::string> model_ids;
    std::vector<std::string> non_catalog_model_ids;
  };
  struct PromptCacheScopeTargetInventory {
    std::string status;
    std::string reason;
    std::vector<PromptCacheScopeTarget> targets;
    PromptCacheScopeTargetMeteredRosterDiagnostic metered_fallback_roster;
    std::optional<std::string> inventory_fingerprint;
    std::optional<std::string> binding_fingerprint;
  };
  struct DerivePromptCacheScopeTargetInventoryInput {
    std::optional<models::codex_models::CodexModelsSnapshot> snapshot;
    std::optional<std::string> catalogVersionstamp;
    json::value codexAuthPool;
    std::optional<std::string> codexAuthPoolVersionstamp;
    std::optional<MeteredFallbackRoster> meteredFallbackRoster;
  };
  struct LoadPromptCacheScopeTargetInventoryOptions {
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::optional<MeteredFallbackRoster> meteredFallbackRoster{};
  };
  std::string buildPromptCacheScopeTargetId(const PromptCacheScopeTargetProvider& provider, const PromptCacheScopeTargetTelemetryProvider& telemetryProvider,
    const PromptCacheScopeTargetTopologyKind& topologyKind, const std::string& model,
    const models::codex_models::PromptCacheScopeProbeProfile& probeProfile = std::string(models::codex_models::PROMPT_CACHE_SCOPE_PROBE_PROFILE));
  rt::Task<PromptCacheScopeTargetInventory> derivePromptCacheScopeTargetInventory(DerivePromptCacheScopeTargetInventoryInput input);
  rt::Task<PromptCacheScopeTargetInventory> loadPromptCacheScopeTargetInventory();
  rt::Task<PromptCacheScopeTargetInventory> loadPromptCacheScopeTargetInventory(LoadPromptCacheScopeTargetInventoryOptions options);
#define UOS_SCOPE_CODEC(T) void tag_invoke(const json::value_from_tag&, json::value& out, const T& value); T tag_invoke(const json::value_to_tag<T>&, const json::value& value);
  UOS_SCOPE_CODEC(MeteredFallbackRoster)
  UOS_SCOPE_CODEC(PromptCacheScopeTargetTopology)
  UOS_SCOPE_CODEC(PromptCacheScopeTargetProbeability)
  UOS_SCOPE_CODEC(PromptCacheScopeTarget)
  UOS_SCOPE_CODEC(PromptCacheScopeTargetMeteredRosterDiagnostic)
  UOS_SCOPE_CODEC(PromptCacheScopeTargetInventory)
#undef UOS_SCOPE_CODEC
}
