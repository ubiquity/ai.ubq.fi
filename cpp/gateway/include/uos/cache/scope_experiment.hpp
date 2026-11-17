#pragma once
// Port of src/cache/scope-experiment.ts.
#include "uos/cache/scope_experiment_model.hpp"
#include "uos/cache/scope_experiment_targets.hpp"

namespace uos::cache::scope_experiment {
  using scope_experiment_model::PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX;
  using scope_experiment_model::PromptCacheScopeExperimentFailedError;
  using scope_experiment_model::PromptCacheScopeExperimentBusyError;
  using scope_experiment_model::PromptCacheScopeExperimentUnavailableError;
  using scope_experiment_model::readPromptCacheScopeExperimentCompletedUsage;
  using scope_experiment_targets::assertPromptCacheScopeExperimentTelemetryBaseline;
  using scope_experiment_targets::readPromptCacheScopeExperimentTelemetryBaseline;
  rt::Task<scope_experiment_model::PromptCacheScopeExperimentResult> runPromptCacheScopeExperiment(
    scope_experiment_model::PromptCacheScopeExperimentTelemetryBaseline telemetryBaseline);
}
