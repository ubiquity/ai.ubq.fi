#pragma once
#include "uos/rt/async.hpp"
#include <cstddef>
#include <optional>
#include <string>

namespace uos::provider::removed_provider_circuit {
  // Readonly<Record<string, never>>: an object without members.
  struct RemovedProviderCircuitProbe {
    friend bool operator==(const RemovedProviderCircuitProbe&, const RemovedProviderCircuitProbe&) = default;
  };

  // { route: "codex"; probe: null; transition: "none" }
  struct RemovedProviderCircuitRoute {
    std::string route{};
    std::nullptr_t probe = nullptr;
    std::string transition{};
  };

  // Each call returns a new, already fulfilled promise (`Promise.resolve(...)`).
  rt::Promise<RemovedProviderCircuitRoute> selectRemovedProviderCircuitRoute();
  rt::Promise<std::nullptr_t> claimRemovedProviderEarlyRecoveryProbe();
  rt::Promise<std::string> closeRemovedProviderCircuit(std::optional<RemovedProviderCircuitProbe> _probe);
  rt::Promise<std::string> recordRemovedProviderEligibleFailure(std::optional<RemovedProviderCircuitProbe> _probe);
  rt::Promise<std::string> releaseRemovedProviderCircuitProbe(std::optional<RemovedProviderCircuitProbe> _probe);
  rt::Promise<void> renewRemovedProviderCircuitProbe(RemovedProviderCircuitProbe _probe);
}
