#pragma once
// Port of src/cache/scope-experiment-targets.ts.
#include "uos/cache/scope_experiment_model.hpp"
#include "uos/cache/telemetry_gate.hpp"
#include <memory>
#include <variant>

namespace uos::cache::scope_experiment_targets {
  struct ResolvedScopeTarget { scope_experiment_model::PromptCacheScopeTargetBinding binding; };
  struct ScopeTargetResolutionResolved {
    static constexpr std::string_view status = "resolved";
    ResolvedScopeTarget value;
  };
  struct ScopeTargetResolutionInconclusive {
    static constexpr std::string_view status = "inconclusive";
    scope_experiment_model::InconclusiveReason reason;
  };
  using ScopeTargetResolution = std::variant<ScopeTargetResolutionResolved, ScopeTargetResolutionInconclusive>;
  struct AcquiredCycle { scope_experiment_model::ExperimentState state; std::string cycle_owner; };
  rt::Task<telemetry_gate::PromptCacheTelemetryBaselineResult> readPromptCacheScopeExperimentTelemetryBaseline();
  rt::Task<telemetry_gate::PromptCacheTelemetryBaselineResult> readPromptCacheScopeExperimentTelemetryBaseline(
    scope_experiment_model::PromptCacheScopeExperimentTelemetryBaselineTestOptions options);
  rt::Task<scope_experiment_model::PromptCacheScopeExperimentTelemetryBaseline> assertPromptCacheScopeExperimentTelemetryBaseline();
  rt::Task<scope_experiment_model::PromptCacheScopeExperimentTelemetryBaseline> assertPromptCacheScopeExperimentTelemetryBaseline(
    scope_experiment_model::PromptCacheScopeExperimentTelemetryBaselineTestOptions options);
  rt::Task<AcquiredCycle> acquireCycle(std::shared_ptr<rt::Kv> kv, scope_experiment_model::PromptCacheScopeTargetBinding target,
                                     std::string authPoolVersionstamp);
  rt::Task<void> renewLease(std::shared_ptr<rt::Kv> kv, scope_experiment_model::ExperimentState state, std::string cycleOwner);
  rt::Task<ScopeTargetResolution> resolveBoundTarget(std::shared_ptr<rt::Kv> kv, scope_experiment_model::PromptCacheScopeTargetBinding expected);
  bool sameTargetCore(const scope_experiment_model::PromptCacheScopeTargetBinding& left,
                      const scope_experiment_model::PromptCacheScopeTargetBinding& right);
}
