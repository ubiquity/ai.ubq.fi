// Native Codex 429 I/O and CAS orchestration over the source-owned portable decisions.
#include "uos/codex/rate_limit_429.hpp"

#include "uos/codex/capacity_routing.hpp"
#include "uos/rt/clock.hpp"

#include <utility>

namespace uos::codex::rate_limit_429 {
  namespace {
    using routing_state::CodexAccountRoutingState;
    using routing_state::CodexRoutingSlot;
    using routing_state::nativeCodexAccountMetadata;
    using routing_state::RoutingAccount;

    rt::Task<void> releaseNonBlockingQuotaProbe(RoutingAccount account) {
      if (!account.probeGeneration || !rt::truthy(account.probeToken)) co_return;
      capacity_routing::CodexRoutingTransform transform = [&account](const CodexAccountRoutingState& state) -> std::optional<CodexAccountRoutingState> {
        const std::optional<CodexRoutingSlot> slot = authority::native_codex_429::nonBlockingQuotaProbeSlot(state, nativeCodexAccountMetadata(account));
        if (!slot) return std::nullopt;
        return capacity_routing::withSlot(state, account.slot, *slot);
      };
      rt::Promise<std::optional<CodexAccountRoutingState>> updating(capacity_routing::updateRoutingState(std::move(transform)));
      co_await updating.wait();
    }
  }

  rt::Task<Codex429Classification> readCodex429(rt::Response response) { return readCodex429(std::move(response), rt::dateNow()); }

  rt::Task<Codex429Classification> readCodex429(rt::Response response, double now) {
    return authority::native_codex_429::classifyCodex429(std::move(response), now);
  }

  rt::Task<Codex429Classification> markCodexQuotaBlockedWithMode(RoutingAccount account, rt::Response response) {
    return markCodexQuotaBlockedWithMode(std::move(account), std::move(response), rt::dateNow(), false);
  }

  rt::Task<Codex429Classification> markCodexQuotaBlockedWithMode(RoutingAccount account, rt::Response response, double now, bool recoveryProbe) {
    rt::Promise<Codex429Classification> reading(readCodex429(std::move(response), now));
    const Codex429Classification parsed = co_await reading.wait();
    if (!parsed.usageLimitReached || !parsed.retryAtMs || !parsed.quotaBlockSource) {
      rt::Promise<void> releasing(releaseNonBlockingQuotaProbe(account));
      co_await releasing.wait();
    } else {
      capacity_routing::CodexRoutingTransform transform = [&account, &parsed, now,
                                                           recoveryProbe](const CodexAccountRoutingState& state) -> std::optional<CodexAccountRoutingState> {
        const std::optional<CodexRoutingSlot> slot =
          authority::native_codex_429::quotaBlockedSlot(state, nativeCodexAccountMetadata(account), parsed, now, recoveryProbe);
        if (!slot) return std::nullopt;
        return capacity_routing::withSlot(state, account.slot, *slot);
      };
      rt::Promise<std::optional<CodexAccountRoutingState>> updating(capacity_routing::updateRoutingState(std::move(transform)));
      co_await updating.wait();
    }
    co_return parsed;
  }

  rt::Task<Codex429Classification> markCodexQuotaBlocked(RoutingAccount account, rt::Response response) {
    return markCodexQuotaBlocked(std::move(account), std::move(response), rt::dateNow());
  }

  rt::Task<Codex429Classification> markCodexQuotaBlocked(RoutingAccount account, rt::Response response, double now) {
    rt::Promise<Codex429Classification> marking(markCodexQuotaBlockedWithMode(std::move(account), std::move(response), now, false));
    const Codex429Classification parsed = co_await marking.wait();
    co_return parsed;
  }
}
