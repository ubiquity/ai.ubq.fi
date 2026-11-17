#pragma once
// Port of src/harmony/probe-scenarios.ts.
#include "uos/harmony/probes.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace uos::harmony::probe_scenarios {
  struct ProbeScenario {
    std::string id;
    probes::ProbeGroup group;
    std::string style;
    std::string description;
    std::optional<probes::ProbeOutcome> expectedOutcome;
    std::function<rt::Task<probes::ProbeScenarioResult>(probes::ProbeContext)> run;
  };
  // The builder only binds deferred scenario callbacks; execution never occurs at import time.
  std::vector<ProbeScenario> makeProbeScenarios();
  inline const std::vector<ProbeScenario> PROBE_SCENARIOS = makeProbeScenarios();
}
