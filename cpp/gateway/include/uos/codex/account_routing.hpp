#pragma once
// Port of src/codex/account-routing.ts.
// Codex account routing: probes, recovery, serial selection (remainder of the original module), split out of src/codex_account_routing.ts.
//
// `export * from` routing-state, capacity-routing and rate-limit-429 are the explicit using lists at
// the end, as the TS places them.
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/rate_limit_429.hpp"
#include "uos/codex/routing_mutations.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uos::codex::account_routing {
  // export { claimCodexRoutingProbe, ..., releaseCodexRoutingProbe } from "./routing-mutations.ts"
  using routing_mutations::claimCodexRoutingProbe;
  using routing_mutations::getCodexQuotaBlockFence;
  using routing_mutations::isCodexQuotaBlockFenceCurrent;
  using routing_mutations::markCodexCredentialInvalid;
  using routing_mutations::markCodexRecoveryProbeQuotaBlocked;
  using routing_mutations::markCodexSuccess;
  using routing_mutations::markCodexUpstreamTimeout;
  using routing_mutations::reconcileCodexQuotaAfterStaleVerifiedReset;
  using routing_mutations::reconcileCodexQuotaAfterVerifiedReset;
  using routing_mutations::reconcileCodexRoutingAccount;
  using routing_mutations::releaseCodexRoutingProbe;

  // `value: unknown`: nullptr is undefined.
  bool isCodexActiveAccountSnapshotCurrent(const json::value& value, const routing_state::CodexActiveAccountSnapshot& snapshot);
  bool isCodexActiveAccountSnapshotCurrent(const json::value* value, const routing_state::CodexActiveAccountSnapshot& snapshot);

  /**
   * Elect the account a verified banked reset selected. The original active
   * snapshot must still be current, the candidate's pool credential and quota
   * routing fence must match, and every current selected pool account must still
   * be authoritatively exhausted: a concurrent active switch, a recovered
   * sibling, or newly eligible ordinary capacity wins over the stale reset. The
   * same account keeps its active generation; an account change advances it.
   */
  // The result is null as std::nullopt.
  [[nodiscard]] rt::Task<std::optional<routing_state::RoutingAccount>> electCodexResetRecoveryAccount(routing_state::RoutingAccount account,
                                                                                                    routing_state::CodexActiveAccountSnapshot originalActive,
                                                                                                    double quotaResetAtMs, bool staleVerified = false);

  /**
   * Final pre-transport admission. The active identity and generation must still
   * match the admitted request; the request's own probe claim or same-identity
   * credential refresh may have advanced the credential/routing fences, so those
   * are refreshed atomically without changing the active identity or generation.
   * A concurrent active switch, pool rotation, or conflicting writer fails.
   */
  [[nodiscard]] rt::Task<bool> refreshCodexActiveAccountAdmission(routing_state::RoutingAccount account);

  /**
   * Slot-scoped eligibility for the internal prompt-cache experiment and other
   * pinned callers that already chose their account. It hydrates the shared
   * routing/capacity caches and reconciles existing capacity observations exactly
   * as the pre-cutover helper did, so a following transition CAS is based on the
   * row this read returned. It never reads or writes the global active-selection
   * row; ordinary inference admissions must use `selectCodexRoutingAccountsStrong`.
   */
  // `now = Date.now()` and `model: string | null = null` are the overloads without them.
  [[nodiscard]] rt::Task<routing_state::RouteSelection> selectCodexRoutingAccounts(::uos::types::CodexAuthPoolState pool,
                                                                                   std::vector<::uos::types::CodexAuthState> orderedAccounts);
  [[nodiscard]] rt::Task<routing_state::RouteSelection> selectCodexRoutingAccounts(::uos::types::CodexAuthPoolState pool,
                                                                                   std::vector<::uos::types::CodexAuthState> orderedAccounts, double now);
  [[nodiscard]] rt::Task<routing_state::RouteSelection> selectCodexRoutingAccounts(::uos::types::CodexAuthPoolState pool,
                                                                                   std::vector<::uos::types::CodexAuthState> orderedAccounts, double now,
                                                                                   std::optional<std::string> model);

  // `now = Date.now()` and `model: string | null = null` are the overloads without them.
  [[nodiscard]] rt::Task<routing_state::RouteSelection> selectCodexRoutingAccountsStrong(::uos::types::CodexAuthPoolState pool,
                                                                                         std::vector<::uos::types::CodexAuthState> orderedAccounts);
  [[nodiscard]] rt::Task<routing_state::RouteSelection> selectCodexRoutingAccountsStrong(::uos::types::CodexAuthPoolState pool,
                                                                                         std::vector<::uos::types::CodexAuthState> orderedAccounts, double now);
  [[nodiscard]] rt::Task<routing_state::RouteSelection> selectCodexRoutingAccountsStrong(::uos::types::CodexAuthPoolState pool,
                                                                                         std::vector<::uos::types::CodexAuthState> orderedAccounts, double now,
                                                                                         std::optional<std::string> model);

  /**
   * Makes a manually redeemed reset eligible for one normal-request probe.
   * This never clears reset-generation ambiguity: only a successful probe can
   * prove recovery and authorize a later provisional identity.
   */
  [[nodiscard]] rt::Task<bool> recheckCodexRoutingSlot(double slotNumber);

  // The state, capacity and 429 layers moved into their own modules; they are
  // re-exported here until the rest of this module is extracted too.

  // export * from "./routing-state.ts" (which itself re-exports ../authority/native-codex-contracts.ts)
  using routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY;
  using routing_state::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY;
  using routing_state::CODEX_AUTH_POOL_KV_KEY;
  using routing_state::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY;
  using routing_state::CODEX_HALF_OPEN_LEASE_MS;
  using routing_state::CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS;
  using routing_state::CODEX_CAPACITY_ROUTING_MAX_AGE_MS;
  using routing_state::ROUTING_CACHE_REVALIDATE_MS;
  using routing_state::CodexQuotaBlockSource;
  using routing_state::CodexProbeCircuit;
  using routing_state::CodexQuotaClass;
  using routing_state::CodexActiveAccountTransitionReason;
  using routing_state::CodexActiveAccountSelection;
  using routing_state::CodexActiveAccountSnapshot;
  using routing_state::CodexQuotaClassBlock;
  using routing_state::CodexQuotaBlocksByClass;
  using routing_state::CodexProbeLease;
  using routing_state::CodexRoutingSlot;
  using routing_state::CodexAccountRoutingState;
  using routing_state::CodexRoutingAccount;
  using routing_state::CodexCapacityRoutingWindow;
  using routing_state::CodexCapacityRoutingWindows;
  using routing_state::CodexCapacityRoutingAdditionalRateLimit;
  using routing_state::CodexCapacityRoutingObservationInput;
  using routing_state::CodexCapacityRoutingObservation;
  using routing_state::CodexBlockedAccountMetadata;
  using routing_state::CodexRouteSelectionEligible;
  using routing_state::CodexRouteSelectionQuotaBlocked;
  using routing_state::CodexRouteSelectionUpstreamBlocked;
  using routing_state::CodexRouteSelectionModelUnavailable;
  using routing_state::CodexRouteSelectionCredentialsInvalid;
  using routing_state::CodexRouteSelectionRoutingUnavailable;
  using routing_state::CodexRouteSelection;
  using routing_state::isSafeMs;
  using routing_state::isLegacyStableResetIdentity;
  using routing_state::hasUnresolvedLegacyResetIdentity;
  using routing_state::parseOptionalSafeMs;
  using routing_state::parseCodexAccountRoutingState;
  using routing_state::isVersionstamp;
  using routing_state::parseCodexActiveAccountSelection;
  using routing_state::CodexRoutingIdentity;
  using routing_state::CodexRoutingAccountModelUnavailable;
  using routing_state::CodexRoutingAccountPolicy;
  using routing_state::CodexRoutingRowRevisions;
  using routing_state::CodexSerialAdmissionWrites;
  using routing_state::CodexSerialAdmissionPlan;
  using routing_state::codexActiveAccountSelectionCurrent;
  using routing_state::toJson;
  using routing_state::RoutingAccount;
  using routing_state::RoutingStateCache;
  using routing_state::routingStateCache;
  using routing_state::isCodexActiveAccountSelectionCurrent;
  using routing_state::setRoutingStateCache;
  using routing_state::getCachedRoutingState;
  using routing_state::CodexBlockedRoutingAccount;
  using routing_state::RouteSelectionEligible;
  using routing_state::RouteSelectionQuotaBlocked;
  using routing_state::RouteSelectionUpstreamBlocked;
  using routing_state::RouteSelectionModelUnavailable;
  using routing_state::RouteSelectionCredentialsInvalid;
  using routing_state::RouteSelectionRoutingUnavailable;
  using routing_state::RouteSelection;
  using routing_state::nativeCodexAccountMetadata;

  // export * from "./capacity-routing.ts" (its own re-exports from ../authority/native-codex-capacity.ts first)
  using capacity_routing::neutralSlot;
  using capacity_routing::parseFinitePercent;
  using capacity_routing::quotaHeadroomFor;
  using capacity_routing::capacityState;
  using capacity_routing::quotaClass;
  using capacity_routing::quotaBlockForClass;
  using capacity_routing::quotaBlocksIncludingLegacy;
  using capacity_routing::quotaSignalObservedAtForClass;
  using capacity_routing::quotaBlockKeyForClass;
  using capacity_routing::withoutQuotaClass;
  using capacity_routing::recheckQuotaClasses;
  using capacity_routing::releaseQuotaClassProbe;
  using capacity_routing::capacityHeadroomForObservation;
  using capacity_routing::capacityObservationIsFresh;
  using capacity_routing::parseStoredCapacityObservationStore;
  using capacity_routing::normalizeRoutingState;
  using capacity_routing::slotFor;
  using capacity_routing::slotMatchesRoutingAccount;
  using capacity_routing::routingProbeCircuit;
  using capacity_routing::probeLeaseMatchesRoutingAccount;
  using capacity_routing::withSlot;
  using capacity_routing::codexCredentialVersion;
  using capacity_routing::routingAccountIdentity;
  using capacity_routing::resetCodexAccountRoutingForTest;
  using capacity_routing::loadCodexAccountRouting;
  using capacity_routing::updateRoutingState;
  using capacity_routing::reconcileCapacityQuotaBlock;
  using capacity_routing::openRoutingKv;
  using capacity_routing::loadCodexCapacityRoutingObservations;
  using capacity_routing::reconcileCapacityRoutingState;
  using capacity_routing::recordCodexCapacityRoutingObservations;

  // export * from "./rate-limit-429.ts"
  using rate_limit_429::Codex429Classification;
  using rate_limit_429::CodexKnownProbeLease;
  using rate_limit_429::readCodex429;
  using rate_limit_429::markCodexQuotaBlockedWithMode;
  using rate_limit_429::markCodexQuotaBlocked;
}
