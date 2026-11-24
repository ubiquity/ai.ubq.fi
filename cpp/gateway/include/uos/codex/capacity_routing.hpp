#pragma once
// Port of src/codex/capacity-routing.ts.
// Routing-state cache, normalization and capacity-observation persistence over the source-owned
// native-codex-capacity decisions.
#include "uos/authority/native_codex_capacity.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::codex::capacity_routing {
  // export { neutralSlot, ..., parseStoredCapacityObservationStore } from "../authority/native-codex-capacity.ts"
  using authority::native_codex_capacity::neutralSlot;
  using authority::native_codex_capacity::parseFinitePercent;
  using authority::native_codex_capacity::quotaHeadroomFor;
  using authority::native_codex_capacity::capacityState;
  using authority::native_codex_capacity::quotaClass;
  using authority::native_codex_capacity::quotaBlockForClass;
  using authority::native_codex_capacity::quotaBlocksIncludingLegacy;
  using authority::native_codex_capacity::quotaSignalObservedAtForClass;
  using authority::native_codex_capacity::quotaBlockKeyForClass;
  using authority::native_codex_capacity::withoutQuotaClass;
  using authority::native_codex_capacity::recheckQuotaClasses;
  using authority::native_codex_capacity::releaseQuotaClassProbe;
  using authority::native_codex_capacity::capacityHeadroomForObservation;
  using authority::native_codex_capacity::capacityObservationIsFresh;
  using authority::native_codex_capacity::parseStoredCapacityObservationStore;

  // `raw: CodexAccountRoutingState | null` (std::nullopt is null); `now = Date.now()` is the
  // overload without it.
  [[nodiscard]] rt::Task<routing_state::CodexAccountRoutingState> normalizeRoutingState(std::optional<routing_state::CodexAccountRoutingState> raw,
                                                                                      ::uos::types::CodexAuthPoolState pool);
  [[nodiscard]] rt::Task<routing_state::CodexAccountRoutingState> normalizeRoutingState(std::optional<routing_state::CodexAccountRoutingState> raw,
                                                                                      ::uos::types::CodexAuthPoolState pool, double now,
                                                                                      bool discardLegacyTimeoutCircuit = false);

  routing_state::CodexRoutingSlot slotFor(const routing_state::CodexAccountRoutingState& state, const routing_state::RoutingAccount& account);

  bool slotMatchesRoutingAccount(const routing_state::CodexRoutingSlot& slot, const routing_state::RoutingAccount& account);

  routing_state::CodexProbeCircuit routingProbeCircuit(const routing_state::RoutingAccount& account);

  bool probeLeaseMatchesRoutingAccount(const routing_state::CodexRoutingSlot& slot, const routing_state::RoutingAccount& account);

  routing_state::CodexAccountRoutingState withSlot(const routing_state::CodexAccountRoutingState& state, double index,
                                                   const routing_state::CodexRoutingSlot& slot);

  [[nodiscard]] rt::Task<std::string> codexCredentialVersion(::uos::types::CodexAuthState auth);

  // `Readonly<{ accountIdHash: string; credentialVersion: string }>` (module-private in the TS): the
  // native-codex-capacity struct of exactly this shape.
  using CodexRoutingAccountIdentity = authority::native_codex_capacity::CodexRoutingAccountIdentity;

  [[nodiscard]] rt::Task<CodexRoutingAccountIdentity> routingAccountIdentity(::uos::types::CodexAuthState auth);

  void resetCodexAccountRoutingForTest();

  /** Loads routing alongside a cold auth read.  KV failure deliberately fails open. */
  [[nodiscard]] rt::Task<routing_state::CodexAccountRoutingState> loadCodexAccountRouting(::uos::types::CodexAuthPoolState pool);

  // `(state: CodexAccountRoutingState) => CodexAccountRoutingState | null` (module-private in the TS);
  // std::nullopt is null.
  using CodexRoutingTransform = std::function<std::optional<routing_state::CodexAccountRoutingState>(const routing_state::CodexAccountRoutingState& state)>;

  /**
   * Apply a small state transition against the latest durable record. This is
   * deliberately compare-and-set rather than a blind `set`: independent slot
   * transitions must not erase each other when different isolates observe
   * failures at the same time.
   */
  [[nodiscard]] rt::Task<std::optional<routing_state::CodexAccountRoutingState>> updateRoutingState(CodexRoutingTransform transform);

  /**
   * Persist a class block from one fresh exhausted capacity observation. The
   * pure transition owns validity and identity rules; this wrapper serializes it
   * through the same compare-and-set path as every other routing mutation.
   */
  // `now = Date.now()` is the overload without it.
  [[nodiscard]] rt::Task<bool> reconcileCapacityQuotaBlock(routing_state::RoutingAccount account, routing_state::CodexCapacityRoutingObservation observation);
  [[nodiscard]] rt::Task<bool> reconcileCapacityQuotaBlock(routing_state::RoutingAccount account, routing_state::CodexCapacityRoutingObservation observation,
                                                           double now);

  /** Routing state reads and writes fail closed when the KV binding itself is unavailable. */
  // `Promise<Kv | null>`: a null pointer is null.
  [[nodiscard]] rt::Task<std::shared_ptr<rt::Kv>> openRoutingKv();

  void setCapacityRoutingObservationCache(std::vector<routing_state::CodexCapacityRoutingObservation> observations, double now);

  [[nodiscard]] rt::Task<std::optional<bool>> readQuotaUsageProbePolicy(std::shared_ptr<rt::Kv> kv, std::string accountId);

  enum class QuotaUsageProbeResult { optedOut, refreshed, unavailable };
  [[nodiscard]] rt::Task<QuotaUsageProbeResult> refreshCodexQuotaUsage(routing_state::RoutingAccount account, std::optional<rt::AbortSignal> signal);
  [[nodiscard]] rt::Task<void> releaseCodexQuotaUsageProbe(routing_state::RoutingAccount account);

  [[nodiscard]] rt::Task<std::vector<routing_state::CodexCapacityRoutingObservation>> loadCodexCapacityRoutingObservations(::uos::types::CodexAuthPoolState pool,
                                                                                                                         bool forceKv = false);

  /**
   * The slot after a positive capacity observation. Clearing the requested class
   * circuit keeps a pending reset claim or half-open lease intact, and only the
   * last remaining class circuit releases the slot-wide reset scope.
   */
  // `model: string | null`
  [[nodiscard]] rt::Task<routing_state::CodexAccountRoutingState> reconcileCapacityRoutingState(
    routing_state::CodexAccountRoutingState state, std::vector<routing_state::CodexCapacityRoutingObservation> observations, double now,
    std::optional<std::string> model);

  /**
   * Persist the redacted, account-hash-bound sampler result and immediately
   * reconcile any matching routing slot. A positive observation clears only an
   * older quota signal; a newer inference 429 remains authoritative.
   */
  // `now = Date.now()` is the overload without it.
  [[nodiscard]] rt::Task<void> recordCodexCapacityRoutingObservations(std::vector<routing_state::CodexCapacityRoutingObservationInput> inputs);
  [[nodiscard]] rt::Task<void> recordCodexCapacityRoutingObservations(std::vector<routing_state::CodexCapacityRoutingObservationInput> inputs, double now);
}
