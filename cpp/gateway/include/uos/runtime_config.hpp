#pragma once
// Port of src/runtime-config.ts.
#include "uos/defaults.hpp"
#include "uos/models/codex_models.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uos::runtime_config {
  // readonly ["uos_ai", "runtime_config", "v2"]
  inline const rt::KvKey RUNTIME_CONFIG_V2_KEY{"uos_ai", "runtime_config", "v2"};
  inline constexpr double RUNTIME_CONFIG_CACHE_TTL_MS = 5 * 60'000;
  inline constexpr double RUNTIME_CONFIG_MAX_BYTES = 4'096;

  struct RuntimeCodexModel {
    std::string slug;
    std::optional<defaults::ReasoningEffort> default_reasoning_level{};
    std::vector<defaults::ReasoningEffort> supported_reasoning_levels;
    // Readonly<Record<string, ReasoningEffort>>: string members in JS property order.
    std::optional<json::object> reasoning_effort_wire_map{};
    std::optional<models::codex_models::RuntimePromptCacheCapabilities> prompt_cache{};
    friend bool operator==(const RuntimeCodexModel&, const RuntimeCodexModel&) = default;
  };

  struct RuntimeCodexModelsSnapshot {
    std::vector<RuntimeCodexModel> models;
    std::string source;
    double updated_at_ms;
    rt::Nullable<std::string> client_version{};  // `client_version?: string | null`
    friend bool operator==(const RuntimeCodexModelsSnapshot&, const RuntimeCodexModelsSnapshot&) = default;
  };

  struct RuntimeConfigV2 {
    double version = 2;  // `version: 2`
    std::string default_model;
    defaults::ReasoningEffort default_reasoning_effort;
    RuntimeCodexModelsSnapshot codex_models;
    double updated_at_ms;
    friend bool operator==(const RuntimeConfigV2&, const RuntimeConfigV2&) = default;
  };

  class RuntimeConfigError : public rt::JsError {
  public:
    explicit RuntimeConfigError(std::string message_) : rt::JsError(std::move(message_), "RuntimeConfigError") {}
  };

  // The records as JSON.stringify sees them: members in declaration order, an undefined
  // optional member omitted.
  json::value toJson(const RuntimeCodexModel& value);
  json::value toJson(const RuntimeCodexModelsSnapshot& value);
  json::value toJson(const RuntimeConfigV2& value);

  /**
   * Compacts one Codex model catalog snapshot. `snapshot` is deliberately typed
   * as possibly nullish: callers hand it values read from KV or casts (for
   * example `codexModels.value as never` in kv_migration.ts), so the runtime
   * guard below still has to reject a missing snapshot with this module's error
   * rather than a property access TypeError.
   */
  // `snapshot: CodexModelsSnapshot | null | undefined`; the pointer overload takes the unchecked
  // value (nullptr is undefined).
  RuntimeCodexModelsSnapshot compactRuntimeCodexModels(const std::optional<models::codex_models::CodexModelsSnapshot>& snapshot);
  RuntimeCodexModelsSnapshot compactRuntimeCodexModels(const json::value* snapshot);

  // `value: unknown`: nullptr is undefined. A value read from KV goes through the KvValue
  // overload, which presents the parts JSON cannot hold as the TS guards see them.
  std::optional<RuntimeConfigV2> normalizeRuntimeConfig(const json::value& value);
  std::optional<RuntimeConfigV2> normalizeRuntimeConfig(const json::value* value);
  std::optional<RuntimeConfigV2> normalizeRuntimeConfig(const rt::KvValue& value);

  // Readonly<{ defaultModel?: string | null; defaultReasoningEffort?: string | null; nowMs?: number }>
  struct BuildRuntimeConfigOptions {
    std::optional<std::string> defaultModel{};            // null and undefined alike
    std::optional<std::string> defaultReasoningEffort{};  // null and undefined alike
    std::optional<double> nowMs{};
  };

  // `buildRuntimeConfig(fullSnapshot, options = {})`
  RuntimeConfigV2 buildRuntimeConfig(const models::codex_models::CodexModelsSnapshot& fullSnapshot);
  RuntimeConfigV2 buildRuntimeConfig(const models::codex_models::CodexModelsSnapshot& fullSnapshot, const BuildRuntimeConfigOptions& options);

  // `loadRuntimeConfig(kvOverride?: Kv | null, nowMs = Date.now())`: in the two-argument
  // overload std::nullopt is an undefined kvOverride and a null pointer is null.
  rt::Task<std::optional<RuntimeConfigV2>> loadRuntimeConfig();
  rt::Task<std::optional<RuntimeConfigV2>> loadRuntimeConfig(std::shared_ptr<rt::Kv> kvOverride);
  rt::Task<std::optional<RuntimeConfigV2>> loadRuntimeConfig(std::optional<std::shared_ptr<rt::Kv>> kvOverride, double nowMs);

  // `cacheRuntimeConfig(config, nowMs = Date.now())`
  void cacheRuntimeConfig(const RuntimeConfigV2& config);
  void cacheRuntimeConfig(const RuntimeConfigV2& config, double nowMs);

  void resetRuntimeConfigCacheForTest();

  // Boost.JSON conversions: value_from writes toJson; value_to is strict (rt::TypeError for a
  // shape the C++ type cannot hold). Stored configs are read with normalizeRuntimeConfig.
  void tag_invoke(const json::value_from_tag&, json::value& out, const RuntimeCodexModel& value);
  RuntimeCodexModel tag_invoke(const json::value_to_tag<RuntimeCodexModel>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const RuntimeCodexModelsSnapshot& value);
  RuntimeCodexModelsSnapshot tag_invoke(const json::value_to_tag<RuntimeCodexModelsSnapshot>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const RuntimeConfigV2& value);
  RuntimeConfigV2 tag_invoke(const json::value_to_tag<RuntimeConfigV2>&, const json::value& value);
}
