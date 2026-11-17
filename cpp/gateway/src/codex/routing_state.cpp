#include "uos/codex/routing_state.hpp"

#include "uos/rt/clock.hpp"

#include <utility>

namespace uos::codex::routing_state {
  RoutingStateCache routingStateCache{std::nullopt, std::nullopt, 0};

  bool isCodexActiveAccountSelectionCurrent(const json::value* value, const RoutingAccount& account, double activeGeneration) {
    return codexActiveAccountSelectionCurrent(value, nativeCodexAccountMetadata(account), activeGeneration);
  }

  bool isCodexActiveAccountSelectionCurrent(const json::value& value, const RoutingAccount& account, double activeGeneration) {
    return isCodexActiveAccountSelectionCurrent(&value, account, activeGeneration);
  }

  void setRoutingStateCache(std::optional<CodexAccountRoutingState> state, std::optional<std::string> versionstamp) {
    routingStateCache.state = std::move(state);
    routingStateCache.versionstamp = std::move(versionstamp);
    routingStateCache.loadedAtMs = rt::dateNow();
  }

  std::optional<CodexAccountRoutingState> getCachedRoutingState() { return routingStateCache.state; }

  CodexRoutingAccount nativeCodexAccountMetadata(const RoutingAccount& account) { return static_cast<const CodexRoutingAccount&>(account); }
}
