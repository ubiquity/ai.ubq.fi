#pragma once
// Port of src/codex/routing-state.ts.
// Native Codex auth-bearing routing facade and isolate cache.
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::codex::routing_state {
  // export * from "../authority/native-codex-contracts.ts"
  using authority::native_codex_contracts::CODEX_ACCOUNT_ROUTING_KV_KEY;
  using authority::native_codex_contracts::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY;
  using authority::native_codex_contracts::CODEX_AUTH_POOL_KV_KEY;
  using authority::native_codex_contracts::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY;
  using authority::native_codex_contracts::CODEX_HALF_OPEN_LEASE_MS;
  using authority::native_codex_contracts::CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS;
  using authority::native_codex_contracts::CODEX_CAPACITY_ROUTING_MAX_AGE_MS;
  using authority::native_codex_contracts::ROUTING_CACHE_REVALIDATE_MS;
  using authority::native_codex_contracts::CodexQuotaBlockSource;
  using authority::native_codex_contracts::CodexProbeCircuit;
  using authority::native_codex_contracts::CodexQuotaClass;
  using authority::native_codex_contracts::CodexActiveAccountTransitionReason;
  using authority::native_codex_contracts::CodexActiveAccountSelection;
  using authority::native_codex_contracts::CodexActiveAccountSnapshot;
  using authority::native_codex_contracts::CodexQuotaClassBlock;
  using authority::native_codex_contracts::CodexQuotaBlocksByClass;
  using authority::native_codex_contracts::CodexProbeLease;
  using authority::native_codex_contracts::CodexRoutingSlot;
  using authority::native_codex_contracts::CodexAccountRoutingState;
  using authority::native_codex_contracts::CodexRoutingAccount;
  using authority::native_codex_contracts::CodexCapacityRoutingWindow;
  using authority::native_codex_contracts::CodexCapacityRoutingWindows;
  using authority::native_codex_contracts::CodexCapacityRoutingAdditionalRateLimit;
  using authority::native_codex_contracts::CodexCapacityRoutingObservationInput;
  using authority::native_codex_contracts::CodexCapacityRoutingObservation;
  using authority::native_codex_contracts::CodexBlockedAccountMetadata;
  using authority::native_codex_contracts::CodexRouteSelectionEligible;
  using authority::native_codex_contracts::CodexRouteSelectionQuotaBlocked;
  using authority::native_codex_contracts::CodexRouteSelectionUpstreamBlocked;
  using authority::native_codex_contracts::CodexRouteSelectionModelUnavailable;
  using authority::native_codex_contracts::CodexRouteSelectionCredentialsInvalid;
  using authority::native_codex_contracts::CodexRouteSelectionRoutingUnavailable;
  using authority::native_codex_contracts::CodexRouteSelection;
  using authority::native_codex_contracts::isSafeMs;
  using authority::native_codex_contracts::isLegacyStableResetIdentity;
  using authority::native_codex_contracts::hasUnresolvedLegacyResetIdentity;
  using authority::native_codex_contracts::parseOptionalSafeMs;
  using authority::native_codex_contracts::parseCodexAccountRoutingState;
  using authority::native_codex_contracts::isVersionstamp;
  using authority::native_codex_contracts::parseCodexActiveAccountSelection;
  using authority::native_codex_contracts::CodexRoutingIdentity;
  using authority::native_codex_contracts::CodexRoutingAccountModelUnavailable;
  using authority::native_codex_contracts::CodexRoutingAccountPolicy;
  using authority::native_codex_contracts::CodexRoutingRowRevisions;
  using authority::native_codex_contracts::CodexSerialAdmissionWrites;
  using authority::native_codex_contracts::CodexSerialAdmissionPlan;
  using authority::native_codex_contracts::codexActiveAccountSelectionCurrent;
  using authority::native_codex_contracts::toJson;

  // `CodexRoutingAccount & Readonly<{ auth: CodexAuthState }>`
  struct RoutingAccount : CodexRoutingAccount {
    ::uos::types::CodexAuthState auth;
  };

  /** Shared routing-state cache: the capacity and probe paths write it, selectors read it. */
  struct RoutingStateCache {
    std::optional<CodexAccountRoutingState> state;
    std::optional<std::string> versionstamp;
    double loadedAtMs;
  };
  // Starts as `{ state: null, versionstamp: null, loadedAtMs: 0 }`.
  extern RoutingStateCache routingStateCache;

  /** Accessors for the routing-state cache, which the probe and selection code shares. */
  // `value: unknown`: nullptr is undefined.
  bool isCodexActiveAccountSelectionCurrent(const json::value* value, const RoutingAccount& account, double activeGeneration);
  bool isCodexActiveAccountSelectionCurrent(const json::value& value, const RoutingAccount& account, double activeGeneration);

  // `state: CodexAccountRoutingState | null, versionstamp?: string | null`: std::nullopt is null (and undefined).
  void setRoutingStateCache(std::optional<CodexAccountRoutingState> state, std::optional<std::string> versionstamp = std::nullopt);

  // A copy of the cached state; std::nullopt is null.
  std::optional<CodexAccountRoutingState> getCachedRoutingState();

  /** A durable provider-deadline circuit that may have an existing reset record to reconcile. */
  // `RoutingAccount & Readonly<{ quotaResetAtMs: number; routingGeneration: number }>`: routingGeneration is
  // the base member, always set on these values.
  struct CodexBlockedRoutingAccount : RoutingAccount {
    double quotaResetAtMs;
  };

  // The members of the RouteSelection union, by `kind`. The members that carry accounts hold
  // RoutingAccounts; the others have exactly the shape of the CodexRouteSelection members.
  struct RouteSelectionEligible {
    static constexpr std::string_view kind = "eligible";
    std::vector<RoutingAccount> accounts;
    std::vector<double> skippedSlots;
    /** Stable absolute quota fences omitted from ordinary eligible routing. */
    std::vector<CodexBlockedRoutingAccount> blockedAccounts;
  };
  struct RouteSelectionQuotaBlocked {
    static constexpr std::string_view kind = "quota_blocked";
    std::vector<double> skippedSlots;
    std::optional<double> retryAtMs;
    /** Stable absolute quota fences omitted from ordinary eligible routing. */
    std::vector<CodexBlockedRoutingAccount> blockedAccounts;
    /**
     * Every current pool account proved authoritative applicable quota or
     * capacity exhaustion. Credential failures never authorize quota resets.
     */
    bool fullCohortExhausted;
    /** Every current subscription is quota-exhausted or has invalid current credentials. */
    std::optional<bool> fullCohortUnavailable{};
    /** The active-selection row exactly as this strong read observed it. */
    CodexActiveAccountSnapshot activeSnapshot;
    /**
     * Canonical strong-read snapshots for pre-election reset fences. Ordered
     * pool membership, credential material, pool update identity, and the
     * stored capacity observations must all stay exactly as observed through
     * inventory and consume.
     */
    std::optional<std::string> poolSnapshotJson;
    std::optional<std::string> capacitySnapshotJson;
  };
  // `blockedAccounts: readonly []`
  using RouteSelectionUpstreamBlocked = CodexRouteSelectionUpstreamBlocked;
  /**
   * No configured account can serve the named model. This is neither a
   * quota nor a credential outcome: no fence was written, so the request is
   * answered as an unavailable model rather than a retryable failure.
   */
  using RouteSelectionModelUnavailable = CodexRouteSelectionModelUnavailable;
  using RouteSelectionCredentialsInvalid = CodexRouteSelectionCredentialsInvalid;
  using RouteSelectionRoutingUnavailable = CodexRouteSelectionRoutingUnavailable;
  using RouteSelection = std::variant<RouteSelectionEligible, RouteSelectionQuotaBlocked, RouteSelectionUpstreamBlocked, RouteSelectionModelUnavailable,
                                      RouteSelectionCredentialsInvalid, RouteSelectionRoutingUnavailable>;

  /** Project a native request account before entering the credential-free decision graph. */
  CodexRoutingAccount nativeCodexAccountMetadata(const RoutingAccount& account);
}
