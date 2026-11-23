#include "uos/codex/routing_state.hpp"
#include "uos/rt/memory.hpp"

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
    const auto& cached = routingStateCache.state;
    std::size_t bytes = 0;
    if (cached) {
      bytes = sizeof(RoutingStateCache) + cached->slots.capacity() * sizeof(CodexRoutingSlot);
      if (routingStateCache.versionstamp) bytes += rt::memory::bytes(*routingStateCache.versionstamp);
      for (const auto& slot : cached->slots) {
        bytes += rt::memory::bytes(slot.credential_version);
        for (const auto* value : {&slot.account_id_hash, &slot.invalid_credential_version, &slot.quota_block_source})
          if (*value) bytes += rt::memory::bytes(**value);
        if (slot.quota_blocked_classes) bytes += rt::memory::bytes(*slot.quota_blocked_classes);
        if (slot.quota_blocks_by_class) for (const auto& [name, block] : *slot.quota_blocks_by_class)
          bytes += sizeof(block) + 2 * (sizeof(name) + rt::memory::bytes(name)) + rt::memory::bytes(block.source);
        if (slot.probe_lease) bytes += rt::memory::bytes(slot.probe_lease->token);
      }
    }
    rt::memory::setCache(rt::memory::Cache::codex_routing, cached ? 1 : 0, bytes);
  }

  std::optional<CodexAccountRoutingState> getCachedRoutingState() { return routingStateCache.state; }

  CodexRoutingAccount nativeCodexAccountMetadata(const RoutingAccount& account) { return static_cast<const CodexRoutingAccount&>(account); }
}
