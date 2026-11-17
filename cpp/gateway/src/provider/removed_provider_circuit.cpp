#include "uos/provider/removed_provider_circuit.hpp"

namespace uos::provider::removed_provider_circuit {
  rt::Promise<RemovedProviderCircuitRoute> selectRemovedProviderCircuitRoute() {
    return rt::Promise<RemovedProviderCircuitRoute>::resolve(RemovedProviderCircuitRoute{.route = "codex", .probe = nullptr, .transition = "none"});
  }

  rt::Promise<std::nullptr_t> claimRemovedProviderEarlyRecoveryProbe() { return rt::Promise<std::nullptr_t>::resolve(nullptr); }

  rt::Promise<std::string> closeRemovedProviderCircuit([[maybe_unused]] std::optional<RemovedProviderCircuitProbe> _probe) {
    return rt::Promise<std::string>::resolve(std::string("none"));
  }

  rt::Promise<std::string> recordRemovedProviderEligibleFailure([[maybe_unused]] std::optional<RemovedProviderCircuitProbe> _probe) {
    return rt::Promise<std::string>::resolve(std::string("none"));
  }

  rt::Promise<std::string> releaseRemovedProviderCircuitProbe([[maybe_unused]] std::optional<RemovedProviderCircuitProbe> _probe) {
    return rt::Promise<std::string>::resolve(std::string("none"));
  }

  rt::Promise<void> renewRemovedProviderCircuitProbe([[maybe_unused]] RemovedProviderCircuitProbe _probe) { return rt::Promise<void>::resolve(); }
}
